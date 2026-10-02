#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Issuance and reissuance from a transparent wallet, blinded and explicit.

On a wallet that does not blind by default, the outputs an issuance creates are
explicit, so a blinded issuance amount has only the change to balance its
blinding against. Two things follow, and this test holds the wallet to both:

- When the change is small enough to go to the fee, it cannot simply be
  dropped: it is kept as a zero-value blinded OP_RETURN instead. Dropping it
  used to leave the issuance with nothing to balance, and the wallet then hit
  an assertion and aborted the node -- from a wallet RPC.
- Whether a REISSUANCE is blinded is fixed by its token: one derived as
  confidential requires a blinded amount (consensus), one derived explicit an
  explicit amount. An explicit reissuance therefore has explicit change, and
  never gets a blinding key on it only to have it taken off again.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

COIN = 100_000_000
FEE_ASSET = 'bitcoin'
SMALL_LEFTOVERS = (15_000, 25_000)   # below the blinded change's cost: dropped
LARGE_LEFTOVER = 200_000             # above it: kept as change


def sat(value):
    return int(round(Decimal(value) * COIN))


class WalletIssuanceTransparentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-con_default_blinded_addresses=0", "-blindedaddresses=0",
                            "-con_blocksubsidy=5000000000", "-validatepegin=0", "-txindex=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, n=1):
        self.generatetoaddress(self.node, n, self.miner, sync_fun=self.no_op)
        self.node.syncwithvalidationinterfacequeue()

    def fresh_wallet(self, name, amount_sat):
        """A wallet holding exactly one explicit coin of `amount_sat`."""
        self.node.createwallet(name)
        w = self.node.get_wallet_rpc(name)
        self.funder.sendtoaddress(address=w.getnewaddress(), amount=Decimal(amount_sat) / COIN,
                                  fee_asset_label=FEE_ASSET)
        self.mine()
        return w

    def fee_sat(self, tx):
        return sum(sat(v['value']) for v in tx['vout'] if v['scriptPubKey']['hex'] == '' and 'value' in v)

    def blinded_opreturns(self, tx):
        return [v for v in tx['vout'] if v['scriptPubKey']['hex'] == '6a' and 'valuecommitment' in v]

    def confirmed(self, txid):
        self.mine()
        tx = self.node.getrawtransaction(txid, True)
        assert_equal(tx['confirmations'], 1)
        return tx

    def run_test(self):
        self.node = self.nodes[0]
        self.funder = self.node.get_wallet_rpc(self.default_wallet_name)
        self.miner = self.funder.getnewaddress()
        self.mine(COINBASE_MATURITY + 10)
        self.test_blind_issuance_small_change()
        self.test_reissue_confidential_token()
        self.test_reissue_explicit_token()

    def test_blind_issuance_small_change(self):
        self.log.info("Blind issuance: measure its fee when it keeps change")
        probe = self.fresh_wallet('probe', COIN)
        r = probe.issueasset(assetamount=1, tokenamount=0, blind=True, fee_asset=FEE_ASSET)
        fee_with_change = self.fee_sat(self.confirmed(r['txid']))
        self.log.info("  fee with change: %d sat", fee_with_change)

        for leftover in SMALL_LEFTOVERS + (LARGE_LEFTOVER,):
            self.log.info("One coin of that fee + %d sat: issueasset(blind=True)", leftover)
            w = self.fresh_wallet('issue%d' % leftover, fee_with_change + leftover)
            r = w.issueasset(assetamount=1, tokenamount=0, blind=True, fee_asset=FEE_ASSET)
            tx = self.confirmed(r['txid'])
            assert 'assetamountcommitment' in tx['vin'][0]['issuance'], "the issuance amount is not blinded"
            assert_equal(w.getbalances()['mine']['trusted'][r['asset']], Decimal(1))
            if leftover == LARGE_LEFTOVER:
                # The change is kept and blinded (it balances the issuance).
                assert_equal(len(self.blinded_opreturns(tx)), 0)
                assert_equal(len([v for v in tx['vout'] if 'valuecommitment' in v]), 1)
            else:
                # The change went to the fee; its slot remains as the blinded
                # zero-value output that balances the issuance.
                dummies = self.blinded_opreturns(tx)
                assert_equal(len(dummies), 1)
                assert_equal(self.fee_sat(tx), fee_with_change + leftover)
                assert_equal(len([v for v in tx['vout'] if 'valuecommitment' in v]), 1)

    def test_reissue_confidential_token(self):
        self.log.info("Reissuing an asset issued blinded, from explicit coins only")
        w = self.fresh_wallet('reissuer', 10 * COIN)
        issued = w.issueasset(assetamount=10, tokenamount=1, blind=True, fee_asset=FEE_ASSET)
        self.confirmed(issued['txid'])
        token = [u for u in w.listunspent() if u['asset'] == issued['token']]
        assert_equal(len(token), 1)
        assert_equal(token[0]['amountblinder'], '00' * 32)   # the token itself is explicit
        blinded = [u for u in w.listunspent() if u['amountblinder'] != '00' * 32]
        w.lockunspent(False, [{'txid': u['txid'], 'vout': u['vout']} for u in blinded])

        self.log.info("  the reissuance amount is blinded and the change balances it")
        self.funder.sendtoaddress(address=w.getnewaddress(), amount=1, fee_asset_label=FEE_ASSET)
        self.mine()
        r = w.reissueasset(asset=issued['asset'], assetamount=1, fee_asset=FEE_ASSET)
        tx = self.confirmed(r['txid'])
        issuance = tx['vin'][r['vin']]['issuance']
        assert issuance['isreissuance'] and 'assetamountcommitment' in issuance

        # The change is what balances the blinded amount: it is blinded although
        # the wallet is transparent and nobody asked for it.
        assert_equal(len([v for v in tx['vout'] if 'valuecommitment' in v]), 1)
        assert_equal(self.blinded_opreturns(tx), [])
        assert_equal(w.getbalances()['mine']['trusted'][issued['asset']], Decimal(11))

    def test_reissue_explicit_token(self):
        self.log.info("Reissuing an asset issued explicitly: explicit amount, explicit change")
        w = self.fresh_wallet('explicit', 10 * COIN)
        issued = w.issueasset(assetamount=10, tokenamount=1, blind=False, fee_asset=FEE_ASSET)
        self.confirmed(issued['txid'])
        with self.node.assert_debug_log(expected_msgs=[], unexpected_msgs=["Unblinding change"]):
            r = w.reissueasset(asset=issued['asset'], assetamount=1, fee_asset=FEE_ASSET)
        tx = self.confirmed(r['txid'])
        issuance = tx['vin'][r['vin']]['issuance']
        assert issuance['isreissuance'] and 'assetamount' in issuance
        assert_equal([v for v in tx['vout'] if 'valuecommitment' in v], [])
        change = [v for v in tx['vout'] if v['scriptPubKey']['hex'] != '' and v['asset'] == self.node.dumpassetlabels()[FEE_ASSET]]
        assert_equal(len(change), 1)
        assert w.getaddressinfo(change[0]['scriptPubKey']['address'])['ismine']


if __name__ == '__main__':
    WalletIssuanceTransparentTest().main()
