#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Coin selection prices change as it will be built.

On a transparent wallet change is explicit unless the transaction is
confidential anyway, so selection prices it as an explicit output (about 66
vbytes) and reserves no blinding dummy, rather than pricing a blinded output
and a dummy of about 1,400 vbytes each. Priced blinded, a send with a leftover
below the dummy's fee failed with "Insufficient funds", and one just above it
gave the whole leftover to the fee.

A blinded coin makes the transaction confidential after all. If selection picks
one, it runs again priced blinded, so the fee still covers what is built.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

COIN = 100_000_000
FEE_ASSET = 'bitcoin'
FEE_RATE = 20  # sat/vB, the functional tests' fallback fee


def sat(value):
    return int(round(Decimal(value) * COIN))


class WalletChangeSizeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-con_default_blinded_addresses=0", "-blindedaddresses=0",
                            "-con_blocksubsidy=5000000000", "-validatepegin=0", "-txindex=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self):
        self.generatetoaddress(self.node, 1, self.miner, sync_fun=self.no_op)
        self.node.syncwithvalidationinterfacequeue()

    def wallet_with(self, name, coins):
        """A fresh wallet holding one coin per (amount, confidential) pair."""
        self.node.createwallet(name)
        w = self.node.get_wallet_rpc(name)
        for amount, confidential in coins:
            addr = w.getnewaddress("", "blech32") if confidential else w.getnewaddress()
            self.funder.sendtoaddress(address=addr, amount=amount, fee_asset_label=FEE_ASSET)
        self.mine()
        return w

    def fund(self, w, amount):
        raw = w.createrawtransaction([], [{self.dest: amount}])
        return w.fundrawtransaction(raw, {'fee_asset': FEE_ASSET})

    def run_test(self):
        self.node = self.nodes[0]
        self.funder = self.node.get_wallet_rpc(self.default_wallet_name)
        self.miner = self.funder.getnewaddress()
        self.generatetoaddress(self.node, COINBASE_MATURITY + 5, self.miner, sync_fun=self.no_op)
        self.dest = self.funder.getnewaddress()
        self.test_transparent_leftovers()
        self.test_blinded_coin()

    def test_transparent_leftovers(self):
        self.log.info("Measure the fee of spending one explicit coin with no change")
        probe = self.wallet_with('probe', [(1, False)])
        sweep = probe.fundrawtransaction(probe.createrawtransaction([], [{self.dest: 1}]),
                                         {'subtractFeeFromOutputs': [0]})
        no_change_fee = sat(sweep['fee'])
        self.log.info("  no-change fee: %d sat", no_change_fee)

        for leftover in (2_000, 10_000, 28_000, 29_500, 31_000):
            w = self.wallet_with('left%d' % leftover, [(1, False)])
            res = self.fund(w, Decimal(COIN - no_change_fee - leftover) / COIN)
            tx = w.decoderawtransaction(res['hex'])
            fee = sat(res['fee'])
            self.log.info("  leftover %6d sat: changepos %d, fee %d sat", leftover, res['changepos'], fee)
            assert_equal([v for v in tx['vout'] if 'valuecommitment' in v], [])
            if leftover == 2_000:
                # Less than an explicit change output costs to make and spend:
                # it goes to the fee.
                assert_equal(res['changepos'], -1)
                assert_equal(fee, no_change_fee + leftover)
            else:
                # Kept as explicit change; the fee grows by the change output only.
                assert res['changepos'] >= 0
                change_fee = fee - no_change_fee
                assert 0 < change_fee <= 100 * FEE_RATE, "change priced at %d sat" % change_fee
                assert_equal(sat(tx['vout'][res['changepos']]['value']), leftover - change_fee)
            signed = w.signrawtransactionwithwallet(res['hex'])
            assert_equal(self.node.testmempoolaccept([signed['hex']])[0]['allowed'], True)

    def test_blinded_coin(self):
        self.log.info("A transparent wallet spending a blinded coin prices the blinded change")
        w = self.wallet_with('blinded', [(1, True)])
        coin = w.listunspent()[0]
        assert coin['amountblinder'] != '00' * 32
        txid = w.sendtoaddress(address=self.dest, amount=Decimal('0.5'), fee_asset_label=FEE_ASSET)
        tx = self.node.getrawtransaction(txid, True)
        fee = sum(sat(v['value']) for v in tx['vout'] if v['scriptPubKey']['hex'] == '')
        blinded = [v for v in tx['vout'] if 'valuecommitment' in v]
        assert_equal(len(blinded), 1)   # the change, balancing the blinded input
        assert_greater_than(fee + 1, FEE_RATE * tx['vsize'])
        self.mine()
        assert_equal(self.node.getrawtransaction(txid, True)['confirmations'], 1)

        self.log.info("  ...and an amount the blinded transaction cannot pay for is refused at selection")
        w = self.wallet_with('blinded_short', [(1, True)])
        sweep = w.fundrawtransaction(w.createrawtransaction([], [{self.dest: 1}]),
                                     {'subtractFeeFromOutputs': [0]})
        blinded_sweep_fee = sat(sweep['fee'])
        # 20,000 sat less than the cheapest transaction spending this coin needs,
        # yet far more than an explicit one would: priced explicit, the coin is
        # selected and the transaction then cannot pay its fee ("Could not cover
        # fee"). Priced blinded on re-selection, selection refuses it.
        assert_raises_rpc_error(-4, "Insufficient funds", self.fund, w,
                                Decimal(COIN - blinded_sweep_fee + 20_000) / COIN)
        res = self.fund(w, Decimal(COIN - blinded_sweep_fee - 100_000) / COIN)
        blinded = w.blindrawtransaction(res['hex'])
        signed = w.signrawtransactionwithwallet(blinded)
        accept = self.node.testmempoolaccept([signed['hex']])[0]
        self.log.info("  funded with 100,000 sat to spare: %s", accept)
        assert_equal(accept['allowed'], True)


if __name__ == '__main__':
    WalletChangeSizeTest().main()
