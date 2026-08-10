#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Committee-equivocation prevention (Change 4a): the certified-sibling guard.

Companion to feature_pos_reorg_of_reorg_recovery.py. That test proves a node
RECONSIDERS its quorum-certified blocks when Bitcoin reorgs back (A -> B -> A).
This test proves the PRODUCER does not race that recovery: after the return to
A, a producer whose round marks were reset by the rollback would otherwise
treat the rolled-back height as vacant and mint a fresh rival IMMEDIATELY
(slot deadlines long past), while restoring the original block takes the
anchor watcher at least one poll tick plus a live parent check. Losing that
race puts two blocks at one height in front of the committee, both eventually
anchored to the returned parent chain — the committee equivocation that froze
the live 96/4 finality split, a tie anchoring cannot break.

The guard (AnchorCertifiedSiblingPending + the Step() hold in pos_producer):
while the watcher holds a quorum-certified child of the current tip that is
not confirmed off the parent chain's best chain, the producer neither proposes
nor drives rounds at that height, bounded by -posanchorrecoverywait.

Topology: node0 = parent ("Bitcoin"); node1 = founder holding THREE unit-weight
stakers (-staker registry) at committee size 3, quorum 2 — so generateposblock
mints quorum-CERTIFIED blocks (m_pos_countersigs >= 2), the predicate the guard
keys on (a committee=1 leader-only chain names no members and is never
immediate-final, so it cannot exercise the guard).

Deterministic reproduction of the race window: the founder EXPERIENCES the
A->B invalidation with production disabled (so its disk state is exactly
"rolled back, certified block BLOCK_FAILED_ANCHOR at keep_h+1"), Bitcoin
returns to A while it is down, and it boots WITH the autonomous producer:
the producer thread starts before the anchor watcher thread and its slot is
due at once — and the escaping-stall valve is wide open (the parent advanced
far beyond the keep-point anchor), so without the guard even a single eligible
staker mints an accepted rival at the rolled-back height. With the guard the
producer holds until the watcher restores the ORIGINAL blocks verbatim.

The last stage proves liveness: when Bitcoin genuinely LEAVES (a plain forward
reorg), the invalidation walk caches the stale verdict before the producer
ever sees the rollback, so the guard never engages and fresh replacement
blocks are produced immediately.
"""

import contextlib
import os

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


COMMITTEE = 3                # 3 unit-weight stakers, quorum 2


class PosCertifiedSiblingGuardTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.stakers = [make_staker() for _ in range(3)]

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

        self.consensus = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % COMMITTEE, "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-con_blocksubsidy=5000000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-poscheckpointscan=4000",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        # Generous patience decouples the guard from checkpoint-scan latency on
        # a loaded CI machine (the hold must outlast the deliberately slow
        # first watcher tick of stage 4).
        self.producer_args = self.consensus + ["-posproducer=1", "-posanchorrecoverywait=120"] + \
            ["-posproducerkey=%s" % wif for wif, _ in self.stakers]
        self.add_nodes(1, [self.consensus], chain=[chain])
        self.start_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def mint_certified(self, node, min_countersigs=2):
        """Mint one quorum-certified block via generateposblock, trying each
        staker as leader (member eligibility is seed-fixed per height; the
        leader draw is the only knob). Asserts the result is certified."""
        last_err = None
        for wif, _ in self.stakers:
            members = [w for w, _ in self.stakers if w != wif]
            try:
                res = node.generateposblock(wif, members)
            except JSONRPCException as e:
                last_err = e
                continue
            assert res["countersignatures"] >= min_countersigs, \
                "minted a sub-quorum block (%d countersigs) where a certified one is required" \
                % res["countersignatures"]
            return res
        raise AssertionError("no staker could mint a certified block at this height: %s" % last_err)

    def run_test(self):
        parent, founder = self.nodes
        assert_equal(founder.getstakerinfo().get(self.stakers[0][1]), 1)

        def anchor_of(node, height):
            return node.getblock(node.getblockhash(height))["anchorheight"]

        # Stage 1: build the A-anchored chain by RPC (production disabled): a
        # low-anchored keep-point, then a CERTIFIED block anchored strictly
        # above it (the one the reorg will orphan, mark BLOCK_FAILED_ANCHOR,
        # and the guard must protect), then a couple more on top. The parent
        # advances only 2 blocks before that mint so the escaping-stall valve
        # (anchor gap >= 3) stays CLOSED: the mint is either certified or
        # fails and is retried, never sub-quorum.
        self.generatetoaddress(parent, 6, parent.getnewaddress(), sync_fun=self.no_op)
        self.mint_certified(founder)
        self.mint_certified(founder)
        keep_h = founder.getblockcount()
        keep_anchor = anchor_of(founder, keep_h)
        self.log.info("keep-point: height %d anchored at parent %d" % (keep_h, keep_anchor))

        self.generatetoaddress(parent, 2, parent.getnewaddress(), sync_fun=self.no_op)
        res = self.mint_certified(founder)
        a_child_hash = res["hash"]
        assert_equal(founder.getblockcount(), keep_h + 1)
        assert anchor_of(founder, keep_h + 1) > keep_anchor
        for _ in range(3):
            self.generatetoaddress(parent, 1, parent.getnewaddress(), sync_fun=self.no_op)
            self.mint_certified(founder, min_countersigs=1)
        a_tip_h = founder.getblockcount()
        a_tip_hash = founder.getblockhash(a_tip_h)
        self.log.info("A-chain tip: height %d (%s); certified orphanable block %s at %d"
                      % (a_tip_h, a_tip_hash[:16], a_child_hash[:16], keep_h + 1))

        # Stage 2: reorg the parent A -> B. The founder's watcher discards the
        # orphaned-anchor blocks; its disk now holds exactly the pre-race
        # state: rolled back to keep_h, the certified block at keep_h+1
        # BLOCK_FAILED_ANCHOR, and (no producer) no B-anchored blocks at all.
        parent_h = parent.getblockcount()
        fork_at = keep_anchor + 1
        a_fork_hash = parent.getblockhash(fork_at)
        parent.invalidateblock(a_fork_hash)
        self.generatetoaddress(parent, (parent_h - fork_at) + 4, parent.getnewaddress(), sync_fun=self.no_op)
        b_fork_hash = parent.getblockhash(fork_at)
        assert b_fork_hash != a_fork_hash
        self.wait_until(lambda: founder.getblockcount() <= keep_h, timeout=150)
        self.log.info("parent reorged A->B; founder (validator mode) rolled back to <= %d" % keep_h)

        # Stage 3: stop the founder; return the parent B -> A while it is down,
        # and EXTEND A by 3000 blocks. The extension (a) makes A decisively
        # best, and (b) makes the founder's first watcher tick slow
        # DETERMINISTICALLY: on a fresh boot the watcher scans the parent chain
        # backwards for checkpoints (-poscheckpointscan=4000 above) BEFORE the
        # recovery pass runs — thousands of RPC round-trips, seconds of work —
        # while the producer's first mint takes well under a second. Without
        # the guard the producer therefore reliably wins the boot race (as it
        # did on the live network); with the guard it must demonstrably hold
        # for that whole window.
        self.stop_node(1)
        parent.invalidateblock(b_fork_hash)
        parent.reconsiderblock(a_fork_hash)
        self.wait_until(lambda: parent.getblockhash(fork_at) == a_fork_hash, timeout=60)
        self.generatetoaddress(parent, 3000, parent.getnewaddress(), sync_fun=self.no_op)
        self.log.info("parent reorged B->A (and extended by 3000) while the founder was down")

        # Stage 4 (THE RACE): boot the founder WITH the autonomous producer.
        # The producer thread starts before the anchor watcher thread, its slot
        # deadlines are long past, and the escaping-stall valve is wide open
        # (the parent tip is far above the keep-point anchor), so without the
        # guard it mints an accepted rival at keep_h+1 before the watcher's
        # first tick can restore the originals. With the guard (the certified
        # sibling is in the seeded recovery set, negative cache empty at boot)
        # it holds, and the watcher restores the ORIGINAL blocks verbatim: the
        # exact original hashes and NO rival branch.
        # The debug-log assertion proves the guard ENGAGED for the scan window
        # (not merely that the watcher happened to win the race). The ad-hoc
        # negative control (running with -posanchorrecoverywait=0 to reproduce
        # the pre-guard behavior) sets GUARD_TEST_EXPECT_NO_HOLD=1 and fails on
        # the substantive assertions below instead.
        expect_hold = os.environ.get("GUARD_TEST_EXPECT_NO_HOLD") != "1"
        log_ctx = self.nodes[1].assert_debug_log(
            ["PoS producer: holding production at height %d" % (keep_h + 1)], timeout=60) \
            if expect_hold else contextlib.nullcontext()
        with log_ctx:
            self.start_node(1, extra_args=self.producer_args)
            founder = self.nodes[1]
            self.wait_until(lambda: founder.getblockcount() >= a_tip_h, timeout=180)
        assert_equal(founder.getblockhash(keep_h + 1), a_child_hash)
        assert_equal(founder.getblockhash(a_tip_h), a_tip_hash)
        branches_above_keep = [t for t in founder.getchaintips() if t["height"] > keep_h]
        assert_equal(len(branches_above_keep), 1)
        self.log.info("guard held: original A-blocks restored verbatim (tip %d = %s), no rival branch"
                      % (a_tip_h, a_tip_hash[:16]))

        # The hold must RELEASE after recovery: production resumes on the
        # restored chain. Advance the parent so the next heights have a fresh
        # anchor (and the stall valve if a seed draw leaves the local keys
        # short of a quorum).
        self.generatetoaddress(parent, 4, parent.getnewaddress(), sync_fun=self.no_op)
        self.wait_until(lambda: founder.getblockcount() > a_tip_h, timeout=120)
        assert_equal(founder.getblockhash(a_tip_h), a_tip_hash)
        self.log.info("production resumed on the restored chain (height %d)" % founder.getblockcount())

        # Stage 5 (LIVENESS on a genuine departure): plain forward reorg
        # A -> B2. The invalidation walk caches the stale verdict before the
        # producer sees the rollback, so the guard never engages and fresh
        # replacement blocks appear. Find the highest block anchored strictly
        # below the new fork so we know the exact rollback base.
        tip2_h = founder.getblockcount()
        fork2_at = anchor_of(founder, tip2_h)
        base2_h = tip2_h
        while base2_h > 0 and anchor_of(founder, base2_h) >= fork2_at:
            base2_h -= 1
        old_child2 = founder.getblockhash(base2_h + 1)
        parent_h2 = parent.getblockcount()
        a2_fork_hash = parent.getblockhash(fork2_at)
        parent.invalidateblock(a2_fork_hash)
        self.generatetoaddress(parent, (parent_h2 - fork2_at) + 4, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.getblockhash(fork2_at) != a2_fork_hash
        self.log.info("parent reorged forward A->B2 (fork_at=%d); founder must roll back to %d and replace"
                      % (fork2_at, base2_h))
        self.wait_until(lambda: founder.getblockcount() > base2_h
                        and founder.getblockhash(base2_h + 1) != old_child2, timeout=180)
        new_child2 = founder.getblock(founder.getblockhash(base2_h + 1))
        assert_equal(parent.getblockhash(new_child2["anchorheight"]), new_child2["anchorhash"])
        self.log.info("fresh B2-anchored block produced at height %d: the guard released on the "
                      "stale verdict (no deadlock when Bitcoin genuinely leaves)" % (base2_h + 1))


if __name__ == '__main__':
    PosCertifiedSiblingGuardTest().main()
