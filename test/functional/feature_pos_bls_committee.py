#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BLS aggregate-committee certification (-posbls).

A block is certified by a single non-interactive BLS12-381 aggregate signature
over the committee's per-member shares (each member's BLS key derived from its
staking key, with the BLS pubkey + proof-of-possession committed in the
coinbase). Here one host holds all three committee keys and the autonomous
producer assembles BLS-certified blocks; a plain validator node must accept and
sync them, exercising the consensus path (CheckProof + CheckPosStakeRules) for
the BLS form on a node that did not produce the blocks.
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


class PosBlsCommitteeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        # A 3-member committee (quorum 2), all keys on the producing host.
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posbls=1",
            "-poscommitteesize=3",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-con_max_block_sig_size=200",  # BLS solution: ~73B leader DER + 96B aggregate
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]

        producer = common + ["-posproducer"] + ["-posproducerkey=%s" % wif for wif, _ in self.stakers]
        self.extra_args = [producer, list(common)]

    def run_test(self):
        producer, validator = self.nodes[0], self.nodes[1]

        # The producer autonomously certifies blocks with a BLS aggregate — no
        # RPC drives it — and a plain node validates and syncs them.
        self.log.info("Producer should create BLS-certified blocks unaided")
        self.wait_until(lambda: producer.getblockcount() >= 4, timeout=60)
        height = producer.getblockcount()
        assert_greater_than(height, 3)

        self.sync_blocks(timeout=60)
        assert_equal(validator.getblockcount(), producer.getblockcount())
        assert_equal(validator.getbestblockhash(), producer.getbestblockhash())

        # Liveness: the BLS committee keeps the chain advancing.
        self.wait_until(lambda: producer.getblockcount() >= height + 3, timeout=60)
        self.sync_blocks(timeout=60)
        assert_equal(validator.getblockcount(), producer.getblockcount())
        self.log.info("BLS committee advanced the chain to height %d" % producer.getblockcount())


if __name__ == '__main__':
    PosBlsCommitteeTest().main()
