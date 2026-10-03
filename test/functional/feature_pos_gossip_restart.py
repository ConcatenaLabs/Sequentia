#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Honest re-proposals after a failed collection are not equivocation.

When every candidate at a height has been backed for a round without reaching a
quorum, the committee abandons the collection and every member builds and
proposes a fresh block at the same height — a new timestamp, so a new hash. A
member whose own restart is a poll behind must not read that as the leader's
second block: excluding the honest leaders in turn would leave nobody to back,
and the height would never be certified, even once the quorum is back.

Four equal members (quorum three); two are cut off, so the remaining two cycle
through collections that cannot certify, re-proposing each time. Neither may
convict the other of equivocation, and once the four are reconnected the chain
must move on.
"""

import re

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosGossipRestartTest(BitcoinTestFramework):
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
        # Whole-second window and rounds: the schedule then runs out exactly on
        # a second boundary, so the first member to restart re-proposes at
        # once. node1's round clock trails by 200 ms, so node0's re-proposal
        # always reaches it while it is still inside the abandoned collection —
        # the case that used to convict honest leaders.
        common += ["-poswindowms=1000", "-posroundms=1000"]
        self.extra_args = [
            common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]]
            for i in range(4)
        ]
        self.extra_args[1].append("-posdebugroundskewms=-200")

    def setup_network(self):
        self.setup_nodes()
        self.connect_all()

    def connect_all(self):
        for a in range(4):
            for b in range(a + 1, 4):
                self.connect_nodes(a, b)

    def restarts_at(self, node, height):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return f.read().count("no certificate at height %d after" % height)

    def run_test(self):
        pair = [self.nodes[0], self.nodes[1]]

        self.log.info("Full committee certifies a few blocks")
        self.wait_until(lambda: all(n.getblockcount() >= 3 for n in self.nodes), timeout=120)

        self.log.info("Cutting off two members: the other two cannot reach the quorum of three")
        for a in range(4):
            for b in range(a + 1, 4):
                if {a, b} != {0, 1}:
                    self.disconnect_nodes(a, b)
        # A block certified as the links went down may still be arriving.
        self.sync_blocks(pair)
        height = pair[0].getblockcount() + 1

        self.log.info("The pair restarts collection at height %d several times" % height)
        self.wait_until(lambda: all(self.restarts_at(n, height) >= 3 for n in pair), timeout=120)
        assert all(n.getblockcount() == height - 1 for n in pair)

        self.log.info("Neither member convicted the other of equivocation")
        for i, n in enumerate(pair):
            with open(n.debug_log_path, encoding='utf-8') as f:
                convicted = set(re.findall(r"PoS gossip: leader ([0-9a-f]{16}) equivocated", f.read()))
            assert_equal((i, convicted), (i, set()))

        self.log.info("Reconnected, the committee certifies past the stalled height")
        self.connect_all()
        self.wait_until(lambda: all(n.getblockcount() >= height + 2 for n in self.nodes), timeout=120)
        h = min(n.getblockcount() for n in self.nodes) - 1
        assert all(n.getblockhash(h) == self.nodes[0].getblockhash(h) for n in self.nodes)


if __name__ == '__main__':
    PosGossipRestartTest().main()
