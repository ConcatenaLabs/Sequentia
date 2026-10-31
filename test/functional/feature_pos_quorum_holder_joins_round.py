#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A host that holds a committee quorum by itself still lets others lead.

Under the public committee, seats are apportioned by stake. A single large
staker can therefore hold a quorum of seats on its own host. Such a host used to
certify its own block at its own slot and never take part in the gossip round,
so it never countersigned anyone else's proposal: every other staker's block
stayed short of a quorum for good, and the large staker produced every block
whatever the election said.

Scene: node0 holds one staker of weight 9; node1 holds three stakers of weight
1 each. With a cap of 4 seats, node0's quota is exactly 3 seats, the quorum of
4, at every height. The exponential-race election gives node1's keys a quarter
of the blocks. The test lets the chain run and asserts that node1's keys lead a
real share of them, which needs node0 to countersign node1's proposals.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


BLOCKS = 48


class PosQuorumHolderJoinsRoundTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.big = make_staker()
        self.small = [make_staker() for _ in range(3)]
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=4", "-posexpraceheight=1",
            "-posslotinterval=1",
            "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        # Idle first (no BLS registrations, no producer) to derive registrations.
        idle = self.common + ["-staker=%s:9" % self.big[1]] + ["-staker=%s:1" % p for _, p in self.small]
        self.extra_args = [idle, list(idle)]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)

    def run_test(self):
        n0 = self.nodes[0]
        specs = ["-staker=%s:9%s" % (self.big[1], n0.getblsregistration(self.big[0])["spec"])]
        specs += ["-staker=%s:1%s" % (p, n0.getblsregistration(w)["spec"]) for w, p in self.small]
        full = self.common + specs

        self.restart_node(0, extra_args=full + ["-posproducer", "-posproducerkey=%s" % self.big[0]])
        self.restart_node(1, extra_args=full + ["-posproducer"] + ["-posproducerkey=%s" % w for w, _ in self.small])
        self.connect_nodes(0, 1)

        self.log.info("node0 alone holds the quorum: 3 of 4 seats, quorum 3")
        self.wait_until(lambda: n0.getblockcount() >= 2, timeout=120)
        hdr = n0.getblockheader(n0.getblockhash(2))
        assert_equal(hdr["posquorum"], 3)

        self.log.info("Letting the chain run %d blocks" % BLOCKS)
        self.wait_until(lambda: min(n.getblockcount() for n in self.nodes) >= BLOCKS + 1, timeout=600)
        self.sync_blocks()

        recent = n0.getposrecentblocks(BLOCKS)["blocks"]
        assert_equal(len(recent), BLOCKS)
        small_pubs = {p for _, p in self.small}
        by_small = sum(1 for b in recent if b["producer"] in small_pubs)
        by_big = sum(1 for b in recent if b["producer"] == self.big[1])
        self.log.info("Of the last %d blocks: node0's key led %d, node1's keys led %d" % (BLOCKS, by_big, by_small))
        assert_equal(by_big + by_small, BLOCKS)
        # A fair election gives node1 about a quarter (12 of 48). Before the fix
        # it got none at all. At 25%, fewer than 3 of 48 has odds below 1 in 4,000.
        assert_greater_than(by_small, 2)
        # node0 countersigned those blocks: they certify with node0's seats.
        for b in recent:
            if b["producer"] in small_pubs:
                hdr = n0.getblockheader(b["hash"])
                assert hdr["poscertified"]
                assert_greater_than(hdr["poscountersigs"], 2)
                break


if __name__ == '__main__':
    PosQuorumHolderJoinsRoundTest().main()
