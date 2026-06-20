#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the dynamic fee-asset whitelist (setdynamicfeerates et al.).

The dynamic layer is the node-side interface of the price server sidecar
(contrib/price-server): rates published there admit assets for fee payment,
while statically configured rates (setfeeexchangerates) always take
precedence. See doc/sequentia/02-open-fee-market.md.
"""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from decimal import Decimal

GASSET = 'b2e15d0d7a0c94e4e2ce0fe6e8691b9e451377f6e46e8045a86f7c4b5d4f0f23'


class DynamicFeeRatesTest(BitcoinTestFramework):
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

        assert_equal(node.getfeeexchangerates(), {'gasset': 100000000})

        # Issue a test asset
        self.issue_amount = Decimal('100')
        issuance = node.issueasset(self.issue_amount, 1, False)
        self.asset = issuance['asset']
        self.generate(node, 1)

        # Initially there are no dynamic rates
        assert_equal(node.getdynamicfeerates(), {})
        policy = node.getfeeacceptancepolicy()
        assert_equal(list(policy.keys()), ['gasset'])
        assert_equal(policy['gasset']['origin'], 'static')

        # Publish a dynamic rate for the new asset
        node.setdynamicfeerates({self.asset: 50000000}, "test-price-server")

        # The dynamic layer reports the entry with metadata
        dyn = node.getdynamicfeerates()
        assert_equal(list(dyn.keys()), [self.asset])
        assert_equal(dyn[self.asset]['rate'], 50000000)
        assert_equal(dyn[self.asset]['source'], 'test-price-server')
        assert_equal(dyn[self.asset]['stale'], False)
        assert dyn[self.asset]['updated_at'] > 0

        # The effective whitelist merges static and dynamic layers
        assert_equal(node.getfeeexchangerates(),
                     {'gasset': 100000000, self.asset: 50000000})
        policy = node.getfeeacceptancepolicy()
        assert_equal(policy[self.asset]['origin'], 'dynamic')
        assert_equal(policy[self.asset]['source'], 'test-price-server')
        assert_equal(policy['gasset']['origin'], 'static')

        # A transaction paying its fee in the dynamically admitted asset is
        # accepted and mined
        addr = node.getnewaddress()
        txid = node.sendtoaddress(
            address=addr,
            amount=1.0,
            assetlabel=self.asset,
            fee_asset_label=self.asset)
        assert txid in node.getrawmempool()
        entry = node.getmempoolentry(txid)
        assert_equal(entry['fees']['asset'], self.asset)
        self.generate(node, 1)
        assert txid not in node.getrawmempool()
        assert_equal(node.gettransaction(txid)['confirmations'], 1)

        # Static rates take precedence over dynamic rates for the same asset
        node.setfeeexchangerates({'gasset': 100000000, self.asset: 75000000})
        node.setdynamicfeerates({self.asset: 50000000}, "test-price-server")
        assert_equal(node.getfeeexchangerates()[self.asset], 75000000)
        assert_equal(node.getfeeacceptancepolicy()[self.asset]['origin'], 'static')

        # Removing the static pin re-exposes the dynamic rate
        node.setfeeexchangerates({'gasset': 100000000})
        assert_equal(node.getfeeexchangerates()[self.asset], 50000000)
        assert_equal(node.getfeeacceptancepolicy()[self.asset]['origin'], 'dynamic')

        # Replacing the dynamic layer drops assets not republished
        node.setdynamicfeerates({}, "test-price-server")
        assert_equal(node.getdynamicfeerates(), {})
        assert_equal(node.getfeeexchangerates(), {'gasset': 100000000})

        # cleardynamicfeerates drops everything dynamic
        node.setdynamicfeerates({self.asset: 50000000}, "test-price-server")
        assert self.asset in node.getfeeexchangerates()
        node.cleardynamicfeerates()
        assert_equal(node.getdynamicfeerates(), {})
        assert self.asset not in node.getfeeexchangerates()

        # A transaction offering a fee in a non-whitelisted asset is rejected:
        # its value is 0 rfa, so it cannot pay a meaningful fee. Use a freshly
        # issued asset that never receives a rate.
        other = node.issueasset(10, 0, False)['asset']
        self.generate(node, 1)
        assert other not in node.getfeeexchangerates()
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

        # Invalid dynamic rates are rejected
        assert_raises_rpc_error(-8, "Error parsing dynamic rates",
            node.setdynamicfeerates, {"not-an-asset": 100000000})

        # A dynamic rate of 0 is ACCEPTED and means "refuse this asset" — the
        # same semantics as the static setfeeexchangerates layer (the effective
        # map sees scaled_value 0, which the conversion treats as not-accepted).
        # Only negative rates are rejected. (Harmonized: the dynamic layer used
        # to reject 0; now it matches the static layer.)
        node.setdynamicfeerates({self.asset: 0}, "test-price-server")
        assert_equal(node.getdynamicfeerates()[self.asset]['rate'], 0)
        # The 0 surfaces in the effective whitelist as a refusal (rate 0), exactly
        # like a static 0 — the conversion treats scaled_value <= 0 as not-accepted.
        assert_equal(node.getfeeexchangerates()[self.asset], 0)
        # Negative dynamic rates remain invalid.
        assert_raises_rpc_error(-8, "must be a non-negative integer",
            node.setdynamicfeerates, {self.asset: -5})
        node.cleardynamicfeerates()
        assert self.asset not in node.getfeeexchangerates()


if __name__ == '__main__':
    DynamicFeeRatesTest().main()
