#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A transparent wallet's change is explicit, however many change outputs.

Sending one asset and paying the fee in another leaves two change outputs, one
per asset. Two outputs are a blindable shape, so a wallet that attached its
blinding key to every change output blinded both, unasked: the transaction grew
about tenfold. Sequentia is transparent by default, so on a wallet that does not
hand out confidential addresses (-blindedaddresses=0) change is blinded only when
the transaction is confidential anyway -- a confidential recipient, a blinded
input, a blinded issuance -- or when a confidential change address is named.

A wallet that blinds by default (-blindedaddresses=1) keeps its behaviour.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

FEE_ASSET = 'bitcoin'


def blinded_outputs(decoded):
    return [v for v in decoded['vout'] if 'valuecommitment' in v]


class WalletTransparentChangeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.base_args = [
            "-con_default_blinded_addresses=0",
            "-con_any_asset_fees=1",
            "-con_blocksubsidy=5000000000",
            "-validatepegin=0",
            "-txindex=1",
        ]
        self.extra_args = [self.base_args + ["-blindedaddresses=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def two_asset_send(self, sender, to_address):
        """Send the issued asset, paying the fee in FEE_ASSET; return the decoded tx."""
        txid = sender.sendtoaddress(address=to_address, amount=10, assetlabel=self.asset,
                                    fee_asset_label=FEE_ASSET)
        decoded = self.node.getrawtransaction(txid, True)
        self.generatetoaddress(self.node, 1, self.miner_addr, sync_fun=self.no_op)
        assert_equal(self.node.getrawtransaction(txid, True)['confirmations'], 1)
        return decoded

    def change_outputs(self, decoded, sender, recipient_address):
        """The outputs of `decoded` paying `sender`'s own change addresses."""
        out = []
        for v in decoded['vout']:
            addr = v['scriptPubKey'].get('address')
            if addr is None or addr == recipient_address:
                continue
            if sender.getaddressinfo(addr)['ismine']:
                out.append(v)
        return out

    def fresh_wallet(self, name):
        """A wallet holding only explicit coins: FEE_ASSET and the issued asset."""
        self.node.createwallet(name)
        w = self.node.get_wallet_rpc(name)
        addr = w.getaddressinfo(w.getnewaddress())['unconfidential']
        self.default.sendtoaddress(address=addr, amount=50, fee_asset_label=FEE_ASSET)
        self.default.sendtoaddress(address=addr, amount=100, assetlabel=self.asset,
                                   fee_asset_label=FEE_ASSET)
        self.generatetoaddress(self.node, 1, self.miner_addr, sync_fun=self.no_op)
        for u in w.listunspent():
            assert_equal(u['amountblinder'], '00' * 32)
        return w

    def run_test(self):
        self.node = self.nodes[0]
        self.default = self.node.get_wallet_rpc(self.default_wallet_name)
        self.miner_addr = self.default.getnewaddress()
        self.generatetoaddress(self.node, COINBASE_MATURITY + 10, self.miner_addr, sync_fun=self.no_op)
        issued = self.default.issueasset(assetamount=1000, tokenamount=0, blind=False, fee_asset=FEE_ASSET)
        self.asset = issued['asset']
        self.generatetoaddress(self.node, 1, self.miner_addr, sync_fun=self.no_op)
        self.node.setfeeexchangerates({FEE_ASSET: 100000000, self.asset: 100000000})

        self.log.info("Transparent wallet: a two-asset send has only explicit outputs")
        w = self.fresh_wallet('transparent')
        dest = self.default.getaddressinfo(self.default.getnewaddress())['unconfidential']
        decoded = self.two_asset_send(w, dest)
        transparent_vsize = decoded['vsize']
        self.log.info("  two-asset send: %d vbytes, %d of %d outputs blinded", transparent_vsize,
                      len(blinded_outputs(decoded)), len(decoded['vout']))
        assert_equal(blinded_outputs(decoded), [])
        # The fee is priced on the explicit transaction, not on a blinded estimate:
        # the framework's fallback fee rate is 0.0002 per kvB.
        fee = sum(v['value'] for v in decoded['vout'] if v['scriptPubKey']['hex'] == '')
        self.log.info("  fee %s", fee)
        assert_greater_than(Decimal('0.0002') * (transparent_vsize + 2) / 1000, fee)
        change = self.change_outputs(decoded, w, dest)
        assert_equal(len(change), 2)
        assert_equal(sorted(c['asset'] for c in change), sorted([self.asset, self.node.dumpassetlabels()[FEE_ASSET]]))

        self.log.info("Transparent wallet: the funding RPCs agree and report nothing demoted")
        raw = w.createrawtransaction([], [{dest: 1, 'asset': self.asset}])
        res = w.fundrawtransaction(raw, {'fee_asset': FEE_ASSET, 'ignoreblindfail': False})
        assert 'warnings' not in res, res.get('warnings')
        funded = w.decoderawtransaction(res['hex'])
        assert_equal([v for v in funded['vout'] if v.get('commitmentnonce_fully_valid')], [])

        self.log.info("Transparent wallet: a confidential recipient still gets a confidential payment")
        conf_dest = self.default.getnewaddress("", "blech32")
        decoded = self.two_asset_send(w, conf_dest)
        # The recipient and both change outputs: the recipient's amount stays hidden.
        assert_equal(len(blinded_outputs(decoded)), 3)

        self.log.info("Transparent wallet: spending a confidential coin keeps its change confidential")
        self.node.createwallet('holder')
        holder = self.node.get_wallet_rpc('holder')
        self.default.sendtoaddress(address=holder.getaddressinfo(holder.getnewaddress())['unconfidential'],
                                   amount=5, fee_asset_label=FEE_ASSET)
        self.default.sendtoaddress(address=holder.getnewaddress("", "blech32"), amount=20,
                                   assetlabel=self.asset, fee_asset_label=FEE_ASSET)
        self.generatetoaddress(self.node, 1, self.miner_addr, sync_fun=self.no_op)
        held = [u for u in holder.listunspent() if u['asset'] == self.asset]
        assert_equal(len(held), 1)
        assert held[0]['amountblinder'] != '00' * 32
        dest = self.default.getaddressinfo(self.default.getnewaddress())['unconfidential']
        decoded = self.two_asset_send(holder, dest)
        change = self.change_outputs(decoded, holder, dest)
        assert_equal(len(change), 2)
        # The asset coin was confidential, so what is left of it stays confidential.
        assert all('valuecommitment' in c for c in change), change
        assert 'valuecommitment' not in [v for v in decoded['vout'] if v['scriptPubKey'].get('address') == dest][0]

        self.log.info("Blinding wallet (-blindedaddresses=1): behaviour unchanged, both change outputs blinded")
        self.restart_node(0, extra_args=self.base_args + ["-blindedaddresses=1"])
        self.node = self.nodes[0]
        self.default = self.node.get_wallet_rpc(self.default_wallet_name)
        self.node.setfeeexchangerates({FEE_ASSET: 100000000, self.asset: 100000000})
        w2 = self.fresh_wallet('blinding')
        dest = self.default.getaddressinfo(self.default.getnewaddress())['unconfidential']
        decoded = self.two_asset_send(w2, dest)
        assert_equal(len(blinded_outputs(decoded)), 2)
        assert 'valuecommitment' not in [v for v in decoded['vout'] if v['scriptPubKey'].get('address') == dest][0]
        self.log.info("  blinding-wallet two-asset send: %d vbytes", decoded['vsize'])
        assert_greater_than(decoded['vsize'], transparent_vsize * 5)


if __name__ == '__main__':
    WalletTransparentChangeTest().main()
