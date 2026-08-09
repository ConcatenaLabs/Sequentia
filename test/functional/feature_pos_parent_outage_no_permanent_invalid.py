#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A parent-chain (Bitcoin) OUTAGE must never PERMANENTLY invalidate a block.

Incident 2026-07-25 / 2026-07-26 (live testnet). With bitcoind unreachable, PoS
validation rejected blocks with `pos-escape-stall-unverifiable` — an intentionally
SOFT, retriable result (BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE:
"this node cannot verify it right now", NOT "this block is invalid"). But the
permanent-failure paths only excluded BLOCK_MUTATED, so the block was marked
BLOCK_FAILED_VALID and persisted to the block index. Nothing ever retried it (the
anchor watcher only reconsiders blocks IT invalidated via MarkAnchorInvalid), so
the node sat ~2300 blocks behind for hours — while reporting blocks == headers and
verificationprogress 1, i.e. believing it was fully synced — and kept proposing on
a dead branch. Recovery required a human running `reconsiderblock`.

HOW THE TRIGGER IS REPRODUCED DETERMINISTICALLY HERE

`pos-escape-stall-unverifiable` needs three things at once, which is why it is
subtle: (1) the block must be sub-quorum AND carry an anchor whose HEIGHT is a
gap above its parent's, so the escaping-stall relaxation is exercised; (2) the
block's header must already have been accepted, because the anchor rule R3 in
ContextualCheckBlockHeader would otherwise reject the header first when the
parent is unreachable; (3) the parent-chain median-time-past lookup must fail.

The node keeps TWO INDEPENDENT caches: `g_anchor_ok_cache` (anchor is on the
parent's best chain, populated by R3 at header time) and `g_anchor_mtp_cache`
(median-time-past per hash, populated only by GetMainchainMedianTime at connect
time). Neither is cleared while the parent is unreachable, because clearing is
gated on observing a parent tip change. So the live failure window is: header
accepted while the parent is reachable (anchor cached OK) -> parent becomes
unreachable -> body connected -> R3 is not re-run, but the MTP lookup misses the
cache, hits the dead RPC, and returns UNKNOWN. That is exactly what this test
builds, via submitheader (parent up) then submitblock (parent down).

Two further environment details this test has to handle, both learned the hard
way while bringing it up:

  * The escaping-stall gap is 600 SECONDS of parent median-time-past
    (DEFAULT_POS_ESCAPE_STALL_MTP_GAP). Plain `generate` in regtest stamps every
    block with the same wall-clock second, so the parent MTP never advances and a
    sub-quorum block can never be certified at all. The parent clock is therefore
    driven with setmocktime, which is also what makes the run deterministic
    instead of dependent on how fast the machine is.
  * A leftover elementsd from an aborted run holds the fixed test ports; the
    gossip peer then never binds and the symptom looks like a consensus failure.

WHAT IS ASSERTED
  1  baseline: the chain climbs while the parent is reachable
  2  the trigger really fired (a parent-dependent reject reason is present) — so
     a green run cannot be a false negative
  3  NOTHING was permanently invalidated: no InvalidChainFound, no `invalid`
     branch in getchaintips, and the block index entry is not BLOCK_FAILED
  4  the deferral does not busy-loop. Merely skipping the permanent mark is NOT
     sufficient and is in fact worse: FindMostWorkChain only drops candidates
     carrying BLOCK_FAILED_MASK, so an unmarked candidate is handed straight back
     and ActivateBestChain spins in a hot loop, re-running validation and
     hammering the dead parent daemon under cs_main. The fix routes the verdict
     into the fStall mechanism Elements already uses for CheckPeginRipeness.
  5  once the parent returns the node converges BY ITSELF — no reconsiderblock
  6  control: permanent invalidation still works, so the fix softened only the
     parent-dependent verdicts rather than disabling invalidation

Topology: node0 = parent ("Bitcoin"); node1 = founder PoS node (sole genesis
staker, -posproducer); node2 = gossip peer, permanently connected to node1
because the producer only proposes while it has peers; node3 = the victim, which
gets isolated so it can be fed the blocks by hand at the chosen moment.
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
STAKE_CSV = 15               # height-based CSV (>= posunbonding 10 * slot 1)
COMMITTEE = 3                # quorum 2 -> the lone founder is sub-quorum

MTP_GAP = 600                # DEFAULT_POS_ESCAPE_STALL_MTP_GAP (seconds)
PARENT_STEP_SECONDS = 900    # parent MTP advance per Sequentia block (> MTP_GAP)
PARENT_BLOCKS_PER_STEP = 12  # >= 11 so the median fully moves to the new time

OUTAGE_SECONDS = 20

# A correct implementation defers once per activation trigger: the anchor watcher
# tick (-anchorpollinterval=1) plus producer slots. The busy-loop regression was
# an unbounded hot loop (thousands per second), so a generous ceiling separates
# the two behaviours decisively.
MAX_STALL_EVENTS = 40 * OUTAGE_SECONDS

# Reject reasons that depend on this node's transient view of the parent chain.
# All are BLOCK_RECENT_CONSENSUS_CHANGE and must never be cached as permanent.
SOFT_REASONS = (
    "pos-escape-stall-unverifiable",
    "anchor-unverifiable",
    "anchor-unknown",
    "anchor-stale",
    "bad-fork-prior-to-pos-final",
)


class PosParentOutageNoPermanentInvalidTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.founder_wif, self.founder_pub = make_key()
        self.peer_wif, _ = make_key()     # not a registered staker; connectivity only
        self.victim_wif, _ = make_key()   # not a registered staker either
        self.mocktime = int(time.time())

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-mocktime=%d" % self.mocktime,
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
            "-posescapestallmtpgap=%d" % MTP_GAP,
            # This RPC is issued from ConnectBlock with cs_main held; the stock
            # 900s default would freeze the node on a hung (not dead) parent.
            "-mainchainrpctimeout=5",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        # Kept so phase 3 can rebuild the victim's args with FRESH parent-RPC
        # credentials: stopping the parent makes it regenerate its .cookie on the
        # next start, which would otherwise leave the victim authenticating with
        # stale credentials forever (HTTP_UNAUTHORIZED -> indistinguishable from
        # an outage that never ends).
        self.chain_name = chain
        self.consensus_args = consensus

        founder_args = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                    "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        peer_args = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
                                 "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        victim_args = consensus + ["-port=%d" % p2p_port(3), "-rpcport=%d" % rpc_port(3),
                                   "-posproducer=0", "-posproducerkey=%s" % self.victim_wif]
        self.add_nodes(1, [founder_args], chain=[chain]); self.start_node(1)
        self.add_nodes(1, [peer_args], chain=[chain]); self.start_node(2)
        self.add_nodes(1, [victim_args], chain=[chain]); self.start_node(3)
        # node2 stays connected to node1 for the whole run: the producer only
        # proposes while it has peers, so isolating the victim must not isolate
        # the founder. The victim is only ever linked to node1, never to node2.
        self.connect_nodes(1, 2)
        self.connect_nodes(1, 3)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    # --- helpers -----------------------------------------------------------

    def advance_parent(self, seconds=PARENT_STEP_SECONDS, nblocks=PARENT_BLOCKS_PER_STEP):
        """Advance the parent chain so its MEDIAN-TIME-PAST really moves forward.

        Timestamps must be strictly increasing (a block at the current MTP is
        rejected as time-too-old), so the mock clock is stepped per block rather
        than jumped once.
        """
        parent = self.nodes[0]
        step = max(1, seconds // nblocks)
        addr = parent.getnewaddress()
        for _ in range(nblocks):
            self.mocktime += step
            parent.setmocktime(self.mocktime)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def read_log(self, node_idx, from_byte=0):
        with open(self.nodes[node_idx].debug_log_path, 'rb') as f:
            f.seek(from_byte)
            return f.read().decode('utf-8', errors='replace')

    def invalid_tips(self, node):
        return [t for t in node.getchaintips() if t.get("status") == "invalid"]

    # --- test --------------------------------------------------------------

    def run_test(self):
        parent, founder, gossip_peer, victim = self.nodes
        VICTIM = 3   # node index, for log reads

        assert_equal(founder.getstakerinfo().get(self.founder_pub), SEED_STAKE)
        assert_equal(founder.getblockcount(), 0)

        # ---- phase 1: baseline, parent reachable --------------------------
        for target in (1, 2):
            self.advance_parent()
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=120)
        assert_equal(founder.getblockcount(), 2)
        self.wait_until(lambda: victim.getblockcount() >= 2, timeout=60)
        assert_equal(self.invalid_tips(founder), [])
        assert_equal(self.invalid_tips(victim), [])
        self.log.info("phase 1 ok: climbed to height 2 with the parent reachable")

        # ---- prepare the failure window -----------------------------------
        # Isolate ONLY the victim (node1<->node2 stays up so the founder keeps
        # proposing). The victim then stays behind, so at least one of the blocks
        # below does not build on its tip: that one skips the accept-time PoS
        # check and is judged at CONNECT time, the path the live incident took.
        self.disconnect_nodes(1, VICTIM)
        base = victim.getblockcount()

        made = []
        for i in (1, 2):
            self.advance_parent()
            self.wait_until(lambda: founder.getblockcount() >= base + i, timeout=120)
            h = founder.getblockhash(base + i)
            made.append((founder.getblockheader(h, False), founder.getblock(h, 0), h))
        self.log.info("founder produced heights %d..%d while the victim was disconnected"
                      % (base + 1, base + 2))

        # Headers first, WHILE THE PARENT IS STILL UP: this is what puts each
        # anchor into g_anchor_ok_cache (R3), without touching the separate
        # g_anchor_mtp_cache. That asymmetry is the live failure window.
        for hdr, _blk, h in made:
            victim.submitheader(hdr)
            assert victim.getblockheader(h) is not None
        self.log.info("victim accepted both headers (anchors now OK-cached, MTP not cached)")

        # ---- phase 2: parent outage, then feed the bodies -----------------
        log_mark = self.nodes[VICTIM].debug_log_bytes()
        self.log.info("stopping the parent daemon")
        self.stop_node(0)

        # Highest first: its parent is missing, so it skips the accept-time PoS
        # check and is stored, to be judged later at connect time.
        for hdr, blk, h in reversed(made):
            try:
                victim.submitblock(blk)
            except Exception as e:
                self.log.info("submitblock(%s) raised (expected while blind): %s" % (h[:16], e))

        deadline = time.time() + OUTAGE_SECONDS
        while time.time() < deadline:
            time.sleep(1)
            # Must stay responsive: a parent RPC blocking under cs_main would
            # hang this (hence -mainchainrpctimeout).
            victim.getblockchaininfo()

        outage_log = self.read_log(VICTIM, log_mark)

        # 2: the trigger really fired — a green run cannot be a false negative.
        seen_soft = [r for r in SOFT_REASONS if r in outage_log]
        stall_events = outage_log.count("STALLING further progress in ConnectTip")
        self.log.info("outage: soft reasons=%s ; ConnectTip stalls=%d" % (seen_soft, stall_events))
        assert seen_soft, (
            "the parent-outage trigger did NOT fire, so this run proves nothing. "
            "Expected one of %s in the victim's log." % (SOFT_REASONS,))

        # 3: nothing was permanently invalidated.
        assert "InvalidChainFound" not in outage_log, (
            "a parent-chain outage permanently invalidated a block:\n"
            + "\n".join(l for l in outage_log.splitlines() if "InvalidChainFound" in l))
        assert_equal(self.invalid_tips(victim), [])
        for _hdr, _blk, h in made:
            st = victim.getchaintips()
            for t in st:
                if t["hash"] == h:
                    assert t["status"] != "invalid", "block %s was marked invalid" % h[:16]

        # 4: no busy loop.
        assert stall_events <= MAX_STALL_EVENTS, (
            "ConnectTip stalled %d times in %ds — that is a busy loop, not a retry"
            % (stall_events, OUTAGE_SECONDS))
        self.log.info("phase 2 ok: trigger fired, nothing permanently invalidated, no busy loop")

        # ---- phase 3: parent returns, victim must converge BY ITSELF ------
        # This is the assertion the live incident actually violated. The block
        # must be merely DEFERRED, so that once the parent is reachable again the
        # ordinary paths accept it. On the unpatched build the BLOCK_FAILED_VALID
        # mark is persisted in the block index, so the block stays poisoned even
        # across the restart below and this phase can never succeed.
        self.log.info("restarting the parent; the victim must catch up with no reconsiderblock")
        self.start_node(0)
        try:
            parent.loadwallet("w")
        except Exception:
            pass
        parent.setmocktime(self.mocktime)

        # The parent regenerated its RPC cookie, so hand the victim the new
        # credentials (test-harness plumbing, not part of what is under test).
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), self.chain_name)
        fresh = []
        for a in self.consensus_args:
            if a.startswith("-mainchainrpcuser="):
                a = "-mainchainrpcuser=%s" % rpc_u
            elif a.startswith("-mainchainrpcpassword="):
                a = "-mainchainrpcpassword=%s" % rpc_p
            fresh.append(a)
        self.restart_node(VICTIM, extra_args=fresh + [
            "-port=%d" % p2p_port(3), "-rpcport=%d" % rpc_port(3),
            "-posproducer=0", "-posproducerkey=%s" % self.victim_wif])
        victim = self.nodes[VICTIM]

        recover_mark = self.nodes[VICTIM].debug_log_bytes()
        self.connect_nodes(1, VICTIM)
        # Re-offer the bodies as a peer would; the point is that they are now
        # ACCEPTED, whereas a permanently-invalidated block would be refused
        # out of the block index forever (BLOCK_CACHED_INVALID / bad-prevblk).
        for hdr, blk, h in reversed(made):
            try:
                victim.submitblock(blk)
            except Exception as e:
                self.log.info("re-submitblock(%s): %s" % (h[:16], e))

        target = base + 2
        self.wait_until(lambda: victim.getblockcount() >= target, timeout=180)
        assert_equal(victim.getblockhash(target), founder.getblockhash(target))
        recover_log = self.read_log(VICTIM, recover_mark)
        assert "reconsiderblock" not in recover_log
        assert "BLOCK_CACHED_INVALID" not in recover_log
        assert_equal(self.invalid_tips(victim), [])
        self.log.info("phase 3 ok: victim converged to height %d with no manual intervention"
                      % victim.getblockcount())

        # ---- phase 4: control — invalidation still works ------------------
        tip = victim.getbestblockhash()
        tip_height = victim.getblockcount()
        victim.invalidateblock(tip)
        assert victim.getblockcount() < tip_height, "invalidateblock did not disconnect the tip"
        assert any(t["hash"] == tip for t in self.invalid_tips(victim)), \
            "permanent invalidation is broken: the block is not reported as invalid"
        victim.reconsiderblock(tip)
        self.wait_until(lambda: victim.getblockcount() >= tip_height, timeout=60)
        self.log.info("phase 4 ok: permanent invalidation machinery intact")

        self.log.info("PASS: a parent-chain outage defers blocks instead of "
                      "permanently invalidating them, and the node self-heals")


if __name__ == '__main__':
    PosParentOutageNoPermanentInvalidTest().main()
