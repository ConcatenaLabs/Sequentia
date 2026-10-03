#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Blocks escaping a stall are judged against their parent's quorum, on the
anchored public committee, and keep that answer across a restart.

Under the public committee a block that may escape a stall (its anchor at
least three parent-chain blocks past its parent's) is valid with fewer
signatures than the quorum, so whether it is certified is not in its headers:
its count is judged against the quorum of the stake state its parent leaves.
That is the quorum ConnectBlock verifies the certificate against, the same on
every node; the quorum of a node's own tip is not, whenever a block changes
the committee.

Topology: node0 is the parent ("Bitcoin") chain, mined at a Bitcoin cadence
with setmocktime; nodes 1-4 are anchored PoS nodes X, Y, Z1, Z2 on a public BLS
committee of cap 5 with four stakers from configuration (quorum 3) and a fifth,
E, registering its key at runtime. After a certified parent P the parent chain
advances three blocks, so P's children may escape a stall. Three siblings by
one leader, so the hash decides between equally certified ones:

  A  E's registration, four signatures: certified; after A the quorum is 4
  B  three signatures: certified, the parent's quorum exactly
  S  two signatures: below the parent's quorum, valid only as a stall escape

rebuilt until S < B < A by hash, so B wins only if it is certified on the node
holding A, and S loses only because it is not certified. X receives A first,
Y receives B first; both must pick B and finalize it. X restarts and still
holds A and B as certified, an answer that needs the parent's stake state and
so is kept from when each block connected. Connected again, X and Y extend
one chain.
"""
import os
import shutil
import time

from test_framework.authproxy import JSONRPCException
from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut, CTxOutAsset
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_auth_cookie, get_datadir_path, p2p_port, rpc_port

FEE = 100_000
PARENT_BLOCK_SECONDS = 600
PARENT_WARMUP_BLOCKS = 12


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def hash_order(h):
    """The order of uint256::operator<, which the comparator's hash key uses."""
    return bytes.fromhex(h)[::-1]


class PosCertifiedStallQuorumTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 5
        self.stakers = [make_staker() for _ in range(4)]
        self.e_wif, self.e_pub = make_staker()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def anchored_args(self, i):
        return [
            "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=5",
            "-posslotinterval=1", "-con_max_block_sig_size=8000",
            "-con_blocksubsidy=0", "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1",
            "-acceptnonstdtxn=1", "-par=1",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % self.rpc_u, "-mainchainrpcpassword=%s" % self.rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
        ]
        self.add_nodes(1, [parent_args], chain=[chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)
        self.parent_time = self.nodes[0].getblockheader(self.parentgenesis)['time']
        self.rpc_u, self.rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

        self.add_nodes(4, [self.anchored_args(i) for i in range(1, 5)], chain=[chain] * 4)
        # The stakers' BLS registrations are derived by a running node.
        self.start_node(1, extra_args=self.anchored_args(1) + ["-staker=%s:1" % pub for _, pub in self.stakers])
        self.specs = ["-staker=%s:1%s" % (pub, self.nodes[1].getblsregistration(wif)["spec"])
                      for wif, pub in self.stakers]
        ereg = self.nodes[1].getblsregistration(self.e_wif)
        self.e_stake = self.nodes[1].getstakescript(self.e_pub, 20, None, ereg["blspubkey"], ereg["pop"])["script"]
        self.stop_node(1)
        shutil.rmtree(os.path.join(self.nodes[1].datadir, chain))
        for i in range(1, 5):
            self.start_node(i, extra_args=self.anchored_args(i) + self.specs)
        self.connect_all()

    def connect_all(self):
        for a in range(1, 5):
            for b in range(a + 1, 5):
                self.connect_nodes(a, b)

    def disconnect_all(self):
        for a in range(1, 5):
            for b in range(a + 1, 5):
                self.disconnect_nodes(a, b)

    def advance_parent(self, blocks):
        """Parent blocks one Bitcoin interval apart, so median-time-past moves."""
        parent = self.nodes[0]
        addr = parent.getnewaddress()
        for _ in range(blocks):
            self.parent_time += PARENT_BLOCK_SECONDS
            parent.setmocktime(self.parent_time)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def produce(self, node, signers, leaders=None):
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

    def sibling(self, node, signers, leader, lower_than):
        """A child of the tip with `signers` countersignatures and a hash below
        `lower_than` (None: any)."""
        for _ in range(30):
            res = self.produce(node, signers, leaders=[leader])
            if lower_than is None or hash_order(res["hash"]) < hash_order(lower_than):
                return res
            node.invalidateblock(res["hash"])
            time.sleep(1.1)
        raise AssertionError("no sibling with the wanted hash order")

    def free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for out in tx['vout']:
                if out['scriptPubKey']['hex'] == '51' and out.get('value', 0) > 0 and node.gettxout(tx['txid'], out['n']):
                    asset = CTxOutAsset(b'\x01' + bytes.fromhex(out['asset'])[::-1])
                    return tx['txid'], out['n'], int(out['value'] * COIN), asset
        raise AssertionError("no free coin")

    def bad_fork_lines(self, node):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return [line for line in f if 'bad-fork-prior-to-pos-final' in line]

    def run_test(self):
        _, x, y, z1, z2 = self.nodes
        leader = self.stakers[0][0]

        self.advance_parent(PARENT_WARMUP_BLOCKS)
        for _ in range(2):
            self.produce(x, 3, leaders=[leader])
        self.sync_blocks(self.nodes[1:])
        parent_height = x.getblockcount()
        parent = x.getbestblockhash()
        assert_equal(x.getblockheader(parent)["poscertified"], True)
        parent_anchor = x.getblockheader(parent)["anchorheight"]

        self.log.info("The parent chain advances three blocks: P's children may escape a stall")
        self.advance_parent(3)
        self.disconnect_all()

        txid, n, amount, asset = self.free_coin(z1)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n))]
        tx.vout = [CTxOut(2 * COIN, bytes.fromhex(self.e_stake), asset),
                   CTxOut(amount - 2 * COIN - FEE, CScript([OP_TRUE]), asset), CTxOut(FEE, b'', asset)]
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
        z2.invalidateblock(B)
        s = self.sibling(z2, 1, leader, lower_than=B)    # S: 2 signatures, below the quorum
        S = s["hash"]
        assert_equal((a["countersignatures"], b["countersignatures"], s["countersignatures"]), (4, 3, 2))
        assert registration in z1.getblock(A)["tx"]
        for h, node in ((A, z1), (B, z2), (S, z2)):
            header = node.getblockheader(h)
            assert_equal(header["previousblockhash"], parent)
            assert header["anchorheight"] >= parent_anchor + 3, "not a stall escape"
        block_a, block_b, block_s = z1.getblock(A, 0), z2.getblock(B, 0), z2.getblock(S, 0)

        self.log.info("X receives A, B, S; Y receives B, A, S: each judged against P's quorum")
        assert_equal(x.submitblock(block_a), None)
        assert_equal(x.getbestblockhash(), A)
        assert_equal(x.submitblock(block_b), None)           # B displaces A on X
        assert_equal(x.submitblock(block_s), "inconclusive")
        assert_equal(y.submitblock(block_b), None)
        assert_equal(y.submitblock(block_a), "inconclusive")
        assert_equal(y.submitblock(block_s), "inconclusive")  # lower hash, but not certified
        for node in (x, y):
            for h, certified in ((A, True), (B, True), (S, False)):
                header = node.getblockheader(h)
                assert_equal(header["posquorum"], 3)
                assert_equal(header["poscertified"], certified)
            assert_equal(node.getbestblockhash(), B)

        self.log.info("Both finalize B")
        for node in (x, y):
            self.wait_until(lambda: node.getposfinality()["finalized_height"] == parent_height + 1)
            assert_equal(node.getposfinality()["finalized_hash"], B)

        self.log.info("After a restart X still holds A and B as certified, S as not")
        self.restart_node(1, extra_args=self.anchored_args(1) + self.specs)
        assert_equal(x.getbestblockhash(), B)
        for h, certified in ((A, True), (B, True), (S, False)):
            assert_equal(x.getblockheader(h)["poscertified"], certified)

        self.log.info("Connected and extended, X and Y stay on one chain")
        self.connect_nodes(1, 2)
        self.produce(y, 3)
        self.sync_blocks([x, y])
        for node in (x, y):
            assert_equal(node.getblockhash(parent_height + 1), B)
            assert_equal(self.bad_fork_lines(node), [])


if __name__ == '__main__':
    PosCertifiedStallQuorumTest().main()
