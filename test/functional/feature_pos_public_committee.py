#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the public fixed-size committee (-pospubliccommittee, impl spec Option A).

Under threshold VRF sortition the eligible committee is a random variable while
the quorum is fixed, so once the staker pool exceeds the committee target, two
disjoint quorums can certify rival same-height blocks (the honest-splits memo,
section 2). The public fixed-size committee closes this by construction:
membership is the first K = min(pool, cap) entries of the deterministic public
schedule, and the quorum derives from that ACTUAL size (strict majority, plus
one at odd sizes, so any two quorums overlap in at least 2 members).

This test runs the pool-larger-than-cap regime that is impossible to certify
safely under the threshold rule, and asserts the fixed-size property directly:
every certified block carries EXACTLY K countersignatures (the producer holds
all staker keys, so every committee member signs; under threshold sortition
this count would fluctuate block to block), the reported quorum is the actual-K
quorum, and a plain validator accepts the chain. A second scene runs the
below-cap launch regime (the committee is the whole pool, odd-sized, so the
odd-size quorum bump is exercised: 4-of-5).
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


SLOT_INTERVAL = 1


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosPublicCommitteeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True

        # Scene 1 (nodes 0+1): pool 7 > cap 4 — the regime where threshold
        # sortition admits disjoint quorums and the public committee must not.
        # K = 4, quorum 3-of-4.
        self.stakers = [make_staker() for _ in range(7)]
        # Scene 2 (node 2): pool 5 < cap 8 — the launch regime; the committee
        # is the whole pool, K = 5 (odd), quorum 4-of-5 via the odd-size bump.
        self.small_stakers = [make_staker() for _ in range(5)]

        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posbls=1",
            "-pospubliccommittee=1",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-con_max_block_sig_size=8000",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ]
        scene1 = common + ["-poscommitteesize=4"] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        producer = scene1 + ["-posproducer"] + ["-posproducerkey=%s" % wif for wif, _ in self.stakers]
        scene2 = common + ["-poscommitteesize=8"] + ["-staker=%s:1" % pub for _, pub in self.small_stakers]
        producer_small = scene2 + ["-posproducer"] + ["-posproducerkey=%s" % wif for wif, _ in self.small_stakers]
        self.extra_args = [producer, list(scene1), producer_small]

    def setup_network(self):
        self.setup_nodes()
        # Scene 1 is nodes 0 <-> 1; scene 2 (node 2) runs its own single-node
        # chain with different -staker args and must stay unconnected.
        self.connect_nodes(0, 1)

    def run_test(self):
        producer, validator, small = self.nodes[0], self.nodes[1], self.nodes[2]

        self.log.info("Scene 1: pool 7 > cap 4 — fixed-size committee, quorum 3-of-4")
        self.wait_until(lambda: producer.getblockcount() >= 5, timeout=120)
        self.wait_until(lambda: validator.getblockcount() >= 5, timeout=120)
        assert_equal(producer.getblockhash(4), validator.getblockhash(4))

        # The fixed-size property, which threshold sortition cannot give: the
        # producer holds every staker key, so each block is countersigned by
        # EXACTLY the K = 4 public committee members — never more (the cap is
        # enforced), never fewer (all members are local). The reported quorum
        # is the actual-K quorum, 3, and every buried block is certified.
        for h in range(2, 5):
            hdr = validator.getblockheader(validator.getblockhash(h))
            assert_equal(hdr["poscountersigs"], 4)
            assert_equal(hdr["posquorum"], 3)
            assert_equal(hdr["poscertified"], True)

        # Liveness continues and the validator follows.
        target = producer.getblockcount() + 3
        self.wait_until(lambda: validator.getblockcount() >= target, timeout=120)
        assert_equal(producer.getblockhash(target), validator.getblockhash(target))

        self.log.info("Scene 2: pool 5 < cap 8 — whole-pool committee, odd-size quorum 4-of-5")
        self.wait_until(lambda: small.getblockcount() >= 4, timeout=120)
        for h in range(2, 4):
            hdr = small.getblockheader(small.getblockhash(h))
            assert_equal(hdr["poscountersigs"], 5)  # the committee is everyone
            assert_equal(hdr["posquorum"], 4)       # odd-size bump: 4-of-5, overlap 3
            assert_equal(hdr["poscertified"], True)

        self.log.info("Public committee: fixed size, actual-K quorum, validator consensus — OK")


if __name__ == '__main__':
    PosPublicCommitteeTest().main()
