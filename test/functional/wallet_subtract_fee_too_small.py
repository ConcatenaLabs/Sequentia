#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A fee subtracted from an output worth less than the fee.

Subtracting the fee from an amount smaller than the fee would leave the output
negative. The wallet refuses that with "The transaction amount is too small to
pay the fee", and builds nothing: no transaction enters the wallet, and the
node keeps running. Exercised from a blinded coin (whose spend is confidential,
so a negative output would have to be blinded) and from an explicit one, to an
unconfidential and to a confidential address.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

# Coins worth less than the fee of spending them: a confidential spend weighs
# over a thousand vbytes, an explicit one a couple of hundred.
COIN_VALUE = {True: Decimal('0.00007520'), False: Decimal('0.00001000')}


class WalletSubtractFeeTooSmallTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-blindedaddresses=1", "-con_blocksubsidy=5000000000", "-validatepegin=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        funder = node.get_wallet_rpc(self.default_wallet_name)
        self.generatetoaddress(node, COINBASE_MATURITY + 5, funder.getnewaddress(), sync_fun=self.no_op)

        for blinded in (True, False):
            name = 'blinded' if blinded else 'explicit'
            node.createwallet(name)
            w = node.get_wallet_rpc(name)
            address = w.getnewaddress()
            if not blinded:
                address = w.getaddressinfo(address)['unconfidential']
            value = COIN_VALUE[blinded]
            funder.sendtoaddress(address=address, amount=value, fee_asset_label='bitcoin')
            self.generatetoaddress(node, 1, funder.getnewaddress(), sync_fun=self.no_op)
            coin = w.listunspent()
            assert_equal(len(coin), 1)
            assert_equal('amountcommitment' in coin[0], blinded)

            confidential = funder.getnewaddress()
            for label, dest in (("unconfidential", funder.getaddressinfo(confidential)['unconfidential']),
                                ("confidential", confidential)):
                self.log.info("Send all of a coin (%s) to an address (%s), fee subtracted", name, label)
                txs_before = len(w.listtransactions())
                assert_raises_rpc_error(-6, "The transaction amount is too small to pay the fee",
                                        w.sendtoaddress, address=dest, amount=value,
                                        subtractfeefromamount=True, assetlabel='bitcoin')
                assert node.process.poll() is None, "the node stopped"
                assert_equal(len(w.listtransactions()), txs_before)
                assert_equal(w.getbalance(), {'bitcoin': value})


if __name__ == '__main__':
    WalletSubtractFeeTooSmallTest().main()
