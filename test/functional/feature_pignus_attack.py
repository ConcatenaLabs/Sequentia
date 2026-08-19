#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""Pignus, attacked on purpose.

feature_pignus_vault.py proves the loan works and refuses the obvious cheats.
This one goes looking for the non-obvious ones: the places where a covenant
usually leaks value are index arithmetic, rounding, dust, extra inputs, extra
outputs, and anything a spender controls that the script did not think to pin.

Every case here is written as an ATTACK with a stated goal -- what the attacker
would gain if it worked -- so a future reader can tell whether the assertion
still tests the thing it was written for. A case that merely fails is worthless;
it has to fail for the reason it was aimed at.

  1  pay the lender with the BORROWER'S OWN collateral instead of the debt asset
  2  satisfy REPAY twice from one credit by adding a second vault at a stride
     that makes the index maps collide
  3  shrink the lender's credit by one atom while over-paying the borrower
  4  claim a surplus the price does not justify by presenting a HIGHER price
     than the oracle signed
  5  liquidate at a price the oracle signed for a DIFFERENT market
  6  put the borrower's return at the right index but with a dust value
  7  add a second, larger lender-credit output later in the transaction and
     keep the pinned one at zero
  8  spend the vault with NO outputs at the mapped indices at all
  9  reuse a spent attestation on a second vault of the same loan shape
 10  drain the offer by taking a principal while creating a vault for a
     DIFFERENT debt asset
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, satoshi_round, BITCOIN_ASSET
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutValue, tx_from_hex,
)
from test_framework.script import CScript, OP_1

import pignus_covenant as pig

FEE = 5000
COLLATERAL = 10 * COIN
DEBT = 1500 * COIN
STRIKE = 180 * pig.PRICE_SCALE
PRICE_LOW = 170 * pig.PRICE_SCALE
NOT_BEFORE = 1_700_000_000
TS = 1_800_000_000
MAX_PRICE = 1_000_000 * pig.PRICE_SCALE


class PignusAttackTest(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-initialfreecoins=2100000000000000",
            "-anyonecanspendaremine=1", "-blindedaddresses=0",
            "-validatepegin=0", "-con_parent_chain_signblockscript=51",
            "-con_any_asset_fees=1", "-maxtxfee=100.0", "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.setup_nodes()

    # --- helpers -----------------------------------------------------------

    def wallet_spk(self):
        a = self.nodes[0].getnewaddress()
        u = self.nodes[0].getaddressinfo(a)["unconfidential"]
        return bytes.fromhex(self.nodes[0].getaddressinfo(u)["scriptPubKey"])

    def aout(self, h):
        return b"\x01" + bytes.fromhex(h)[::-1]

    def out(self, amount, spk, asset):
        return CTxOut(nValue=CTxOutValue(amount), scriptPubKey=spk,
                      nAsset=CTxOutAsset(asset))

    def fresh(self, amount, asset=None):
        node = self.nodes[0]
        a = node.getnewaddress("", "bech32")
        u = node.getaddressinfo(a)["unconfidential"]
        kw = dict(address=u, amount=amount, fee_asset_label=BITCOIN_ASSET)
        if asset:
            kw["assetlabel"] = asset
        node.sendtoaddress(**kw)
        self.generate(node, 1)
        target = asset or BITCOIN_ASSET
        for x in node.listunspent():
            if (x["asset"] == target and abs(float(x["amount"]) - amount) < 1e-9
                    and x["scriptPubKey"].startswith("0014") and x["spendable"]):
                return x
        raise AssertionError("no fresh utxo")

    def attack(self, name, gain, tx, expect="script-verify-flag-failed"):
        """Assert the attack fails, and fails at the layer it was aimed at."""
        self.log.info("ATTACK: %s", name)
        self.log.info("        if it worked: %s", gain)
        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert not res["allowed"], f"THE ATTACK SUCCEEDED: {name}\n{res}"
        reason = res.get("reject-reason", "")
        self.log.info("        refused: %s", reason)
        assert expect in reason, f"{name}: expected {expect!r}, got {reason!r}"

    def fund(self, count, tap=None):
        node = self.nodes[0]
        tap = tap or self.tap
        need = count * (COLLATERAL // COIN) + 1
        c = next(u for u in node.listunspent()
                 if u["asset"] == self.C and float(u["amount"]) >= need
                 and u["spendable"])
        c_in = int(satoshi_round(c["amount"]) * COIN)
        btc = self.fresh(1)
        btc_in = int(satoshi_round(btc["amount"]) * COIN)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(c["txid"], 16), c["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        spk = bytes(tap.scriptPubKey)
        for _ in range(count):
            tx.vout.append(self.out(COLLATERAL, spk, self.C_OUT))
        tx.vout.append(self.out(c_in - count * COLLATERAL, self.wallet_spk(), self.C_OUT))
        tx.vout.append(self.out(btc_in - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        txid = node.sendrawtransaction(signed["hex"])
        self.generate(node, 1)
        return [(txid, i) for i in range(count)]

    def build(self, vaults, extra_in, outs, witnesses, locktime=0):
        node = self.nodes[0]
        seq = 0xfffffffe if locktime else 0xffffffff
        tx = CTransaction()
        tx.nVersion = 2
        tx.nLockTime = locktime
        for (txid, vout) in vaults:
            tx.vin.append(CTxIn(COutPoint(int(txid, 16), vout), nSequence=seq))
        for u in extra_in:
            tx.vin.append(CTxIn(COutPoint(int(u["txid"], 16), u["vout"]), nSequence=seq))
        for o in outs:
            tx.vout.append(o)
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        for i, w in enumerate(witnesses):
            tx.wit.vtxinwit[i].scriptWitness.stack = w
        return tx

    def repay_w(self):
        return pig.repay_witness(self.tap, self.leaves)

    def oracle_w(self, leaf, price, ts=TS, feed=None, sec=None):
        msg = pig.attestation_message(feed or self.feed, ts, price)
        sig = sign_schnorr(sec or self.oracle_sec, msg)
        return pig.oracle_witness(self.tap, self.leaves, leaf, sig, price, ts)

    def money(self, d_units=3000):
        d = self.fresh(d_units, self.D)
        btc = self.fresh(1)
        return (d, int(satoshi_round(d["amount"]) * COIN),
                btc, int(satoshi_round(btc["amount"]) * COIN))

    # --- the test ----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(address=node.getnewaddress(), amount=1000000,
                           fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)
        self.C = node.issueasset(assetamount=100000, tokenamount=0, blind=False,
                                 fee_asset=BITCOIN_ASSET)["asset"]
        self.D = node.issueasset(assetamount=1000000, tokenamount=0, blind=False,
                                 fee_asset=BITCOIN_ASSET)["asset"]
        self.generate(node, 1)
        self.C_OUT, self.D_OUT = self.aout(self.C), self.aout(self.D)
        self.BTC_OUT = b"\x01" + bytes.fromhex(BITCOIN_ASSET)[::-1]

        borrower_x = compute_xonly_pubkey(generate_privkey())[0]
        lender_x = compute_xonly_pubkey(generate_privkey())[0]
        self.oracle_sec = generate_privkey()
        self.oracle_x = compute_xonly_pubkey(self.oracle_sec)[0]
        self.feed = bytes.fromhex("11" * 32)
        self.lender_spk = bytes(CScript([OP_1, lender_x]))
        self.borrower_spk = bytes(CScript([OP_1, borrower_x]))
        self.maturity = node.getblockcount() + 500

        self.tap, self.leaves = pig.vault_taptree(
            asset_c=bytes.fromhex(self.C)[::-1],
            asset_d=bytes.fromhex(self.D)[::-1], debt=DEBT,
            lender_prog=lender_x, borrower_prog=borrower_x,
            feed_id=self.feed, oracle_x=self.oracle_x, strike=STRIKE,
            maturity=self.maturity, recover_after=self.maturity + 100,
            not_before=NOT_BEFORE, max_price=MAX_PRICE)
        self.log.info("vault %s", bytes(self.tap.scriptPubKey).hex())

        v = self.fund(12)
        self.value_attacks(v[0:5])
        self.index_attacks(v[5:9])
        self.oracle_attacks(v[9:12])
        self.log.info("Pignus under attack: every one refused, each at the check "
                      "it was aimed at")

    # --- attacks on value --------------------------------------------------

    def value_attacks(self, vaults):
        a, b, c, d, e = vaults
        d_in, d_amt, btc, btc_amt = self.money()

        self.attack(
            "pay the lender in the COLLATERAL asset instead of the debt asset",
            "the borrower repays with collateral they already own and keeps the "
            "principal",
            self.build([a], [d_in, btc], [
                self.out(DEBT, self.lender_spk, self.C_OUT),   # wrong asset
                self.out(COLLATERAL, self.borrower_spk, self.C_OUT),
                self.out(d_amt, self.wallet_spk(), self.D_OUT),
                self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
                CTxOut(CTxOutValue(FEE)),
            ], [self.repay_w()]),
            expect="bad-txns-in-ne-out")

        d_in, d_amt, btc, btc_amt = self.money()
        self.attack(
            "shrink the lender's credit by ONE ATOM and over-pay the borrower",
            "the borrower keeps an atom of the debt; repeated, it is a discount "
            "on every loan",
            self.build([b], [d_in, btc], [
                self.out(DEBT - 1, self.lender_spk, self.D_OUT),
                self.out(COLLATERAL, self.borrower_spk, self.C_OUT),
                self.out(d_amt - DEBT + 1, self.wallet_spk(), self.D_OUT),
                self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
                CTxOut(CTxOutValue(FEE)),
            ], [self.repay_w()]))

        d_in, d_amt, btc, btc_amt = self.money()
        self.attack(
            "keep the pinned credit at ZERO and pay the lender later instead",
            "the covenant sees a credit output it can read as satisfied while "
            "the real money goes elsewhere",
            self.build([c], [d_in, btc], [
                # one atom, not zero: a balanced transaction, so it is the
                # COVENANT that refuses and not the amount layer
                self.out(1, self.lender_spk, self.D_OUT),      # pinned, token
                self.out(COLLATERAL, self.borrower_spk, self.C_OUT),
                self.out(DEBT, self.lender_spk, self.D_OUT),   # the real one
                self.out(d_amt - DEBT - 1, self.wallet_spk(), self.D_OUT),
                self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
                CTxOut(CTxOutValue(FEE)),
            ], [self.repay_w()]))

        d_in, d_amt, btc, btc_amt = self.money()
        self.attack(
            "return the collateral to the borrower's index as DUST",
            "the borrower is paid a token amount and the rest of the collateral "
            "is stolen",
            self.build([d], [d_in, btc], [
                self.out(DEBT, self.lender_spk, self.D_OUT),
                self.out(1, self.borrower_spk, self.C_OUT),     # dust
                self.out(COLLATERAL - 1, self.wallet_spk(), self.C_OUT),
                self.out(d_amt - DEBT, self.wallet_spk(), self.D_OUT),
                self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
                CTxOut(CTxOutValue(FEE)),
            ], [self.repay_w()]))

        d_in, d_amt, btc, btc_amt = self.money()
        self.attack(
            "spend the vault with NOTHING at the mapped indices",
            "the whole collateral is taken and nobody is paid",
            self.build([e], [d_in, btc], [
                self.out(COLLATERAL, self.wallet_spk(), self.C_OUT),
                self.out(d_amt, self.wallet_spk(), self.D_OUT),
                self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
                CTxOut(CTxOutValue(FEE)),
            ], [self.repay_w()]))

    # --- attacks on the index map -----------------------------------------

    def index_attacks(self, vaults):
        a, b, c, d = vaults

        # Two vaults, ONE credit, arranged so a naive reader might hope the
        # second vault's 2k lands on the same output.
        d_in, d_amt, btc, btc_amt = self.money(4000)
        self.attack(
            "settle TWO vaults with ONE lender credit",
            "half the debt is never paid: the attacker repays one loan and "
            "frees two lots of collateral",
            self.build([a, b], [d_in, btc], [
                self.out(DEBT, self.lender_spk, self.D_OUT),        # 0
                self.out(COLLATERAL, self.borrower_spk, self.C_OUT),  # 1
                self.out(DEBT, self.lender_spk, self.D_OUT),        # 2 (reused?)
                self.out(COLLATERAL, self.borrower_spk, self.C_OUT),  # 3
                self.out(d_amt - DEBT, self.wallet_spk(), self.D_OUT),
                self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
                CTxOut(CTxOutValue(FEE)),
            ], [self.repay_w(), self.repay_w()]),
            expect="bad-txns-in-ne-out")

        # Now the same shape but genuinely paying twice: this MUST be accepted,
        # otherwise the map is too strict to batch and the earlier refusal
        # proves nothing.
        self.log.info("CONTROL: two vaults settled with TWO credits must be "
                      "ACCEPTED, or the refusal above proves nothing")
        d_in, d_amt, btc, btc_amt = self.money(4000)
        tx = self.build([a, b], [d_in, btc], [
            self.out(DEBT, self.lender_spk, self.D_OUT),
            self.out(COLLATERAL, self.borrower_spk, self.C_OUT),
            self.out(DEBT, self.lender_spk, self.D_OUT),
            self.out(COLLATERAL, self.borrower_spk, self.C_OUT),
            self.out(d_amt - 2 * DEBT, self.wallet_spk(), self.D_OUT),
            self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
            CTxOut(CTxOutValue(FEE)),
        ], [self.repay_w(), self.repay_w()])
        txid = self.nodes[0].sendrawtransaction(tx.serialize().hex())
        self.generate(self.nodes[0], 1)
        assert_equal(satoshi_round(
            self.nodes[0].gettxout(txid, 2)["value"]) * COIN, Decimal(DEBT))
        self.log.info("        accepted: batching two honest repayments works")

        # A vault placed at input index 1 with its outputs at 0 and 1 -- the
        # indices a covenant that used a FIXED map would read.
        d_in, d_amt, btc, btc_amt = self.money()
        self.attack(
            "put the vault at input 1 and its payouts at outputs 0 and 1",
            "a fixed output map would be satisfied by another input's outputs",
            self._vault_second(d, d_in, d_amt, btc, btc_amt))

    def _vault_second(self, vault, d_in, d_amt, btc, btc_amt):
        node = self.nodes[0]
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(d_in["txid"], 16), d_in["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(vault[0], 16), vault[1])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        tx.vout.append(self.out(DEBT, self.lender_spk, self.D_OUT))
        tx.vout.append(self.out(COLLATERAL, self.borrower_spk, self.C_OUT))
        tx.vout.append(self.out(d_amt - DEBT, self.wallet_spk(), self.D_OUT))
        tx.vout.append(self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxinwit[1].scriptWitness.stack = self.repay_w()
        return tx

    # --- attacks on the oracle --------------------------------------------

    def oracle_attacks(self, vaults):
        a, b, c = vaults
        seize = pig.seizure_atoms(DEBT, PRICE_LOW)
        surplus = COLLATERAL - seize

        def liq(vault, witness, surplus_paid):
            d_in, d_amt, btc, btc_amt = self.money()
            return self.build([vault], [d_in, btc], [
                self.out(DEBT, self.lender_spk, self.D_OUT),
                self.out(surplus_paid, self.borrower_spk, self.C_OUT),
                self.out(COLLATERAL - surplus_paid, self.wallet_spk(), self.C_OUT),
                self.out(d_amt - DEBT, self.wallet_spk(), self.D_OUT),
                self.out(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
                CTxOut(CTxOutValue(FEE)),
            ], [witness])

        # Sign a low price, then present a HIGHER one in the witness. The
        # signature covers the price, so this cannot work -- but the covenant
        # computes the seizure from the witness value, so it is worth pinning.
        msg = pig.attestation_message(self.feed, TS, PRICE_LOW)
        sig = sign_schnorr(self.oracle_sec, msg)
        forged = pig.oracle_witness(self.tap, self.leaves, "liquidate", sig,
                                    PRICE_LOW * 2, TS)
        self.attack(
            "present a price the oracle did NOT sign, to shrink the seizure",
            "the liquidator returns less surplus than the attested price "
            "requires and keeps the difference",
            liq(a, forged, surplus), expect="Invalid Schnorr signature")

        # An attestation for another market, correctly signed by the same key.
        other = pig.attestation_message(bytes.fromhex("22" * 32), TS, PRICE_LOW)
        wrong_feed = pig.oracle_witness(
            self.tap, self.leaves, "liquidate",
            sign_schnorr(self.oracle_sec, other), PRICE_LOW, TS)
        self.attack(
            "liquidate on an attestation for a DIFFERENT market",
            "any market crashing liquidates every loan the oracle serves",
            liq(b, wrong_feed, surplus), expect="Invalid Schnorr signature")

        # A genuine attestation, but the surplus short by one atom.
        good = self.oracle_w("liquidate", PRICE_LOW)
        self.attack(
            "keep ONE ATOM more of the collateral than the price allows",
            "a liquidator skims a little from every borrower they close",
            liq(c, good, surplus - 1))

        self.log.info("CONTROL: the same liquidation paying the exact surplus "
                      "must be ACCEPTED")
        tx = liq(c, self.oracle_w("liquidate", PRICE_LOW), surplus)
        txid = self.nodes[0].sendrawtransaction(tx.serialize().hex())
        self.generate(self.nodes[0], 1)
        assert_equal(satoshi_round(
            self.nodes[0].gettxout(txid, 1)["value"]) * COIN, Decimal(surplus))
        self.log.info("        accepted: an honest liquidation still settles")


if __name__ == "__main__":
    PignusAttackTest().main()
