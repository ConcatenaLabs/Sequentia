#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Issuance and reissuance from a transparent wallet, blinded and explicit.

On a wallet that does not blind by default, the outputs an issuance creates are
explicit, so a blinded issuance amount has only the change to balance its
blinding against. Three things follow, and this test holds the wallet to each:

- The change cannot be dropped: dropping it used to leave the issuance with
  nothing to balance, and the wallet then hit an assertion and aborted the
  node -- from a wallet RPC. Since its slot stays, its value stays too unless
  it is dust: giving a leftover to the fee saves only the change's script, so
  the fee never exceeds that of the same issuance with its change kept, but
  for a dust leftover.
- Since the slot stays, coin selection prices it as part of the transaction.
  Every coin from F, the fee of the issuance with its change kept, funds the
  issuance and the reissuance; a coin below F gets "Insufficient funds", never
  the wallet's internal "Could not cover fee".
- Whether a REISSUANCE is blinded is fixed by its token: one derived as
  confidential requires a blinded amount (consensus), one derived explicit an
  explicit amount. An explicit reissuance therefore has explicit change, and
  never gets a blinding key on it only to have it taken off again.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

COIN = 100_000_000
FEE_ASSET = 'bitcoin'
FEE_RATE = Decimal('0.0002')         # 20 sat/vB, set on every wallet
# A change output is dust below what spending it would cost at the discard rate
# (10 sat/vB): a P2WPKH output carrying a blinding nonce (98 bytes) plus the
# 67 vbytes of the input that spends it.
DUST = 1_650
# Leftovers over the fee of the issuance with its change kept: below it, at it,
# either side of dust, and on both sides of the blinded change's cost (about
# 1,200 vbytes of fee), which is where change used to be given to the fee.
LEFTOVERS = (-1_000, -1, 0, 1, 500, DUST - 1, DUST, 6_000, 16_000, 28_000, 40_000, 200_000)


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
        w.settxfee(FEE_RATE)
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
        # The wallet's internal-error branch, which says "Could not cover fee",
        # must not be what answers any of these.
        with self.node.assert_debug_log(expected_msgs=[], unexpected_msgs=["not enough coins to cover for fee"]):
            self.test_blind_issuance_small_change()
            self.test_reissue_confidential_token()
            self.test_reissue_explicit_token()
            self.test_reissue_confidential_token_funding()

    def test_blind_issuance_small_change(self):
        for token_amount in (0, 1):
            self.log.info("Blind issuance, token %d: measure its fee when it keeps change", token_amount)
            probe = self.fresh_wallet('probe%d' % token_amount, COIN)
            r = probe.issueasset(assetamount=1, tokenamount=token_amount, blind=True, fee_asset=FEE_ASSET)
            fee_with_change = self.fee_sat(self.confirmed(r['txid']))
            self.log.info("  fee with change: %d sat", fee_with_change)

            for leftover in LEFTOVERS:
                w = self.fresh_wallet('issue%d_%d' % (token_amount, leftover), fee_with_change + leftover)
                issue = lambda: w.issueasset(assetamount=1, tokenamount=token_amount, blind=True, fee_asset=FEE_ASSET)
                if leftover < 0:
                    self.log.info("  one coin of that fee %+6d sat: Insufficient funds", leftover)
                    assert_raises_rpc_error(-4, "Insufficient funds", issue)
                    continue
                r = issue()
                tx = self.confirmed(r['txid'])
                assert 'assetamountcommitment' in tx['vin'][0]['issuance'], "the issuance amount is not blinded"
                assert_equal(w.getbalances()['mine']['trusted'][r['asset']], Decimal(1))
                self.check_lone_change(tx, fee_with_change, leftover)

    def check_lone_change(self, tx, fee_with_change, leftover):
        """The fee of a transaction whose blinded change balances its issuance,
        funded by one coin of fee_with_change + leftover."""
        fee = self.fee_sat(tx)
        zeroed = self.blinded_opreturns(tx)
        self.log.info("  one coin of that fee %+6d sat: fee %d sat, %s", leftover, fee,
                      "change zeroed" if zeroed else "change kept")
        # Exactly one blinded output, kept or zeroed: it balances the issuance,
        # which is the only other thing blinded.
        assert_equal(len([v for v in tx['vout'] if 'valuecommitment' in v]), 1)
        if leftover < DUST:
            # Dust goes to the fee; the slot stays as a zero-value blinded OP_RETURN.
            assert_equal(len(zeroed), 1)
            assert_equal(fee, fee_with_change + leftover)
        else:
            # Never more than the same transaction with its change kept: the
            # slot stays either way, so giving the change to the fee saves
            # nothing but its script.
            assert_equal(zeroed, [])
            assert fee <= fee_with_change, "fee %d overpays the %d of the same transaction with change kept" % (fee, fee_with_change)

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

    def reissuer_holding(self, name, fee_coin_sat):
        """A wallet holding the explicit token of an asset issued blinded, and
        exactly one fee-asset coin of `fee_coin_sat`."""
        w = self.fresh_wallet(name, COIN)
        issued = w.issueasset(assetamount=1, tokenamount=1, blind=True, fee_asset=FEE_ASSET)
        self.mine()
        # Subtracting the fee from the amount pays it in the asset sent.
        w.sendtoaddress(address=self.funder.getnewaddress(), amount=w.getbalance()[FEE_ASSET],
                        subtractfeefromamount=True, assetlabel=FEE_ASSET)
        self.mine()
        assert_equal(w.getbalance().get(FEE_ASSET, 0), 0)
        self.funder.sendtoaddress(address=w.getnewaddress(), amount=Decimal(fee_coin_sat) / COIN,
                                  fee_asset_label=FEE_ASSET)
        self.mine()
        return w, issued['asset']

    def test_reissue_confidential_token_funding(self):
        self.log.info("Reissuance of a token derived as confidential: measure its fee when it keeps change")
        w, asset = self.reissuer_holding('rprobe', COIN // 10)
        r = w.reissueasset(asset=asset, assetamount=1, fee_asset=FEE_ASSET)
        fee_with_change = self.fee_sat(self.confirmed(r['txid']))
        self.log.info("  fee with change: %d sat", fee_with_change)
        for leftover in LEFTOVERS:
            w, asset = self.reissuer_holding('reissue%d' % leftover, fee_with_change + leftover)
            reissue = lambda: w.reissueasset(asset=asset, assetamount=1, fee_asset=FEE_ASSET)
            if leftover < 0:
                self.log.info("  one coin of that fee %+6d sat: Insufficient funds", leftover)
                assert_raises_rpc_error(-4, "Insufficient funds", reissue)
                continue
            r = reissue()
            tx = self.confirmed(r['txid'])
            assert 'assetamountcommitment' in tx['vin'][r['vin']]['issuance'], "the reissuance amount is not blinded"
            assert_equal(w.getbalances()['mine']['trusted'][asset], Decimal(2))
            self.check_lone_change(tx, fee_with_change, leftover)


if __name__ == '__main__':
    WalletIssuanceTransparentTest().main()
