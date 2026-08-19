#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""Funded resting loan offers: a lender's principal taken by an offline lender.

The lender locks the principal in a covenant and goes away. A borrower turns up
later, locks collateral in a correctly formed vault and draws the principal, in
one transaction the lender never sees. The covenant satisfies itself that the
vault really is a Pignus vault on the agreed terms by RECONSTRUCTING its taproot
address in script from the borrower key in the witness -- streaming SHA-256 for
the leaf and the tweak, OP_TWEAKVERIFY for the point addition.

Proven here:

  PASS   a taker draws one principal; the remainder re-rests at the same offer
  PASS   a second taker draws the rest, from the re-rested offer
  PASS   the vault the offer created really works: REPAY settles it
  PASS   and so does LIQUIDATE, so the single-leaf format enforces what the
         four-leaf one does
  REJECT a vault built for a DIFFERENT borrower key than the witness claims
  REJECT a vault whose terms differ from the offer's by one atom of debt
  REJECT under-collateralising the vault
  REJECT the wrong collateral asset
  REJECT drawing more than one principal
  REJECT drawing less than one principal
  REJECT sending the remainder to a different script
  REJECT the wrong taproot parity byte
  REJECT the offer refund before expiry; PASS after
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, satoshi_round, BITCOIN_ASSET
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutValue, uint256_from_str, tx_from_hex,
)
from test_framework.script import CScript, OP_1, TaprootSignatureHash

import pignus_covenant as pig
import pignus_offer as off

FEE = 5000
COLLATERAL = 10 * COIN
PRINCIPAL = 1450 * COIN
DEBT = 1500 * COIN
STRIKE = 180 * pig.PRICE_SCALE
PRICE_LOW = 170 * pig.PRICE_SCALE
NOT_BEFORE = 1_700_000_000
TS = 1_800_000_000
LOTS = 3                      # the offer is funded with three loans' worth


class PignusOfferTest(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-initialfreecoins=2100000000000000",
            "-anyonecanspendaremine=1",
            "-blindedaddresses=0",
            "-validatepegin=0",
            "-con_parent_chain_signblockscript=51",
            "-con_any_asset_fees=1",
            "-maxtxfee=100.0",
            "-txindex=1",
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

    def asset_out(self, h):
        return b"\x01" + bytes.fromhex(h)[::-1]

    def ctxout(self, amount, spk, aout):
        return CTxOut(nValue=CTxOutValue(amount), scriptPubKey=spk,
                      nAsset=CTxOutAsset(aout))

    def fresh(self, amount, asset=None):
        node = self.nodes[0]
        bech = node.getnewaddress("", "bech32")
        unconf = node.getaddressinfo(bech)["unconfidential"]
        kw = dict(address=unconf, amount=amount, fee_asset_label=BITCOIN_ASSET)
        if asset is not None:
            kw["assetlabel"] = asset
        node.sendtoaddress(**kw)
        self.generate(node, 1)
        target = asset or BITCOIN_ASSET
        for u in node.listunspent():
            if (u["asset"] == target and abs(float(u["amount"]) - amount) < 1e-9
                    and u["scriptPubKey"].startswith("0014") and u["spendable"]):
                return u
        raise AssertionError("no fresh utxo for %s" % target)

    def assert_rejected(self, tx, want="script-verify-flag-failed"):
        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert not res["allowed"], res
        reason = res.get("reject-reason", "")
        self.log.info("  reject-reason: %s", reason)
        assert want in reason, "expected %r in %r" % (want, reason)

    def vault_kwargs(self, **over):
        kw = dict(asset_c=self.asset_c, asset_d=self.asset_d, debt=DEBT,
                  lender_prog=self.lender_x, lender_x=self.lender_x,
                  feed_id=self.feed, oracle_x=self.oracle_x, strike=STRIKE,
                  maturity=self.maturity, recover_after=self.maturity + 100,
                  not_before=NOT_BEFORE, max_price=1_000_000 * pig.PRICE_SCALE)
        kw.update(over)
        return kw

    def vault_for(self, borrower_x, **over):
        return off.offer_vault_taptree(borrower_prog=borrower_x,
                                       **self.vault_kwargs(**over))

    # --- building a take ---------------------------------------------------

    def take_tx(self, offer_utxo, offer_value, borrower_x, *,
                vault_tap=None, vault_spk=None, collateral=COLLATERAL,
                collateral_asset=None, draw=PRINCIPAL, remainder=None,
                remainder_spk=None, parity=None, witness_x=None, btc_units=1):
        """Build a TAKE. Every keyword exists so one rejection case can break
        exactly one rule and leave the rest honest."""
        node = self.nodes[0]
        if vault_tap is None:
            vault_tap, _leaf = self.vault_for(borrower_x)
        if vault_spk is None:
            vault_spk = bytes(vault_tap.scriptPubKey)
        if remainder is None:
            remainder = offer_value - draw
        c_in = self.fresh(COLLATERAL // COIN + 5, self.C)
        c_amt = int(satoshi_round(c_in["amount"]) * COIN)
        btc = self.fresh(btc_units)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        c_out = collateral_asset or self.C_OUT

        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(offer_utxo[0], 16), offer_utxo[1])))
        tx.vin.append(CTxIn(COutPoint(int(c_in["txid"], 16), c_in["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))

        # Output 0 is the vault. Output 1 is the offer's remainder when there is
        # one; when there is not, it must be something that is NOT the debt
        # asset, because the covenant reads asset D at 2k+1 as a remainder claim.
        tx.vout.append(self.ctxout(collateral, vault_spk, c_out))
        c_change = c_amt - (collateral if c_out == self.C_OUT else 0)
        if remainder > 0:
            tx.vout.append(self.ctxout(
                remainder, remainder_spk or bytes(self.offer_tap.scriptPubKey),
                self.D_OUT))
        else:
            # Taking the WHOLE offer still needs an output at 2k+1 that is not
            # the debt asset: the covenant reads asset D there as a remainder
            # claim and would demand it be re-paid to itself. The collateral
            # change is the natural thing to put there.
            tx.vout.append(self.ctxout(c_change, self.wallet_spk(), self.C_OUT))
            c_change = 0
        # Whatever the borrower drew.
        if draw > 0:
            tx.vout.append(self.ctxout(draw, self.wallet_spk(), self.D_OUT))
        # Change, computed so every asset balances however the case broke the
        # rules -- an unbalanced transaction is refused before script
        # verification, which would prove nothing about the covenant.
        if c_change > 0:
            tx.vout.append(self.ctxout(c_change, self.wallet_spk(), self.C_OUT))
        btc_change = btc_amt - FEE - (collateral if c_out == self.BTC_OUT else 0)
        tx.vout.append(self.ctxout(btc_change, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))

        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        pbyte = parity if parity is not None else bytes(
            [0x03 if vault_tap.negflag else 0x02])
        tx.wit.vtxinwit[0].scriptWitness.stack = [
            pbyte, witness_x or borrower_x,
            bytes(self.offer_leaves["take"]), off.control_block(self.offer_tap, "take")]
        return tx

    def fund_offer(self, value):
        node = self.nodes[0]
        d = self.fresh(value // COIN + 10, self.D)
        d_amt = int(satoshi_round(d["amount"]) * COIN)
        btc = self.fresh(1)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(d["txid"], 16), d["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        tx.vout.append(self.ctxout(value, bytes(self.offer_tap.scriptPubKey), self.D_OUT))
        tx.vout.append(self.ctxout(d_amt - value, self.wallet_spk(), self.D_OUT))
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        txid = node.sendrawtransaction(signed["hex"])
        self.generate(node, 1)
        return (txid, 0)

    # --- the test ----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(address=node.getnewaddress(), amount=1000000,
                           fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)
        self.genesis = uint256_from_str(bytes.fromhex(node.getblockhash(0))[::-1])

        self.C = node.issueasset(assetamount=100000, tokenamount=0, blind=False,
                                 fee_asset=BITCOIN_ASSET)["asset"]
        self.generate(node, 1)
        self.D = node.issueasset(assetamount=1000000, tokenamount=0, blind=False,
                                 fee_asset=BITCOIN_ASSET)["asset"]
        self.generate(node, 1)
        self.C_OUT, self.D_OUT = self.asset_out(self.C), self.asset_out(self.D)
        self.BTC_OUT = b"\x01" + bytes.fromhex(BITCOIN_ASSET)[::-1]
        self.asset_c = bytes.fromhex(self.C)[::-1]
        self.asset_d = bytes.fromhex(self.D)[::-1]

        self.lender_sec = generate_privkey()
        self.lender_x = compute_xonly_pubkey(self.lender_sec)[0]
        self.oracle_sec = generate_privkey()
        self.oracle_x = compute_xonly_pubkey(self.oracle_sec)[0]
        self.feed = bytes.fromhex("11" * 32)
        self.maturity = node.getblockcount() + 400
        self.expiry = node.getblockcount() + 200

        self.offer_tap, self.offer_leaves = off.offer_taptree(
            asset_c=self.asset_c, asset_d=self.asset_d, principal=PRINCIPAL,
            collateral=COLLATERAL, vault_kwargs=self.vault_kwargs(),
            expiry_locktime=self.expiry, lender_x=self.lender_x)
        self.log.info("offer spk %s", bytes(self.offer_tap.scriptPubKey).hex())
        self.log.info("  TAKE leaf %d bytes, single-leaf vault %d bytes",
                      len(bytes(self.offer_leaves["take"])),
                      len(bytes(off.offer_vault_leaf(
                          borrower_prog=bytes(32), **self.vault_kwargs()))))

        self.take_and_rest()
        self.reject_cases()
        self.vault_works()
        self.refund_case()
        self.log.info("Pignus funded offers: an offline lender's principal drawn "
                      "by a borrower the offer verified in script")

    def take_and_rest(self):
        node = self.nodes[0]
        self.log.info("PASS: a taker draws one principal, the rest re-rests")
        self.offer0 = self.fund_offer(LOTS * PRINCIPAL)
        self.borrower_sec = generate_privkey()
        self.borrower_x = compute_xonly_pubkey(self.borrower_sec)[0]
        vault_tap, self.vault_leaf = self.vault_for(self.borrower_x)
        self.vault_tap = vault_tap

        tx = self.take_tx(self.offer0, LOTS * PRINCIPAL, self.borrower_x,
                          vault_tap=vault_tap)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(node.gettxout(txid, 0)["scriptPubKey"]["hex"],
                     bytes(vault_tap.scriptPubKey).hex())
        assert_equal(satoshi_round(node.gettxout(txid, 0)["value"]) * COIN,
                     Decimal(COLLATERAL))
        assert_equal(node.gettxout(txid, 1)["scriptPubKey"]["hex"],
                     bytes(self.offer_tap.scriptPubKey).hex())
        assert_equal(satoshi_round(node.gettxout(txid, 1)["value"]) * COIN,
                     Decimal((LOTS - 1) * PRINCIPAL))
        self.vault0 = (txid, 0)
        self.offer1 = (txid, 1)
        self.log.info("  vault created at the reconstructed address; %d principal "
                      "re-rested for the next borrower", LOTS - 1)

        self.log.info("PASS: a second, different borrower draws from the remainder")
        sec2 = generate_privkey()
        x2 = compute_xonly_pubkey(sec2)[0]
        vt2, _ = self.vault_for(x2)
        tx = self.take_tx(self.offer1, (LOTS - 1) * PRINCIPAL, x2, vault_tap=vt2)
        txid2 = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(node.gettxout(txid2, 0)["scriptPubKey"]["hex"],
                     bytes(vt2.scriptPubKey).hex())
        assert bytes(vt2.scriptPubKey) != bytes(vault_tap.scriptPubKey)
        self.offer2 = (txid2, 1)
        self.log.info("  a different key produced a different vault, as it must")

    def reject_cases(self):
        node = self.nodes[0]
        offer = self.fund_offer(2 * PRINCIPAL)
        value = 2 * PRINCIPAL
        bx = compute_xonly_pubkey(generate_privkey())[0]
        other = compute_xonly_pubkey(generate_privkey())[0]

        self.log.info("REJECT: the vault is for a different key than the witness claims")
        vt_other, _ = self.vault_for(other)
        self.assert_rejected(self.take_tx(offer, value, bx, vault_tap=vt_other,
                                          witness_x=bx))

        self.log.info("REJECT: the vault's debt differs from the offer's by one atom")
        vt_bad, _ = self.vault_for(bx, debt=DEBT + 1)
        self.assert_rejected(self.take_tx(offer, value, bx, vault_tap=vt_bad))

        self.log.info("REJECT: the vault is under-collateralised")
        self.assert_rejected(self.take_tx(offer, value, bx,
                                          collateral=COLLATERAL - 1))

        # A third asset, so the transaction still balances and it is the
        # covenant -- not the amount layer -- that refuses it.
        self.log.info("REJECT: the wrong asset in the vault")
        self.assert_rejected(self.take_tx(offer, value, bx,
                                          collateral_asset=self.BTC_OUT,
                                          btc_units=30))

        self.log.info("REJECT: drawing more than one principal")
        self.assert_rejected(self.take_tx(offer, value, bx, draw=PRINCIPAL + 1,
                                          remainder=value - PRINCIPAL - 1))

        self.log.info("REJECT: drawing less than one principal")
        self.assert_rejected(self.take_tx(offer, value, bx, draw=PRINCIPAL - 1,
                                          remainder=value - PRINCIPAL + 1))

        self.log.info("REJECT: the remainder sent somewhere other than the offer")
        self.assert_rejected(self.take_tx(offer, value, bx,
                                          remainder=value - PRINCIPAL,
                                          remainder_spk=self.wallet_spk()))

        self.log.info("REJECT: the wrong taproot parity byte")
        vt, _ = self.vault_for(bx)
        flipped = bytes([0x02 if vt.negflag else 0x03])
        self.assert_rejected(self.take_tx(offer, value, bx, vault_tap=vt,
                                          parity=flipped))

    def vault_works(self):
        """The point of the exercise: the vault the offer built is a real loan
        vault. Settle one by REPAY and one by LIQUIDATE, through the single-leaf
        format, so the offer path is not just address arithmetic."""
        node = self.nodes[0]
        self.log.info("PASS: REPAY settles a vault the offer created")
        lender_spk = bytes(CScript([OP_1, self.lender_x]))
        borrower_spk = bytes(CScript([OP_1, self.borrower_x]))
        d = self.fresh(DEBT // COIN + 10, self.D)
        d_amt = int(satoshi_round(d["amount"]) * COIN)
        btc = self.fresh(1)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(self.vault0[0], 16), self.vault0[1])))
        tx.vin.append(CTxIn(COutPoint(int(d["txid"], 16), d["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        tx.vout.append(self.ctxout(DEBT, lender_spk, self.D_OUT))
        tx.vout.append(self.ctxout(COLLATERAL, borrower_spk, self.C_OUT))
        tx.vout.append(self.ctxout(d_amt - DEBT, self.wallet_spk(), self.D_OUT))
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxinwit[0].scriptWitness.stack = off.vault_witness(
            self.vault_tap, self.vault_leaf, "repay")
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(node.gettxout(txid, 1)["scriptPubKey"]["hex"], borrower_spk.hex())
        self.log.info("  lender paid, collateral home -- the single-leaf vault "
                      "enforces what the four-leaf one does")

        self.log.info("PASS: LIQUIDATE settles another one, oracle-attested")
        # Build a fresh loan through the offer so there is a live vault to hit.
        bx = compute_xonly_pubkey(generate_privkey())[0]
        vt, leaf = self.vault_for(bx)
        take = self.take_tx(self.offer2, (LOTS - 2) * PRINCIPAL, bx,
                            vault_tap=vt, remainder=0)
        ttxid = node.sendrawtransaction(take.serialize().hex())
        self.generate(node, 1)

        seize = pig.seizure_atoms(DEBT, PRICE_LOW)
        surplus = COLLATERAL - seize
        sig = sign_schnorr(self.oracle_sec,
                           pig.attestation_message(self.feed, TS, PRICE_LOW))
        d = self.fresh(DEBT // COIN + 10, self.D)
        d_amt = int(satoshi_round(d["amount"]) * COIN)
        btc = self.fresh(1)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(ttxid, 16), 0)))
        tx.vin.append(CTxIn(COutPoint(int(d["txid"], 16), d["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        tx.vout.append(self.ctxout(DEBT, lender_spk, self.D_OUT))
        tx.vout.append(self.ctxout(surplus, bytes(CScript([OP_1, bx])), self.C_OUT))
        tx.vout.append(self.ctxout(seize, self.wallet_spk(), self.C_OUT))
        tx.vout.append(self.ctxout(d_amt - DEBT, self.wallet_spk(), self.D_OUT))
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxinwit[0].scriptWitness.stack = off.vault_witness(
            vt, leaf, "liquidate",
            data=[sig, pig.le8(PRICE_LOW), pig.le8(TS)])
        ltxid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(satoshi_round(node.gettxout(ltxid, 1)["value"]) * COIN,
                     Decimal(surplus))
        self.log.info("  liquidated at %d: lender made whole, borrower kept %d",
                      PRICE_LOW, surplus)

    def refund_case(self):
        node = self.nodes[0]
        offer = self.fund_offer(PRINCIPAL)
        self.log.info("REJECT: the lender withdraws the offer before expiry")
        self.assert_rejected(self.build_refund(offer, node.getblockcount()),
                             "Locktime requirement not satisfied")
        self.generate(node, self.expiry - node.getblockcount() + 1)
        self.log.info("PASS: and succeeds after it")
        tx = self.build_refund(offer, self.expiry)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(satoshi_round(node.gettxout(txid, 0)["value"]) * COIN,
                     Decimal(PRINCIPAL))

    def build_refund(self, offer, locktime):
        node = self.nodes[0]
        btc = self.fresh(1)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        dest = self.wallet_spk()
        tx = CTransaction()
        tx.nVersion = 2
        tx.nLockTime = locktime
        tx.vin.append(CTxIn(COutPoint(int(offer[0], 16), offer[1]), nSequence=0xfffffffe))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"]), nSequence=0xfffffffe))
        tx.vout.append(self.ctxout(PRINCIPAL, dest, self.D_OUT))
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        spent = [self.ctxout(PRINCIPAL, bytes(self.offer_tap.scriptPubKey), self.D_OUT),
                 self.ctxout(btc_amt, bytes.fromhex(btc["scriptPubKey"]),
                             b"\x01" + bytes.fromhex(btc["asset"])[::-1])]
        msg = TaprootSignatureHash(tx, spent, 0, self.genesis, 0, scriptpath=True,
                                   script=self.offer_leaves["refund"])
        sig = sign_schnorr(self.lender_sec, msg)
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxinwit[0].scriptWitness.stack = off.offer_refund_witness(
            self.offer_tap, self.offer_leaves, sig)
        return tx


if __name__ == "__main__":
    PignusOfferTest().main()
