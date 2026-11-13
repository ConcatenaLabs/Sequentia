#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A producer does not build on a parent whose anchor its Bitcoin daemon shows replaced.

Topology:
  node0: the parent chain (stands in for Bitcoin; an elementsregtest chain)
  node1: an anchored node whose watcher polls every second
  node2: the producer under test, whose watcher polls every 60 seconds
  node3: a second parent chain with fewer blocks (a daemon that is behind)

Covers:
 - after a parent reorganization replaces the tip's anchor, node2's template
   is refused before its own watcher has ticked, instead of producing a block
   on a parent the rest of the network has already disconnected;
 - the refusal wakes node2's watcher, which disconnects the stale tip at once,
   and production resumes on the canonical anchor;
 - with the daemon down, and with a daemon that is behind, the producer keeps
   producing (the check needs an answer it can trust).

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


class AnchorStaleParentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def parent_args(self, i):
        return ["-port=" + str(p2p_port(i)), "-rpcport=" + str(rpc_port(i)), "-validatepegin=0",
                "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                "-signblockscript=51"]

    def anchored_args(self, i, parent, poll):
        return ["-port=" + str(p2p_port(i)), "-rpcport=" + str(rpc_port(i)), "-validatepegin=0",
                "-initialfreecoins=10000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
                "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=%d" % poll,
                "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%s" % rpc_port(parent),
                "-mainchainrpcuser=%s" % PARENT_USER, "-mainchainrpcpassword=%s" % PARENT_PASS,
                "-parentgenesisblockhash=%s" % self.parentgenesisblockhash]

    def setup_network(self, split=False):
        self.nodes = []
        # Two parent chains with fixed credentials, so they can be restarted.
        self.add_nodes(1, [self.parent_args(0)], chain=["elementsregtest"])
        for n in [0]:
            self.fixed_credentials(n)
        self.start_node(0)
        self.parentgenesisblockhash = self.nodes[0].getblockhash(0)
        self.add_nodes(2, [self.anchored_args(1, 0, 1), self.anchored_args(2, 0, 60)],
                       chain=["elementsregtest", "elementsregtest"])
        self.add_nodes(1, [self.parent_args(3)], chain=["elementsregtest"])
        self.fixed_credentials(3)
        for n in (1, 2, 3):
            self.start_node(n)
        self.connect_nodes(1, 2)
        for n in self.nodes:
            n.createwallet(wallet_name="w", descriptors=True)

    def fixed_credentials(self, n):
        conf = [f for f in os.listdir(get_datadir_path(self.options.tmpdir, n)) if f.endswith(".conf")][0]
        with open(os.path.join(get_datadir_path(self.options.tmpdir, n), conf), "a", encoding="utf8") as f:
            f.write("rpcuser=%s\nrpcpassword=%s\n" % (PARENT_USER, PARENT_PASS))

    def parent_mine(self, n, node=None):
        node = node or self.nodes[0]
        return self.generatetoaddress(node, n, node.getnewaddress(), sync_fun=self.no_op)

    def produce(self, node):
        return self.generatetoaddress(node, 1, node.getnewaddress(), sync_fun=self.no_op)[0]

    def anchor(self, node, blockhash=None):
        h = node.getblockheader(blockhash or node.getbestblockhash())
        return h["anchorheight"], h["anchorhash"]

    def wait(self, pred, what, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return time.time()
            time.sleep(0.1)
        raise AssertionError("timed out: " + what)

    def run_test(self):
        parent, fast, prod, behind = self.nodes
        self.parent_mine(20)
        self.parent_mine(10, behind)  # the second parent chain stays behind
        for _ in range(3):
            self.produce(prod)
        self.sync_blocks([fast, prod])
        tip_anchor = self.anchor(prod)
        assert_equal(tip_anchor, (parent.getblockcount(), parent.getbestblockhash()))

        self.log.info("A parent reorganization replaces the tip's anchor")
        stale = prod.getbestblockhash()
        parent.invalidateblock(tip_anchor[1])
        self.generatetoaddress(parent, 2, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.getblockhash(tip_anchor[0]) != tip_anchor[1]
        t0 = time.time()

        self.log.info("The producer whose watcher has not ticked refuses its template on the stale parent")
        try:
            self.produce(prod)
            produced = True
        except Exception as e:
            produced = False
            assert "is no longer on the parent chain's best chain" in str(e), str(e)
            self.log.info("    refused: %s", str(e)[:200])
        assert not produced, "a block was produced on a parent whose anchor the daemon shows replaced"

        self.log.info("The refusal wakes the producer's watcher: the stale tip is disconnected at once")
        t1 = self.wait(lambda: prod.getblockheader(stale)["confirmations"] < 0, "the stale tip disconnected", 20)
        self.log.info("    disconnected %.1f s after the reorganization (the watcher polls every 60 s)", t1 - t0)
        assert t1 - t0 < 20
        blk = self.produce(prod)
        h, hh = self.anchor(prod, blk)
        assert_equal(parent.getblockhash(h), hh)
        self.sync_blocks([fast, prod])
        assert_equal(fast.getbestblockhash(), prod.getbestblockhash())

        self.log.info("With the daemon down, the producer keeps producing on the previous anchor")
        before = self.anchor(prod)
        self.stop_node(0)
        blk = self.produce(prod)
        assert_equal(self.anchor(prod, blk), before)
        self.start_node(0)
        self.connect_nodes(1, 2)

        self.log.info("With a daemon that is behind, the producer keeps producing")
        before = self.anchor(prod)
        assert behind.getblockcount() < before[0]
        self.restart_node(2, self.anchored_args(2, 3, 60))
        prod = self.nodes[2]
        prod.loadwallet("w")
        blk = self.produce(prod)
        assert_equal(self.anchor(prod, blk), before)
        log = open(os.path.join(prod.datadir, "elementsregtest", "debug.log"), encoding="utf8").read()
        assert "is no longer on the parent chain's best chain" not in log.split("Shutdown: done")[-1]


if __name__ == '__main__':
    AnchorStaleParentTest().main()
