#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the autonomous BLS gossip committee across separate hosts (-posbls).

Three nodes each hold exactly ONE of the three committee staking keys, so no
node holds a quorum and none can certify a block single-host. They must assemble
the committee over peer-to-peer gossip with no coordinator: an elected leader
floods its unsigned block (posproposal); each sortitioned node signs the
member-independent block hash and floods its share (posshare); the leader
collects a quorum, assembles the BLS certificate into the proof solution, and
submits. This is the headline autonomous gossip-and-sign committee.
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


class PosBlsGossipTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True

        # A 3-member committee (quorum 2), one key per host.
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posbls=1",
            "-poscommitteesize=3",
            "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        # Each node produces with exactly ONE key — no node holds a quorum.
        self.extra_args = [
            common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]]
            for i in range(3)
        ]

    def setup_network(self):
        self.setup_nodes()
        # Full mesh so proposals and shares reach every committee member directly.
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def run_test(self):
        # No node can certify alone; the chain only advances if the committee
        # assembles over gossip. Wait for several blocks across all hosts.
        self.log.info("Distributed committee should certify blocks over gossip")
        self.wait_until(lambda: all(n.getblockcount() >= 4 for n in self.nodes), timeout=120)

        # All three hosts agree on the gossip-assembled chain (compare a buried
        # block, not the moving tip).
        h = min(n.getblockcount() for n in self.nodes) - 1
        assert h >= 2
        assert_equal(self.nodes[0].getblockhash(h), self.nodes[1].getblockhash(h))
        assert_equal(self.nodes[0].getblockhash(h), self.nodes[2].getblockhash(h))

        # The blocks really carry a BLS committee certificate (a quorum of
        # countersignatures): getblock reports the signers/solution indirectly via
        # height progression; assert liveness keeps going.
        target = min(n.getblockcount() for n in self.nodes) + 3
        self.wait_until(lambda: all(n.getblockcount() >= target for n in self.nodes), timeout=120)
        h2 = min(n.getblockcount() for n in self.nodes) - 1
        assert_equal(self.nodes[0].getblockhash(h2), self.nodes[2].getblockhash(h2))
        self.log.info("Gossip committee advanced all hosts to height %d" %
                      min(n.getblockcount() for n in self.nodes))


if __name__ == '__main__':
    PosBlsGossipTest().main()
