#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Members locked on a rival of a certified parent sign above it once it is
final, without waiting for Bitcoin (#369).

The share-lock's ancestry arm stops a member that share-signed block X at
height h from signing anything above a different block X' that the chain took
at h. It used to release only when the escaping-stall gap on Bitcoin passed
(about three Bitcoin blocks), even when X' was certified. When enough seats
were locked that way fewer than a quorum could sign at h+1, and block
production stopped until Bitcoin moved, or for good while it did not. A
certified parent is now treated like any other certified block: once it is
final on the member's node (its observation window has passed) the member
signs above it.

Topology: node0 is the parent ("Bitcoin") chain, mined only during warm-up and
then left still; nodes 1-4 are autonomous producers A, B, C and D, one seat
each on a public BLS committee (quorum 3); node5, Z, builds the rival block.

  1. Producers certify a few blocks together.
  2. They are split into {A, B} and {C, D}. Neither half holds a quorum, so
     each runs rounds that never certify, and every member is left
     share-locked on a block of its own half at height h.
  3. Z certifies X' at h with three of the keys, and every producer receives
     it. No member signed X', so all four are locked on a rival of their tip.
  4. Reconnected, they must certify h+1 while Bitcoin does not move.
"""
import os
import shutil
import time

from test_framework.authproxy import JSONRPCException
from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_auth_cookie, get_datadir_path, p2p_port, rpc_port

PARENT_BLOCK_SECONDS = 600
PARENT_WARMUP_BLOCKS = 12
PRODUCERS = [1, 2, 3, 4]
Z = 5


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosAncestryLockCertifiedParentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 6
        self.stakers = [make_staker() for _ in range(4)]

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

    def node_args(self, i):
        args = self.anchored_args(i) + self.specs
        if i in PRODUCERS:
            args += ["-posproducer=1", "-posproducerkey=%s" % self.stakers[i - 1][0]]
        return args

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

        self.add_nodes(5, [self.anchored_args(i) for i in range(1, 6)], chain=[chain] * 5)
        # The stakers' BLS registrations are derived by a running node.
        self.start_node(Z, extra_args=self.anchored_args(Z) + ["-staker=%s:1" % pub for _, pub in self.stakers])
        self.specs = ["-staker=%s:1%s" % (pub, self.nodes[Z].getblsregistration(wif)["spec"])
                      for wif, pub in self.stakers]
        self.stop_node(Z)
        shutil.rmtree(os.path.join(self.nodes[Z].datadir, chain))

    def advance_parent(self, blocks):
        """Parent blocks one Bitcoin interval apart, so median-time-past moves."""
        parent = self.nodes[0]
        addr = parent.getnewaddress()
        for _ in range(blocks):
            self.parent_time += PARENT_BLOCK_SECONDS
            parent.setmocktime(self.parent_time)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def connect_producers(self, pairs):
        for a, b in pairs:
            self.connect_nodes(a, b)

    def log_count(self, node, text):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return sum(1 for line in f if text in line)

    def run_test(self):
        parent = self.nodes[0]
        self.advance_parent(PARENT_WARMUP_BLOCKS)
        parent_height = parent.getblockcount()
        for i in range(1, 6):
            self.start_node(i, extra_args=self.node_args(i))
        mesh = [(a, b) for a in PRODUCERS for b in PRODUCERS if a < b] + [(1, Z)]
        self.connect_producers(mesh)
        producers = [self.nodes[i] for i in PRODUCERS]
        z = self.nodes[Z]

        self.log.info("1) The producers certify a few blocks together")
        self.wait_until(lambda: min(n.getblockcount() for n in producers) >= 3, timeout=120)
        self.sync_blocks(producers + [z])

        self.log.info("2) Split into {A, B} and {C, D}: no half holds a quorum")
        for a, b in mesh:
            self.disconnect_nodes(a, b)
        self.connect_producers([(1, 2), (3, 4)])
        base = producers[0].getblockcount()
        base_hash = producers[0].getbestblockhash()
        h = base + 1
        restart = "no certificate at height %d after" % h
        # Every member share-signs its half's proposals in the rounds that run.
        self.wait_until(lambda: all(self.log_count(n, restart) >= 1 for n in producers), timeout=120)
        for n in producers + [z]:
            assert_equal(n.getbestblockhash(), base_hash)

        self.log.info("3) Z certifies a rival X' at h that no producer signed; every producer takes it")
        wifs = [w for w, _ in self.stakers]
        res, last = None, None
        for _ in range(60):
            for leader in wifs:
                try:
                    res = z.generateposblock(leader, [w for w in wifs if w != leader][:2])
                    break
                except JSONRPCException as e:
                    last = e.error
            if res:
                break
            time.sleep(0.5)
        assert res, "could not certify X': %s" % last
        x_prime = res["hash"]
        assert_equal(z.getblockheader(x_prime)["poscertified"], True)
        raw = z.getblock(x_prime, 0)
        for n in producers:
            assert n.submitblock(raw) in (None, "duplicate")
        for n in producers:
            self.wait_until(lambda: n.getbestblockhash() == x_prime, timeout=30)

        self.log.info("4) Reconnected, with Bitcoin still at height %d, they certify h+1" % parent_height)
        # Every member is share-locked on a block of its own half: each queried
        # its peers for that block's certificate when its collection restarted.
        for n in producers:
            assert self.log_count(n, "share-lock at ") >= 1, "a member was not share-locked"
        self.connect_producers([(a, b) for a, b in mesh if (a, b) not in ((1, 2), (3, 4))])
        t0 = time.time()
        self.wait_until(lambda: min(n.getblockcount() for n in producers) >= h + 1, timeout=60)
        self.log.info("  height %d certified %.1f s after reconnecting" % (h + 1, time.time() - t0))
        assert_equal(parent.getblockcount(), parent_height)
        self.sync_blocks(producers)
        for n in producers:
            assert_equal(n.getblockhash(h), x_prime)
            self.wait_until(lambda: n.getposfinality()["finalized_height"] >= h + 1, timeout=60)


if __name__ == '__main__':
    PosAncestryLockCertifiedParentTest().main()
