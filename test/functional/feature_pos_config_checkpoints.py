#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests operator-configured static PoS checkpoints (-poscheckpoint).

Unlike the dynamic parent-chain SEQCKPT checkpoints (which only finalize history
a node already validated), a configured checkpoint is supplied up front and so
protects a *fresh* sync against a long-range alternate history: a block
presented at the pinned height must carry the pinned hash, otherwise it — and
any branch built on it — is rejected. The mechanism is reject-only and needs no
block download. See doc/sequentia/04-proof-of-stake.md §11.

Covered:
 - a configured checkpoint with the WRONG hash makes a fresh node reject the
   block at that height and stall below it
 - reconfiguring with the CORRECT hash lets the same node finish syncing
 - getcheckpointinfo surfaces the configured checkpoints
 - malformed -poscheckpoint values are rejected at startup
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosConfigCheckpointTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.wif, self.pub = make_staker()
        self.common = [
            "-con_pos=1",
            "-posslotinterval=1",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-staker=%s:1" % self.pub,
            "-validatepegin=0",
        ]
        self.extra_args = [list(self.common), list(self.common)]

    def setup_network(self):
        # Start the nodes but leave them unconnected, so node1 stays at genesis
        # until we deliberately connect it after configuring a checkpoint.
        self.setup_nodes()

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]

        # node0 builds a chain of 5 blocks on its own.
        for _ in range(5):
            n0.generateposblock(self.wif)
        assert_equal(n0.getblockcount(), 5)
        good3 = n0.getblockhash(3)
        # A plausible-but-wrong hash for height 3 (flip the last nibble).
        bad3 = good3[:-1] + ('0' if good3[-1] != '0' else '1')

        # --- Rejection: a fresh node pinned to the wrong block at height 3
        # rejects that block (bad-pos-checkpoint) and so cannot cross the
        # checkpoint height; with only the lying peer to learn from, it stays
        # below height 3. ---
        self.restart_node(1, extra_args=self.common + ["-poscheckpoint=3:%s" % bad3])
        n1 = self.nodes[1]
        info = n1.getcheckpointinfo()
        assert_equal(info['configured'], [{'height': 3, 'blockhash': bad3}])

        self.connect_nodes(0, 1)
        # The bad header is rejected almost immediately; give it a moment, then
        # confirm node1 has not crossed the checkpoint.
        time.sleep(3)
        assert_equal(n0.getblockcount(), 5)
        assert n1.getblockcount() < 3, n1.getblockcount()

        # --- Acceptance: reconfigure the same node with the CORRECT hash; it now
        # syncs the whole chain to the tip. ---
        self.restart_node(1, extra_args=self.common + ["-poscheckpoint=3:%s" % good3])
        n1 = self.nodes[1]
        assert_equal(n1.getcheckpointinfo()['configured'],
                     [{'height': 3, 'blockhash': good3}])
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(n1.getblockcount(), 5)
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())
        # The checkpointed height now carries exactly the pinned block.
        assert_equal(n1.getblockhash(3), good3)

        # --- Startup validation: malformed -poscheckpoint values are refused. ---
        self.stop_node(1)
        n1.assert_start_raises_init_error(
            self.common + ["-poscheckpoint=notanumber:%s" % good3],
            "Invalid -poscheckpoint", match=ErrorMatch.PARTIAL_REGEX)
        n1.assert_start_raises_init_error(
            self.common + ["-poscheckpoint=3:deadbeef"],
            "block hash must be 64 hex chars", match=ErrorMatch.PARTIAL_REGEX)
        # Restart so the framework's shutdown bookkeeping is happy.
        self.start_node(1, extra_args=self.common)


if __name__ == '__main__':
    PosConfigCheckpointTest().main()
