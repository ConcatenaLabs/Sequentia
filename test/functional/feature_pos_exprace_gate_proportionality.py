#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Does the exp-race SCORE-SECOND time gate still hand out blocks in stake proportion?

feature_pos_exprace_gate.py proves the gate lands blocks where the rule says. This
test asks the follow-up question: the pre-fix whole-interval gate separated
candidates by 30 s per score unit, so in the usual round the lowest scorer was the
only node with a proposal on the table -- it won by EXCLUSIVITY. The score-second
gate separates them by 1 s, so nearly every staker's proposal is on the table at
the same moment and the winner is picked by PosProducer::BackedForRound
(src/pos_producer.cpp:975-981), which orders by (1) freshest Bitcoin anchor,
(2) lowest exp-race score. Does that still pay each staker its stake share?

Four stakers hold 50% / 30% / 15% / 5%. One chain runs across the gate activation
height. At every height the test draws each staker's REAL VRF output from the node
(vrfprove), scores them with a bit-exact mirror of PosVrfScoreExp, and elects a
winner under the rule in force -- then, as the load-bearing check, elects a winner
under the OTHER rule from the SAME draws and asserts the two agree at every single
height. That is the proposition under test stated exactly: with the committee
agreeing on the Bitcoin anchor, the gate scale is a pure time shift and never
reorders candidates. The realised shares are then measured against stake with a
chi-square goodness-of-fit test on each side of the fork.

WHAT THIS TEST CANNOT SEE: with no parent chain attached every block carries the
same (null) anchor, so the anchor-first half of BackedForRound never fires. The
case where nodes DISAGREE about Bitcoin's tip -- where a fresher-anchored, worse-
scoring proposal beats a lower-scoring one, and where the score-second gate makes
that happen several times more often because the field is wider -- is measured over
200,000 elections in the pos_exprace_gate_anchor_divergence_skew unit test
(src/test/pos_tests.cpp) instead. See doc/sequentia/04-proof-of-stake.md.
"""

import math

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

SLOT_INTERVAL = 30          # seconds; the producer's cadence floor
GATE_SECONDS = 1            # POS_EXPRACE_GATE_SECONDS (src/pos.h)
WEIGHTS = [50, 30, 15, 5]   # deliberately unequal stakes
TOTAL = sum(WEIGHTS)
PER_ARM = 2000              # blocks measured under each gate rule
GATE_HEIGHT = PER_ARM + 1   # whole-interval gate below, score-second gate from here

MAX_SLOT = 1 << 20
FRAC = 32
P = 61
LN2_Q32 = 2977044472        # round(ln2 * 2^32) -- must match src/pos.cpp


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


def gamma_q(a, x):
    """Regularized upper incomplete gamma Q(a,x) (Numerical Recipes 6.2)."""
    if x <= 0:
        return 1.0
    gln = math.lgamma(a)
    if x < a + 1.0:
        ap, s, d = a, 1.0 / a, 1.0 / a
        for _ in range(1000):
            ap += 1.0
            d *= x / ap
            s += d
            if abs(d) < abs(s) * 1e-15:
                break
        return 1.0 - s * math.exp(-x + a * math.log(x) - gln)
    fpmin = 1e-300
    b, c, d = x + 1.0 - a, 1.0 / fpmin, 1.0 / (x + 1.0 - a)
    h = d
    for i in range(1, 1001):
        an = -i * (i - a)
        b += 2.0
        d = an * d + b
        if abs(d) < fpmin:
            d = fpmin
        c = b + an / c
        if abs(c) < fpmin:
            c = fpmin
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 1e-15:
            break
    return math.exp(-x + a * math.log(x) - gln) * h


def chisq_p(observed, weights):
    """Chi-square goodness of fit of block counts against stake shares."""
    n = sum(observed)
    tot = sum(weights)
    chi2 = 0.0
    for obs, w in zip(observed, weights):
        exp = n * w / tot
        chi2 += (obs - exp) ** 2 / exp
    return chi2, gamma_q((len(weights) - 1) / 2.0, chi2 / 2.0)


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosExpRaceGateProportionalityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in WEIGHTS]

        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-posexpraceheight=1",                          # exp-race election throughout
            "-posexpracegateheight=%d" % GATE_HEIGHT,       # the gate scale under test
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ]
        common += ["-staker=%s:%d" % (pub, w) for (_, pub), w in zip(self.stakers, WEIGHTS)]
        self.extra_args = [list(common), list(common)]

    def draw(self, node):
        """Draw every staker's real VRF output for the coming height."""
        # getposslot reports the seed as a uint256, i.e. in display order;
        # vrfprove takes the VRF input as raw bytes, the order the producer feeds
        # it in (Span(seed.begin(), 32)). Reverse to match.
        seed = bytes.fromhex(node.getposslot()['seed'])[::-1].hex()
        return [int(node.vrfprove(wif, seed)['output'], 16) for wif, _ in self.stakers]

    def elect(self, betas, unit):
        """One round: who publishes, under a gate worth `unit` seconds per score point?

        Mirrors PosProducer::Step (proposal time max(gate, cadence floor)) and
        BackedForRound with the anchor key inert -- every proposal here carries
        the same anchor, so the ordering falls through to the lowest score.
        """
        scores = [exprace_score(b, w, TOTAL) for b, w in zip(betas, WEIGHTS)]
        slots = [exprace_slot(b, w, TOTAL) for b, w in zip(betas, WEIGHTS)]
        ready = [max(s * unit, SLOT_INTERVAL) for s in slots]
        first = min(ready)
        field = [i for i in range(len(WEIGHTS)) if ready[i] == first]
        winner = min(field, key=lambda i: scores[i])
        return winner, slots[winner]

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]
        assert_equal(n0.getposslot()['slot_interval'], SLOT_INTERVAL)

        parent_time = n0.getblockheader(n0.getbestblockhash())['time']
        wins = {'whole': [0] * len(WEIGHTS), 'fine': [0] * len(WEIGHTS)}
        late = {'whole': 0, 'fine': 0}
        field_size = {'whole': 0, 'fine': 0}

        for height in range(1, 2 * PER_ARM + 1):
            for n in (n0, n1):
                n.setmocktime(parent_time + SLOT_INTERVAL)

            betas = self.draw(n0)
            fine_active = height >= GATE_HEIGHT
            arm = 'fine' if fine_active else 'whole'
            unit = GATE_SECONDS if fine_active else SLOT_INTERVAL

            winner, slot = self.elect(betas, unit)

            # THE LOAD-BEARING CHECK: the same draws, elected under the OTHER
            # gate scale, must return the same winner. The gate is a pure time
            # shift; it never reorders candidates when anchors agree.
            other, _ = self.elect(betas, SLOT_INTERVAL if fine_active else GATE_SECONDS)
            assert_equal(other, winner)

            # Field width: how many proposals are on the table when the round is
            # decided. This is what the fix actually changes.
            slots = [exprace_slot(b, w, TOTAL) for b, w in zip(betas, WEIGHTS)]
            ready = [max(s * unit, SLOT_INTERVAL) for s in slots]
            field_size[arm] += sum(1 for r in ready if r == min(ready))

            wif = self.stakers[winner][0]
            r = n0.generateposblock(wif)
            assert_equal(r['vrf_slot'], slot)

            # The node's own gate agrees with ours, so the election above is the
            # one the real consensus rule permits.
            expected = max(SLOT_INTERVAL, slot * unit)
            block_time = n0.getblockheader(r['hash'])['time']
            assert_equal(block_time - parent_time, expected)
            if block_time - parent_time > SLOT_INTERVAL:
                late[arm] += 1

            wins[arm][winner] += 1
            parent_time = block_time

        # Every block validated by the peer under both rules.
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())
        assert_equal(n1.getblockcount(), 2 * PER_ARM)

        # Realised share against stake share, each side of the fork.
        for arm, label in (('whole', 'whole-interval gate (pre-fix)'),
                           ('fine', 'score-second gate (the fix)')):
            chi2, p = chisq_p(wins[arm], WEIGHTS)
            shares = " / ".join("%.1f%%" % (100.0 * c / PER_ARM) for c in wins[arm])
            self.log.info(
                "%s: %d blocks, realised %s against stake 50.0%% / 30.0%% / 15.0%% / 5.0%%; "
                "chi2=%.3f p=%.3f; mean deciding field %.2f/4; %d block(s) past the cadence floor"
                % (label, PER_ARM, shares, chi2, p, field_size[arm] / PER_ARM, late[arm]))
            # n=2000 per arm: the 5%% staker.s expected count is 100 with sd 9.7, so
            # this arm catches a gross skew, not a subtle one -- the 200,000-round
            # unit test carries the statistical power. A p below 0.001 here would
            # mean the election is grossly non-proportional.
            assert p > 0.001, "%s: realised share is not stake-proportional (p=%g)" % (label, p)

        # The fix widens the deciding field (that IS the change), and closes the
        # throughput hole: under the whole-interval gate the network is silent for
        # whole slots with probability e^-2 ~ 13.5%; under the score-second gate it
        # would take a network-minimum score above 30, e^-31 ~ 3e-14.
        assert field_size['fine'] > field_size['whole'], "the score-second gate did not widen the field"
        assert late['whole'] >= 1, "the whole-interval gate did not reproduce the throughput loss"
        assert_equal(late['fine'], 0)


if __name__ == '__main__':
    PosExpRaceGateProportionalityTest().main()
