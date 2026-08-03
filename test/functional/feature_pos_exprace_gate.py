#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the exponential-race leader TIME-GATE scale hard fork (-posexpracegateheight).

The exp-race election (-posexpraceheight) replaced a RANK with a RATE. The legacy
PosVrfSlot is uniform in [0, W/w), so a staker of share s always draws below 1/s
and the best draw on the network is 0 or 1; multiplying that rank by the slot
interval is the whitepaper's rank-r liveness gate and costs nothing. The exp-race
score is -ln(U)*W/w, whose MINIMUM over all stakers is Exponential(1): mean 1, with
an unbounded geometric tail. Multiplying THAT by the whole slot interval leaves the
network silent for floor(min score) whole intervals before anyone may propose --
P(at least one interval lost) = e^-2 ~ 13.5%, mean interval ~1.21 intervals. That is
what the Sequentia testnet has done since its exp-race fork at height 44300 (0.0% of
intervals over one slot below it, 15.2% above it).

From pos_exprace_gate_height the gate is measured in SECONDS of score instead
(POS_EXPRACE_GATE_SECONDS), so the winner is gated at ~1 s and the producer's
cadence floor of one interval decides the cadence again, while a candidate scoring
N units worse still cannot produce for N more seconds.

This test runs one chain across the activation height. At every height it recomputes
both stakers' exp-race scores itself (from vrfprove), elects the lowest, and checks
the block the node stamps lands EXACTLY at the gate the rule in force at that height
prescribes -- and that the peer fully validates it, so the block assembler and the
consensus gate agree on both sides of the fork. Below the height it asserts the
throughput loss is reproduced (whole intervals lost, always an exact multiple of the
interval); from the height it asserts it is gone (no interval ever exceeds the
cadence floor). See doc/sequentia/04-proof-of-stake.md.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

SLOT_INTERVAL = 30         # seconds; the cadence floor a producer holds
GATE_SECONDS = 1           # POS_EXPRACE_GATE_SECONDS (src/pos.h)
BIG_W, SMALL_W = 8, 2
TOTAL = BIG_W + SMALL_W
GATE_HEIGHT = 151          # whole-interval gate below, score-second gate from here
PRE = GATE_HEIGHT - 1      # heights 1 .. 150
POST = 150                 # heights 151 .. 300

MAX_SLOT = 1 << 20
FRAC = 32
P = 61
LN2_Q32 = 2977044472       # round(ln2 * 2^32) -- must match src/pos.cpp


def _log2_q32(beta, n):
    v = (beta >> (n - P)) if n >= P else (beta << (P - n))
    frac = 0
    for i in range(1, FRAC + 1):
        v = (v * v) >> P
        if v >> (P + 1):
            frac |= 1 << (FRAC - i)
            v >>= 1
    return (n << FRAC) | frac


def exprace_score(beta, w, tot):
    """Mirror of PosVrfScoreExp (src/pos.cpp): the Q32 election key, bit-exact."""
    if w == 0 or tot == 0 or beta == 0:
        return (MAX_SLOT + 1) << FRAC
    n = beta.bit_length() - 1
    log2b = _log2_q32(beta, n)
    Lc = (256 << FRAC) - log2b
    neg_ln = (Lc * LN2_Q32) >> FRAC
    return (neg_ln * tot) // w


def exprace_slot(beta, w, tot):
    """Mirror of PosVrfSlotExp (src/pos.cpp)."""
    return min(exprace_score(beta, w, tot) >> FRAC, MAX_SLOT)


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosExpRaceGateTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.big_wif, self.big_pub = make_staker()
        self.small_wif, self.small_pub = make_staker()

        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-posexpraceheight=1",                          # exp-race election throughout
            "-posexpracegateheight=%d" % GATE_HEIGHT,       # the rule under test
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-staker=%s:%d" % (self.big_pub, BIG_W),
            "-staker=%s:%d" % (self.small_pub, SMALL_W),
            "-validatepegin=0",
        ]
        self.extra_args = [list(common), list(common)]

    def elect(self, node):
        """Run the exponential race ourselves: return (wif, slot) of the winner."""
        # getposslot reports the seed as a uint256, i.e. in display order;
        # vrfprove takes the VRF input as raw bytes, which is the order the
        # producer feeds it in (Span(seed.begin(), 32)). Reverse to match.
        seed = bytes.fromhex(node.getposslot()['seed'])[::-1].hex()
        best = None
        for wif, w in ((self.big_wif, BIG_W), (self.small_wif, SMALL_W)):
            beta = int(node.vrfprove(wif, seed)['output'], 16)
            score = exprace_score(beta, w, TOTAL)
            if best is None or score < best[1]:
                best = (wif, score, exprace_slot(beta, w, TOTAL))
        return best[0], best[2]

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]
        assert_equal(n0.getposslot()['slot_interval'], SLOT_INTERVAL)

        parent_time = n0.getblockheader(n0.getbestblockhash())['time']
        pre_late, post_late = [], []

        for height in range(1, PRE + POST + 1):
            # The producer never proposes sooner than one interval after the
            # parent (PosProducer::Step's cadence floor), so that is the earliest
            # wall-clock time it would try. Anything later is the gate, and the
            # gate alone -- which is what we are measuring.
            for n in (n0, n1):
                n.setmocktime(parent_time + SLOT_INTERVAL)

            wif, slot = self.elect(n0)
            r = n0.generateposblock(wif)
            assert_equal(r['vrf_slot'], slot)

            # The rule in force at THIS height decides what a slot is worth.
            unit = GATE_SECONDS if height >= GATE_HEIGHT else SLOT_INTERVAL
            expected = max(SLOT_INTERVAL, slot * unit)

            block_time = n0.getblockheader(r['hash'])['time']
            interval = block_time - parent_time
            assert_equal(interval, expected)
            assert_equal(interval % SLOT_INTERVAL if unit == SLOT_INTERVAL else 0, 0)

            if interval > SLOT_INTERVAL:
                (post_late if height >= GATE_HEIGHT else pre_late).append((height, slot, interval))
            parent_time = block_time

        # The defect, reproduced: below the activation height whole slots are
        # lost, and every lost interval is an exact multiple of the slot interval
        # (the chain waits out whole slots, it does not merely run late).
        assert len(pre_late) >= 1, \
            "no interval exceeded the cadence floor below the activation height -- " \
            "the whole-interval gate did not reproduce the throughput loss"
        for _, _, interval in pre_late:
            assert_equal(interval % SLOT_INTERVAL, 0)

        # The fix: from the activation height the gate never binds past the
        # cadence floor, so every block lands exactly one interval after its
        # parent. (P(a single block still binds) = e^-30, ~1e-13.)
        assert_equal(post_late, [])

        # Both nodes validated every block on both sides of the fork: the block
        # assembler's gate and the consensus gate agree, so a producer cannot
        # build a block its own peers reject as bad-posvrf-early.
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())
        assert_equal(n1.getblockcount(), PRE + POST)

        pre_mean = (PRE * SLOT_INTERVAL + sum(i - SLOT_INTERVAL for _, _, i in pre_late)) / PRE
        self.log.info(
            "exp-race time-gate fork at height %d: below it %d/%d blocks lost at least one "
            "whole slot (mean interval %.1fs against a %ds target, %.1f%% overshoot); "
            "from it 0/%d, mean exactly %ds. Peer validated all %d blocks."
            % (GATE_HEIGHT, len(pre_late), PRE, pre_mean, SLOT_INTERVAL,
               100.0 * (pre_mean - SLOT_INTERVAL) / SLOT_INTERVAL, POST, SLOT_INTERVAL,
               PRE + POST))


if __name__ == '__main__':
    PosExpRaceGateTest().main()
