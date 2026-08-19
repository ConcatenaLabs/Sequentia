#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""Pignus loan vault: non-custodial collateralised lending, enforced by consensus.

A borrower locks collateral in ONE taproot UTXO whose four leaves are the only
exits (internal key NUMS, so there is no key path). Every loan term -- the two
asset ids, the total repayment, both payout scriptPubKeys, the oracle key, the
price feed, the strike, the maturity -- is baked into the leaves and therefore
committed inside the taproot output key. Nobody can restate a term after
origination; a spender can only satisfy one exit exactly.

What this proves, and why each case is here:

  PASS   REPAY, oracle-free           the solvent exit works with no oracle, no
                                      signature and no witness data at all
  REJECT REPAY underpaying the lender
  REJECT REPAY returning collateral to a script that is not the borrower's
  REJECT REPAY returning less than the whole collateral
  PASS   LIQUIDATE under the strike   surplus computed on chain and forced home
  REJECT LIQUIDATE at a price above the strike (attestation genuine)
  REJECT LIQUIDATE on a forged attestation (right numbers, wrong signer)
  REJECT LIQUIDATE on an attestation for a DIFFERENT feed (right signer)
  REJECT LIQUIDATE on a stale attestation (ts < not_before)
  REJECT LIQUIDATE keeping the borrower's surplus (short by one atom)
  REJECT DEFAULT before maturity      the term is not up
  PASS   DEFAULT after maturity at a price ABOVE the strike -- the loan is
                                      callable at any price once due, and the
                                      surplus still goes home
  REJECT RECOVER before recover_after
  PASS   RECOVER after recover_after  the oracle-liveness backstop
  REJECT a blinded lender credit      the covenant cannot police what it cannot
                                      read, so it refuses to try
  REJECT output aliasing              one payment cannot settle two loans

Every rejection is the node's own script interpreter (code -26), asserted by
reject-reason, so a case that fails for an incidental reason cannot pass for
the wrong one.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, satoshi_round, BITCOIN_ASSET
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutNonce, CTxOutValue, uint256_from_str, tx_from_hex,
)
from test_framework.script import CScript, OP_1, TaprootSignatureHash

import pignus_covenant as pig

FEE = 5000                       # bitcoin network fee (atoms)

# The loan. Collateral is a gold-like asset C; the principal is a dollar-like
# asset D. Prices are quoted as D-atoms per C-atom, scaled by PRICE_SCALE.
COLLATERAL = 10 * COIN           # 10 units of C locked
DEBT = 1500 * COIN               # total repayment: principal plus term interest
PRICE_OPEN = 300 * pig.PRICE_SCALE   # 1 C unit = 300 D units at origination
STRIKE = 180 * pig.PRICE_SCALE       # liquidate under 120% collateralisation
PRICE_LOW = 170 * pig.PRICE_SCALE    # a genuine dip, under the strike
PRICE_HIGH = 400 * pig.PRICE_SCALE   # a rally, well over the strike
MAX_PRICE = 1_000_000 * pig.PRICE_SCALE   # declared ceiling, pins DEFAULT's bound
BONUS_NUM, BONUS_DEN = 105, 100  # the liquidator keeps a 5% bonus

NOT_BEFORE = 1_700_000_000       # origination: no earlier attestation counts
TS_GOOD = 1_800_000_000
TS_STALE = 1_600_000_000


class PignusVaultTest(BitcoinTestFramework):

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

    # --- small helpers -----------------------------------------------------

    def wallet_spk(self):
        addr = self.nodes[0].getnewaddress()
        unconf = self.nodes[0].getaddressinfo(addr)["unconfidential"]
        return bytes.fromhex(self.nodes[0].getaddressinfo(unconf)["scriptPubKey"])

    def addr_spk(self, addr):
        info = self.nodes[0].getaddressinfo(addr)
        unconf = info.get("unconfidential", addr)
        return bytes.fromhex(self.nodes[0].getaddressinfo(unconf)["scriptPubKey"])

    def asset_out(self, display_hex):
        return b"\x01" + bytes.fromhex(display_hex)[::-1]

    def ctxout(self, amount, spk, asset_out):
        return CTxOut(nValue=CTxOutValue(amount), scriptPubKey=spk,
                      nAsset=CTxOutAsset(asset_out))

    def fresh_segwit_utxo(self, amount, asset_display=None):
        """A fresh bech32 (segwit v0) utxo holding `amount` whole units, for the
        repayer's principal and the network fee."""
        node = self.nodes[0]
        bech = node.getnewaddress("", "bech32")
        unconf = node.getaddressinfo(bech)["unconfidential"]
        if asset_display is None:
            node.sendtoaddress(address=unconf, amount=amount,
                               fee_asset_label=BITCOIN_ASSET)
            target = BITCOIN_ASSET
        else:
            node.sendtoaddress(address=unconf, amount=amount, assetlabel=asset_display,
                               fee_asset_label=BITCOIN_ASSET)
            target = asset_display
        self.generate(node, 1)
        for u in node.listunspent():
            if (u["asset"] == target and abs(float(u["amount"]) - amount) < 1e-9
                    and u["scriptPubKey"].startswith("0014") and u["spendable"]):
                return u
        raise AssertionError("no fresh segwit utxo for %s" % target)

    def find_asset_utxo(self, asset_display, min_units):
        for u in self.nodes[0].listunspent():
            if u["asset"] == asset_display and float(u["amount"]) >= min_units and u["spendable"]:
                return u
        raise AssertionError("no wallet utxo of %s" % asset_display)

    def assert_rejected(self, tx, want="script-verify-flag-failed"):
        """Assert the node rejects the tx, and surface the reject reason so we can
        confirm WHICH layer refused it -- the covenant interpreter, not some
        incidental accounting error."""
        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert not res["allowed"], res
        reason = res.get("reject-reason", "")
        self.log.info("  reject-reason: %s", reason)
        assert want in reason, "expected %r in %r" % (want, reason)

    # --- the oracle --------------------------------------------------------

    def attest(self, price, timestamp=TS_GOOD, feed_id=None, sec=None):
        """A BIP340 signature over `feed_id || ts || price`, exactly the 48 bytes
        the covenant reassembles with OP_CAT. Defaults sign the real feed with
        the real oracle key; the arguments exist so the rejection cases can vary
        one thing at a time."""
        msg = pig.attestation_message(feed_id or self.feed_id, timestamp, price)
        return sign_schnorr(sec or self.oracle_sec, msg)

    # --- vault funding -----------------------------------------------------

    def fund_vaults(self, count):
        """Fund `count` loan vaults (COLLATERAL of asset C each) in one
        transaction. In production this is the origination transaction, atomic
        with the lender's principal payment to the borrower; here we only need
        the vaults to exist. Returns [(txid, vout), ...]."""
        node = self.nodes[0]
        need_units = count * (COLLATERAL // COIN) + 1
        c_utxo = self.find_asset_utxo(self.C_display, need_units)
        c_in = int(satoshi_round(c_utxo["amount"]) * COIN)
        btc = self.fresh_segwit_utxo(1)
        btc_in = int(satoshi_round(btc["amount"]) * COIN)

        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(c_utxo["txid"], 16), c_utxo["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        vault_spk = bytes(self.tap.scriptPubKey)
        for _ in range(count):
            tx.vout.append(self.ctxout(COLLATERAL, vault_spk, self.C_OUT))
        tx.vout.append(self.ctxout(c_in - count * COLLATERAL, self.wallet_spk(), self.C_OUT))
        tx.vout.append(self.ctxout(btc_in - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        txid = node.sendrawtransaction(signed["hex"])
        self.generate(node, 1)
        return [(txid, i) for i in range(count)]

    def assemble(self, vaults, wallet_ins, outs, witnesses, locktime=0):
        """Build a vault spend: covenant inputs first (consensus indices
        0..len-1), then wallet inputs. `outs` = [(amount, spk, asset_out or
        None-for-fee)]; `witnesses` = one witness stack per covenant input."""
        node = self.nodes[0]
        seq = 0xfffffffe if locktime else 0xffffffff
        tx = CTransaction()
        tx.nVersion = 2
        tx.nLockTime = locktime
        for (txid, vout) in vaults:
            tx.vin.append(CTxIn(COutPoint(int(txid, 16), vout), nSequence=seq))
        for u in wallet_ins:
            tx.vin.append(CTxIn(COutPoint(int(u["txid"], 16), u["vout"]), nSequence=seq))
        for (amt, spk, aout) in outs:
            tx.vout.append(CTxOut(CTxOutValue(amt)) if aout is None
                           else self.ctxout(amt, spk, aout))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        for i, w in enumerate(witnesses):
            tx.wit.vtxinwit[i].scriptWitness.stack = w
        return tx

    def spend_inputs(self, d_units=3000):
        """The principal/fee inputs a repayer or liquidator brings."""
        d_in = self.fresh_segwit_utxo(d_units, self.D_display)
        btc = self.fresh_segwit_utxo(1)
        return (d_in, int(satoshi_round(d_in["amount"]) * COIN),
                btc, int(satoshi_round(btc["amount"]) * COIN))

    # ---------------------------------------------------------------- repay

    def repay_tx(self, vault, *, credit=None, collateral_spk=None,
                 returned=None):
        """A REPAY spend. The keyword arguments exist so each rejection case can
        break exactly one rule."""
        d_in, d_amt, btc, btc_amt = self.spend_inputs()
        credit = DEBT if credit is None else credit
        returned = COLLATERAL if returned is None else returned
        collateral_spk = collateral_spk or self.borrower_spk
        outs = [
            (credit, self.lender_spk, self.D_OUT),               # 0 lender credit
            (returned, collateral_spk, self.C_OUT),              # 1 collateral home
        ]
        if returned < COLLATERAL:                                # keep C balanced
            outs.append((COLLATERAL - returned, self.wallet_spk(), self.C_OUT))
        outs += [
            (d_amt - credit, self.wallet_spk(), self.D_OUT),
            (btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
            (FEE, b"", None),
        ]
        return self.assemble([vault], [d_in, btc], outs,
                             [pig.repay_witness(self.tap, self.leaves)])

    # ------------------------------------------------------- seizure spends

    def seizure_tx(self, vault, leaf, price, *, sig=None, ts=TS_GOOD,
                   witness_price=None, surplus_delta=0, locktime=0):
        """A LIQUIDATE or DEFAULT spend at `price`. `witness_price` lets a case
        present a price the oracle did not sign; `surplus_delta` shorts the
        borrower's return."""
        d_in, d_amt, btc, btc_amt = self.spend_inputs()
        seize = pig.seizure_atoms(DEBT, price, BONUS_NUM, BONUS_DEN)
        surplus = COLLATERAL - seize
        assert surplus > 0, "test parameters must leave the borrower a surplus"
        paid_surplus = surplus - surplus_delta
        sig = sig if sig is not None else self.attest(price, ts)
        outs = [
            (DEBT, self.lender_spk, self.D_OUT),                 # 0 lender credit
            (paid_surplus, self.borrower_spk, self.C_OUT),       # 1 surplus home
            (COLLATERAL - paid_surplus, self.wallet_spk(), self.C_OUT),  # liquidator
            (d_amt - DEBT, self.wallet_spk(), self.D_OUT),
            (btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
            (FEE, b"", None),
        ]
        w = pig.oracle_witness(self.tap, self.leaves, leaf, sig,
                               witness_price if witness_price is not None else price, ts)
        return self.assemble([vault], [d_in, btc], outs, [w], locktime=locktime)

    # -------------------------------------------------------------- recover

    def recover_tx(self, vault, locktime):
        node = self.nodes[0]
        btc = self.fresh_segwit_utxo(1)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        tx = CTransaction()
        tx.nVersion = 2
        tx.nLockTime = locktime
        tx.vin.append(CTxIn(COutPoint(int(vault[0], 16), vault[1]), nSequence=0xfffffffe))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"]), nSequence=0xfffffffe))
        tx.vout.append(self.ctxout(COLLATERAL, self.lender_spk, self.C_OUT))
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        spent = [self.ctxout(COLLATERAL, bytes(self.tap.scriptPubKey), self.C_OUT),
                 self.ctxout(btc_amt, bytes.fromhex(btc["scriptPubKey"]),
                             b"\x01" + bytes.fromhex(btc["asset"])[::-1])]
        msg = TaprootSignatureHash(tx, spent, 0, self.genesis, 0,
                                   scriptpath=True, script=self.leaves["recover"])
        sig = sign_schnorr(self.lender_sec, msg)
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxinwit[0].scriptWitness.stack = pig.recover_witness(
            self.tap, self.leaves, sig)
        return tx

    # ----------------------------------------------------------- the test

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        # There is no coinbase subsidy on Sequentia; the initialfreecoins
        # anyone-can-spend output is the only bitcoin, so move it into ordinary
        # wallet utxos for multi-asset coin selection to work with.
        node.sendtoaddress(address=node.getnewaddress(), amount=1000000,
                           fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)
        self.genesis = uint256_from_str(bytes.fromhex(node.getblockhash(0))[::-1])

        # C is the collateral (a gold-like asset), D the principal (a dollar-like
        # asset). Both explicit: Sequentia is transparent by default.
        self.C_display = node.issueasset(assetamount=100000, tokenamount=0,
                                         blind=False, fee_asset=BITCOIN_ASSET)["asset"]
        self.generate(node, 1)
        self.D_display = node.issueasset(assetamount=1000000, tokenamount=0,
                                         blind=False, fee_asset=BITCOIN_ASSET)["asset"]
        self.generate(node, 1)
        self.C_OUT, self.D_OUT = self.asset_out(self.C_display), self.asset_out(self.D_display)
        self.BTC_OUT = b"\x01" + bytes.fromhex(BITCOIN_ASSET)[::-1]
        asset_c = bytes.fromhex(self.C_display)[::-1]      # internal byte order
        asset_d = bytes.fromhex(self.D_display)[::-1]

        # The three parties. Payout programs are v1 taproot; the lender's key
        # additionally authorises RECOVER, and the oracle's signs attestations.
        borrower_x = compute_xonly_pubkey(generate_privkey())[0]
        self.lender_sec = generate_privkey()
        lender_x = compute_xonly_pubkey(self.lender_sec)[0]
        self.oracle_sec = generate_privkey()
        oracle_x = compute_xonly_pubkey(self.oracle_sec)[0]
        self.borrower_spk = bytes(CScript([OP_1, borrower_x]))
        self.lender_spk = bytes(CScript([OP_1, lender_x]))
        self.feed_id = bytes.fromhex(
            "11" * 32)                                    # H("C/D") in production

        self.maturity = node.getblockcount() + 400        # absolute-height CLTV
        self.recover_after = self.maturity + 100
        self.tap, self.leaves = pig.vault_taptree(
            asset_c=asset_c, asset_d=asset_d, debt=DEBT,
            lender_prog=lender_x, borrower_prog=borrower_x, lender_x=lender_x,
            feed_id=self.feed_id, oracle_x=oracle_x, strike=STRIKE,
            maturity=self.maturity, recover_after=self.recover_after,
            not_before=NOT_BEFORE, bonus_num=BONUS_NUM, bonus_den=BONUS_DEN,
            max_price=MAX_PRICE)
        self.log.info("vault spk %s", bytes(self.tap.scriptPubKey).hex())
        for name, leaf in self.leaves.items():
            self.log.info("  leaf %-10s %4d bytes", name, len(bytes(leaf)))
        self.log.info("loan: %d C collateral, %d D debt, open %d, strike %d",
                      COLLATERAL // COIN, DEBT // COIN,
                      PRICE_OPEN // pig.PRICE_SCALE, STRIKE // pig.PRICE_SCALE)

        v = self.fund_vaults(16)
        self.log.info("funded 16 identical loan vaults of %d C each", COLLATERAL // COIN)

        self.repay_cases(v[0:4])
        self.liquidate_cases(v[4:10])
        self.aliasing_case(v[10:12])
        self.blinded_case(v[12])
        self.default_cases(v[13:15])
        self.recover_case(v[15])

        self.log.info("Pignus vault: every exit proven, every cheat refused by "
                      "the script interpreter")

    # ---------------------------------------------------------------- cases

    def repay_cases(self, vaults):
        node = self.nodes[0]
        ok, under, wrong_spk, short = vaults

        self.log.info("PASS: REPAY -- oracle-free, signature-free, permissionless")
        tx = self.repay_tx(ok)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(node.gettxout(txid, 0)["scriptPubKey"]["hex"], self.lender_spk.hex())
        assert_equal(satoshi_round(node.gettxout(txid, 0)["value"]) * COIN, Decimal(DEBT))
        assert_equal(node.gettxout(txid, 1)["scriptPubKey"]["hex"], self.borrower_spk.hex())
        assert_equal(satoshi_round(node.gettxout(txid, 1)["value"]) * COIN, Decimal(COLLATERAL))
        self.log.info("  lender paid %d D, borrower got all %d C back",
                      DEBT // COIN, COLLATERAL // COIN)

        self.log.info("REJECT: REPAY underpaying the lender by one atom")
        self.assert_rejected(self.repay_tx(under, credit=DEBT - 1))

        self.log.info("REJECT: REPAY sending the collateral to another script")
        self.assert_rejected(self.repay_tx(wrong_spk, collateral_spk=self.wallet_spk()))

        self.log.info("REJECT: REPAY keeping one atom of the collateral")
        self.assert_rejected(self.repay_tx(short, returned=COLLATERAL - 1))

    def liquidate_cases(self, vaults):
        node = self.nodes[0]
        ok, above, forged, wrongfeed, stale, greedy = vaults
        seize = pig.seizure_atoms(DEBT, PRICE_LOW, BONUS_NUM, BONUS_DEN)
        surplus = COLLATERAL - seize

        self.log.info("PASS: LIQUIDATE at %d (under the %d strike)",
                      PRICE_LOW // pig.PRICE_SCALE, STRIKE // pig.PRICE_SCALE)
        tx = self.seizure_tx(ok, "liquidate", PRICE_LOW)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(satoshi_round(node.gettxout(txid, 0)["value"]) * COIN, Decimal(DEBT))
        assert_equal(node.gettxout(txid, 1)["scriptPubKey"]["hex"], self.borrower_spk.hex())
        assert_equal(satoshi_round(node.gettxout(txid, 1)["value"]) * COIN, Decimal(surplus))
        # The liquidator's take is the baked bonus and nothing more: the seizure
        # covers `gross` and overshoots it by less than the value of a single
        # collateral atom, because the ceiling in `seize` rounds the last atom
        # the liquidator's way rather than letting a rounding loss strand the
        # position.
        taken = seize * PRICE_LOW // pig.PRICE_SCALE
        gross = pig.gross_owed(DEBT, BONUS_NUM, BONUS_DEN)
        assert gross <= taken < gross + PRICE_LOW // pig.PRICE_SCALE, (taken, gross)
        self.log.info("  lender paid %d D, liquidator kept %d C atoms (the 5%% bonus), "
                      "borrower kept the %d C atom surplus", DEBT // COIN, seize, surplus)

        self.log.info("REJECT: LIQUIDATE at %d -- genuine attestation, price over the strike",
                      PRICE_HIGH // pig.PRICE_SCALE)
        self.assert_rejected(self.seizure_tx(above, "liquidate", PRICE_HIGH))

        self.log.info("REJECT: LIQUIDATE on a forged attestation (wrong signer)")
        bad = sign_schnorr(generate_privkey(),
                           pig.attestation_message(self.feed_id, TS_GOOD, PRICE_LOW))
        self.assert_rejected(self.seizure_tx(forged, "liquidate", PRICE_LOW, sig=bad))

        self.log.info("REJECT: LIQUIDATE on an attestation for a DIFFERENT feed")
        other = self.attest(PRICE_LOW, feed_id=bytes.fromhex("22" * 32))
        self.assert_rejected(self.seizure_tx(wrongfeed, "liquidate", PRICE_LOW, sig=other))

        self.log.info("REJECT: LIQUIDATE on an attestation predating origination")
        self.assert_rejected(self.seizure_tx(stale, "liquidate", PRICE_LOW, ts=TS_STALE))

        self.log.info("REJECT: LIQUIDATE keeping one atom of the borrower's surplus")
        self.assert_rejected(self.seizure_tx(greedy, "liquidate", PRICE_LOW,
                                             surplus_delta=1))

    def aliasing_case(self, vaults):
        """Two vaults in one transaction, one lender credit. The input-bound
        output map sends the vault at consensus index 1 to look at output 2, so
        a single payment cannot close both loans."""
        self.log.info("REJECT: output aliasing -- one payment settling two loans")
        d_in, d_amt, btc, btc_amt = self.spend_inputs()
        w = pig.repay_witness(self.tap, self.leaves)
        tx = self.assemble(vaults, [d_in, btc], [
            (DEBT, self.lender_spk, self.D_OUT),                  # 0 the ONE credit
            (COLLATERAL, self.borrower_spk, self.C_OUT),          # 1 vault 0's collateral
            (COLLATERAL, self.borrower_spk, self.C_OUT),          # 2 vault 1 reads this as
                                                                  #   its credit: not asset D
            (d_amt - DEBT, self.wallet_spk(), self.D_OUT),
            (btc_amt - FEE, self.wallet_spk(), self.BTC_OUT),
            (FEE, b"", None),
        ], [w, w])
        self.assert_rejected(tx)

    def blinded_case(self, vault):
        """A REPAY whose lender credit is confidential. The commitment is REAL
        and balanced (built with rawblindrawtransaction), so it clears the amount
        layer and it is the covenant -- which asserts every introspected prefix
        is 0x01 -- that refuses it."""
        node = self.nodes[0]
        self.log.info("REJECT: blinded lender credit the covenant cannot read")
        d_in, d_amt, btc, btc_amt = self.spend_inputs()
        lender_conf = node.getnewaddress("", "blech32")
        change_conf = node.getnewaddress("", "blech32")
        lender_ck = bytes.fromhex(node.getaddressinfo(lender_conf)["confidential_key"])
        change_ck = bytes.fromhex(node.getaddressinfo(change_conf)["confidential_key"])

        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(vault[0], 16), vault[1])))
        tx.vin.append(CTxIn(COutPoint(int(d_in["txid"], 16), d_in["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        o0 = self.ctxout(DEBT, self.addr_spk(lender_conf), self.D_OUT)
        o0.nNonce = CTxOutNonce(lender_ck)                        # credit -> BLINDED
        o1 = self.ctxout(COLLATERAL, self.borrower_spk, self.C_OUT)
        o2 = self.ctxout(d_amt - DEBT, self.addr_spk(change_conf), self.D_OUT)
        o2.nNonce = CTxOutNonce(change_ck)                        # a second blinded output,
        tx.vout += [o0, o1, o2]                                   # so the blinders balance
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        z = "00" * 32   # every input is explicit -> zero blinders
        blinded = node.rawblindrawtransaction(
            tx.serialize().hex(), [z, z, z],
            [satoshi_round(Decimal(COLLATERAL) / COIN), satoshi_round(d_in["amount"]),
             satoshi_round(btc["amount"])],
            [self.C_display, self.D_display, BITCOIN_ASSET], [z, z, z], "", False)
        tx = tx_from_hex(blinded)
        assert tx.vout[0].nAsset.vchCommitment[0] in (0x0a, 0x0b), \
            tx.vout[0].nAsset.vchCommitment[0]
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxinwit[0].scriptWitness.stack = pig.repay_witness(self.tap, self.leaves)
        self.assert_rejected(tx)

    def default_cases(self, vaults):
        node = self.nodes[0]
        early, due = vaults

        self.log.info("REJECT: DEFAULT before maturity (height %d, maturity %d)",
                      node.getblockcount(), self.maturity)
        self.assert_rejected(self.seizure_tx(early, "default", PRICE_HIGH,
                                             locktime=node.getblockcount()))

        self.generate(node, self.maturity - node.getblockcount() + 1)
        self.log.info("PASS: DEFAULT after maturity at %d -- ABOVE the strike, so "
                      "this could never have been a liquidation",
                      PRICE_HIGH // pig.PRICE_SCALE)
        seize = pig.seizure_atoms(DEBT, PRICE_HIGH, BONUS_NUM, BONUS_DEN)
        tx = self.seizure_tx(due, "default", PRICE_HIGH, locktime=self.maturity)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(satoshi_round(node.gettxout(txid, 0)["value"]) * COIN, Decimal(DEBT))
        assert_equal(node.gettxout(txid, 1)["scriptPubKey"]["hex"], self.borrower_spk.hex())
        assert_equal(satoshi_round(node.gettxout(txid, 1)["value"]) * COIN,
                     Decimal(COLLATERAL - seize))
        self.log.info("  called at maturity: lender paid, borrower kept the %d C "
                      "atom surplus a higher price earned them", COLLATERAL - seize)

    def recover_case(self, vault):
        node = self.nodes[0]
        self.log.info("REJECT: RECOVER before the backstop height %d", self.recover_after)
        self.assert_rejected(self.recover_tx(vault, node.getblockcount()))

        self.generate(node, self.recover_after - node.getblockcount() + 1)
        self.log.info("PASS: RECOVER after %d -- the oracle-liveness backstop",
                      self.recover_after)
        tx = self.recover_tx(vault, self.recover_after)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(node.gettxout(txid, 0)["scriptPubKey"]["hex"], self.lender_spk.hex())


if __name__ == "__main__":
    PignusVaultTest().main()
