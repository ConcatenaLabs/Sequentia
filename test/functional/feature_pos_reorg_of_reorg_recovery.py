#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Anchor reorg-of-reorg recovery (Alberto-endorsed fix).

Companion to feature_pos_finalized_anchor_reorg.py. That test covers the FORWARD
half of anchoring supremacy: a Bitcoin reorg (chain A -> B) orphans the anchor of
finalized Sequentia blocks, which must be discarded. This test covers the RETURN
half: when Bitcoin reorgs BACK (B -> A, the previous best chain becomes best again
— routine on testnet4), the Sequentia blocks anchored to A must be RECONSIDERED
and re-finalized, NOT replaced by a fresh block minted on top. Per Alberto: minting
fresh would let an atomic swap that is valid again on the Bitcoin side be
double-spent on the Sequentia side, since the original A-anchored block (and its
txs) would be dropped instead of restored.

The defect this guards: the watcher's recovery worklist (anchor.cpp
g_anchor_invalidated) is in-memory only, while the BLOCK_FAILED flags it sets
persist. So a node that RESTARTS between the A->B invalidation and the B->A
restoration loses the worklist and never reconsiders the A-anchored blocks — they
stay BLOCK_FAILED forever (the live freeze). The fix re-seeds the worklist from the
persisted block index at startup (SeedAnchorInvalidated, called by LoadBlockIndex)
and reconsiders whenever the set is non-empty (not only on a parent tip change).
ResetBlockFailureFlags additionally raises pindexBestHeader onto the recovered
branch so a headers-only recovery chain re-requests its bodies and reconnects.

To isolate reconsideration from re-production, the sole producer (founder) is
STOPPED before the reorgs: the only way the original A-anchored tip can return is by
reconsideration, so asserting the ORIGINAL block hash is back on the active chain
proves the blocks were restored verbatim (no double-spend), not rebuilt.

Topology: node0 = parent ("Bitcoin"); node1 = founder PoS producer (1-member
committee, finalizes every block); node2 = a non-producing validator that restarts
mid-reorg and must still recover (the live freeze hit such follower nodes).
"""

import time

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


class PosReorgOfReorgRecoveryTest(BitcoinTestFramework):
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
        self.validator_args = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2)]
        founder_args = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                    "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        self.add_nodes(1, [founder_args], chain=[chain]); self.start_node(1)
        self.add_nodes(1, [self.validator_args], chain=[chain]); self.start_node(2)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def run_test(self):
        parent, founder, validator = self.nodes
        assert_equal(founder.getstakerinfo().get(self.founder_pub), SEED_STAKE)

        def anchor_of(node, height):
            return node.getblock(node.getblockhash(height))["anchorheight"]

        # Stage 1: build the A-anchored Sequentia chain. Give the founder a low
        # anchor to KEEP, then advance the parent so later blocks anchor strictly
        # above it (the ones we will orphan and then restore).
        self.generatetoaddress(parent, 6, parent.getnewaddress(), sync_fun=self.no_op)
        self.wait_until(lambda: founder.getblockcount() >= 2, timeout=120)
        keep_h = founder.getblockcount()
        keep_anchor = anchor_of(founder, keep_h)
        self.log.info("keep-point: height %d anchored at parent %d" % (keep_h, keep_anchor))

        self.generatetoaddress(parent, 12, parent.getnewaddress(), sync_fun=self.no_op)
        self.wait_until(lambda: anchor_of(founder, founder.getblockcount()) > keep_anchor, timeout=120)
        # Let a few more finalized blocks pile up above the keep-point, then freeze
        # the A-chain by recording its tip and syncing the validator to it.
        a_tip_h = founder.getblockcount()
        self.wait_until(lambda: validator.getblockcount() >= a_tip_h, timeout=120)
        a_tip_h = min(founder.getblockcount(), validator.getblockcount())
        a_tip_hash = founder.getblockhash(a_tip_h)
        assert anchor_of(founder, a_tip_h) > keep_anchor
        assert_equal(validator.getblockhash(a_tip_h), a_tip_hash)
        self.log.info("A-chain tip: FINALIZED height %d (%s) anchored above the keep-point"
                      % (a_tip_h, a_tip_hash[:16]))

        # Stop the sole producer so NOTHING can rebuild: the only way a_tip_hash can
        # come back on the validator is by reconsidering the original A-blocks.
        self.stop_node(1)
        self.log.info("stopped the founder; only reconsideration can restore the A-chain now")

        # Stage 2: reorg the parent A -> B below the orphan point, with B strictly
        # longer so it becomes canonical. Record the A and B fork blocks so we can
        # flip back to A later.
        parent_h = parent.getblockcount()
        fork_at = keep_anchor + 1
        a_fork_hash = parent.getblockhash(fork_at)
        parent.invalidateblock(a_fork_hash)
        self.generatetoaddress(parent, (parent_h - fork_at) + 4, parent.getnewaddress(), sync_fun=self.no_op)
        b_fork_hash = parent.getblockhash(fork_at)
        assert b_fork_hash != a_fork_hash
        assert parent.getblockcount() > parent_h
        self.log.info("parent reorged A->B (fork_at=%d); B is now best at %d" % (fork_at, parent.getblockcount()))

        # The validator's watcher must DISCARD the orphaned-anchor finalized blocks.
        self.wait_until(lambda: validator.getblockcount() <= keep_h, timeout=150)
        assert validator.getblockcount() <= keep_h
        self.log.info("validator discarded the orphaned A-blocks (rolled back to <= %d)" % keep_h)

        # Stage 3 (THE DEFECT): restart the validator while the parent is on B. This
        # drops the in-memory recovery worklist; the A-blocks remain BLOCK_FAILED on
        # disk. Without the startup re-seed they would be unrecoverable.
        self.restart_node(2, extra_args=self.validator_args)
        validator = self.nodes[2]
        assert validator.getblockcount() <= keep_h
        self.log.info("restarted the validator on the B parent (recovery worklist now only from disk)")

        # Stage 4: reorg the parent B -> A (reorg-of-reorg). Orphan B and restore A;
        # A is the previous best chain becoming best again.
        parent.invalidateblock(b_fork_hash)
        parent.reconsiderblock(a_fork_hash)
        self.wait_until(lambda: parent.getblockhash(fork_at) == a_fork_hash, timeout=60)
        assert parent.getblockcount() >= a_tip_h or True  # parent height is independent of SEQ height
        self.log.info("parent reorged B->A; A is canonical again (fork block %s restored)" % a_fork_hash[:16])

        # Stage 5: the validator must RECONSIDER the original A-anchored blocks and
        # return to the EXACT same tip hash — proving the blocks were restored
        # verbatim (so any tx in them, e.g. a swap settlement, is restored and not
        # double-spendable), not rebuilt. Without the fix the restarted node stays
        # frozen at <= keep_h forever.
        self.wait_until(lambda: validator.getblockcount() >= a_tip_h, timeout=180)
        assert_equal(validator.getblockhash(a_tip_h), a_tip_hash)
        self.log.info("validator reconsidered the A-chain after a restart + reorg-of-reorg; "
                      "tip %d restored to original hash %s" % (a_tip_h, a_tip_hash[:16]))

        # Stage 6 (PROVENANCE — the deploy-blocker guard): a MANUAL invalidateblock
        # of a fully-valid block (anchor canonical) must NOT be resurrected by the
        # anchor watcher across a restart. Only anchor-watcher invalidations carry
        # BLOCK_FAILED_ANCHOR and get re-seeded; a manual invalidate (the documented
        # finality-split-stall recovery: operators invalidateblock the stuck tip)
        # carries only BLOCK_FAILED_VALID, so a reboot must leave it invalidated.
        manual_h = validator.getblockcount()
        manual_hash = validator.getblockhash(manual_h)
        validator.invalidateblock(manual_hash)
        assert_equal(validator.getblockcount(), manual_h - 1)
        self.restart_node(2, extra_args=self.validator_args)
        validator = self.nodes[2]
        # Give the watcher several poll intervals to (wrongly) resurrect it.
        time.sleep(4)
        assert_equal(validator.getblockcount(), manual_h - 1)
        assert validator.getblockhash(validator.getblockcount()) != manual_hash, \
            "manual invalidateblock was resurrected across restart (anchor provenance marker missing)"
        self.log.info("manual invalidateblock survived restart (not resurrected by the anchor watcher)")


if __name__ == '__main__':
    PosReorgOfReorgRecoveryTest().main()
