#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Reissue an asset whose reissuance token sits on an UNBLINDED output.

Elements marks a transaction input as a reissuance by giving
assetIssuance.assetBlindingNonce a non-null value, and it populates that nonce
with the asset BLINDING FACTOR of the reissuance token being spent. Consensus
then re-derives the token's blinded asset tag from (token id, nonce) and compares
it against the spent output's asset commitment.

A token held on an unblinded output has a blinding factor of zero. The nonce
therefore came out null, consensus read the input as a brand new issuance rather
than a reissuance, and the wallet emitted a transaction that could never confirm
while still marking the token spent -- a phantom.

That is harmless on Elements, where addresses are confidential by default, but
Sequentia deliberately flipped that default (m_default_blinded_addresses = false):
addresses are explicit bech32 and confidentiality is opt-in. Reissuance therefore
has to work with the token on an unblinded output just as well as on a blinded
one, which is what this test pins down.

Node 0 runs with the default unblinded addresses, node 1 with -blindedaddresses=1
so the blinded path is exercised unchanged.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

NULL_BLINDER = "00" * 32


class UnblindedReissuanceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Node 0 keeps the chain default (unblinded addresses, see the test config
        # written by the framework); node 1 opts in to confidential addresses.
        self.extra_args = [[], ["-blindedaddresses=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.sync_all()

    def find_token_utxo(self, node, token):
        """Return the single listunspent entry holding `token`."""
        utxos = [u for u in node.listunspent() if u["asset"] == token]
        assert_equal(len(utxos), 1)
        return utxos[0]

    def assert_confirmed(self, node, txid):
        assert_equal(node.gettransaction(txid)["confirmations"], 1)

    def run_test(self):
        self.generate(self.nodes[0], 101)
        self.generate(self.nodes[1], 101)
        self.sync_all()

        self.test_unblinded_token_reissuance()
        self.test_blinded_token_reissuance()
        self.test_wrong_explicit_token_rejected()

    def test_unblinded_token_reissuance(self):
        """The fix: token on an explicit output, reissuance must confirm."""
        self.log.info("Reissuing from an UNBLINDED reissuance token")
        node = self.nodes[0]

        # SEQUENTIA: an open-fee-market chain has no default fee asset, and an
        # issuance has no output for the fee to come out of.
        issued = node.issueasset(assetamount=10, tokenamount=1, blind=False,
                                 fee_asset='bitcoin')
        asset, token = issued["asset"], issued["token"]
        self.generate(node, 1)
        self.sync_all()

        assert_equal(node.getwalletinfo()["balance"][asset], 10)
        assert_equal(node.getwalletinfo()["balance"][token], 1)

        # The point of the test: this token really is on an unblinded output, so
        # its asset blinding factor -- the value Elements puts in the nonce -- is
        # zero.
        token_utxo = self.find_token_utxo(node, token)
        assert_equal(token_utxo["assetblinder"], NULL_BLINDER)
        assert_equal(token_utxo["amountblinder"], NULL_BLINDER)

        reissue = node.reissueasset(asset, 5, 'bitcoin')
        # Before the fix the transaction was built with a null nonce, was read by
        # consensus as a new issuance, and never entered a block. Requiring a
        # confirmation is what makes this test meaningful.
        self.generate(node, 1)
        self.sync_all()
        self.assert_confirmed(node, reissue["txid"])

        assert_equal(node.getwalletinfo()["balance"][asset], 15)
        # Reissuing does not consume the token.
        assert_equal(node.getwalletinfo()["balance"][token], 1)

        # The issuance input must have been recorded as a reissuance, not as a new
        # issuance: a non-null nonce and the original entropy.
        raw = node.decoderawtransaction(node.gettransaction(reissue["txid"])["hex"])
        issuance = raw["vin"][reissue["vin"]]["issuance"]
        assert issuance["assetBlindingNonce"] != NULL_BLINDER
        assert_equal(issuance["asset"], asset)
        assert_equal(Decimal(str(issuance["assetamount"])), Decimal(5))

        # And the token is still spendable, so the authority to reissue survived.
        second = node.reissueasset(asset, 2, 'bitcoin')
        self.generate(node, 1)
        self.sync_all()
        self.assert_confirmed(node, second["txid"])
        assert_equal(node.getwalletinfo()["balance"][asset], 17)

        self.unblinded_asset = asset
        self.unblinded_token = token

    def test_blinded_token_reissuance(self):
        """Mirror case: the pre-existing blinded path must be unaffected."""
        self.log.info("Reissuing from a BLINDED reissuance token (no regression)")
        node = self.nodes[1]

        issued = node.issueasset(assetamount=10, tokenamount=1, blind=True,
                                 fee_asset='bitcoin')
        asset, token = issued["asset"], issued["token"]
        self.generate(node, 1)
        self.sync_all()

        # Node 1 runs with -blindedaddresses=1, so the token landed on a
        # confidential output and carries a real blinding factor.
        token_utxo = self.find_token_utxo(node, token)
        assert token_utxo["assetblinder"] != NULL_BLINDER

        reissue = node.reissueasset(asset, 5, 'bitcoin')
        self.generate(node, 1)
        self.sync_all()
        self.assert_confirmed(node, reissue["txid"])
        assert_equal(node.getwalletinfo()["balance"][asset], 15)

        raw = node.decoderawtransaction(node.gettransaction(reissue["txid"])["hex"])
        issuance = raw["vin"][reissue["vin"]]["issuance"]
        # The blinded path still carries the token's real blinding factor, not the
        # explicit-token sentinel.
        assert issuance["assetBlindingNonce"] != NULL_BLINDER

    def test_wrong_explicit_token_rejected(self):
        """A reissuance input whose explicit asset is not the token is invalid."""
        self.log.info("Rejecting a reissuance whose explicit input is the wrong asset")
        node = self.nodes[0]
        asset = self.unblinded_asset

        entropy = node.listissuances(asset)[0]["entropy"]

        # Build a funded transaction and attach a reissuance of `asset` to an input
        # that does NOT hold the reissuance token. Every input here is the policy
        # asset, held explicitly, so consensus derives the reissuance token id and
        # finds it does not equal the input's explicit asset.
        addr = node.getnewaddress()
        raw = node.createrawtransaction([], [{addr: Decimal("1.0")}])
        funded = node.fundrawtransaction(
            raw, {"feeRate": Decimal("0.00050000"), "fee_asset": "bitcoin"})["hex"]

        decoded = node.decoderawtransaction(funded)
        assert_greater_than(len(decoded["vin"]), 0)
        # Confirm the chosen input is not the reissuance token.
        chosen = decoded["vin"][0]
        prev_tx = node.decoderawtransaction(node.gettransaction(chosen["txid"])["hex"])
        prev = prev_tx["vout"][chosen["vout"]]
        assert prev["asset"] != self.unblinded_token

        reissued = node.rawreissueasset(funded, [{
            "asset_amount": 5,
            "asset_address": node.getnewaddress(),
            "input_index": 0,
            "asset_blinder": NULL_BLINDER,
            "entropy": entropy,
        }])["hex"]

        signed = node.signrawtransactionwithwallet(reissued)
        assert_raises_rpc_error(-26, None, node.sendrawtransaction, signed["hex"])

        # The chain is unharmed: supply did not move.
        assert_equal(node.getwalletinfo()["balance"][asset], 17)


if __name__ == "__main__":
    UnblindedReissuanceTest().main()
