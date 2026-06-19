#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The autonomous BLS gossip committee at a larger scale.

Beyond the 3-member smoke test, exercise a 15-member committee (quorum 8) spread
across three hosts that each hold five committee keys. This stresses the larger
BLS certificate carried in the proof solution (15 members), nodes that emit
several shares per round, and the bigger quorum — over gossip with no
coordinator. It also confirms the committee tolerates a host failure when the
survivors still hold a quorum of keys (10 of 15 > 8).
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


COMMITTEE = 15
PER_NODE = 5  # keys per host -> 3 hosts cover the whole committee


class PosBlsLargeCommitteeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(COMMITTEE)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % COMMITTEE,
            "-posslotinterval=1",
            "-con_max_block_sig_size=8000",  # 15 members * ~257B + leader sig + aggregate
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = []
        for i in range(3):
            keys = self.stakers[i * PER_NODE:(i + 1) * PER_NODE]
            self.extra_args.append(common + ["-posproducer"] +
                                   ["-posproducerkey=%s" % wif for wif, _ in keys])

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def run_test(self):
        # The 15-member committee certifies blocks over gossip.
        self.log.info("15-member committee should certify blocks over gossip")
        self.wait_until(lambda: all(n.getblockcount() >= 4 for n in self.nodes), timeout=120)
        h = min(n.getblockcount() for n in self.nodes) - 1
        assert h >= 2
        assert_equal(self.nodes[0].getblockhash(h), self.nodes[1].getblockhash(h))
        assert_equal(self.nodes[0].getblockhash(h), self.nodes[2].getblockhash(h))

        # The certified block really carries a >=8-member BLS certificate: confirm
        # the proof solution is substantial (15 * ~257B), not a single-signer stub.
        tip_hash = self.nodes[0].getblockhash(h)
        block_hex = self.nodes[0].getblock(tip_hash, 0)
        assert len(block_hex) // 2 > 2000, "certified block unexpectedly small for a 15-member committee"

        # A host fails; the two survivors hold 10 of 15 keys, still a quorum.
        self.log.info("Losing one host (10 of 15 keys remain) must not stall the committee")
        self.stop_node(2)
        survivors = [self.nodes[0], self.nodes[1]]
        target = min(n.getblockcount() for n in survivors) + 3
        self.wait_until(lambda: all(n.getblockcount() >= target for n in survivors), timeout=180)
        h2 = min(n.getblockcount() for n in survivors) - 1
        assert_equal(survivors[0].getblockhash(h2), survivors[1].getblockhash(h2))
        self.log.info("Large committee advanced to height %d (incl. through a host failure)" %
                      min(n.getblockcount() for n in survivors))


if __name__ == '__main__':
    PosBlsLargeCommitteeTest().main()
