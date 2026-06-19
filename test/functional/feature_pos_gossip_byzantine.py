#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The autonomous committee tolerates a Byzantine (equivocating) leader.

One of three committee members is Byzantine: whenever it is elected leader it
equivocates — proposing two different valid blocks to disjoint halves of the
committee — and contributes no signature shares, so its round cannot reach a
quorum. A naive design would stall at every height that member leads. The
committee's deterministic round-robin must instead exclude the failed leader and
converge on the next-lowest-VRF leader, so the chain keeps advancing. Across
many heights the Byzantine member is elected repeatedly, so sustained progress
demonstrates the round-robin works (fault injection via -posbyzantineequivocate).
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


class PosGossipByzantineTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True

        # Equal weights so all three are always committee members (quorum 2); the
        # two honest members are themselves a quorum, so the Byzantine member can
        # never block progress by withholding — only by leading, which the
        # round-robin must route around.
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=3",
            "-posslotinterval=1", "-con_max_block_sig_size=4000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = []
        for i in range(3):
            args = common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]]
            if i == 2:
                args.append("-posbyzantineequivocate")  # node2 is the Byzantine leader
            self.extra_args.append(args)

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def run_test(self):
        honest = [self.nodes[0], self.nodes[1]]

        # Advance well past the point where the Byzantine member must have led a
        # few times; sustained progress proves the round-robin excludes it.
        self.log.info("Committee must keep advancing despite a Byzantine equivocating leader")
        self.wait_until(lambda: all(n.getblockcount() >= 8 for n in honest), timeout=240)

        # The honest nodes agree on a single chain (no fork) — compare a buried block.
        h = min(n.getblockcount() for n in honest) - 1
        assert h >= 4
        assert_equal(honest[0].getblockhash(h), honest[1].getblockhash(h))

        # Keep going to confirm it is sustained, not a lucky early streak.
        target = min(n.getblockcount() for n in honest) + 4
        self.wait_until(lambda: all(n.getblockcount() >= target for n in honest), timeout=240)
        h2 = min(n.getblockcount() for n in honest) - 1
        assert_equal(honest[0].getblockhash(h2), honest[1].getblockhash(h2))
        self.log.info("Round-robin routed around the Byzantine leader to height %d" %
                      min(n.getblockcount() for n in honest))


if __name__ == '__main__':
    PosGossipByzantineTest().main()
