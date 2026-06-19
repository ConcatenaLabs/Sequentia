#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Quantify how inter-node clock skew (synchrony loss) maps to fork risk.

The gossip round-robin elects round 0's leader (lowest VRF) and advances the
backed leader every ROUND_MS; safety needs all honest nodes in the SAME round at
once, so they all sign the same block and only one block per height can reach a
quorum. The round index is anchored to the round-0 leader's block timestamp (a
network-global value) precisely so nodes stay aligned.

This harness deliberately breaks that alignment by injecting a GRADIENT of clock
skew across the committee: node i runs its round scheduler skew = i * STEP ms off.
With enough spread, nodes straddle ROUND_MS boundaries, sign across rounds, and
two competing certificates can form -> a fork. We then check, at every common
height, whether all nodes agree on the block hash (the gold-standard fork test)
and report a machine-readable verdict. A sweep over STEP shows where forks set in.

  POS_SKEW_STEP_MS  per-node skew increment (node i skew = i*STEP), default 0
  POS_SKEW_NODES    committee size, one key per node, default 12
  POS_SKEW_SECONDS  observation window, default 35

Always exits 0 (so a sweep can continue); prints  SKEW_RESULT ...  with the verdict.
"""

import os
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

STEP = int(os.environ.get("POS_SKEW_STEP_MS", "0"))
N = int(os.environ.get("POS_SKEW_NODES", "12"))
SECONDS = int(os.environ.get("POS_SKEW_SECONDS", "35"))

import test_framework.util as _util
import test_framework.test_framework as _tf
_util.MAX_NODES = max(_util.MAX_NODES, N + 1)
_tf.MAX_NODES = _util.MAX_NODES

QUORUM = N // 2 + 1


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosRoundSkewTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = N
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(N)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % N, "-posslotinterval=1",
            "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = []
        for i in range(N):
            wif = self.stakers[i][0]
            args = common + ["-posproducer", "-posproducerkey=%s" % wif]
            if STEP and i > 0:
                args.append("-posdebugroundskewms=%d" % (i * STEP))
            self.extra_args.append(args)

    def setup_network(self):
        self.setup_nodes()
        # Ring + star backbone through node 0: connected, low diameter.
        for i in range(N):
            self.connect_nodes(i, (i + 1) % N)
        for i in range(2, N):
            self.connect_nodes(i, 0)

    def run_test(self):
        spread = (N - 1) * STEP
        self.log.info("skew gradient: node i = i*%d ms (max %d ms across %d nodes), watching %ds"
                      % (STEP, spread, N, SECONDS))
        deadline = time.time() + SECONDS
        while time.time() < deadline:
            time.sleep(3)
            hs = [n.getblockcount() for n in self.nodes]
            self.log.info("  heights min=%d max=%d" % (min(hs), max(hs)))

        # Fork test: at every height up to each node's tip, do all nodes that have
        # that height agree on its hash? Any disagreement is a definitive fork.
        heights = [n.getblockcount() for n in self.nodes]
        top = min(heights)
        fork = False
        fork_height = -1
        for h in range(1, top + 1):
            hashes = {n.getblockhash(h) for n in self.nodes}
            if len(hashes) > 1:
                fork = True
                fork_height = h
                break
        spread_h = max(heights) - min(heights)
        verdict = "YES" if fork else "no"
        # A stuck/diverged node also shows as a large height spread that never closes.
        print("SKEW_RESULT step=%d max_skew=%d nodes=%d fork=%s fork_height=%d "
              "min_h=%d max_h=%d height_spread=%d"
              % (STEP, spread, N, verdict, fork_height, min(heights), max(heights), spread_h))
        self.log.info("verdict: fork=%s (fork_height=%d) height_spread=%d" % (verdict, fork_height, spread_h))


if __name__ == '__main__':
    PosRoundSkewTest().main()
