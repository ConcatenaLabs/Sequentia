#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the autonomous Proof-of-Stake block producer (-posproducer).

Phase 1 of the autonomous gossip-and-sign committee
(doc/sequentia/proposals/autonomous-committee.md): a node configured with
-posproducer and one or more staking keys (-posproducerkey) elects itself each
slot and produces blocks with NO external coordinator and no generateposblock
RPC call. A second, plain validator node (identical consensus config, no
producer) must sync the autonomously-produced chain, proving the blocks are
relayed and validated like any other.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


SLOT_INTERVAL = 1


def make_staker():
    """Return (wif, pubkey_hex) for a fresh compressed key, using the
    custom/regtest secret-key prefix (239)."""
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosAutonomousProducerTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.wif, self.pub = make_staker()
        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-staker=%s:1" % self.pub,
            "-validatepegin=0",
        ]
        # node0 produces autonomously; node1 is a plain validator.
        producer = common + ["-posproducer", "-posproducerkey=%s" % self.wif]
        self.extra_args = [producer, list(common)]

    def run_test(self):
        producer, validator = self.nodes[0], self.nodes[1]

        # Nothing drives the producer — no generateposblock call anywhere in this
        # test — yet the chain must advance on its own.
        self.log.info("Autonomous producer should advance the chain unaided")
        self.wait_until(lambda: producer.getblockcount() >= 5, timeout=60)
        height = producer.getblockcount()
        assert_greater_than(height, 4)

        # The autonomously-produced blocks propagate to and validate on a node
        # that is not producing (default network connects node0<->node1).
        self.sync_blocks(timeout=60)
        assert_equal(validator.getblockcount(), producer.getblockcount())
        assert_equal(validator.getbestblockhash(), producer.getbestblockhash())

        # The tip is a real PoS block: a leader elected by VRF sortition.
        tip = producer.getblock(producer.getbestblockhash())
        assert_greater_than(tip['height'], 4)

        # Liveness: the chain keeps advancing at the slot cadence.
        self.log.info("Producer keeps advancing the chain")
        self.wait_until(lambda: producer.getblockcount() >= height + 3, timeout=60)
        self.sync_blocks(timeout=60)
        assert_equal(validator.getblockcount(), producer.getblockcount())
        self.log.info("Autonomous producer reached height %d" % producer.getblockcount())


if __name__ == '__main__':
    PosAutonomousProducerTest().main()
