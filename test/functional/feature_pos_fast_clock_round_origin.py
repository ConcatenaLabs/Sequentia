#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A leader whose clock runs fast does not hold the committee's rounds back (#286).

The round schedule starts from round 0's proposal stamp, which every member
reads off the same block. A leader whose clock runs two minutes fast stamps its
proposal two minutes ahead, and members used to wait for their clocks to reach
that stamp before signing anything: every block it led cost two minutes. The
origin is now the stamp only while it lies within a few seconds of when the
member received the proposal, and the receipt plus that slack otherwise.

Four producers, one seat each (quorum three). Producer A's clock runs 120 s
fast. The chain must keep certifying with no gap anywhere near that long,
including blocks A led (seen as blocks stamped well ahead of real time).
"""
import time

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

FAST_SECONDS = 120
BLOCKS = 12
MAX_GAP_SECONDS = 30


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosFastClockRoundOriginTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(4)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-con_max_block_sig_size=4000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]]
                           for i in range(4)]

    def setup_network(self):
        self.setup_nodes()
        for a in range(4):
            for b in range(a + 1, 4):
                self.connect_nodes(a, b)

    def run_test(self):
        fast, ok = self.nodes[0], self.nodes[1]

        self.log.info("The committee certifies a few blocks with every clock right")
        self.wait_until(lambda: all(n.getblockcount() >= 3 for n in self.nodes), timeout=120)

        self.log.info("Producer A's clock now runs %d s fast" % FAST_SECONDS)
        start_height = ok.getblockcount()
        last_height, last_at = start_height, time.time()
        max_gap, max_ahead = 0.0, 0
        deadline = time.time() + 600
        while True:
            fast.setmocktime(int(time.time()) + FAST_SECONDS)
            height = ok.getblockcount()
            now = time.time()
            if height > last_height:
                max_gap = max(max_gap, now - last_at)
                for h in range(last_height + 1, height + 1):
                    stamp = ok.getblockheader(ok.getblockhash(h))["time"]
                    max_ahead = max(max_ahead, stamp - int(now))
                last_height, last_at = height, now
            else:
                max_gap = max(max_gap, now - last_at)
            assert max_gap < MAX_GAP_SECONDS, "no block for %.0f s at height %d" % (max_gap, height)
            if height >= start_height + BLOCKS and max_ahead >= FAST_SECONDS - 20:
                break
            assert time.time() < deadline, "A never led a block (chain at most %d s ahead)" % max_ahead
            time.sleep(0.2)
        self.log.info("%d blocks, A led at least one (chain up to %d s ahead), longest gap %.1f s" %
                      (last_height - start_height, max_ahead, max_gap))

        self.log.info("The members agree on the chain")
        self.sync_blocks()
        h = min(n.getblockcount() for n in self.nodes)
        assert_equal(len({n.getblockhash(h) for n in self.nodes}), 1)


if __name__ == '__main__':
    PosFastClockRoundOriginTest().main()
