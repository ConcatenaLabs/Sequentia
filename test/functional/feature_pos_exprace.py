#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the exponential-race leader-election hard fork (-posexpraceheight).

Below the activation height the legacy PosVrfSlot / raw-beta election is in
force; from the height on, the exponential-race sortition (PosVrfSlotExp, which
is exactly stake-proportional and split-neutral). The switch changes which slot
a leader gets, so it is a coordinated hard fork gated by height.

We produce blocks across the activation height on a two-staker chain and check,
for every block, that the node's own reported vrf_slot matches the rule that
should be in force at that height (recomputed here from the block's VRF output),
and that the peer fully validates every block (so the producer time-gate and the
consensus time-gate agree on both sides of the fork -- a mismatch would split
the chain silently). See doc/sequentia/04-proof-of-stake.md.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

SLOT_INTERVAL = 2
TOTAL = 10                 # 8 + 2
EXPRACE_HEIGHT = 6         # legacy for heights < 6, exp-race from height 6

MAX_SLOT = 1 << 20
FRAC = 32
P = 61
LN2_Q32 = 2977044472       # round(ln2 * 2^32) -- must match src/pos.cpp


def legacy_slot(beta, w, tot):
    """Mirror of PosVrfSlot (src/pos.cpp)."""
    if w == 0 or tot == 0:
        return MAX_SLOT
    q = beta // w
    slot = ((q >> 192) * tot) >> 64
    return min(slot, MAX_SLOT)


def _log2_q32(beta, n):
    v = (beta >> (n - P)) if n >= P else (beta << (P - n))
    frac = 0
    for i in range(1, FRAC + 1):
        v = (v * v) >> P
        if v >> (P + 1):
            frac |= 1 << (FRAC - i)
            v >>= 1
    return (n << FRAC) | frac


def exprace_slot(beta, w, tot):
    """Mirror of PosVrfSlotExp / PosVrfScoreExp (src/pos.cpp), bit-exact."""
    if w == 0 or tot == 0 or beta == 0:
        return MAX_SLOT
    n = beta.bit_length() - 1
    log2b = _log2_q32(beta, n)
    Lc = (256 << FRAC) - log2b
    neg_ln = (Lc * LN2_Q32) >> FRAC
    slot = (neg_ln * tot // w) >> FRAC
    return min(slot, MAX_SLOT)


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosExpRaceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.big_wif, self.big_pub = make_staker()
        self.small_wif, self.small_pub = make_staker()

        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-posexpraceheight=%d" % EXPRACE_HEIGHT,
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-staker=%s:8" % self.big_pub,
            "-staker=%s:2" % self.small_pub,
            "-validatepegin=0",
        ]
        self.extra_args = [list(common), list(common)]

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]
        assert_equal(n0.getposschedule()['total_weight'], TOTAL)

        weight_of = {self.big_wif: 8, self.small_wif: 2}

        # --- Below the activation height: the legacy raw-beta slot rule. ---
        for _ in range(1, EXPRACE_HEIGHT):          # heights 1 .. H-1
            r = n0.generateposblock(self.big_wif)
            beta = int(r['vrf_output'], 16)
            assert_equal(r['vrf_slot'], legacy_slot(beta, 8, TOTAL))
        assert_equal(n0.getblockcount(), EXPRACE_HEIGHT - 1)

        # --- From the activation height on: the exponential-race slot rule.
        # Alternate stakers so both weights exercise the new formula. Count how
        # many differ from what the legacy rule would have given -- proving the
        # rule actually switched, not that it silently stayed legacy. ---
        # A long post-activation run: many blocks, both stakers, checking every
        # block's slot matches the exp-race formula and the peer validates it.
        POST = 200
        differs_from_legacy = 0
        big_wins = small_wins = 0
        for h in range(EXPRACE_HEIGHT, EXPRACE_HEIGHT + POST):
            wif = self.big_wif if (h % 2 == 0) else self.small_wif
            w = weight_of[wif]
            r = n0.generateposblock(wif)
            beta = int(r['vrf_output'], 16)
            assert_equal(r['vrf_slot'], exprace_slot(beta, w, TOTAL))
            if r['vrf_slot'] != legacy_slot(beta, w, TOTAL):
                differs_from_legacy += 1
            if wif == self.big_wif:
                big_wins += 1
            else:
                small_wins += 1
        assert differs_from_legacy >= 1, "exp-race produced identical slots to legacy -- rule did not switch"

        # --- The peer fully validated every block across the fork and the long
        # run: producer time-gate and consensus time-gate agree on both sides. ---
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())
        assert_equal(n1.getblockcount(), EXPRACE_HEIGHT + POST - 1)

        self.log.info("exp-race hard fork: %d/%d post-activation blocks differ from the "
                      "legacy rule; chain reached height %d, peer validated every block "
                      "across activation height %d without stall"
                      % (differs_from_legacy, POST, n0.getblockcount(), EXPRACE_HEIGHT))


if __name__ == '__main__':
    PosExpRaceTest().main()
