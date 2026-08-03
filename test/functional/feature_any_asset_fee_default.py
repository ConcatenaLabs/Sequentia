#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The wallet back end never infers a fee asset from the transaction.

The RPC send paths used to default the fee asset to the asset being sent
(sendtoaddress/sendmany) or to the first output's asset (fundrawtransaction).
Issued assets are generally priced on this network, so that mostly worked -- but
a REISSUANCE TOKEN is not priced, so sending a token was refused outright with
"The chosen fee asset is not accepted", with nothing in the call to explain why.

The rule under test: absent an explicit fee_asset_label / options.fee_asset, the
fee is paid in the POLICY asset, whatever the transaction happens to contain. An
explicit choice is honoured verbatim, and still errors when unusable. Preferring
the asset being sent is a GUI preselection, not a back-end rule.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class AnyAssetFeeDefaultTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [[
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
            "-txindex=1",
        ]] * self.num_nodes
        self.extra_args[0].append("-anyonecanspendaremine=1")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def fee_assets(self, txid):
        """The assets a confirmed wallet transaction actually paid its fee in."""
        tx = self.nodes[0].gettransaction(txid)
        assert_equal(tx["confirmations"], 1)
        # gettransaction reports the fee as an asset -> amount map, so the key
        # set IS the answer to "which asset paid the fee".
        return sorted(tx["fee"].keys())

    def send_and_confirm(self, send):
        txid = send()
        self.generate(self.nodes[0], 1)
        self.sync_all()
        return txid

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, COINBASE_MATURITY + 1)
        self.sync_all()

        self.policy_asset = node.dumpassetlabels()["bitcoin"]
        assert_equal(node.getfeeexchangerates(), {"bitcoin": 100000000})

        # An ordinary issued asset and its reissuance token. The asset is priced,
        # as issued assets are on this network; the token is not, which is the
        # case the old inferred default made impossible.
        issuance = node.issueasset(100, 1, False)
        self.asset = issuance["asset"]
        self.token = issuance["token"]
        self.generate(node, 1)
        self.sync_all()

        rates = {"bitcoin": 100000000, self.asset: 100000000}
        for n in self.nodes:
            n.setfeeexchangerates(rates)
            assert_equal(n.getfeeexchangerates(), rates)

        self.address = self.nodes[1].getnewaddress()

        self.test_sendtoaddress()
        self.test_sendmany()
        self.test_fundrawtransaction()
        self.test_subtract_fee_is_symmetric()
        self.test_subtract_fee_impossible_combinations()

    def test_sendtoaddress(self):
        node = self.nodes[0]

        # The bug: this send was impossible. The wallet inferred the fee asset
        # from the asset being sent, the node has no rate for a reissuance token,
        # and the send was refused before it was ever built.
        self.log.info("sendtoaddress: an unpriced reissuance token pays its fee in the policy asset")
        txid = self.send_and_confirm(lambda: node.sendtoaddress(
            address=self.address, amount=0.1, assetlabel=self.token))
        assert_equal(self.fee_assets(txid), ["bitcoin"])

        # Even a perfectly acceptable asset does not become the fee asset by
        # accident: the back end infers nothing at all.
        self.log.info("sendtoaddress: a priced asset is still not inferred as the fee asset")
        txid = self.send_and_confirm(lambda: node.sendtoaddress(
            address=self.address, amount=1.0, assetlabel=self.asset))
        assert_equal(self.fee_assets(txid), ["bitcoin"])

        self.log.info("sendtoaddress: an explicit fee asset is honoured verbatim")
        txid = self.send_and_confirm(lambda: node.sendtoaddress(
            address=self.address, amount=1.0, assetlabel=self.asset,
            fee_asset_label=self.asset))
        assert_equal(self.fee_assets(txid), [self.asset])

        # Only the default changed. Naming an unusable asset is still an error,
        # never a silent substitution.
        self.log.info("sendtoaddress: an explicit unusable fee asset still errors")
        assert_raises_rpc_error(
            -6, "The chosen fee asset is not accepted",
            node.sendtoaddress, address=self.address, amount=1.0,
            assetlabel=self.asset, fee_asset_label=self.token)

    def test_sendmany(self):
        node = self.nodes[0]

        self.log.info("sendmany: an unpriced reissuance token pays its fee in the policy asset")
        txid = self.send_and_confirm(lambda: node.sendmany(
            amounts={self.address: 0.1},
            output_assets={self.address: self.token}))
        assert_equal(self.fee_assets(txid), ["bitcoin"])

        self.log.info("sendmany: a priced asset is still not inferred as the fee asset")
        txid = self.send_and_confirm(lambda: node.sendmany(
            amounts={self.address: 1.0},
            output_assets={self.address: self.asset}))
        assert_equal(self.fee_assets(txid), ["bitcoin"])

        self.log.info("sendmany: an explicit fee asset is honoured verbatim")
        txid = self.send_and_confirm(lambda: node.sendmany(
            amounts={self.address: 1.0},
            output_assets={self.address: self.asset},
            fee_asset=self.asset))
        assert_equal(self.fee_assets(txid), [self.asset])

        self.log.info("sendmany: an explicit unusable fee asset still errors")
        assert_raises_rpc_error(
            -6, "The chosen fee asset is not accepted",
            node.sendmany, amounts={self.address: 1.0},
            output_assets={self.address: self.asset}, fee_asset=self.token)

    def subtract_fee_case(self, asset):
        """Send with the fee subtracted from the output, naming NO fee asset.

        Returns what happened, so the issued-asset and policy-asset runs can be
        compared against each other. `asset` doubles as the balance-map and
        fee-map key: the policy asset is keyed by its label, an issued asset by
        its hex, and both are accepted as an assetlabel.
        """
        node, recipient = self.nodes[0], self.nodes[1]
        amount = Decimal("5.0")
        before = recipient.getbalances()["mine"]["trusted"].get(asset, Decimal(0))

        txid = self.send_and_confirm(lambda: node.sendtoaddress(
            address=self.address, amount=amount, assetlabel=asset,
            subtractfeefromamount=True))

        tx = node.gettransaction(txid)
        after = recipient.getbalances()["mine"]["trusted"].get(asset, Decimal(0))
        fee = -tx["fee"][asset]  # gettransaction reports fees as negative
        return {
            "fee_assets": sorted(tx["fee"].keys()),
            "fee_positive": fee > 0,
            "received": after - before,
            "received_is_amount_minus_fee": (after - before) == amount - fee,
        }

    def test_subtract_fee_is_symmetric(self):
        # Subtracting the fee from an output means the fee comes OUT of that
        # output, so it can only be paid in that output's asset -- arithmetic,
        # not a preference the wallet guessed. No fee_asset_label is needed.
        self.log.info("subtractfeefromamount: issued asset, no fee_asset_label")
        issued = self.subtract_fee_case(self.asset)
        assert_equal(issued["fee_assets"], [self.asset])
        assert issued["fee_positive"]
        assert issued["received_is_amount_minus_fee"]

        # The same call against the policy asset. This always worked, but only
        # because the fallback happened to coincide with the output's asset --
        # a privilege the issued-asset case did not get.
        self.log.info("subtractfeefromamount: policy asset, no fee_asset_label")
        policy = self.subtract_fee_case("bitcoin")
        assert_equal(policy["fee_assets"], ["bitcoin"])
        assert policy["fee_positive"]
        assert policy["received_is_amount_minus_fee"]

        # The point of the whole change: identical behaviour, same code path, no
        # asset special-cased. Every observation matches once the asset identity
        # itself is set aside, including the fee actually charged.
        self.log.info("subtractfeefromamount: the two are indistinguishable")
        assert_equal({k: v for k, v in issued.items() if k != "fee_assets"},
                     {k: v for k, v in policy.items() if k != "fee_assets"})

    def test_subtract_fee_impossible_combinations(self):
        node = self.nodes[0]

        # Naming a fee asset that contradicts the subtract-from output is not a
        # question about defaults, it is impossible. Refused in BOTH directions,
        # so neither asset is the privileged one.
        self.log.info("subtractfeefromamount: an explicit conflicting fee asset errors")
        assert_raises_rpc_error(
            -8, "necessarily paid in that output's asset",
            node.sendtoaddress, address=self.address, amount=1.0,
            assetlabel=self.asset, subtractfeefromamount=True,
            fee_asset_label="bitcoin")
        assert_raises_rpc_error(
            -8, "necessarily paid in that output's asset",
            node.sendtoaddress, address=self.address, amount=1.0,
            assetlabel="bitcoin", subtractfeefromamount=True,
            fee_asset_label=self.asset)

        # A transaction pays its fee in exactly one asset, so it cannot be
        # subtracted from outputs of two different ones.
        self.log.info("subtractfeefromamount: spanning two assets errors")
        other = self.nodes[1].getnewaddress()
        assert_raises_rpc_error(
            -8, "outputs of different assets",
            node.sendmany, amounts={self.address: 1.0, other: 1.0},
            output_assets={self.address: self.asset, other: "bitcoin"},
            subtractfeefrom=[self.address, other])

    def test_fundrawtransaction(self):
        node = self.nodes[0]

        self.log.info("fundrawtransaction: an unpriced token output funds its fee in the policy asset")
        raw = node.createrawtransaction(outputs=[{self.address: 0.1, "asset": self.token}])
        funded = node.fundrawtransaction(raw)
        assert_equal(funded["fee_asset"], self.policy_asset)
        assert_equal(node.decoderawtransaction(funded["hex"])["fee"],
                     {self.policy_asset: Decimal(funded["fee"])})

        self.log.info("fundrawtransaction: the first output's asset is not inferred")
        raw = node.createrawtransaction(outputs=[{self.address: 1.0, "asset": self.asset}])
        funded = node.fundrawtransaction(raw)
        assert_equal(funded["fee_asset"], self.policy_asset)

        self.log.info("fundrawtransaction: an explicit fee asset is honoured verbatim")
        raw = node.createrawtransaction(outputs=[{self.address: 1.0, "asset": self.asset}])
        funded = node.fundrawtransaction(raw, {"fee_asset": self.asset})
        assert_equal(funded["fee_asset"], self.asset)

        # subtract_fee_from_outputs forces the fee asset here too, and does so
        # for an issued asset exactly as it does for the policy asset.
        self.log.info("fundrawtransaction: subtract_fee_from_outputs forces the output's asset")
        raw = node.createrawtransaction(outputs=[{self.address: 1.0, "asset": self.asset}])
        funded = node.fundrawtransaction(raw, {"subtract_fee_from_outputs": [0]})
        assert_equal(funded["fee_asset"], self.asset)

        raw = node.createrawtransaction(outputs=[{self.address: 1.0}])
        funded = node.fundrawtransaction(raw, {"subtract_fee_from_outputs": [0]})
        assert_equal(funded["fee_asset"], self.policy_asset)

        self.log.info("fundrawtransaction: an explicit fee asset conflicting with it errors")
        raw = node.createrawtransaction(outputs=[{self.address: 1.0, "asset": self.asset}])
        assert_raises_rpc_error(
            -8, "necessarily paid in that output's asset",
            node.fundrawtransaction, raw,
            {"subtract_fee_from_outputs": [0], "fee_asset": "bitcoin"})


if __name__ == '__main__':
    AnyAssetFeeDefaultTest().main()
