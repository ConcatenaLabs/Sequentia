#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The fee asset is named explicitly unless the transaction already determines it.

Sequentia has no privileged coin: outside staking eligibility the policy asset
(SEQ) stands exactly level with every issued asset. A wallet that quietly settles
on the policy asset whenever the caller says nothing makes SEQ the fee currency by
default, which is the privilege the open fee market exists to abolish.

ONE RULE: the fee asset must be named explicitly unless the transaction already
determines it.

  - The transaction determines it in two ways, and neither is an exception to the
    rule -- both are the transaction stating the answer:
      * an explicit FEE OUTPUT names the asset the fee is paid in;
      * subtractfeefromamount takes the fee OUT of that output, so it is
        necessarily denominated in that output's asset -- output_amount -= fee,
        and a GOLD output cannot be reduced by an amount denominated in USDX.
    No argument is needed, and passing one is REFUSED even when it agrees: it
    would look like a selection and would not be one. Stated twice, the two must
    agree.
  - nothing determines it, so it is named: an explicit fee asset is honoured
    verbatim.
  - named nothing and nothing determines it: RPC_INVALID_PARAMETER.

Every assertion below that involves the policy asset is run against an issued
asset too, and the results compared. The symmetry IS the property under test.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

MUST_NAME = "Nothing in this transaction determines the fee asset"
ALREADY_DETERMINED = "already determines the fee asset"
TWO_ASSETS = "outputs of different assets"
TWO_WAYS = "determines the fee asset in two ways that disagree"


class AnyAssetFeeNoDefaultTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [[
            # Sequentia is transparent by default (principle: confidentiality is
            # opt-in), and `send` cannot blind a transaction it builds.
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
            "-txindex=1",
        ]] * self.num_nodes
        self.extra_args[0].append("-anyonecanspendaremine=1")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ------------------------------------------------------------------ helpers

    def send_and_confirm(self, send):
        txid = send()
        self.generate(self.nodes[0], 1)
        self.sync_all()
        return txid

    def fee_assets(self, txid, confirmed=True):
        """The assets a wallet transaction actually paid its fee in."""
        tx = self.nodes[0].gettransaction(txid)
        if confirmed:
            assert_equal(tx["confirmations"], 1)
        return sorted(tx["fee"].keys())

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, COINBASE_MATURITY + 1)
        self.sync_all()

        self.policy_asset = node.dumpassetlabels()["bitcoin"]
        assert_equal(node.getfeeexchangerates(), {"bitcoin": 100000000})

        # issueasset is itself one of the paths under test: it has to be told
        # which asset pays for it, and there is nothing to infer from.
        self.log.info("issueasset: naming no fee asset errors")
        assert_raises_rpc_error(-8, MUST_NAME, node.issueasset, 100, 1, False)

        issuance = node.issueasset(assetamount=1000, tokenamount=1, blind=False,
                                   fee_asset="bitcoin")
        self.asset = issuance["asset"]
        self.token = issuance["token"]
        self.generate(node, 1)
        self.sync_all()

        rates = {"bitcoin": 100000000, self.asset: 100000000}
        for n in self.nodes:
            n.setfeeexchangerates(rates)
            assert_equal(n.getfeeexchangerates(), rates)

        self.address = self.nodes[1].getnewaddress()

        self.test_must_be_named()
        self.test_explicit_is_honoured()
        self.test_subtract_fee_is_symmetric()
        self.test_impossible_combinations()
        self.test_fund_paths()
        self.test_fee_output_determines()
        self.test_reissueasset()
        self.test_unacceptable_asset_still_errors()

    # ------------------------------------------------------- (c) there is no default

    def test_must_be_named(self):
        """Naming nothing errors -- for the policy asset exactly as for any other."""
        node = self.nodes[0]

        # The same send, twice, differing only in which asset is being moved.
        # Both are refused: the asset being sent has never been a reason to pay
        # the fee in it, and neither has the policy asset's identity.
        for label, asset_hex in (("bitcoin", self.policy_asset), (self.asset, self.asset)):
            self.log.info(f"sendtoaddress: no fee asset named, sending {label[:8]} -> error")
            assert_raises_rpc_error(
                -8, MUST_NAME, node.sendtoaddress,
                address=self.address, amount=1.0, assetlabel=label)

            self.log.info(f"sendmany: no fee asset named, sending {label[:8]} -> error")
            assert_raises_rpc_error(
                -8, MUST_NAME, node.sendmany,
                amounts={self.address: 1.0}, output_assets={self.address: label})

            self.log.info(f"fundrawtransaction: no fee asset named, output {label[:8]} -> error")
            raw = node.createrawtransaction(outputs=[{self.address: 1.0, "asset": asset_hex}])
            assert_raises_rpc_error(-8, MUST_NAME, node.fundrawtransaction, raw)

            self.log.info(f"walletcreatefundedpsbt: no fee asset named, output {label[:8]} -> error")
            assert_raises_rpc_error(
                -8, MUST_NAME, node.walletcreatefundedpsbt,
                [], [{self.address: 1.0, "asset": asset_hex}])

            self.log.info(f"send: no fee asset named, output {label[:8]} -> error")
            assert_raises_rpc_error(
                -8, MUST_NAME, node.send,
                outputs=[{self.address: 1.0, "asset": asset_hex}])

        # The error has to be actionable: it names the parameter to pass and says
        # why there is nothing to fall back on.
        try:
            node.sendtoaddress(address=self.address, amount=1.0)
            raise AssertionError("expected sendtoaddress to refuse")
        except Exception as e:
            message = str(e)
        assert "fee_asset_label" in message, message
        assert "open fee market" in message, message
        assert "must be named" in message, message

    # ---------------------------------------------- (b) an explicit choice is honoured

    def test_explicit_is_honoured(self):
        node = self.nodes[0]

        self.log.info("sendtoaddress: an explicit fee asset is honoured verbatim")
        # Deliberately crossed: send the issued asset, pay in the policy asset,
        # and vice versa. Neither is inferred from the other.
        txid = self.send_and_confirm(lambda: node.sendtoaddress(
            address=self.address, amount=1.0, assetlabel=self.asset,
            fee_asset_label="bitcoin"))
        assert_equal(self.fee_assets(txid), ["bitcoin"])

        txid = self.send_and_confirm(lambda: node.sendtoaddress(
            address=self.address, amount=1.0, assetlabel="bitcoin",
            fee_asset_label=self.asset))
        assert_equal(self.fee_assets(txid), [self.asset])

        self.log.info("sendmany: an explicit fee asset is honoured verbatim")
        txid = self.send_and_confirm(lambda: node.sendmany(
            amounts={self.address: 1.0}, output_assets={self.address: "bitcoin"},
            fee_asset=self.asset))
        assert_equal(self.fee_assets(txid), [self.asset])

        # `send` shares FundTransaction with fundrawtransaction and
        # walletcreatefundedpsbt, whose success paths are asserted in
        # test_fund_paths; only its refusal is asserted here, because `send`
        # itself cannot complete on this chain (it signs the PSBT it builds and
        # trips BLINDING_REQUIRED on the blinded change output, with or without
        # an explicit fee asset -- a limitation upstream of nothing in this rule).

    # ------------------------------- (a) subtract-fee settles it, for every asset alike

    def subtract_fee_case(self, asset):
        """Send with the fee subtracted from the output, naming NO fee asset.

        Returns what happened so the issued-asset and policy-asset runs can be
        compared. `asset` doubles as the balance-map and fee-map key: the policy
        asset is keyed by its label, an issued asset by its hex, and both are
        accepted as an assetlabel.
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
        # No fee_asset_label anywhere below. The fee comes OUT of the output, so
        # it can only be that output's asset; the wallet is not guessing a
        # preference, it is computing the only value that exists.
        self.log.info("subtractfeefromamount: issued asset, no fee asset named")
        issued = self.subtract_fee_case(self.asset)
        assert_equal(issued["fee_assets"], [self.asset])
        assert issued["fee_positive"]
        assert issued["received_is_amount_minus_fee"]

        self.log.info("subtractfeefromamount: policy asset, no fee asset named")
        policy = self.subtract_fee_case("bitcoin")
        assert_equal(policy["fee_assets"], ["bitcoin"])
        assert policy["fee_positive"]
        assert policy["received_is_amount_minus_fee"]

        # The point of the whole rule: identical behaviour, same code path, no
        # asset special-cased. Every observation matches once the asset identity
        # itself is set aside, including the fee actually charged.
        self.log.info("subtractfeefromamount: the two are indistinguishable")
        assert_equal({k: v for k, v in issued.items() if k != "fee_assets"},
                     {k: v for k, v in policy.items() if k != "fee_assets"})

        # sendmany's subtractfeefrom settles it the same way.
        self.log.info("sendmany subtractfeefrom: settles the fee asset with no argument")
        for label, expected in (("bitcoin", "bitcoin"), (self.asset, self.asset)):
            txid = self.send_and_confirm(lambda: self.nodes[0].sendmany(
                amounts={self.address: 2.0},
                output_assets={self.address: label},
                subtractfeefrom=[self.address]))
            assert_equal(self.fee_assets(txid), [expected])

    def test_impossible_combinations(self):
        node = self.nodes[0]

        # The subtract-fee output already determines the fee asset, so naming one
        # is refused -- it would be a parameter that looks like a selection and is
        # not one. Refused in BOTH directions, so neither asset is the privileged
        # one, and refused even when the value AGREES, because agreement does not
        # make it a choice.
        self.log.info("subtractfeefromamount: naming a fee asset at all errors")
        for sent, named in ((self.asset, "bitcoin"),      # issued output, policy fee asset
                            ("bitcoin", self.asset),      # policy output, issued fee asset
                            (self.asset, self.asset),     # matching, issued
                            ("bitcoin", "bitcoin")):      # matching, policy
            assert_raises_rpc_error(
                -8, ALREADY_DETERMINED, node.sendtoaddress, address=self.address,
                amount=1.0, assetlabel=sent, subtractfeefromamount=True,
                fee_asset_label=named)

        # A transaction pays its fee in exactly one asset, so it cannot be
        # subtracted from outputs of two different ones.
        self.log.info("subtractfeefromamount: spanning two assets errors")
        other = self.nodes[1].getnewaddress()
        assert_raises_rpc_error(
            -8, TWO_ASSETS, node.sendmany,
            amounts={self.address: 1.0, other: 1.0},
            output_assets={self.address: self.asset, other: "bitcoin"},
            subtractfeefrom=[self.address, other])

    # ------------------------------------------------------- the funding RPC family

    def test_fund_paths(self):
        node = self.nodes[0]

        self.log.info("fundrawtransaction: an explicit fee asset is honoured verbatim")
        raw = node.createrawtransaction(outputs=[{self.address: 1.0, "asset": self.policy_asset}])
        funded = node.fundrawtransaction(raw, {"fee_asset": self.asset})
        assert_equal(funded["fee_asset"], self.asset)

        # subtract_fee_from_outputs settles it here too, and does so for an issued
        # asset exactly as for the policy asset.
        self.log.info("fundrawtransaction: subtract_fee_from_outputs settles it, both assets alike")
        for expected in (self.policy_asset, self.asset):
            raw = node.createrawtransaction(outputs=[{self.address: 1.0, "asset": expected}])
            funded = node.fundrawtransaction(raw, {"subtract_fee_from_outputs": [0]})
            assert_equal(funded["fee_asset"], expected)
            assert_equal(node.decoderawtransaction(funded["hex"])["fee"],
                         {expected: Decimal(funded["fee"])})

        self.log.info("fundrawtransaction: naming a fee asset alongside it errors, matching or not")
        raw = node.createrawtransaction(outputs=[{self.address: 1.0, "asset": self.asset}])
        for named in ("bitcoin", self.asset):
            assert_raises_rpc_error(
                -8, ALREADY_DETERMINED, node.fundrawtransaction, raw,
                {"subtract_fee_from_outputs": [0], "fee_asset": named})

        self.log.info("walletcreatefundedpsbt: an explicit fee asset is honoured verbatim")
        psbt = node.walletcreatefundedpsbt(
            [], [{self.address: 1.0, "asset": self.policy_asset}], 0, {"fee_asset": self.asset})
        assert_equal(psbt["fee_asset"], self.asset)

        self.log.info("walletcreatefundedpsbt: subtract_fee_from_outputs settles it")
        psbt = node.walletcreatefundedpsbt(
            [], [{self.address: 1.0, "asset": self.asset}], 0,
            {"subtract_fee_from_outputs": [0]})
        assert_equal(psbt["fee_asset"], self.asset)

    def test_fee_output_determines(self):
        """A raw transaction's own fee output names the asset the fee is paid in.

        Note the fee output must carry a positive value: createrawtransaction
        drops a zero-value one, so `{"fee": 0, ...}` produces a transaction with no
        fee output at all and therefore no statement about the fee asset.
        """
        node = self.nodes[0]

        # Same code path as subtract-fee, same standing for both assets: the
        # transaction states the answer, so no argument is needed.
        self.log.info("fundrawtransaction: an explicit fee output determines the fee asset")
        for asset in (self.policy_asset, self.asset):
            raw = node.createrawtransaction(outputs=[
                {self.address: 1.0, "asset": asset},
                {"fee": 0.0001, "fee_asset": asset}])
            funded = node.fundrawtransaction(raw)
            assert_equal(funded["fee_asset"], asset)

        # It is stated, not chosen, so naming it is refused -- agreeing or not.
        self.log.info("fundrawtransaction: naming a fee asset alongside a fee output errors")
        raw = node.createrawtransaction(outputs=[
            {self.address: 1.0, "asset": self.asset},
            {"fee": 0.0001, "fee_asset": self.asset}])
        for named in ("bitcoin", self.asset):
            assert_raises_rpc_error(
                -8, ALREADY_DETERMINED, node.fundrawtransaction, raw, {"fee_asset": named})

        # The transaction may state it twice. Agreement is ordinary...
        self.log.info("fundrawtransaction: a fee output and subtract-fee that agree is fine")
        raw = node.createrawtransaction(outputs=[
            {self.address: 1.0, "asset": self.asset},
            {"fee": 0.0001, "fee_asset": self.asset}])
        funded = node.fundrawtransaction(raw, {"subtract_fee_from_outputs": [0]})
        assert_equal(funded["fee_asset"], self.asset)

        # ...and disagreement is impossible: the fee cannot both be denominated in
        # the fee output's asset and come out of an output of another.
        self.log.info("fundrawtransaction: a fee output contradicting subtract-fee errors")
        raw = node.createrawtransaction(outputs=[
            {self.address: 1.0, "asset": self.asset},
            {"fee": 0.0001, "fee_asset": self.policy_asset}])
        assert_raises_rpc_error(
            -8, TWO_WAYS, node.fundrawtransaction, raw, {"subtract_fee_from_outputs": [0]})

    def test_reissueasset(self):
        node = self.nodes[0]

        self.log.info("reissueasset: naming no fee asset errors")
        assert_raises_rpc_error(-8, MUST_NAME, node.reissueasset, self.asset, 1)

        self.log.info("reissueasset: an explicit fee asset is honoured verbatim")
        res = node.reissueasset(self.asset, 1, "bitcoin")
        # Not asserted as confirmed: reissuing against an UNBLINDED reissuance
        # token builds a transaction the network rejects (bad-txns-in-ne-out).
        # That defect is unrelated to fee-asset selection and is being fixed
        # separately; the fee asset the wallet settled on is still recorded and
        # is what this test is about.
        assert_equal(self.fee_assets(res["txid"], confirmed=False), ["bitcoin"])

    def test_unacceptable_asset_still_errors(self):
        """Only the default changed: an unusable explicit choice is still refused."""
        node = self.nodes[0]
        # The reissuance token has no exchange rate here, so no producer would
        # accept a fee in it. Naming it is an error, never a silent substitution.
        self.log.info("an explicit fee asset this node cannot price still errors")
        assert_raises_rpc_error(
            -6, "The chosen fee asset is not accepted",
            node.sendtoaddress, address=self.address, amount=1.0,
            assetlabel="bitcoin", fee_asset_label=self.token)


if __name__ == '__main__':
    AnyAssetFeeNoDefaultTest().main()
