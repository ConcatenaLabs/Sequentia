#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the per-chain maximum block weight (-con_maxblockweight).

Sequentia caps block weight at 400,000 (a tenth of Bitcoin's, for ~1-minute
blocks; whitepaper §3.10). Here we set a small cap and verify the miner
respects it: with more mempool transactions than fit, a generated block stays
within the cap and leaves the rest in the mempool.
"""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_greater_than

MAX_WEIGHT = 40000


class MaxBlockWeightTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-con_maxblockweight=%d" % MAX_WEIGHT,
            "-con_blocksubsidy=5000000000",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        # Fund the (auto-created descriptor) wallet via block subsidies.
        self.generatetoaddress(node, COINBASE_MATURITY + 20, node.getnewaddress())

        # Queue many transactions — more than fit in one MAX_WEIGHT block.
        addr = node.getnewaddress()
        for _ in range(120):
            node.sendtoaddress(addr, 1.0)
        mempool_before = len(node.getrawmempool())
        assert_greater_than(mempool_before, 80)

        # Mine one block: it must respect the cap, so it fills up to the cap and
        # leaves the rest in the mempool (proving the per-chain limit bit).
        blockhash = self.generatetoaddress(node, 1, node.getnewaddress())[0]
        weight = node.getblock(blockhash)['weight']
        mempool_after = len(node.getrawmempool())
        self.log.info("block weight=%d cap=%d mempool %d -> %d",
                      weight, MAX_WEIGHT, mempool_before, mempool_after)
        assert weight <= MAX_WEIGHT, (weight, MAX_WEIGHT)
        assert_greater_than(mempool_before, mempool_after)   # some were mined
        assert_greater_than(mempool_after, 0)                # but not all (cap bit)


if __name__ == '__main__':
    MaxBlockWeightTest().main()
