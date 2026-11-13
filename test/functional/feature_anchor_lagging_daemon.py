#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A block left aside while the Bitcoin daemon is behind is taken once it catches up.

A node that watches Bitcoin rejects, for now, a block whose anchor its own
daemon cannot vouch for yet, and keeps no record of it, so that nothing is
marked invalid. That block must not then wait for the next announcement: as
soon as the daemon catches up, or answers again after being unreachable, the
node asks its peers for the block again.

Topology:
  node0: the parent chain (stands in for Bitcoin; an elementsregtest chain)
  node1: the producer, anchored to node0
  node2: the node under test, anchored to node3, its watcher polling every second
  node3: a second copy of the parent chain, cut off from node0 (a daemon that
         is behind) and connected again when the test lets it catch up

Covers:
 - the daemon is behind: node2 leaves the block aside, and its log says the
   daemon is behind, at which height, and where the anchor is;
 - the daemon catches up and nothing else happens (no new block, no
   announcement): node2 takes the block within a few seconds;
 - an anchor unknown to a daemon that does not change is asked for at most
   once, so a peer cannot turn the retry into a loop;
 - the daemon is unreachable, then answers again while still behind, then
   catches up: the log says it was unreachable, nothing is asked for while it
   is behind, and the block is taken once it has caught up.

See doc/sequentia/03-bitcoin-anchoring.md.
"""

import os
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    get_datadir_path,
    rpc_port,
    p2p_port,
)

PARENT_USER, PARENT_PASS = "parent", "parentpass"
TAKEN_WITHIN = 15  # seconds: a watcher poll, a getheaders and a block fetch


class AnchorLaggingDaemonTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def parent_args(self, i):
        return ["-port=" + str(p2p_port(i)), "-rpcport=" + str(rpc_port(i)), "-validatepegin=0",
                "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                "-signblockscript=51"]

    def anchored_args(self, i, parent):
        return ["-port=" + str(p2p_port(i)), "-rpcport=" + str(rpc_port(i)), "-validatepegin=0",
                "-initialfreecoins=10000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
                "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1",
                "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%s" % rpc_port(parent),
                "-mainchainrpcuser=%s" % PARENT_USER, "-mainchainrpcpassword=%s" % PARENT_PASS,
                "-parentgenesisblockhash=%s" % self.parentgenesisblockhash]

    def setup_network(self, split=False):
        self.nodes = []
        # Two copies of the parent chain with fixed credentials, so they can be
        # restarted under the same login.
        self.add_nodes(1, [self.parent_args(0)], chain=["elementsregtest"])
        self.fixed_credentials(0)
        self.start_node(0)
        self.parentgenesisblockhash = self.nodes[0].getblockhash(0)
        self.add_nodes(1, [self.anchored_args(1, 0)], chain=["elementsregtest"])
        self.add_nodes(1, [self.anchored_args(2, 3)], chain=["elementsregtest"])
        self.add_nodes(1, [self.parent_args(3)], chain=["elementsregtest"])
        self.fixed_credentials(3)
        self.start_node(3)
        for n in (1, 2):
            self.start_node(n)
        self.connect_nodes(1, 2)
        for n in self.nodes:
            n.createwallet(wallet_name="w", descriptors=True)

    def fixed_credentials(self, n):
        conf = [f for f in os.listdir(get_datadir_path(self.options.tmpdir, n)) if f.endswith(".conf")][0]
        with open(os.path.join(get_datadir_path(self.options.tmpdir, n), conf), "a", encoding="utf8") as f:
            f.write("rpcuser=%s\nrpcpassword=%s\n" % (PARENT_USER, PARENT_PASS))

    def parent_mine(self, n):
        node = self.nodes[0]
        return self.generatetoaddress(node, n, node.getnewaddress(), sync_fun=self.no_op)

    def produce(self, node):
        return self.generatetoaddress(node, 1, node.getnewaddress(), sync_fun=self.no_op)[0]

    def anchor(self, node, blockhash=None):
        h = node.getblockheader(blockhash or node.getbestblockhash())
        return h["anchorheight"], h["anchorhash"]

    def log_text(self, node):
        with open(os.path.join(node.datadir, "elementsregtest", "debug.log"), encoding="utf-8") as f:
            return f.read()

    def wait(self, pred, what, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return time.time()
            time.sleep(0.1)
        raise AssertionError("timed out: " + what)

    def catch_up(self):
        """Let the second parent copy reach the first one's tip."""
        self.connect_nodes(3, 0)
        self.sync_blocks([self.nodes[0], self.nodes[3]])
        self.disconnect_nodes(3, 0)

    def run_test(self):
        parent, prod, node, behind = self.nodes

        self.log.info("Both daemons at the same height; the anchored nodes agree")
        self.parent_mine(10)
        self.catch_up()
        for _ in range(2):
            self.produce(prod)
        self.sync_blocks([prod, node])

        self.log.info("node2's daemon falls behind: a block anchored above it is left aside")
        self.parent_mine(10)
        assert_equal(behind.getblockcount(), 10)
        old_tip = node.getbestblockhash()
        blk = self.produce(prod)
        assert_equal(self.anchor(prod, blk)[0], 20)
        self.wait(lambda: "cannot validate anchor %s" % self.anchor(prod, blk)[1] in self.log_text(node),
                  "node2 leaves the block aside", 30)
        assert_equal(node.getbestblockhash(), old_tip)

        self.log.info("The daemon catches up, and nothing else happens: node2 takes the block")
        t0 = time.time()
        self.catch_up()
        t1 = self.wait(lambda: node.getbestblockhash() == blk, "node2 takes the block left aside", 60)
        self.log.info("    taken %.1f s after its daemon caught up", t1 - t0)
        assert t1 - t0 < TAKEN_WITHIN, "the block waited %.1f s" % (t1 - t0)
        assert "asking peers again for the block(s) left aside" in self.log_text(node)

        self.log.info("The log said the daemon was behind, not unreachable")
        assert "the parent chain daemon is behind (at height 10, anchor at 20)" in self.log_text(node)
        assert "parent chain daemon unreachable" not in self.log_text(node)

        self.log.info("The daemon is unreachable: the block is left aside as unreachable")
        self.stop_node(3)
        self.parent_mine(2)
        old_tip = node.getbestblockhash()
        blk = self.produce(prod)
        assert_equal(self.anchor(prod, blk)[0], 22)
        self.wait(lambda: "parent chain daemon unreachable; the block is asked for again once it answers" in self.log_text(node),
                  "node2 leaves the block aside as unreachable", 30)
        assert_equal(node.getbestblockhash(), old_tip)

        self.log.info("It answers again, still behind: nothing is asked for until it has caught up")
        log_before = len(self.log_text(node))
        self.start_node(3)
        time.sleep(4)  # several watcher polls with the daemon answering
        assert "asking peers again" not in self.log_text(node)[log_before:]
        assert_equal(node.getbestblockhash(), old_tip)

        self.log.info("It catches up: node2 takes the block")
        t0 = time.time()
        self.catch_up()
        t1 = self.wait(lambda: node.getbestblockhash() == blk, "node2 takes the block left aside", 60)
        self.log.info("    taken %.1f s after its daemon caught up", t1 - t0)
        assert t1 - t0 < TAKEN_WITHIN, "the block waited %.1f s" % (t1 - t0)

        self.log.info("An anchor that stays unknown to an unchanged daemon is asked for once, not at every poll")
        # node2's daemon moves to a block of its own at the next height, so it
        # is synced there but never learns the block node1 anchors to.
        self.generatetoaddress(behind, 1, parent.getnewaddress(), sync_fun=self.no_op)
        self.parent_mine(1)
        assert parent.getbestblockhash() != behind.getbestblockhash()
        time.sleep(3)  # node2's watcher sees its daemon's move with nothing pending
        log_before = len(self.log_text(node))
        old_tip = node.getbestblockhash()
        blk = self.produce(prod)
        assert_equal(self.anchor(prod, blk)[0], 23)
        self.wait(lambda: "not (yet) known to the parent chain daemon" in self.log_text(node)[log_before:],
                  "node2 leaves the block aside as unknown", 30)
        time.sleep(8)  # eight watcher polls
        asks = self.log_text(node)[log_before:].count("asking peers again")
        self.log.info("    peers asked again %d time(s) in eight polls", asks)
        assert asks <= 1, "asked %d times on an unchanged daemon" % asks
        assert_equal(node.getbestblockhash(), old_tip)


if __name__ == '__main__':
    AnchorLaggingDaemonTest().main()
