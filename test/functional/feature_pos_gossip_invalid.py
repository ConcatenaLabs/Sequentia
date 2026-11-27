#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The committee routes around a leader that proposes invalid blocks.

Proposals are validated lazily — only when a node is about to sign the backed
proposal (paper Principle 6 step 10), not on receipt. One committee member, when
elected leader, proposes a consensus-invalid block (a second coinbase, with the
block's commitments made again so that its body matches its header) and serves
it to members that ask for its body (fault injection via -posbyzantineinvalid).
Honest nodes record it cheaply, but at sign time validation rejects it
(bad-cb-multiple) and each honest member excludes that leader, and the height is
certified on another leader's block.
"""

import re

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

    def log_lines(self, i):
        return open("%s/%s/debug.log" % (self.nodes[i].datadir, self.chain), encoding="utf8").read().splitlines()

    def run_test(self):
        honest = [self.nodes[0], self.nodes[1]]

        self.log.info("Each honest member excludes the leader of an invalid block when it is about to sign it")
        pattern = re.compile(r"proposal ([0-9a-f]{64}) at height (\d+) is invalid \(bad-cb-multiple\); excluding its leader")
        bad = re.compile(r"BYZANTINE proposing invalid block ([0-9a-f]{64}) at height (\d+)")

        def exclusions(i):
            return {(m.group(1), int(m.group(2))) for line in self.log_lines(i) for m in [pattern.search(line)] if m}

        def proposed_bad():
            return {(m.group(1), int(m.group(2))) for line in self.log_lines(2) for m in [bad.search(line)] if m}
        self.wait_until(lambda: exclusions(0) & exclusions(1) & proposed_bad(), timeout=600)
        (block_hash, height), = sorted(exclusions(0) & exclusions(1) & proposed_bad())[:1]
        self.log.info("node2's invalid block %s at height %d: excluded by both honest members", block_hash, height)

        # That height is certified on another leader's block, the same on both honest nodes.
        self.wait_until(lambda: all(n.getblockcount() >= height + 1 for n in honest), timeout=240)
        certified = honest[0].getblockhash(height)
        assert_equal(certified, honest[1].getblockhash(height))
        assert certified != block_hash
        assert not any("proposing block %s at height %d" % (certified, height) in line for line in self.log_lines(2))
        self.log.info("height %d certified on %s, another leader's block", height, certified)

        # And the chain keeps advancing.
        target = min(n.getblockcount() for n in honest) + 4
        self.wait_until(lambda: all(n.getblockcount() >= target for n in honest), timeout=240)
        h2 = min(n.getblockcount() for n in honest) - 1
        assert_equal(honest[0].getblockhash(h2), honest[1].getblockhash(h2))


if __name__ == '__main__':
    PosGossipInvalidTest().main()
