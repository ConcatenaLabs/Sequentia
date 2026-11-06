#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Anchoring supremacy: a Bitcoin reorg must override immediate finality.

Per the theoretical paper (Principle 5 Anchoring theorem, Principle 6 "immediate
finality UNLESS a change in the status of the Bitcoin blockchain enforces a chain
reorganisation", Principle 11 "the Anchoring theorem has priority over
checkpoints"): Sequentia reorgs whenever Bitcoin does, with NO exception — even a
quorum-FINALIZED block must be discarded if its Bitcoin anchor is orphaned.

This guards a deadlock observed on the live testnet: a testnet4 reorg orphaned the
anchor of a finalized Sequentia block, and a committee minority + the explorer and
gateway nodes froze at that height. The immediate-finality accept-time gate
(validation.cpp ContextualCheckBlockHeader, "bad-fork-prior-to-pos-final") kept
rejecting the anchor-canonical recovery chain because the (now-orphaned) finalized
point still pinned the chain — finality wrongly outranking anchoring. The fix
releases the gate when the finalized block has itself been anchor-invalidated
(BLOCK_FAILED), so the recovery chain connects; UpdateTip then recomputes a fresh
finalized point from the recovered chain.

Unlike feature_pos_parent_reorg_recovery.py (which uses a sub-quorum committee so
the reorged block is NOT finalized), here a four-member public BLS committee
certifies every block (quorum 3), so each block becomes final once its
observation window has passed. The test checks that the victim really is final
on both nodes before the reorg orphans its anchor, and that both recover past
it: the producing node and a validator that only follows.

Topology: node0 = parent ("Bitcoin"); node1 = the producing node, holding the
four stakers' keys and building blocks with generateposblock; node2 = a
non-producing validator that watches the anchor and must follow the recovery
(the live freeze hit such nodes).
"""

import os
import shutil
import time

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


COMMITTEE = 4                # four stakers of equal weight: quorum 3


class PosFinalizedAnchorReorgTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.stakers = [make_key() for _ in range(COMMITTEE)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

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
        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, chain)

        consensus = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=%d" % (COMMITTEE + 1), "-posslotinterval=1",
            "-con_max_block_sig_size=8000",
            "-con_blocksubsidy=0", "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1",
            "-acceptnonstdtxn=1", "-par=1",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        args = {i: consensus + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i)] for i in (1, 2)}
        self.add_nodes(2, [args[1], args[2]], chain=[chain] * 2)
        # The stakers' BLS registrations are derived by a node that knows the
        # stakers; the chain is then started afresh with them.
        self.start_node(1, extra_args=args[1] + ["-staker=%s:1" % pub for _, pub in self.stakers])
        specs = ["-staker=%s:1%s" % (pub, self.nodes[1].getblsregistration(wif)["spec"]) for wif, pub in self.stakers]
        self.stop_node(1)
        shutil.rmtree(os.path.join(self.nodes[1].datadir, chain))
        self.start_node(1, extra_args=args[1] + specs)
        self.start_node(2, extra_args=args[2] + specs)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def produce(self):
        """One block on node1, certified by all four stakers."""
        wifs = [w for w, _ in self.stakers]
        last = None
        for _ in range(40):
            try:
                block = self.nodes[1].generateposblock(wifs[0], wifs[1:])
                self.sync_blocks(self.nodes[1:])
                return block["hash"]
            except JSONRPCException as e:
                last = e.error
                time.sleep(0.25)
        raise AssertionError("could not produce: %s" % last)

    def run_test(self):
        parent, founder, validator = self.nodes

        def anchor_of(node, height):
            return node.getblock(node.getblockhash(height))["anchorheight"]

        # Stage 1: two blocks on an early anchor, then advance the parent so the
        # later blocks anchor strictly above them.
        self.generatetoaddress(parent, 6, parent.getnewaddress(), sync_fun=self.no_op)
        self.produce()
        self.produce()
        keep_h = founder.getblockcount()                 # a FINALIZED block we will KEEP
        keep_anchor = anchor_of(founder, keep_h)
        self.log.info("finalized keep-point: height %d anchored at parent %d" % (keep_h, keep_anchor))

        self.generatetoaddress(parent, 12, parent.getnewaddress(), sync_fun=self.no_op)
        victim_h = keep_h + 1
        victim_hash_old = self.produce()
        self.produce()
        tip_before = founder.getblockcount()
        assert anchor_of(founder, victim_h) > keep_anchor
        # The victim carries the committee's quorum and is final on both nodes:
        # this is anchoring over finality, not over an unfinalized block.
        hdr = founder.getblockheader(victim_hash_old)
        assert_equal((hdr["poscertified"], hdr["posquorum"]), (True, 3))
        for node in (founder, validator):
            self.wait_until(lambda: node.getposfinality()["finalized_height"] >= victim_h, timeout=30)
            assert_equal(node.getblockhash(victim_h), victim_hash_old)
        self.log.info("FINALIZED victim block height %d (%s) anchored above the keep-point; tip %d"
                      % (victim_h, victim_hash_old[:16], tip_before))

        # Reorg the parent BELOW the victim's anchor (but at/above keep_anchor), then mine a
        # strictly longer competing branch so it becomes canonical. This orphans the anchor
        # of victim_h and every finalized block above it.
        parent_h = parent.getblockcount()
        fork_at = keep_anchor + 1
        parent.invalidateblock(parent.getblockhash(fork_at))
        self.generatetoaddress(parent, (parent_h - fork_at) + 6, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.getblockcount() > parent_h
        self.log.info("parent reorged below the victim's anchor (fork_at=%d); new parent best %d"
                      % (fork_at, parent.getblockcount()))

        # Anchoring supremacy: both nodes' watchers must DISCARD the orphaned-anchor
        # finalized blocks — finality does NOT protect them.
        self.wait_until(lambda: founder.getblockcount() == keep_h, timeout=60)
        self.wait_until(lambda: validator.getblockcount() == keep_h, timeout=60)
        self.log.info("both nodes discarded the orphaned FINALIZED blocks (rolled back to %d)" % keep_h)

        # THE REGRESSION: a recovery chain on a fresh anchor connects on both nodes.
        # Without the gate fix the stale finalized point pins the chain to the
        # orphaned finalized block and the recovery chain is rejected forever
        # ("bad-fork-prior-to-pos-final") — a permanent deadlock (the live freeze).
        while founder.getblockcount() < tip_before:
            self.produce()
        assert founder.getblockhash(victim_h) != victim_hash_old, \
            "victim height must be a NEW block on a fresh anchor, not the orphaned finalized one"
        assert_equal(founder.getblockhash(tip_before), validator.getblockhash(tip_before))
        for node in (founder, validator):
            self.wait_until(lambda: node.getposfinality()["finalized_height"] >= victim_h, timeout=30)
            assert_equal(node.getposfinality()["finalized_hash"] is not None, True)
        self.log.info("recovered past the orphaned FINALIZED block; founder+validator agree at %d"
                      % tip_before)


if __name__ == '__main__':
    PosFinalizedAnchorReorgTest().main()
