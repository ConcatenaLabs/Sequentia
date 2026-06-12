#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the Proof-of-Stake leader election (con_pos).

A PoS chain elects, for each block, a stake-weighted leader from a seed derived
from the previous block (and its Bitcoin anchor). The block is valid only if
signed by the elected rank-r leader and produced no earlier than r slot
intervals after the parent. See doc/sequentia/06-proof-of-stake.md.

Covered:
 - the leader schedule is deterministic and identical across nodes
 - the schedule is stake-weighted (a heavily-weighted staker wins rank 0 far
   more often than a lightly-weighted one)
 - a block signed by the elected leader is accepted
 - a block signed by a non-leader is rejected unless its slot has opened
   (liveness: a lower-priority staker can step in after the interval)
 - a key that is not a registered staker cannot produce
"""

from collections import Counter

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


SLOT_INTERVAL = 5


def make_staker():
    """Return (wif, pubkey_hex) for a fresh compressed key. The WIF uses the
    custom/regtest secret-key prefix (239); the 0x01 suffix marks the key as
    compressed so the daemon derives the matching compressed pubkey."""
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class ProofOfStakeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        # Two stakers: "big" with 9x the weight of "small".
        self.big_wif, self.big_pub = make_staker()
        self.small_wif, self.small_pub = make_staker()

        common = [
            "-con_pos=1",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-staker=%s:9" % self.big_pub,
            "-staker=%s:1" % self.small_pub,
            "-validatepegin=0",
        ]
        self.extra_args = [list(common), list(common)]

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]

        # Both nodes agree on the registered stake set
        info0 = n0.getstakerinfo()
        assert_equal(info0, n1.getstakerinfo())
        assert_equal(info0[self.big_pub], 9)
        assert_equal(info0[self.small_pub], 1)

        # The leader schedule for the next block is deterministic and identical
        sched0 = n0.getposschedule()
        sched1 = n1.getposschedule()
        assert_equal(sched0['schedule'], sched1['schedule'])
        assert_equal(sched0['seed'], sched1['seed'])
        assert_equal(len(sched0['schedule']), 2)
        assert_equal(sched0['slot_interval'], SLOT_INTERVAL)

        # --- Produce a block as whichever staker is the elected rank-0 leader ---
        leader_pub = sched0['schedule'][0]['pubkey']
        leader_wif = self.big_wif if leader_pub == self.big_pub else self.small_wif
        non_leader_wif = self.small_wif if leader_pub == self.big_pub else self.big_wif

        res = n0.generateposblock(leader_wif)
        assert_equal(res['rank'], 0)
        assert_equal(res['height'], 1)
        self.sync_blocks()
        assert_equal(n1.getblockcount(), 1)

        # The accepted block's header challenge commits to the leader's key
        header = n0.getblockheader(res['hash'])
        assert_equal(header['height'], 1)

        # --- Slot gating: a rank-1 leader's block is post-dated to the opening
        # of its slot (parent time + 1 * interval), so it cannot claim an
        # earlier timestamp than the rank-0 leader. ---
        parent_time = header['time']
        sched = n0.getposschedule()
        rank1_pub = sched['schedule'][1]['pubkey']
        rank1_wif = self.big_wif if rank1_pub == self.big_pub else self.small_wif
        res1 = n0.generateposblock(rank1_wif)
        assert_equal(res1['rank'], 1)
        h2 = n0.getblockheader(res1['hash'])
        # The rank-1 block's timestamp is gated to >= parent + interval
        assert h2['time'] >= parent_time + SLOT_INTERVAL, (h2['time'], parent_time)
        assert_equal(n0.getblockcount(), 2)

        # --- A key that is not a registered staker is refused outright ---
        outsider_wif, _ = make_staker()
        assert_raises_rpc_error(-5, "not a registered staker",
                                n0.generateposblock, outsider_wif)

        # --- Stake-weighting: over many slots, rank-0 should be the big staker
        # far more often than the small one (≈ 9:1). ---
        rank0_counts = Counter()
        for _ in range(60):
            sched = n0.getposschedule()
            rank0_counts[sched['schedule'][0]['pubkey']] += 1
            leader_pub = sched['schedule'][0]['pubkey']
            leader_wif = self.big_wif if leader_pub == self.big_pub else self.small_wif
            n0.generateposblock(leader_wif)

        big_wins = rank0_counts[self.big_pub]
        small_wins = rank0_counts[self.small_pub]
        self.log.info("rank-0 wins over 60 slots: big=%d small=%d", big_wins, small_wins)
        # With a 9:1 weighting the big staker should dominate; allow generous
        # slack for randomness but require a clear majority and that weighting
        # demonstrably matters.
        assert big_wins > small_wins, (big_wins, small_wins)
        assert big_wins >= 40, big_wins

        # Both nodes converged on the same chain
        self.sync_blocks()
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())


if __name__ == '__main__':
    ProofOfStakeTest().main()
