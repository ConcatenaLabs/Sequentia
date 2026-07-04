#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the public fixed-size committee + bitfield certificate (-pospubliccommittee).

Impl spec Option A. Under threshold VRF sortition the eligible committee is a
random variable while the quorum is fixed, so once the staker pool exceeds the
committee target, two disjoint quorums can certify rival same-height blocks
(the honest-splits memo, section 2). The public fixed-size committee closes this
by construction: membership is the first K = min(#registered stakers, cap)
entries of the deterministic public schedule, the quorum derives from that
ACTUAL size (strict majority, plus one at odd sizes), and the certificate names
its signers by a BITFIELD over the committee order (phase 2), collapsing the
~257-byte-per-member certificate to the leader signature, one aggregate, and one
bit per seat.

Committee members must register the BLS key they sign with (the validator names
signers by bitfield and looks the keys up); the -staker config carries it, and
getblsregistration derives it from the staking key. This test derives the
registrations from idle nodes, restarts with the full config, then asserts:
pool > cap runs a fixed K=4 committee at quorum 3-of-4 with EXACTLY 4
countersigs and a plain validator follows; the certified block is small (the
bitfield certificate, not the ~1.2 KB full-member form); and a pool < cap
launch runs the whole pool at the odd-size quorum 4-of-5.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than
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
        self.stakers = [make_staker() for _ in range(7)]
        # Scene 2 (node 2): pool 5 < cap 8 — the launch regime; committee is the
        # whole pool, K = 5 (odd), quorum 4-of-5 via the odd-size bump.
        self.small_stakers = [make_staker() for _ in range(5)]

        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        # Initial args: staker weights but NO BLS registrations and NO producer,
        # so the nodes idle (no committee forms) while we derive registrations by
        # RPC; then we restart with the full config.
        s1 = self.common + ["-poscommitteesize=4"] + ["-staker=%s:1" % p for _, p in self.stakers]
        s2 = self.common + ["-poscommitteesize=8"] + ["-staker=%s:1" % p for _, p in self.small_stakers]
        self.extra_args = [s1, list(s1), s2]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)

    def staker_specs(self, node, stakers):
        # -staker=<pub>:<weight><:blspub:pop>, the BLS registration derived by
        # getblsregistration from each staking key.
        specs = []
        for wif, pub in stakers:
            reg = node.getblsregistration(wif)["spec"]
            specs.append("-staker=%s:1%s" % (pub, reg))
        return specs

    def run_test(self):
        # Derive the committee BLS registrations (pure key derivation on the idle
        # nodes) and restart every node with the full config + producers.
        self.log.info("Deriving committee BLS registrations and restarting with the full config")
        s1_specs = self.staker_specs(self.nodes[0], self.stakers)
        s2_specs = self.staker_specs(self.nodes[2], self.small_stakers)

        scene1 = self.common + ["-poscommitteesize=4"] + s1_specs
        producer1 = scene1 + ["-posproducer"] + ["-posproducerkey=%s" % w for w, _ in self.stakers]
        scene2 = self.common + ["-poscommitteesize=8"] + s2_specs
        producer2 = scene2 + ["-posproducer"] + ["-posproducerkey=%s" % w for w, _ in self.small_stakers]

        self.restart_node(0, extra_args=producer1)
        self.restart_node(1, extra_args=scene1)
        self.restart_node(2, extra_args=producer2)
        self.connect_nodes(0, 1)

        producer, validator, small = self.nodes[0], self.nodes[1], self.nodes[2]

        self.log.info("Scene 1: pool 7 > cap 4 — fixed-size committee, quorum 3-of-4")
        self.wait_until(lambda: producer.getblockcount() >= 5, timeout=120)
        self.wait_until(lambda: validator.getblockcount() >= 5, timeout=120)
        assert_equal(producer.getblockhash(4), validator.getblockhash(4))

        # The fixed-size property, which threshold sortition cannot give: the
        # producer holds every staker key, so each block is countersigned by
        # EXACTLY the K = 4 public committee members — never more (the cap is
        # enforced), never fewer (all members are local). Quorum is 3-of-4.
        for h in range(2, 5):
            hdr = validator.getblockheader(validator.getblockhash(h))
            assert_equal(hdr["poscountersigs"], 4)
            assert_equal(hdr["posquorum"], 3)
            assert_equal(hdr["poscertified"], True)

        # The bitfield certificate is small: a full-member certificate for K=4
        # would carry ~257 bytes per member (~1 KB) plus the block; the bitfield
        # form is the leader signature, one 96-byte aggregate, and ~1 byte of
        # bitfield. The whole serialized block stays well under what the
        # full-member certificate alone would occupy.
        raw = producer.getblock(producer.getblockhash(4), 0)
        block_bytes = len(raw) // 2
        assert_greater_than(900, block_bytes)  # a full-member K=4 cert alone is > 1 KB
        self.log.info("Certified block is %d bytes (bitfield certificate)" % block_bytes)

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

        self.log.info("Public committee + bitfield certificate: fixed size, actual-K quorum, small cert, consensus — OK")


if __name__ == '__main__':
    PosPublicCommitteeTest().main()
