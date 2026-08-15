#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests getfeeassetinfo and estimatesmartfee's fee_asset argument.

These are the queries a wallet asks before offering an asset as a way to pay a
fee. What is pinned down here is that they answer about the FEE WHITELIST — the
thing the mempool actually consults — and that they keep "this node refuses it"
apart from "the registry does not publish it" and "nothing quotes a price for
it", since only the first stops a transaction from leaving the wallet at all.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

GASSET = 'b2e15d0d7a0c94e4e2ce0fe6e8691b9e451377f6e46e8045a86f7c4b5d4f0f23'


class AnyAssetFeeInfoTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
            "-defaultpeggedassetname=gasset",
            "-anyonecanspendaremine=1",
            "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def init(self):
        self.node = self.nodes[0]
        self.generate(self.node, COINBASE_MATURITY + 1)

        issuance = self.node.issueasset(
            assetamount=Decimal('100'), tokenamount=1, blind=False, fee_asset='gasset')
        self.asset = issuance['asset']
        self.token = issuance['token']
        self.node.generatetoaddress(1, self.node.getnewaddress(), invalid_call=False)

    def test_bootstrap_state(self):
        """Out of the box only the policy asset is whitelisted, and the RPC says so
        for it while denying every other asset."""
        info = self.node.getfeeassetinfo()
        assert GASSET in info
        assert_equal(info[GASSET]['listed'], True)
        assert_equal(info[GASSET]['accepted'], True)
        assert_equal(info[GASSET]['rate'], 100000000)
        assert_equal(info[GASSET]['label'], 'gasset')

        # The issued asset exists on chain but nothing has priced it. It carries
        # no label either, so it is absent from the unfiltered listing and has to
        # be asked about by id — which is itself the answer a wallet needs.
        assert self.asset not in info
        single = self.node.getfeeassetinfo(self.asset)
        assert_equal(list(single.keys()), [self.asset])
        assert_equal(single[self.asset]['listed'], False)
        assert_equal(single[self.asset]['accepted'], False)
        assert 'rate' not in single[self.asset]
        # No registry is configured in these tests, so nothing is published.
        assert_equal(single[self.asset]['registry_listed'], False)
        assert 'market_price' not in single[self.asset]

        # A label works as well as a hex id, and both name the same entry.
        assert_equal(self.node.getfeeassetinfo('gasset'), {GASSET: info[GASSET]})
        assert_raises_rpc_error(-5, "Unknown label and invalid asset hex: nosuchasset",
                                self.node.getfeeassetinfo, 'nosuchasset')

    def test_refusal_is_not_absence(self):
        """Rate 0 and absence both refuse a fee, but they are different states: one
        is a policy someone wrote down, the other an asset nobody configured. The
        wallet says different things about them, so the RPC must not merge them
        the way a bare 'is it usable' boolean would."""
        self.node.setfeeexchangerates({'gasset': 100000000, self.asset: 0}, False)

        listed = self.node.getfeeassetinfo(self.asset)[self.asset]
        assert_equal(listed['listed'], True)     # someone wrote this down
        assert_equal(listed['accepted'], False)  # and what they wrote was "no"
        assert_equal(listed['rate'], 0)

        absent = self.node.getfeeassetinfo(self.token)[self.token]
        assert_equal(absent['listed'], False)    # nobody ever mentioned it
        assert_equal(absent['accepted'], False)
        assert 'rate' not in absent

    def test_policy_asset_is_not_privileged(self):
        """Dropping the policy asset from the whitelist must report it refused like
        any other asset. An exception for it here would be the silent fallback the
        open fee market exists to remove."""
        self.node.setfeeexchangerates({self.asset: 50000000}, False)
        info = self.node.getfeeassetinfo()
        assert_equal(info[GASSET]['listed'], False)
        assert_equal(info[GASSET]['accepted'], False)
        assert_equal(info[self.asset]['accepted'], True)
        assert_equal(info[self.asset]['rate'], 50000000)

    def test_estimatesmartfee_fee_asset(self):
        """fee_asset converts the estimate and reports acceptance. Acceptance does
        not depend on there being an estimate, and must be answered either way."""
        self.node.setfeeexchangerates({'gasset': 100000000, self.asset: 50000000}, False)

        # Omitting the argument must leave the answer exactly as it always was.
        plain = self.node.estimatesmartfee(6)
        assert 'asset' not in plain
        assert 'accepted' not in plain

        base = self.node.estimatesmartfee(6, 'conservative', 'gasset')
        assert_equal(base['asset'], GASSET)
        assert_equal(base['accepted'], True)
        assert_equal(base['rate'], 100000000)

        other = self.node.estimatesmartfee(6, 'conservative', self.asset)
        assert_equal(other['accepted'], True)
        assert_equal(other['rate'], 50000000)

        # An asset this node refuses gets no conversion and says why, rather than
        # quoting a zero that reads as "free".
        refused = self.node.estimatesmartfee(6, 'conservative', self.token)
        assert_equal(refused['accepted'], False)
        assert 'asset_feerate' not in refused
        assert any('does not accept fees in' in e for e in refused['errors'])

        # Named arguments must work without also naming estimate_mode: the omitted
        # middle argument is padded to null and must not be type-checked.
        named = self.node.estimatesmartfee(conf_target=6, fee_asset='gasset')
        assert_equal(named['asset'], GASSET)

        assert_raises_rpc_error(-5, "Unknown label and invalid asset hex: nope",
                                self.node.estimatesmartfee, 6, 'conservative', 'nope')

        # A chain this short has no fee history, so there is usually no estimate to
        # convert. When there is one, the conversion must follow the whitelist: the
        # issued asset is worth half a gasset per unit, so the same value costs
        # twice as many of its atoms.
        if 'feerate' in base:
            assert_equal(base['asset_feerate'], base['feerate'])
            assert_equal(other['asset_atoms_per_kvb'], 2 * base['asset_atoms_per_kvb'])

    def run_test(self):
        self.init()
        self.test_bootstrap_state()
        self.test_refusal_is_not_absence()
        self.test_policy_asset_is_not_privileged()
        self.test_estimatesmartfee_fee_asset()


if __name__ == '__main__':
    AnyAssetFeeInfoTest().main()
