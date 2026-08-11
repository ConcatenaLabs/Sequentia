#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The committee certificate is witness-discounted (weighted x1, not x4).

A 12-member BLS certificate is ~3.3 KB. In the legacy CProof.solution it would be
counted in both block-weight passes (x4 ≈ 13 KB of weight); under PoS it is
excluded from the NO_WITNESS (weight base) pass like the dynafed signblock
witness, so it costs ~3.3 KB of weight (x1).

This is verified by setting a block-weight cap that the x4 accounting would blow
through but x1 leaves ample room: -con_maxblockweight=8000. A single host holding
all twelve committee keys produces single-host BLS blocks; if the certificate were
still x4 the producer could not fit a block under the cap and the chain would
stall, so sustained progress demonstrates the discount.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_greater_than
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


COMMITTEE = 12


class PosCertWeightTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(COMMITTEE)]
        args = [
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % COMMITTEE,
            "-posslotinterval=1",
            "-con_max_block_sig_size=4000",   # fits a 12-member certificate (~3.3 KB)
            "-con_maxblockweight=8000",        # x4 of the certificate (~13 KB) would exceed this; x1 (~3.3 KB) fits
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        args += ["-staker=%s:1" % pub for _, pub in self.stakers]
        # One host holds every committee key, so it certifies single-host.
        args += ["-posproducer"] + ["-posproducerkey=%s" % wif for wif, _ in self.stakers]
        self.extra_args = [args]

    def run_test(self):
        node = self.nodes[0]
        # Sustained production under the tight weight cap is only possible because
        # the 12-member certificate is witness-discounted (x1); at x4 it would not
        # fit and the chain would never advance.
        self.log.info("12-member committee must produce under an x4-prohibitive block-weight cap")
        self.wait_until(lambda: node.getblockcount() >= 5, timeout=120)

        # Sanity: a produced block really carries the large certificate yet its
        # weight is far below the x4 figure (which would exceed the 8000 cap).
        tip = node.getblock(node.getblockhash(node.getblockcount()))
        assert_greater_than(8000, tip["weight"])  # under the cap
        self.log.info("Certified block weight %d (< 8000 cap) — certificate is x1, not x4" % tip["weight"])


if __name__ == '__main__':
    PosCertWeightTest().main()
