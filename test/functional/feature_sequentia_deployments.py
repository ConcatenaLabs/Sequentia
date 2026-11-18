#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""getdeploymentinfo reports Sequentia's height-gated rules.

A wallet decides which swap against on-chain bitcoin to build from this report
(doc/sequentia/bitcoin-payment-proof.md, section 7): the payment lock where
"btc_payment" is active for the next block, today's swap where it is not or is
absent. The rule for the transactions of a lost block ("lost_blocks") is shown
on Proof-of-Stake chains; feature_pos_lost_blocks.py case "dep" checks it on a
node that watches Bitcoin.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

GATE = 20


class SequentiaDeploymentsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [[], ["-con_btc_payment_height=%d" % GATE]]

    def setup_network(self):
        self.setup_nodes()

    def dep(self, node, blockhash=None):
        args = [blockhash] if blockhash else []
        return node.getdeploymentinfo(*args)["deployments"]

    def run_test(self):
        fresh, gated = self.nodes
        self.log.info("A fresh chain has the payment lock from genesis")
        assert_equal(self.dep(fresh)["btc_payment"], {"type": "buried", "active": True, "height": 0})
        assert "lost_blocks" not in self.dep(fresh)

        self.log.info("A chain that names a height has it from that height on")
        assert_equal(self.dep(gated)["btc_payment"], {"type": "buried", "active": False, "height": GATE})
        addr = gated.getnewaddress() if gated.listwallets() else None
        if addr is None:
            gated.createwallet("w")
            addr = gated.getnewaddress()
        hashes = self.generatetoaddress(gated, GATE - 2, addr, sync_fun=self.no_op)
        # The tip is at GATE - 2: the next block is below the gate.
        assert_equal(self.dep(gated)["btc_payment"]["active"], False)
        hashes += self.generatetoaddress(gated, 1, addr, sync_fun=self.no_op)
        # The tip is at GATE - 1: the next block is the first under the rule.
        assert_equal(self.dep(gated)["btc_payment"], {"type": "buried", "active": True, "height": GATE})
        # Asked at an earlier block, it answers for the block after that one.
        assert_equal(self.dep(gated, hashes[-2])["btc_payment"]["active"], False)
        # getblockchaininfo carries the same report.
        info = gated.getblockchaininfo()
        if "softforks" in info:
            assert_equal(info["softforks"]["btc_payment"]["active"], True)


if __name__ == '__main__':
    SequentiaDeploymentsTest().main()
