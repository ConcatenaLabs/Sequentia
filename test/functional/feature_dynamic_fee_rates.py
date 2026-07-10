#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the fee-asset exchange-rate whitelist (setfeeexchangerates).

There is a SINGLE whitelist. "Static" versus "dynamic" is only how it is
operated, not a protocol distinction: an operator setting rates by hand persists
them to exchangerates.json (the default), while a price server driving the
whitelist automatically pushes with persist=false so its rates are re-sent each
poll and never outlive a dead feed across a restart. See
doc/sequentia/02-open-fee-market.md.
"""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from decimal import Decimal


class FeeExchangeRatesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            # Fund the (descriptor) wallet via block subsidies
            "-con_blocksubsidy=5000000000",
            "-con_any_asset_fees=1",
            "-defaultpeggedassetname=gasset",
            "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generatetoaddress(node, COINBASE_MATURITY + 1, node.getnewaddress())

        # The policy asset is on the whitelist by default, valued 1:1.
        assert_equal(node.getfeeexchangerates(), {'gasset': 100000000})

        # Issue a test asset.
        self.asset = node.issueasset(Decimal('100'), 1, False)['asset']
        self.generate(node, 1)

        # It is not on the whitelist yet.
        assert self.asset not in node.getfeeexchangerates()
        assert_equal(list(node.getfeeacceptancepolicy().keys()), ['gasset'])

        # --- Automated (price-server-style) push: persist=false --------------
        # Replaces the whole whitelist in memory without touching the config file.
        node.setfeeexchangerates({'gasset': 100000000, self.asset: 50000000}, False)
        assert_equal(node.getfeeexchangerates(),
                     {'gasset': 100000000, self.asset: 50000000})
        assert_equal(node.getfeeacceptancepolicy()[self.asset]['rate'], 50000000)

        # A transaction paying its fee in the whitelisted asset is accepted+mined.
        addr = node.getnewaddress()
        txid = node.sendtoaddress(
            address=addr, amount=1.0,
            assetlabel=self.asset, fee_asset_label=self.asset)
        assert txid in node.getrawmempool()
        assert_equal(node.getmempoolentry(txid)['fees']['asset'], self.asset)
        self.generate(node, 1)
        assert txid not in node.getrawmempool()
        assert_equal(node.gettransaction(txid)['confirmations'], 1)

        # A persist=false push does NOT survive a restart: the whitelist reverts
        # to whatever was last persisted (here, the default policy-asset entry).
        # This is the fail-safe for a dead price server.
        self.restart_node(0)
        assert_equal(node.getfeeexchangerates(), {'gasset': 100000000})

        # --- Operator (static) set: persist defaults to true -----------------
        node.setfeeexchangerates({'gasset': 100000000, self.asset: 75000000})
        assert_equal(node.getfeeexchangerates()[self.asset], 75000000)
        # It survives a restart.
        self.restart_node(0)
        assert_equal(node.getfeeexchangerates()[self.asset], 75000000)

        # Clearing an asset is just a replacing set; passing {} empties the
        # whitelist entirely. Persist it so the drop sticks.
        node.setfeeexchangerates({'gasset': 100000000})
        assert self.asset not in node.getfeeexchangerates()

        # A transaction offering a fee in a non-whitelisted asset is rejected:
        # its value is 0 rfa, so it cannot pay a meaningful fee. Use a freshly
        # issued asset that never receives a rate.
        other = node.issueasset(10, 0, False)['asset']
        self.generate(node, 1)
        assert other not in node.getfeeexchangerates()
        # Re-admit self.asset (in memory) so the wallet can spend it.
        node.setfeeexchangerates({'gasset': 100000000, self.asset: 50000000}, False)
        assert_raises_rpc_error(-6, None,
            node.sendtoaddress,
            address=addr, amount=1.0, assetlabel=self.asset, fee_asset_label=other)

        # SEQ (the policy asset, here 'gasset') is NOT privileged for fees — it
        # is special only for staking. A producer can re-price or refuse it like
        # any asset; the 1:1 valuation is only a default, no longer hardcoded.
        # Refuse it by pinning its rate to 0:
        node.setfeeexchangerates({'gasset': 0})
        assert_raises_rpc_error(-6, None,
            node.sendtoaddress, address=addr, amount=1.0, fee_asset_label='gasset')
        # Make a DIFFERENT asset the 1:1 reference and value SEQ at half — the map
        # now honours the custom policy-asset rate (was forced to 1:1 before).
        node.setfeeexchangerates({self.asset: 100000000, 'gasset': 50000000})
        assert_equal(node.getfeeexchangerates()['gasset'], 50000000)
        # Restore the 1:1 default so the wallet can pay fees in SEQ again.
        node.setfeeexchangerates({'gasset': 100000000})
        assert_equal(node.getfeeexchangerates()['gasset'], 100000000)

        # Invalid rates are rejected.
        assert_raises_rpc_error(-8, "Error loading rates from JSON",
            node.setfeeexchangerates, {"not-an-asset": 100000000})
        # A rate of exactly 0 is ACCEPTED and means "refuse this asset" — the
        # effective map sees scaled_value 0, which the conversion treats as
        # not-accepted (no divide-by-zero). Only negative rates are invalid.
        node.setfeeexchangerates({'gasset': 100000000, self.asset: 0})
        assert_equal(node.getfeeexchangerates()[self.asset], 0)
        # Negative rates remain invalid.
        assert_raises_rpc_error(-8, "must be a non-negative integer",
            node.setfeeexchangerates, {self.asset: -5})


if __name__ == '__main__':
    FeeExchangeRatesTest().main()
