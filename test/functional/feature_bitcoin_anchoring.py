#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests Bitcoin anchoring (con_bitcoin_anchor).

Topology:
  node0: the "parent chain" (stands in for Bitcoin; an elementsregtest chain,
         whose RPC surface is identical for the purposes of anchoring)
  node1: the anchored (Sequentia-style) chain, connected to node0 via the
         -mainchainrpc* settings

Covers:
 - every block carries an anchor to the parent chain
 - anchor heights are monotonically non-decreasing and follow the parent tip
 - getanchorstatus reports the anchor as canonical
 - a parent chain reorganization reorganizes the anchored chain (the core
   property: Sequentia reorgs iff Bitcoin reorgs)
 - blocks re-anchor to the new parent branch afterwards

See doc/sequentia/03-bitcoin-anchoring.md.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    get_auth_cookie,
    get_datadir_path,
    rpc_port,
    p2p_port,
)

ANCHOR_POLL_SECS = 1


class BitcoinAnchoringTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []

        # Parent chain node (stands in for bitcoind)
        parent_chain = "elementsregtest"
        parent_args = [
            "-port=" + str(p2p_port(0)),
            "-rpcport=" + str(rpc_port(0)),
            "-validatepegin=0",
            "-initialfreecoins=0",
            # A non-zero subsidy makes coinbases address-dependent, so a
            # competing branch (mined to a different address) produces
            # different block hashes than the invalidated branch.
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",  # OP_TRUE
        ]
        self.add_nodes(1, [parent_args], chain=[parent_chain])
        self.start_node(0)
        self.parentgenesisblockhash = self.nodes[0].getblockhash(0)

        # Anchored chain node
        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, parent_chain)
        anchored_args = [
            "-port=" + str(p2p_port(1)),
            "-rpcport=" + str(rpc_port(1)),
            "-validatepegin=0",
            "-initialfreecoins=10000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-con_bitcoin_anchor=1",
            "-validateanchor=1",
            "-anchorpollinterval=%d" % ANCHOR_POLL_SECS,
            "-mainchainrpchost=127.0.0.1",
            "-mainchainrpcport=%s" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u,
            "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesisblockhash,
        ]
        self.add_nodes(1, [anchored_args], chain=["elementsregtest"])
        self.start_node(1)

        # Wallets are not auto-created; both nodes need one for getnewaddress
        for node in self.nodes:
            node.createwallet(wallet_name="w", descriptors=True)

    def wait_for_anchor_tip_change(self, node, old_tip, timeout=30):
        """Wait for the anchor watcher to reorganize the chain away from old_tip."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if node.getbestblockhash() != old_tip:
                return
            time.sleep(0.25)
        raise AssertionError("anchored chain did not reorganize within %ds" % timeout)

    def run_test(self):
        parent = self.nodes[0]
        node = self.nodes[1]

        parent_addr = parent.getnewaddress()
        node_addr = node.getnewaddress()

        # Grow the parent chain a little
        self.generatetoaddress(parent, 5, parent_addr, sync_fun=self.no_op)
        parent_tip_hash = parent.getbestblockhash()
        parent_tip_height = parent.getblockcount()
        assert_equal(parent_tip_height, 5)

        # --- Anchors are set and follow the parent tip ---
        self.generatetoaddress(node, 3, node_addr, sync_fun=self.no_op)
        for h in range(1, 4):
            header = node.getblockheader(node.getblockhash(h))
            assert_equal(header['anchorheight'], parent_tip_height)
            assert_equal(header['anchorhash'], parent_tip_hash)

        status = node.getanchorstatus()
        assert_equal(status['validateanchor'], True)
        assert_equal(status['tipheight'], 3)
        assert_equal(status['anchorheight'], parent_tip_height)
        assert_equal(status['anchorhash'], parent_tip_hash)
        assert_equal(status['anchorstatus'], 'ok')

        # New parent blocks advance the anchor; heights are monotone
        self.generatetoaddress(parent, 2, parent_addr, sync_fun=self.no_op)
        new_parent_tip = parent.getbestblockhash()
        self.generatetoaddress(node, 1, node_addr, sync_fun=self.no_op)
        header = node.getblockheader(node.getbestblockhash())
        assert_equal(header['anchorheight'], 7)
        assert_equal(header['anchorhash'], new_parent_tip)
        prev = node.getblockheader(node.getblockhash(3))
        assert header['anchorheight'] >= prev['anchorheight']

        # --- The core property: a parent chain reorg reorganizes this chain ---
        # node tip (height 4) anchors to parent block 7. Reorganize the parent
        # back below height 7 and onto a different branch.
        invalidated = parent.getblockhash(6)
        anchored_tip = node.getbestblockhash()
        confirmed_tip = node.getblockhash(3)  # anchored to parent block 5: survives

        parent.invalidateblock(invalidated)
        assert_equal(parent.getblockcount(), 5)
        # Mine a competing parent branch so heights 6..8 exist on a new branch.
        # Use a fresh address so the branch's blocks differ from the
        # invalidated ones.
        self.generatetoaddress(parent, 3, parent.getnewaddress(), sync_fun=self.no_op)

        # The anchor watcher must notice and reorganize the anchored chain:
        # block 4 (anchored to the now-stale parent block 7) is disconnected.
        self.wait_for_anchor_tip_change(node, anchored_tip)
        assert_equal(node.getbestblockhash(), confirmed_tip)
        assert_equal(node.getblockcount(), 3)

        # --- After the reorg, production resumes anchored to the new branch ---
        self.generatetoaddress(node, 2, node_addr, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), 5)
        header = node.getblockheader(node.getbestblockhash())
        assert_equal(header['anchorheight'], 8)
        assert_equal(header['anchorhash'], parent.getbestblockhash())
        assert_equal(node.getanchorstatus()['anchorstatus'], 'ok')

        # Anchors of surviving blocks are still canonical
        for h in range(1, 4):
            header = node.getblockheader(node.getblockhash(h))
            assert_equal(header['anchorhash'], parent.getblockhash(header['anchorheight']))

        # --- Anchors persist across restart (disk block-index round-trip) ---
        tip = node.getbestblockhash()
        tip_header = node.getblockheader(tip)
        self.restart_node(1)
        assert_equal(node.getbestblockhash(), tip)
        restarted = node.getblockheader(tip)
        assert_equal(restarted['anchorheight'], tip_header['anchorheight'])
        assert_equal(restarted['anchorhash'], tip_header['anchorhash'])
        assert_equal(node.getanchorstatus()['anchorstatus'], 'ok')


if __name__ == '__main__':
    BitcoinAnchoringTest().main()
