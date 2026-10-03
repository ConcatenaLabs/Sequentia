#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The finalized point survives a restart exactly.

The immediate-finality point is written with the block index and restored from
it at startup. A restart neither moves it nor finalizes the tip merely because
it is the tip:

 1. A block inside its observation window when the node stops is not final
    after the restart. Its window runs again from load time, so a sibling that
    ranks above it and arrives then is adopted, as it would have been without
    the restart, and the node ends on the same block as one that never
    restarted.
 2. A block final before the restart is final after it, at the same height,
    even when that block registered a committee key and so raised the quorum
    for the blocks after it. A longer rival branch forking below it is refused
    before and after the restart.

Public BLS committee of cap 5, four stakers from configuration (quorum 3) and a
fifth, E, registering its key in block R of part 2 (quorum 4 after R). Nodes:
X and Y (a 10 s observation window), Z the producer, V (the default window), W
isolated from the start, with a longer branch of its own from genesis.
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
WINDOW_MS = 10_000
X, Y, Z, V, W = range(5)


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def hash_order(h):
    """The order of uint256::operator<, which the comparator's hash key uses."""
    return bytes.fromhex(h)[::-1]


class PosFinalityRestartTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 5
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

    def args_for(self, i):
        args = self.common + self.specs
        if i in (X, Y):
            args = args + ["-posfinalitydelayms=%d" % WINDOW_MS]
        return args

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
            self.start_node(i, extra_args=self.args_for(i))

    def produce(self, node, signers):
        """A block led by staker 0, countersigned by `signers` other stakers."""
        leader = self.stakers[0][0]
        others = [w for w, _ in self.stakers[1:]][:signers]
        last = None
        for _ in range(60):
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

    def final(self, node):
        f = node.getposfinality()
        return f["finalized_height"], f.get("finalized_hash")

    def bad_fork_lines(self, node):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return [line for line in f if 'bad-fork-prior-to-pos-final' in line]

    def offer(self, node, branch):
        for block in branch:
            node.submitblock(block)

    def run_test(self):
        x, y, z, v, w = self.nodes

        # W builds a branch of its own from genesis, isolated throughout.
        for _ in range(8):
            self.produce(w, 2)
        w_branch = [w.getblock(w.getblockhash(h), 0) for h in range(1, w.getblockcount() + 1)]

        self.log.info("Part 1: a block inside its window when the node stops")
        self.connect_nodes(Z, X)
        self.connect_nodes(Z, Y)
        for _ in range(2):
            self.produce(z, 2)
        self.sync_blocks([x, y, z])
        final_2 = (2, z.getblockhash(2))
        for node in (x, y):
            self.wait_until(lambda: self.final(node) == final_2, timeout=60)
        self.disconnect_nodes(Z, X)
        self.disconnect_nodes(Z, Y)

        # Two builds of block 3 by the same leader, so equal VRF scores: the
        # one with the lower hash, S2, ranks above the other, S1.
        a = self.produce(z, 2)["hash"]
        a_hex = z.getblock(a, 0)
        z.invalidateblock(a)
        time.sleep(1.1)
        b = self.produce(z, 2)["hash"]
        b_hex = z.getblock(b, 0)
        z.invalidateblock(b)
        assert a != b
        (s1, s1_hex), (s2, s2_hex) = sorted([(a, a_hex), (b, b_hex)], key=lambda e: hash_order(e[0]), reverse=True)
        for node in (x, y):
            assert_equal(node.submitblock(s1_hex), None)
            assert_equal(node.getbestblockhash(), s1)
            assert_equal(self.final(node), final_2)

        # Y does not restart: S2 arrives inside S1's window and wins.
        assert_equal(y.submitblock(s2_hex), None)
        assert_equal(y.getbestblockhash(), s2)

        # X restarts with S1 inside its window: S1 is its tip, not final.
        self.restart_node(X, extra_args=self.args_for(X))
        assert_equal(x.getbestblockhash(), s1)
        assert_equal(self.final(x), final_2)
        assert_equal(x.submitblock(s2_hex), None)
        assert_equal(x.getbestblockhash(), s2)
        assert_equal(self.bad_fork_lines(x), [])
        for node in (x, y):
            self.wait_until(lambda: self.final(node) == (3, s2), timeout=60)

        self.log.info("Part 2: the quorum rises, then a restart")
        self.connect_nodes(Z, V)
        for _ in range(3):
            self.produce(z, 2)              # three signatures, the quorum of four stakers
        self.sync_blocks([z, v])
        txid, n, amount = self.free_coin(z)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n))]
        tx.vout = [CTxOut(2 * COIN, bytes.fromhex(self.e_stake)),
                   CTxOut(amount - 2 * COIN - FEE, CScript([OP_TRUE])), CTxOut(FEE)]
        registration = z.sendrawtransaction(tx.serialize().hex())
        r = self.produce(z, 2)              # R: E's registration, three signatures
        assert registration in z.getblock(r["hash"])["tx"]
        self.sync_blocks([z, v])
        self.disconnect_nodes(Z, V)
        header = v.getblockheader(r["hash"])
        assert_equal((header["poscountersigs"], header["posquorum"], header["poscertified"]), (3, 3, True))
        final_r = (r["height"], r["hash"])
        self.wait_until(lambda: self.final(v) == final_r, timeout=60)
        assert w.getblockcount() > v.getblockcount()

        self.log.info("W's longer branch, forking below R, is refused before the restart")
        self.offer(v, w_branch)
        assert_equal(v.getbestblockhash(), r["hash"])
        assert self.bad_fork_lines(v)

        self.log.info("and after it: W's branch, stored, is refused again as the node loads")
        with v.assert_debug_log(["bad-fork-prior-to-pos-final"]):
            self.restart_node(V, extra_args=self.args_for(V))
            assert_equal(self.final(v), final_r)
        assert_equal(v.getbestblockhash(), r["hash"])
        self.offer(v, w_branch)
        assert_equal(v.getbestblockhash(), r["hash"])
        assert_equal(self.final(v), final_r)


if __name__ == '__main__':
    PosFinalityRestartTest().main()
