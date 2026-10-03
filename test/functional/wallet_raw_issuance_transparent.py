#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The raw blinded-issuance flow on a transparent wallet.

fundrawtransaction -> rawissueasset(blind) -> blindrawtransaction -> sign ->
send. Confidentiality is chosen per output, so on a wallet that does not blind
by default the change funding adds is explicit, and a blinded issuance amount
needs a confidential output beside it to balance its blinding. Without one,
blindrawtransaction refuses (the rawissueasset and blindrawtransaction help say
so). With one -- a confidential asset_address, or a confidential changeAddress
when funding -- the issuance confirms.

The changeAddress route needs the wallet's blinding dummy to sit before the fee
output: rawissueasset requires the fee to be the last output.
"""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

FEE_ASSET = 'bitcoin'


class WalletRawIssuanceTransparentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-con_default_blinded_addresses=0", "-blindedaddresses=0",
                            "-con_blocksubsidy=5000000000", "-validatepegin=0", "-txindex=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def fund(self, options=None):
        n = self.nodes[0]
        o = {"fee_asset": FEE_ASSET}
        o.update(options or {})
        funded = n.fundrawtransaction(n.createrawtransaction([], [{n.getnewaddress(): 1}]), o)
        vout = n.decoderawtransaction(funded['hex'])['vout']
        assert_equal(vout[-1]['scriptPubKey']['hex'], '')   # the fee output is last
        return funded['hex'], vout

    def issue_blind_and_send(self, funded_hex, asset_address):
        n = self.nodes[0]
        issued = n.rawissueasset(funded_hex, [{"asset_amount": 1, "asset_address": asset_address, "blind": True}])[0]
        blinded = n.blindrawtransaction(issued['hex'], False, [], True)
        signed = n.signrawtransactionwithwallet(blinded)
        assert_equal(signed['complete'], True)
        txid = n.sendrawtransaction(signed['hex'])
        self.generatetoaddress(n, 1, n.getnewaddress(), sync_fun=self.no_op)
        tx = n.getrawtransaction(txid, True)
        assert_equal(tx['confirmations'], 1)
        assert 'assetamountcommitment' in tx['vin'][issued['vin']]['issuance']
        assert_equal(n.getbalances()['mine']['trusted'].get(issued['asset']), 1)

    def run_test(self):
        n = self.nodes[0]
        self.generatetoaddress(n, COINBASE_MATURITY + 10, n.getnewaddress(), sync_fun=self.no_op)

        self.log.info("Explicit change only: nothing balances the blinded issuance, and blinding is refused")
        funded_hex, vout = self.fund()
        assert_equal([v for v in vout if v.get('commitmentnonce_fully_valid')], [])
        issued = n.rawissueasset(funded_hex, [{"asset_amount": 1, "asset_address": n.getnewaddress(), "blind": True}])[0]
        assert_raises_rpc_error(-8, "Unable to blind transaction: Add another output to blind in order to complete the blinding.",
                                n.blindrawtransaction, issued['hex'], False, [], True)

        self.log.info("A confidential asset_address balances it")
        funded_hex, _ = self.fund()
        self.issue_blind_and_send(funded_hex, n.getnewaddress("", "blech32"))

        self.log.info("A confidential changeAddress balances it; the funding's dummy sits before the fee")
        funded_hex, vout = self.fund({"changeAddress": n.getnewaddress("", "blech32")})
        dummies = [i for i, v in enumerate(vout) if v['scriptPubKey']['hex'] == '6a']
        assert_equal(len(dummies), 1)
        assert_equal(dummies[0], len(vout) - 2)
        self.issue_blind_and_send(funded_hex, n.getnewaddress())


if __name__ == '__main__':
    WalletRawIssuanceTransparentTest().main()
