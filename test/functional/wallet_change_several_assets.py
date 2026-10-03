#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Funding a send that leaves change in several assets.

A send of assets other than the fee asset leaves change in each of them as
well as in the fee asset, and the transaction carries one change output per
asset. Coin selection must price all of them: priced as one, a fee coin that
pays for one change output but not two passed selection and then failed with
the wallet's internal "Could not cover fee".

For a send of one, two and three other assets, this sweeps the value of the
wallet's only fee-asset coin and holds the wallet to one boundary: below it
"Insufficient funds", from it on a transaction that pays at least its own fee
rate, and nothing else. The boundary is the cheapest transaction that can be
built, the fee asset's change given to the fee: the funded transaction at the
boundary pays within one sweep step of its rate.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

COIN = 100_000_000
FEE_ASSET = 'bitcoin'
FEE_RATE = Decimal('0.0002')  # 20 sat/vB
RATE = 20
STEP = 250


class WalletChangeSeveralAssetsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-con_default_blinded_addresses=0", "-blindedaddresses=0",
                            "-con_blocksubsidy=5000000000", "-validatepegin=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, n=1):
        self.generatetoaddress(self.node, n, self.funder.getnewaddress(), sync_fun=self.no_op)

    def run_test(self):
        self.node = node = self.nodes[0]
        self.funder = funder = node.get_wallet_rpc(self.default_wallet_name)
        self.mine(COINBASE_MATURITY + 5)
        assets = [funder.issueasset(assetamount=1000, tokenamount=0, blind=False, fee_asset=FEE_ASSET)['asset']
                  for _ in range(3)]
        self.mine()

        node.createwallet('w')
        w = node.get_wallet_rpc('w')
        w.settxfee(FEE_RATE)
        for asset in assets:
            funder.sendtoaddress(address=w.getnewaddress(), amount=10, assetlabel=asset, fee_asset_label=FEE_ASSET)
        self.mine()

        # One fee-asset coin for every value swept, locked but for the one in use.
        values = list(range(4_000, 18_001, STEP)) + [40_000, 150_000]
        for i, v in enumerate(values):
            funder.sendtoaddress(address=w.getnewaddress(), amount=Decimal(v) / COIN, fee_asset_label=FEE_ASSET)
            if i % 20 == 19:
                self.mine()
        self.mine()
        fee_asset = node.dumpassetlabels()[FEE_ASSET]
        coin_of = {int(round(u['amount'] * COIN)): {'txid': u['txid'], 'vout': u['vout']}
                   for u in w.listunspent() if u['asset'] == fee_asset}
        assert_equal(sorted(coin_of), values)

        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["not enough coins to cover for fee"]):
            for n in (1, 2, 3):
                outputs = [{funder.getnewaddress(): 1, 'asset': asset} for asset in assets[:n]]
                self.sweep(w, n, outputs, values, coin_of)

    def sweep(self, w, n, outputs, values, coin_of):
        raw = w.createrawtransaction([], outputs)
        refused, funded = [], []
        for v in values:
            others = [c for vv, c in coin_of.items() if vv != v]
            w.lockunspent(False, others)
            try:
                r = w.fundrawtransaction(raw, {'fee_asset': FEE_ASSET})
            except Exception as e:
                assert 'Insufficient funds' in str(e), "fee coin %d: %s" % (v, e)
                refused.append(v)
            else:
                signed = w.signrawtransactionwithwallet(r['hex'])['hex']
                tx = w.decoderawtransaction(signed)
                fee = int(round(r['fee'] * COIN))
                assert fee >= tx['vsize'] * RATE, "fee coin %d: fee %d under the rate for %d vB" % (v, fee, tx['vsize'])
                assert self.node.testmempoolaccept([signed])[0]['allowed']
                funded.append((v, fee, tx['vsize'], len(tx['vout'])))
            w.lockunspent(True, others)
        first = funded[0]
        self.log.info("%d other asset(s): refused up to %d sat, first funded %d sat (fee %d for %d vB, %d outputs)",
                      n, refused[-1], first[0], first[1], first[2], first[3])
        assert max(refused) < first[0], "a value is refused above one that is funded"
        # The boundary is the cheapest buildable transaction: n change outputs
        # besides the recipients and the fee, and its fee within a step of its rate.
        assert_equal(first[3], 2 * n + 1)
        assert first[1] - first[2] * RATE < STEP, "the first funded value is more than a step above its need"


if __name__ == '__main__':
    WalletChangeSeveralAssetsTest().main()
