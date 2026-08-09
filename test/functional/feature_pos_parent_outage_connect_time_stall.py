#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A parent-chain outage at CONNECT time must STALL, not invalidate (incident 2026-07-26).

Companion to feature_pos_parent_outage_no_permanent_invalid.py, which exercises
the ACCEPT-time path. This one covers the CONNECT-time path — the one the live
incident log actually shows:

    ERROR: ConnectBlock: CheckPosStakeRules: pos-escape-stall-unverifiable
    ERROR: ConnectTip: ConnectBlock a2fc54d9...e333d3 failed
    InvalidChainFound: invalid block=a2fc54d9...e333d3 height=47716

and the one where a naive fix is actively dangerous. Two distinct things must
hold at once here, which is why this path needs its own test:

  * the block must NOT be marked BLOCK_FAILED_VALID, because that verdict is
    persisted and nothing ever retries it (the anchor watcher only reconsiders
    blocks IT invalidated via MarkAnchorInvalid), which is what left the live
    node ~2300 blocks behind until a human ran reconsiderblock; AND
  * the retry must not BUSY-LOOP. Simply skipping the mark is not enough and is
    in fact worse: FindMostWorkChain only drops candidates carrying
    BLOCK_FAILED_MASK, so an unmarked candidate is handed straight back on the
    next iteration and ActivateBestChain spins, re-running validation and
    hammering the unreachable parent daemon through an RPC taken under cs_main.

The fix satisfies both by routing the soft verdict into the fStall mechanism
Elements already uses for CheckPeginRipeness: make no progress, mark nothing,
and break out of BOTH activation loops.

HOW THE CONNECT-TIME PATH IS FORCED

AcceptBlock runs the full PoS check only for a block building on the tip or a
sibling of it; anything deeper is deferred to connect time. Rather than depend on
that shape, this test uses blocks that are ALREADY on disk:

  1. sync the victim normally while the parent is reachable (blocks stored AND
     connected, so ConnectTip will read them from disk later)
  2. invalidateblock the lower of them — a manual, deliberate invalidation, a
     path the fix does not touch
  3. RESTART the victim WHILE THE PARENT IS STILL REACHABLE, then stop the
     parent. Both halves of that order matter. The restart is what clears
     `g_anchor_mtp_cache`, which is keyed by hash and otherwise never
     invalidated: a warm cache would answer the median-time-past lookup from
     memory and no failure could occur. And the restart must precede the outage
     because init.cpp refuses to start at all when the parent is unreachable
     under -validateanchor=1 ("Sequentia could not reach the Bitcoin node it
     anchors its blocks to"). That startup guard landed in d5c5a40 on
     2026-07-26, i.e. in response to this very incident — it closes the
     start-up variant, but by construction it cannot cover a parent daemon that
     dies or stops answering while the node is ALREADY RUNNING, which is the
     case reproduced here and the one the fix exists for.
  4. reconsiderblock -> ActivateBestChain -> ConnectTip -> ConnectBlock ->
     CheckPosStakeRules -> escaping-stall gap -> MTP lookup misses the cold
     cache, hits the dead RPC -> UNKNOWN -> pos-escape-stall-unverifiable

On the unpatched build step 4 produces InvalidBlockFound + InvalidChainFound and
the block is poisoned permanently; on the patched build it produces a ConnectTip
stall and the chain converges by itself once the parent returns.

See the companion test for the two regtest environment traps this also handles
(the 600-second parent MTP gap needing setmocktime, and leftover elementsd
holding the fixed test ports).
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


SEED_STAKE = 1000000000
STAKE_CSV = 15
COMMITTEE = 3

MTP_GAP = 600                # DEFAULT_POS_ESCAPE_STALL_MTP_GAP (seconds)
PARENT_STEP_SECONDS = 900    # parent MTP advance per Sequentia block (> MTP_GAP)
PARENT_BLOCKS_PER_STEP = 12  # >= 11 so the median fully moves

OUTAGE_SECONDS = 20

# With the parent unreachable the anchor watcher returns early (it cannot even
# fetch the parent tip), so during the outage the only activation triggers are
# the reconsiderblock below plus any peer announcement. A correct implementation
# therefore stalls a handful of times; the busy-loop regression would spin
# thousands of times per second.
MAX_STALL_EVENTS = 40 * OUTAGE_SECONDS


class PosParentOutageConnectTimeStallTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.founder_wif, self.founder_pub = make_key()
        self.peer_wif, _ = make_key()
        self.victim_wif, _ = make_key()
        self.mocktime = int(time.time())

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def victim_args(self, rpc_u, rpc_p):
        fresh = []
        for a in self.consensus_args:
            if a.startswith("-mainchainrpcuser="):
                a = "-mainchainrpcuser=%s" % rpc_u
            elif a.startswith("-mainchainrpcpassword="):
                a = "-mainchainrpcpassword=%s" % rpc_p
            fresh.append(a)
        return fresh + ["-port=%d" % p2p_port(3), "-rpcport=%d" % rpc_port(3),
                        "-posproducer=0", "-posproducerkey=%s" % self.victim_wif]

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"
        self.chain_name = chain
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-mocktime=%d" % self.mocktime,
        ]
        self.add_nodes(1, [parent_args], chain=[chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)

        self.consensus_args = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % COMMITTEE, "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-con_blocksubsidy=5000000000",
            "-con_genesis_stake=%s:%d:%d" % (self.founder_pub, SEED_STAKE, STAKE_CSV),
            "-con_connect_genesis_outputs=1", "-initialfreecoins=500000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-posescapestallmtpgap=%d" % MTP_GAP,
            # Issued from ConnectBlock with cs_main held; the stock 900s default
            # would freeze the whole node on a hung (rather than dead) parent.
            "-mainchainrpctimeout=5",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        founder_args = self.consensus_args + [
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        peer_args = self.consensus_args + [
            "-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
            "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        self.add_nodes(1, [founder_args], chain=[chain]); self.start_node(1)
        self.add_nodes(1, [peer_args], chain=[chain]); self.start_node(2)
        self.add_nodes(1, [self.victim_args(rpc_u, rpc_p)], chain=[chain]); self.start_node(3)
        # node2 stays connected to node1 all run: the producer only proposes
        # while it has peers, so isolating the victim must not isolate the founder.
        self.connect_nodes(1, 2)
        self.connect_nodes(1, 3)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    # --- helpers -----------------------------------------------------------

    def advance_parent(self, seconds=PARENT_STEP_SECONDS, nblocks=PARENT_BLOCKS_PER_STEP):
        """Advance the parent so its MEDIAN-TIME-PAST really moves.

        Stepped per block, not jumped: timestamps must be strictly increasing or
        the parent rejects its own block as time-too-old.
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
        VICTIM = 3

        assert_equal(founder.getstakerinfo().get(self.founder_pub), SEED_STAKE)

        # ---- phase 1: sync the victim normally ----------------------------
        for target in (1, 2, 3):
            self.advance_parent()
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=120)
        self.wait_until(lambda: victim.getblockcount() >= 3, timeout=90)
        assert_equal(victim.getblockcount(), 3)
        assert_equal(self.invalid_tips(victim), [])
        h2 = victim.getblockhash(2)
        self.log.info("phase 1 ok: victim synced to height 3; blocks are on disk AND connected")

        # ---- phase 2: force a CONNECT-time judgement with the parent down --
        # Isolate the victim so the reconsiderblock below is the only activation
        # trigger, which keeps the stall accounting unambiguous.
        self.disconnect_nodes(1, VICTIM)

        # Manual invalidation (a path the fix does not touch) so the blocks are
        # disconnected but still on disk, ready to be re-connected on demand.
        victim.invalidateblock(h2)
        assert_equal(victim.getblockcount(), 1)

        # Restart the victim FIRST, while the parent is still reachable. The
        # restart clears g_anchor_mtp_cache (in-memory, keyed by hash, otherwise
        # never invalidated) — without that the lookup below is served from
        # memory and nothing can fail. It has to happen before the outage
        # because init.cpp aborts startup when the parent is unreachable under
        # -validateanchor=1 (guard added in d5c5a40, 2026-07-26, in response to
        # this incident). That guard covers the start-up variant; it cannot
        # cover a parent daemon that dies while the node is already running,
        # which is exactly what is simulated next.
        self.log.info("restarting the victim (parent still up) to clear the MTP cache")
        self.restart_node(VICTIM, extra_args=self.nodes[VICTIM].extra_args)
        victim = self.nodes[VICTIM]
        assert_equal(victim.getblockcount(), 1)

        self.log.info("stopping the parent: simulating bitcoind dying AT RUNTIME")
        self.stop_node(0)

        log_mark = self.nodes[VICTIM].debug_log_bytes()
        victim.reconsiderblock(h2)   # -> ActivateBestChain -> ConnectTip -> ConnectBlock

        deadline = time.time() + OUTAGE_SECONDS
        while time.time() < deadline:
            time.sleep(1)
            # Must stay responsive: an RPC blocking under cs_main would hang this.
            victim.getblockchaininfo()

        outage_log = self.read_log(VICTIM, log_mark)
        stall_events = outage_log.count("STALLING further progress in ConnectTip")
        soft = "pos-escape-stall-unverifiable" in outage_log
        self.log.info("outage: soft reason seen=%s ; ConnectTip stalls=%d" % (soft, stall_events))

        # The trigger must really have fired, and specifically at CONNECT time —
        # otherwise a green run would prove nothing about the fStall path.
        assert soft, "pos-escape-stall-unverifiable did not fire; this run proves nothing"
        assert "ConnectBlock" in outage_log or "ConnectTip" in outage_log, \
            "the rejection did not happen on the connect path"
        assert stall_events > 0, (
            "the connect-time verdict did not go through the fStall path: the block "
            "was rejected instead of deferred")

        # Nothing may be permanently invalidated. On the unpatched build this is
        # where InvalidBlockFound/InvalidChainFound fire, so it is a real
        # discriminator on this path (unlike the accept-time companion test).
        assert "InvalidChainFound" not in outage_log, (
            "a parent-chain outage permanently invalidated a block:\n"
            + "\n".join(l for l in outage_log.splitlines() if "InvalidChainFound" in l))
        assert_equal(self.invalid_tips(victim), [])

        # And it must not busy-loop.
        assert stall_events <= MAX_STALL_EVENTS, (
            "ConnectTip stalled %d times in %ds — that is a busy loop, not a retry"
            % (stall_events, OUTAGE_SECONDS))
        assert_equal(victim.getblockcount(), 1)   # no progress, as intended
        self.log.info("phase 2 ok: connect-time verdict deferred via fStall, "
                      "nothing invalidated, no busy loop, node responsive")

        # ---- phase 3: parent returns, victim must recover BY ITSELF -------
        self.log.info("restarting the parent; the victim must reconnect the blocks by itself")
        self.start_node(0)
        try:
            parent.loadwallet("w")
        except Exception:
            pass
        parent.setmocktime(self.mocktime)

        # The parent regenerated its RPC cookie (harness plumbing, not part of
        # what is under test), so hand the victim the new credentials.
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), self.chain_name)
        self.restart_node(VICTIM, extra_args=self.victim_args(rpc_u, rpc_p))
        victim = self.nodes[VICTIM]

        recover_mark = self.nodes[VICTIM].debug_log_bytes()
        # No reconsiderblock here on purpose: the blocks must still be live
        # candidates, so ordinary activation reconnects them on its own.
        self.wait_until(lambda: victim.getblockcount() >= 3, timeout=180)
        assert_equal(victim.getblockhash(3), founder.getblockhash(3))
        recover_log = self.read_log(VICTIM, recover_mark)
        assert "reconsiderblock" not in recover_log
        assert_equal(self.invalid_tips(victim), [])
        self.log.info("phase 3 ok: victim reconnected to height %d with no manual intervention"
                      % victim.getblockcount())

        self.log.info("PASS: a connect-time parent-chain outage stalls and self-heals "
                      "instead of permanently invalidating the block")


if __name__ == '__main__':
    PosParentOutageConnectTimeStallTest().main()
