#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""Pignus threshold oracle sets: m-of-n independent price signers, in consensus.

A single oracle key is one thing to compromise and one thing to lose. A vault
may instead name a SET of oracle keys and a threshold, and the covenant counts
the valid signatures itself -- so no oracle has to know the others exist, none of
them run a joint signing protocol, and there is no coordinator to attack.

Each oracle signs its OWN `(timestamp, price)`; they never have to agree on a
byte. That matters, because requiring several independent price sources to emit
an identical timestamp and an identical price is a coordination protocol, not an
oracle set. Each accepted price must independently clear the strike, and the
price carried into the seizure is the MAXIMUM of the accepted ones -- the
borrower-favourable choice, and the one that makes shopping for a low price
pointless.

Proven here:

  PASS   2-of-3 signed by oracles 0 and 1
  PASS   2-of-3 signed by oracles 0 and 2   (which slot abstains does not matter)
  PASS   2-of-3 signed by oracles 1 and 2   (no privileged first key)
  PASS   3-of-3, and the seizure uses the HIGHEST accepted price, not the lowest
  REJECT only one oracle signs                 (under the threshold)
  REJECT two sign but one price is over the strike
  REJECT two sign but one attestation is stale
  REJECT a slot filled with a non-empty INVALID signature (aborts, cannot be
         passed off as an abstention)
  REJECT one oracle's signature replayed into a second slot for a different key
  PASS   DEFAULT at maturity under the same threshold rule
  builder REFUSES a duplicate key in the set, and a threshold outside 1..n
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
LOW_A = 170 * pig.PRICE_SCALE      # under the strike
LOW_B = 175 * pig.PRICE_SCALE      # also under, but HIGHER: the one that counts
HIGH = 400 * pig.PRICE_SCALE       # over the strike
NOT_BEFORE = 1_700_000_000
TS = 1_800_000_000
TS_STALE = 1_600_000_000
THRESHOLD = 2


class PignusOracleSetTest(BitcoinTestFramework):

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
        raise AssertionError("no fresh utxo")

    def assert_rejected(self, tx, want="script-verify-flag-failed"):
        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert not res["allowed"], res
        reason = res.get("reject-reason", "")
        self.log.info("  reject-reason: %s", reason)
        assert want in reason, "expected %r in %r" % (want, reason)

    def attest(self, idx, price, ts=TS, feed=None, sec=None):
        """A signature from oracle `idx` over its own (ts, price)."""
        msg = pig.attestation_message(feed or self.feed, ts, price)
        return sign_schnorr(sec or self.oracle_secs[idx], msg)

    def slots(self, entries):
        """entries: {index: (price, ts, sig)} -> a full slot list with the rest
        abstaining."""
        return [entries.get(i) for i in range(len(self.oracle_xs))]

    def fund(self, count):
        node = self.nodes[0]
        need = count * (COLLATERAL // COIN) + 1
        c = next(u for u in node.listunspent()
                 if u["asset"] == self.C and float(u["amount"]) >= need and u["spendable"])
        c_in = int(satoshi_round(c["amount"]) * COIN)
        btc = self.fresh(1)
        btc_in = int(satoshi_round(btc["amount"]) * COIN)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(c["txid"], 16), c["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        spk = bytes(self.tap.scriptPubKey)
        for _ in range(count):
            tx.vout.append(self.ctxout(COLLATERAL, spk, self.C_OUT))
        tx.vout.append(self.ctxout(c_in - count * COLLATERAL, self.wallet_spk(), self.C_OUT))
        tx.vout.append(self.ctxout(btc_in - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        txid = node.sendrawtransaction(signed["hex"])
        self.generate(node, 1)
        return [(txid, i) for i in range(count)]

    def seizure_tx(self, vault, leaf, slots, price_for_layout, locktime=0):
        """Build a seizure whose outputs are laid out for `price_for_layout`.
        Passing a layout price that differs from what the covenant will compute
        is how the max-price rule gets tested."""
        node = self.nodes[0]
        d = self.fresh(DEBT // COIN + 10, self.D)
        d_amt = int(satoshi_round(d["amount"]) * COIN)
        btc = self.fresh(1)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        seize = pig.seizure_atoms(DEBT, price_for_layout)
        surplus = COLLATERAL - seize
        assert surplus > 0
        seq = 0xfffffffe if locktime else 0xffffffff
        tx = CTransaction()
        tx.nVersion = 2
        tx.nLockTime = locktime
        tx.vin.append(CTxIn(COutPoint(int(vault[0], 16), vault[1]), nSequence=seq))
        tx.vin.append(CTxIn(COutPoint(int(d["txid"], 16), d["vout"]), nSequence=seq))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"]), nSequence=seq))
        tx.vout.append(self.ctxout(DEBT, self.lender_spk, self.D_OUT))          # 0
        tx.vout.append(self.ctxout(surplus, self.borrower_spk, self.C_OUT))     # 1
        tx.vout.append(self.ctxout(seize, self.wallet_spk(), self.C_OUT))
        tx.vout.append(self.ctxout(d_amt - DEBT, self.wallet_spk(), self.D_OUT))
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(), self.BTC_OUT))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxinwit[0].scriptWitness.stack = pig.threshold_oracle_witness(
            self.tap, self.leaves, leaf, slots)
        return tx

    # --- the test ----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.builder_refusals()

        self.generate(node, 101)
        node.sendtoaddress(address=node.getnewaddress(), amount=1000000,
                           fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)
        self.C = node.issueasset(assetamount=100000, tokenamount=0, blind=False,
                                 fee_asset=BITCOIN_ASSET)["asset"]
        self.generate(node, 1)
        self.D = node.issueasset(assetamount=1000000, tokenamount=0, blind=False,
                                 fee_asset=BITCOIN_ASSET)["asset"]
        self.generate(node, 1)
        self.C_OUT, self.D_OUT = self.asset_out(self.C), self.asset_out(self.D)
        self.BTC_OUT = b"\x01" + bytes.fromhex(BITCOIN_ASSET)[::-1]

        borrower_x = compute_xonly_pubkey(generate_privkey())[0]
        lender_x = compute_xonly_pubkey(generate_privkey())[0]
        self.borrower_spk = bytes(CScript([OP_1, borrower_x]))
        self.lender_spk = bytes(CScript([OP_1, lender_x]))
        self.oracle_secs = [generate_privkey() for _ in range(3)]
        self.oracle_xs = [compute_xonly_pubkey(s)[0] for s in self.oracle_secs]
        self.feed = bytes.fromhex("11" * 32)

        self.maturity = node.getblockcount() + 300
        self.tap, self.leaves = pig.vault_taptree(
            asset_c=bytes.fromhex(self.C)[::-1], asset_d=bytes.fromhex(self.D)[::-1],
            debt=DEBT, lender_prog=lender_x, borrower_prog=borrower_x,
            lender_x=lender_x, feed_id=self.feed,
            oracles=self.oracle_xs, oracle_threshold=THRESHOLD,
            strike=STRIKE, maturity=self.maturity,
            recover_after=self.maturity + 100, not_before=NOT_BEFORE,
            max_price=1_000_000 * pig.PRICE_SCALE)
        self.log.info("2-of-3 vault spk %s", bytes(self.tap.scriptPubKey).hex())
        for n, l in self.leaves.items():
            self.log.info("  leaf %-10s %4d bytes", n, len(bytes(l)))

        v = self.fund(10)
        self.log.info("funded 10 threshold vaults")

        self.pass_cases(v[0:4])
        self.reject_cases(v[4:9])
        self.default_case(v[9])
        self.log.info("Pignus oracle sets: threshold enforced by the interpreter, "
                      "abstention is position-independent, the max price wins")

    def builder_refusals(self):
        """Terms that would produce a vault whose threshold does not mean what it
        says are refused at construction, not discovered later."""
        self.log.info("builder refuses malformed oracle sets")
        k = [bytes([i]) * 32 for i in range(1, 4)]
        common = dict(asset_c=bytes(32), asset_d=bytes(32), debt=1000,
                      lender_prog=bytes(32), borrower_prog=bytes(32),
                      feed_id=bytes(32), strike=1000, not_before=0)
        for label, kw in [
            ("duplicate key", dict(oracle_x=None, oracles=[k[0], k[1], k[0]],
                                   oracle_threshold=2)),
            ("threshold above n", dict(oracle_x=None, oracles=k, oracle_threshold=4)),
            ("threshold zero", dict(oracle_x=None, oracles=k, oracle_threshold=0)),
            ("both forms", dict(oracle_x=k[0], oracles=k, oracle_threshold=2)),
            ("neither form", dict(oracle_x=None)),
            ("threshold with one key", dict(oracle_x=k[0], oracle_threshold=2)),
        ]:
            try:
                pig.build_liquidate_leaf(**common, **kw)
                raise AssertionError(f"builder accepted {label}")
            except ValueError as e:
                self.log.info("  refused %-18s %s", label, str(e)[:60])

    def pass_cases(self, vaults):
        node = self.nodes[0]
        a, b, c, d = vaults

        for label, vault, idxs in [("oracles 0+1", a, (0, 1)),
                                   ("oracles 0+2", b, (0, 2)),
                                   ("oracles 1+2", c, (1, 2))]:
            self.log.info("PASS: 2-of-3 signed by %s", label)
            slots = self.slots({i: (self.attest(i, LOW_A), LOW_A, TS) for i in idxs})
            tx = self.seizure_tx(vault, "liquidate", slots, LOW_A)
            txid = node.sendrawtransaction(tx.serialize().hex())
            self.generate(node, 1)
            assert_equal(satoshi_round(node.gettxout(txid, 0)["value"]) * COIN,
                         Decimal(DEBT))
            self.log.info("  settled; the abstaining slot's position did not matter")

        # All three sign, at DIFFERENT prices. The covenant must use the HIGHEST,
        # which is the borrower-favourable one -- so a liquidator cannot drag the
        # seizure up by adding a low attestation.
        self.log.info("PASS: 3-of-3 at mixed prices -- the HIGHEST is used")
        slots = self.slots({0: (self.attest(0, LOW_A), LOW_A, TS),
                            1: (self.attest(1, LOW_B), LOW_B, TS),
                            2: (self.attest(2, LOW_A), LOW_A, TS)})
        seize_high = pig.seizure_atoms(DEBT, LOW_B)
        seize_low = pig.seizure_atoms(DEBT, LOW_A)
        assert seize_high < seize_low, "test needs the prices to differ materially"
        # Laid out for the LOW price (a greedier seizure): the covenant must
        # refuse, because it computes the surplus from the maximum.
        greedy = self.seizure_tx(d, "liquidate", slots, LOW_A)
        self.assert_rejected(greedy)
        self.log.info("  refused a seizure sized by the LOWEST price")
        tx = self.seizure_tx(d, "liquidate", slots, LOW_B)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(satoshi_round(node.gettxout(txid, 1)["value"]) * COIN,
                     Decimal(COLLATERAL - seize_high))
        self.log.info("  accepted at the highest price: borrower kept %d atoms "
                      "instead of %d", COLLATERAL - seize_high, COLLATERAL - seize_low)

    def reject_cases(self, vaults):
        one, over, stale, garbage, replay = vaults

        self.log.info("REJECT: only one of three oracles signs")
        slots = self.slots({0: (self.attest(0, LOW_A), LOW_A, TS)})
        self.assert_rejected(self.seizure_tx(one, "liquidate", slots, LOW_A))

        self.log.info("REJECT: two sign, but one price is over the strike")
        slots = self.slots({0: (self.attest(0, LOW_A), LOW_A, TS),
                            1: (self.attest(1, HIGH), HIGH, TS)})
        self.assert_rejected(self.seizure_tx(over, "liquidate", slots, LOW_A))

        self.log.info("REJECT: two sign, but one attestation predates the loan")
        slots = self.slots({0: (self.attest(0, LOW_A), LOW_A, TS),
                            1: (self.attest(1, LOW_A, ts=TS_STALE), LOW_A, TS_STALE)})
        self.assert_rejected(self.seizure_tx(stale, "liquidate", slots, LOW_A))

        # A non-empty invalid signature ABORTS rather than counting as an
        # abstention: OP_CHECKSIGFROMSTACK only pushes false for an EMPTY
        # signature. So a slot cannot be stuffed with rubbish to look absent.
        self.log.info("REJECT: a slot filled with a non-empty invalid signature")
        bad = sign_schnorr(generate_privkey(),
                           pig.attestation_message(self.feed, TS, LOW_A))
        slots = self.slots({0: (self.attest(0, LOW_A), LOW_A, TS),
                            1: (self.attest(1, LOW_A), LOW_A, TS),
                            2: (bad, LOW_A, TS)})
        self.assert_rejected(self.seizure_tx(garbage, "liquidate", slots, LOW_A))

        # Oracle 0's signature moved into oracle 1's slot: each slot pins its own
        # key, so one compromised signer cannot fill the threshold alone.
        self.log.info("REJECT: one oracle's signature replayed into another slot")
        sig0 = self.attest(0, LOW_A)
        slots = self.slots({0: (sig0, LOW_A, TS), 1: (sig0, LOW_A, TS)})
        self.assert_rejected(self.seizure_tx(replay, "liquidate", slots, LOW_A))

    def default_case(self, vault):
        node = self.nodes[0]
        self.log.info("PASS: DEFAULT at maturity under the same threshold rule")
        self.generate(node, self.maturity - node.getblockcount() + 1)
        # Above the strike: this could never have been a liquidation.
        slots = self.slots({0: (self.attest(0, HIGH), HIGH, TS),
                            2: (self.attest(2, HIGH), HIGH, TS)})
        tx = self.seizure_tx(vault, "default", slots, HIGH, locktime=self.maturity)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        seize = pig.seizure_atoms(DEBT, HIGH)
        assert_equal(satoshi_round(node.gettxout(txid, 1)["value"]) * COIN,
                     Decimal(COLLATERAL - seize))
        self.log.info("  called at maturity by 2 of 3, surplus %d returned",
                      COLLATERAL - seize)


if __name__ == "__main__":
    PignusOracleSetTest().main()
