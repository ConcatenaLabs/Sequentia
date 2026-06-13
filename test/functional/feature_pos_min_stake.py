#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the minimum-stake blocksigner eligibility floor (-posminstake).

The whitepaper (§3.3) requires a minimum stake — 0.01% of supply, 40,000 SEQ —
to be an eligible blocksigner. Stake below the floor is ignored by the leader
schedule and sortition, while still appearing in the registry. See
doc/sequentia/06-proof-of-stake.md §5.

Covered:
 - getstakerinfo reports every registered staker (the floor is not a filter on
   registration), but the leader schedule excludes sub-floor stakers
 - a sub-floor staker cannot produce a block
 - an at/above-floor staker is elected and produces normally
 - both nodes agree (the floor is a shared consensus parameter)
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosMinStakeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.big_wif, self.big_pub = make_staker()     # 100, above the floor
        self.small_wif, self.small_pub = make_staker()  # 10, below the floor
        common = [
            "-con_pos=1",
            "-posslotinterval=1",
            "-posminstake=50",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-staker=%s:100" % self.big_pub,
            "-staker=%s:10" % self.small_pub,
            "-validatepegin=0",
        ]
        self.extra_args = [list(common), list(common)]

    def run_test(self):
        n0, n1 = self.nodes

        # The registry still lists both stakers (the floor gates eligibility,
        # not registration), and both nodes agree.
        info = n0.getstakerinfo()
        assert_equal(info, n1.getstakerinfo())
        assert_equal(info[self.big_pub], 100)
        assert_equal(info[self.small_pub], 10)

        # But the leader schedule excludes the sub-floor staker entirely.
        sched = n0.getposschedule()
        assert_equal(sched['schedule'], n1.getposschedule()['schedule'])
        sched_pubs = [e['pubkey'] for e in sched['schedule']]
        assert_equal(sched_pubs, [self.big_pub])
        assert self.small_pub not in sched_pubs

        # The sub-floor staker cannot produce a block...
        assert_raises_rpc_error(-5, "not a registered staker for the current slot",
                                n0.generateposblock, self.small_wif)

        # ...while the eligible staker is rank 0 and produces normally.
        res = n0.generateposblock(self.big_wif)
        assert_equal(res['rank'], 0)
        assert_equal(res['height'], 1)
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), res['hash'])

        # Produce several more; the eligible staker is always the sole leader.
        for h in range(2, 6):
            sched = n0.getposschedule()
            assert_equal([e['pubkey'] for e in sched['schedule']], [self.big_pub])
            res = n0.generateposblock(self.big_wif)
            assert_equal(res['height'], h)
        self.sync_blocks()
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())
        assert_equal(n0.getblockcount(), 5)


if __name__ == '__main__':
    PosMinStakeTest().main()
