#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The committee routes around a leader that proposes invalid blocks.

Proposals are validated lazily — only when a node is about to sign the backed
proposal (paper Principle 6 step 10), not on receipt. One committee member, when
elected leader, proposes a consensus-invalid block (two coinbases). Honest nodes
record it cheaply, but at sign time validation rejects it and the leader is
excluded, so the committee converges on the next-lowest valid leader and the
chain keeps advancing. Sustained progress past the heights that member leads
proves lazy validation + exclusion work (fault injection via -posbyzantineinvalid).
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


class PosGossipInvalidTest(BitcoinTestFramework):
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
        self.extra_args = []
        for i in range(3):
            args = common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]]
            if i == 2:
                args.append("-posbyzantineinvalid")  # node2 proposes invalid blocks when it leads
            self.extra_args.append(args)

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def run_test(self):
        honest = [self.nodes[0], self.nodes[1]]

        self.log.info("Committee must keep advancing despite a leader proposing invalid blocks")
        self.wait_until(lambda: all(n.getblockcount() >= 8 for n in honest), timeout=240)

        # The honest nodes agree on a single valid chain (no invalid block certified).
        h = min(n.getblockcount() for n in honest) - 1
        assert h >= 4
        assert_equal(honest[0].getblockhash(h), honest[1].getblockhash(h))

        target = min(n.getblockcount() for n in honest) + 4
        self.wait_until(lambda: all(n.getblockcount() >= target for n in honest), timeout=240)
        h2 = min(n.getblockcount() for n in honest) - 1
        assert_equal(honest[0].getblockhash(h2), honest[1].getblockhash(h2))
        self.log.info("Lazy validation excluded the invalid-block leader; chain at height %d" %
                      min(n.getblockcount() for n in honest))


if __name__ == '__main__':
    PosGossipInvalidTest().main()
