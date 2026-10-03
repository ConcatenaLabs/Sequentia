#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Whether a block is certified is judged against its parent's quorum.

A block's certificate is verified against the certification quorum of the
stake state its parent leaves. Fork choice ranks same-height blocks first by
whether they are certified, and the finality pass finalizes only certified
blocks, so that answer has to be the same on every node holding the block.
Measured against each node's own tip it is not, whenever one of two siblings
changes the committee: the node whose tip is the sibling that registers a new
committee member sees a larger committee and a higher quorum, and judges the
other sibling, which carries exactly the parent's quorum, uncertified. The two
nodes then pick different siblings, finalize them when the observation window
closes, and refuse each other's branch with bad-fork-prior-to-pos-final.

Public BLS committee, cap 5, four stakers from configuration (k=4, quorum 3)
and a fifth, E, that registers its BLS key in a staking output. Two siblings
by one leader, so their VRF scores are equal and the lower hash decides:
A carries E's registration and four signatures, after which the committee is
five and the quorum 4; B carries three. Both carry their parent's quorum, so
both are certified on every node. The pair is rebuilt until B has the lower
hash, the order in which a node judging by its own tip goes wrong. X receives
A first, Y receives B first. Both must pick B, finalize B, and stay on one
chain once connected and extended.
"""
import os
import shutil
import time

from test_framework.authproxy import JSONRPCException
from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

FEE = 100_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def hash_order(h):
    """The order of uint256::operator<, which the comparator's hash key uses:
    the stored bytes, least significant first, i.e. the displayed hex reversed."""
    return bytes.fromhex(h)[::-1]


class PosCertifiedParentQuorumTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4   # X, Y, Z1 (builds A), Z2 (builds B)
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(4)]
        self.e_wif, self.e_pub = make_staker()
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=5",
            "-posslotinterval=1", "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=0", "-initialfreecoins=1000000000000",
            "-con_connect_genesis_outputs=1", "-anyonecanspendaremine=1", "-validatepegin=0",
            "-acceptnonstdtxn=1", "-par=1",
        ]
        self.extra_args = [list(self.common) for _ in range(self.num_nodes)]

    def setup_network(self):
        # The stakers' BLS registrations are derived by a running node.
        self.add_nodes(self.num_nodes, self.extra_args)
        self.start_node(0, extra_args=self.common + ["-staker=%s:1" % pub for _, pub in self.stakers])
        self.specs = ["-staker=%s:1%s" % (pub, self.nodes[0].getblsregistration(wif)["spec"])
                      for wif, pub in self.stakers]
        ereg = self.nodes[0].getblsregistration(self.e_wif)
        self.e_stake = self.nodes[0].getstakescript(self.e_pub, 20, None, ereg["blspubkey"], ereg["pop"])["script"]
        self.stop_node(0)
        shutil.rmtree(os.path.join(self.nodes[0].datadir, self.chain))
        for i in range(self.num_nodes):
            self.start_node(i, extra_args=self.common + self.specs)
        self.connect_all()

    def connect_all(self):
        for a in range(self.num_nodes):
            for b in range(a + 1, self.num_nodes):
                self.connect_nodes(a, b)

    def disconnect_all(self):
        for a in range(self.num_nodes):
            for b in range(a + 1, self.num_nodes):
                self.disconnect_nodes(a, b)

    def produce(self, node, signers, leaders=None):
        """A block led by the first leader whose slot opens, countersigned by
        `signers` other stakers."""
        wifs = [w for w, _ in self.stakers]
        last = None
        for _ in range(60):
            for leader in (leaders or wifs):
                others = [w for w in wifs if w != leader][:signers]
                try:
                    return node.generateposblock(leader, others)
                except JSONRPCException as e:
                    last = e.error
            time.sleep(0.5)
        raise AssertionError("could not produce: %s" % last)

    def free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for out in tx['vout']:
                if out['scriptPubKey']['hex'] == '51' and out.get('value', 0) > 0 and node.gettxout(tx['txid'], out['n']):
                    return tx['txid'], out['n'], int(out['value'] * COIN)
        raise AssertionError("no free coin")

    def bad_fork_lines(self, node):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return [line for line in f if 'bad-fork-prior-to-pos-final' in line]

    def run_test(self):
        x, y, z1, z2 = self.nodes
        leader = self.stakers[0][0]

        for _ in range(2):
            self.produce(x, 3, leaders=[leader])
        self.sync_blocks()
        parent_height = x.getblockcount()
        parent = x.getbestblockhash()
        assert_equal(x.getblockheader(parent)["posquorum"], 3)
        self.disconnect_all()

        self.log.info("E registers a committee key in A only")
        txid, n, amount = self.free_coin(z1)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n))]
        tx.vout = [CTxOut(2 * COIN, bytes.fromhex(self.e_stake)),
                   CTxOut(amount - 2 * COIN - FEE, CScript([OP_TRUE])), CTxOut(FEE)]
        registration = z1.sendrawtransaction(tx.serialize().hex())

        for _ in range(20):
            a = self.produce(z1, 3, leaders=[leader])   # A: E's registration, 4 signatures
            b = self.produce(z2, 2, leaders=[leader])   # B: 3 signatures
            A, B = a["hash"], b["hash"]
            if hash_order(B) < hash_order(A):
                break
            z1.invalidateblock(A)
            z2.invalidateblock(B)
            time.sleep(1.1)
        else:
            raise AssertionError("B never had the lower hash")
        assert_equal(a["countersignatures"], 4)
        assert_equal(b["countersignatures"], 3)
        assert registration in z1.getblock(A)["tx"]
        assert registration not in z2.getblock(B)["tx"]
        block_a, block_b = z1.getblock(A, 0), z2.getblock(B, 0)

        self.log.info("X receives A then B, Y receives B then A: both are certified on both")
        # submitblock answers "inconclusive" for a valid block that does not
        # become the tip.
        assert_equal(x.submitblock(block_a), None)
        assert_equal(x.getbestblockhash(), A)
        assert_equal(x.submitblock(block_b), None)          # B displaces A on X
        assert_equal(y.submitblock(block_b), None)
        assert_equal(y.submitblock(block_a), "inconclusive")  # and A does not displace B on Y
        for node in (x, y):
            for h in (A, B):
                header = node.getblockheader(h)
                assert_equal(header["previousblockhash"], parent)
                assert_equal(header["posquorum"], 3)        # the parent's quorum, whichever tip
                assert_equal(header["poscertified"], True)
            assert_equal(node.getbestblockhash(), B)        # equal VRF score: the lower hash

        self.log.info("Both finalize B once the observation window has passed")
        for node in (x, y):
            self.wait_until(lambda: node.getposfinality()["finalized_height"] == parent_height + 1)
            assert_equal(node.getposfinality()["finalized_hash"], B)

        # Z1 and Z2 each held only their own sibling through the window and
        # finalized it, as any node that never saw the other certificate does.
        self.log.info("Connected and extended, X and Y stay on one chain")
        self.connect_nodes(0, 1)
        self.produce(y, 3)
        self.sync_blocks([x, y])
        assert_equal(x.getbestblockhash(), y.getbestblockhash())
        for node in (x, y):
            assert_equal(node.getblockhash(parent_height + 1), B)
            assert_equal(self.bad_fork_lines(node), [])


if __name__ == '__main__':
    PosCertifiedParentQuorumTest().main()
