#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""issueasset and reissueasset wait for the wallet to catch up with the chain.

A block reaches the wallet through the validation interface queue, after the
RPC that mined it has returned. Until the wallet has processed it, a coin
another wallet sent confirms in the chain but is still an untrusted, unconfirmed
receipt to this one. The spend RPCs wait for the wallet to catch up
(BlockUntilSyncedToCurrentChain); the issuance RPCs did not, and failed now and
then with "Insufficient funds" when called right after the block that funds
them. Every round here funds a wallet, mines one block and issues at once.

The race only shows when the queue is slow, so the test keeps every wallet
loaded: each one is another listener the queue serves for every block.
"""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

ROUNDS = 30
FEE_ASSET = 'bitcoin'


class WalletIssuanceSyncTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        n = self.nodes[0]
        funder = n.get_wallet_rpc(self.default_wallet_name)
        self.generatetoaddress(n, COINBASE_MATURITY + 40, funder.getnewaddress(), sync_fun=self.no_op)

        self.log.info("issueasset right after the block that funds the wallet, %d rounds", ROUNDS)
        for i in range(ROUNDS):
            n.createwallet('issuer%d' % i)
            w = n.get_wallet_rpc('issuer%d' % i)
            funder.sendtoaddress(address=w.getnewaddress(), amount=1, fee_asset_label=FEE_ASSET)
            n.generatetoaddress(1, funder.getnewaddress(), invalid_call=False)
            w.issueasset(assetamount=1, tokenamount=1, blind=False, fee_asset=FEE_ASSET)

        self.log.info("reissueasset right after the block that brings its fee coin, %d rounds", ROUNDS // 3)
        self.generatetoaddress(n, 1, funder.getnewaddress(), sync_fun=self.no_op)
        for i in range(ROUNDS // 3):
            w = n.get_wallet_rpc('issuer%d' % i)
            n.syncwithvalidationinterfacequeue()
            asset = [a for a in w.listissuances() if not a['isreissuance']][0]['asset']
            # Only the newly received coin may pay: lock everything else.
            w.lockunspent(False, [{'txid': u['txid'], 'vout': u['vout']} for u in w.listunspent()
                                  if u['asset'] == n.dumpassetlabels()[FEE_ASSET]])
            funder.sendtoaddress(address=w.getnewaddress(), amount=1, fee_asset_label=FEE_ASSET)
            n.generatetoaddress(1, funder.getnewaddress(), invalid_call=False)
            w.reissueasset(asset=asset, assetamount=1, fee_asset=FEE_ASSET)

        self.generatetoaddress(n, 1, funder.getnewaddress(), sync_fun=self.no_op)
        for i in range(ROUNDS // 3):
            w = n.get_wallet_rpc('issuer%d' % i)
            assert_equal(len(w.listissuances()), 2)


if __name__ == '__main__':
    WalletIssuanceSyncTest().main()
