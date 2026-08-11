#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests Proof-of-Stake committee certification (-poscommitteesize > 1).

With committee certification enabled, each block's challenge requires the
elected leader's signature AND countersignatures from a strict majority of the
slot's committee (the first n schedule entries) — the PoC form of the paper's
51-of-100 certification giving immediate finality (principle 6). A block
without a committee quorum cannot exist. See doc/sequentia/04-proof-of-stake.md.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

COMMITTEE_SIZE = 3
QUORUM = 2  # strict majority of 3


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosCommitteeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        # Four equal stakers; each slot's committee is the first three of the
        # schedule, and two of those three must countersign.
        self.stakers = [make_staker() for _ in range(4)]
        self.wif_by_pub = {pub: wif for wif, pub in self.stakers}

        common = [
            "-con_pos=1",
            "-poscommitteesize=%d" % COMMITTEE_SIZE,
            "-posslotinterval=5",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [list(common), list(common)]

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]

        # The schedule exposes the committee and its quorum, identically on
        # both nodes.
        sched = n0.getposschedule()
        assert_equal(sched, n1.getposschedule())
        assert_equal(len(sched['committee']), COMMITTEE_SIZE)
        assert_equal(sched['quorum'], QUORUM)
        # The committee is the schedule prefix.
        assert_equal(sched['committee'],
                     [e['pubkey'] for e in sched['schedule'][:COMMITTEE_SIZE]])

        leader_wif = self.wif_by_pub[sched['schedule'][0]['pubkey']]
        committee_wifs = [self.wif_by_pub[p] for p in sched['committee']]

        # --- Below quorum: the leader alone cannot certify a block (the
        # rank-0 leader is committee[0], so its key yields exactly one
        # countersignature — short of the 2-of-3 quorum) ---
        assert_raises_rpc_error(-1, "Insufficient committee keys",
                                n0.generateposblock, leader_wif)
        # Passing the leader's own key again as a committee key adds nothing.
        assert_raises_rpc_error(-1, "Insufficient committee keys",
                                n0.generateposblock, leader_wif, [leader_wif])

        # --- At quorum: leader + 2 committee keys certify the block ---
        res = n0.generateposblock(leader_wif, committee_wifs[:QUORUM])
        assert_equal(res['rank'], 0)
        assert_equal(res['countersignatures'], QUORUM)
        assert_equal(n0.getblockcount(), 1)
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())

        # --- Produce a run of certified blocks; both nodes stay converged ---
        for _ in range(10):
            sched = n0.getposschedule()
            leader_wif = self.wif_by_pub[sched['schedule'][0]['pubkey']]
            committee_wifs = [self.wif_by_pub[p] for p in sched['committee']]
            res = n0.generateposblock(leader_wif, committee_wifs)
            assert res['countersignatures'] >= QUORUM
        assert_equal(n0.getblockcount(), 11)
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())


if __name__ == '__main__':
    PosCommitteeTest().main()
