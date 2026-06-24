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
the reorged block is NOT finalized), here COMMITTEE=1 -> quorum 1, so the founder
FINALIZES every block, and the reorg orphans a finalized block's anchor. Both the
producer (founder) and a non-producing validator must recover past it.

Topology: node0 = parent ("Bitcoin"); node1 = founder PoS producer (sole genesis
staker, the whole 1-member committee); node2 = a non-producing validator that
watches the anchor and must follow the recovery (the live freeze hit such nodes).
"""

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


SEED_STAKE = 1000000000      # atoms in the founder's genesis staking output
STAKE_CSV = 15
COMMITTEE = 1                # quorum 1 -> the founder finalizes every block it produces


class PosFinalizedAnchorReorgTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.founder_wif, self.founder_pub = make_key()

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
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % COMMITTEE, "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-con_blocksubsidy=5000000000",
            "-con_genesis_stake=%s:%d:%d" % (self.founder_pub, SEED_STAKE, STAKE_CSV),
            "-con_connect_genesis_outputs=1", "-initialfreecoins=500000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        founder_args = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                    "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        # node2: a pure VALIDATOR (no -posproducer) that watches the anchor — the live
        # freeze hit exactly such follower nodes (explorer/gateway).
        validator_args = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2)]
        self.add_nodes(1, [founder_args], chain=[chain]); self.start_node(1)
        self.add_nodes(1, [validator_args], chain=[chain]); self.start_node(2)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def run_test(self):
        parent, founder, validator = self.nodes
        assert_equal(founder.getstakerinfo().get(self.founder_pub), SEED_STAKE)

        def anchor_of(node, height):
            return node.getblock(node.getblockhash(height))["anchorheight"]

        # Stage 1: give the founder an early anchor, then advance the parent deeper so
        # LATER Sequentia blocks anchor strictly above the early ones (committee=1 produces
        # continuously, so we control anchoring via the parent, not block cadence).
        self.generatetoaddress(parent, 6, parent.getnewaddress(), sync_fun=self.no_op)
        self.wait_until(lambda: founder.getblockcount() >= 2, timeout=120)
        keep_h = founder.getblockcount()                 # a FINALIZED block we will KEEP
        keep_anchor = anchor_of(founder, keep_h)
        self.log.info("finalized keep-point: height %d anchored at parent %d" % (keep_h, keep_anchor))

        # Advance the parent so subsequent finalized blocks anchor ABOVE keep_anchor.
        self.generatetoaddress(parent, 12, parent.getnewaddress(), sync_fun=self.no_op)
        self.wait_until(lambda: anchor_of(founder, founder.getblockcount()) > keep_anchor, timeout=120)
        self.wait_until(lambda: validator.getblockcount() >= founder.getblockcount() - 1, timeout=120)
        tip_before = min(founder.getblockcount(), validator.getblockcount())
        # The block just past keep_h is FINALIZED (committee=1) and anchored above keep_anchor.
        victim_h = keep_h + 1
        victim_hash_old = founder.getblockhash(victim_h)
        assert anchor_of(founder, victim_h) > keep_anchor
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
        # finalized blocks (roll back to <= keep_h) — finality does NOT protect them.
        self.wait_until(lambda: founder.getblockcount() <= keep_h, timeout=150)
        self.wait_until(lambda: validator.getblockcount() <= keep_h, timeout=150)
        self.log.info("both nodes discarded the orphaned FINALIZED blocks (rolled back to <= %d)" % keep_h)

        # THE REGRESSION: the producer rebuilds past the rolled-back height on a fresh anchor
        # and the validator follows. Without the gate fix the stale finalized point pins the
        # chain to the orphaned finalized block and the recovery chain is rejected forever
        # ("bad-fork-prior-to-pos-final") — a permanent deadlock (the live freeze).
        self.wait_until(lambda: founder.getblockcount() >= tip_before, timeout=180)
        self.wait_until(lambda: validator.getblockcount() >= tip_before, timeout=180)
        assert founder.getblockhash(victim_h) != victim_hash_old, \
            "victim height must be a NEW block on a fresh anchor, not the orphaned finalized one"
        assert_equal(founder.getblockhash(tip_before), validator.getblockhash(tip_before))
        self.log.info("recovered past the orphaned FINALIZED block; founder+validator agree at %d"
                      % tip_before)


if __name__ == '__main__':
    PosFinalizedAnchorReorgTest().main()
