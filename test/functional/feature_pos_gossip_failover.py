#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Liveness of the autonomous committee under a crashed member.

A three-member committee (quorum two) must keep certifying blocks after one of
the three hosts goes offline — including when the departed host would have been
the round's leader. The two survivors re-converge on an available leader (the
collection-window convergence, plus the round-recovery reset when a leader
departs after proposing) and continue with no coordinator.
"""

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


class PosGossipFailoverTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=3",
            "-posslotinterval=1", "-con_max_block_sig_size=4000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [
            common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]]
            for i in range(3)
        ]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def run_test(self):
        survivors = [self.nodes[0], self.nodes[1]]

        # The full committee gets going.
        self.log.info("Full committee certifies a few blocks")
        self.wait_until(lambda: all(n.getblockcount() >= 3 for n in self.nodes), timeout=120)

        # One member crashes. The remaining two are still a quorum (2 of 3) and
        # must keep the chain advancing on their own.
        self.log.info("Crashing one committee member; the two survivors must continue")
        self.stop_node(2)

        start = min(n.getblockcount() for n in survivors)
        target = start + 4
        self.wait_until(lambda: all(n.getblockcount() >= target for n in survivors), timeout=180)

        # The survivors agree on the post-failure chain (buried block).
        h = min(n.getblockcount() for n in survivors) - 1
        assert_equal(survivors[0].getblockhash(h), survivors[1].getblockhash(h))
        self.log.info("Committee survived a member crash; chain at height %d" %
                      min(n.getblockcount() for n in survivors))


if __name__ == '__main__':
    PosGossipFailoverTest().main()
