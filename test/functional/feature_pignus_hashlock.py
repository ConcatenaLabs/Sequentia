#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""The hashlock sweep: how a fact crosses from Sequentia to Bitcoin.

Bitcoin has no introspection, so a loan collateralised in native Bitcoin cannot
run the vault covenant on the parent chain. What binds the two halves together
is a secret: one side is paid on Sequentia only by publishing it, and publishing
it is what completes a Bitcoin spend signed in advance. This is the Sequentia
half of that -- a two-leaf output with

  CLAIM   SHA256 <h> EQUALVERIFY, then the whole input value to a pinned payee
  REFUND  <deadline> CLTV DROP,   then the whole input value to the sender

Neither leaf takes a signature, which is the property that lets a browser wallet
drive a cross-chain loan: the extension can sign its own inputs, not a covenant
leaf. Anyone may trigger either one, and that is safe because the payout is
pinned in both -- publishing the secret can only ever pay the party it was
always going to pay.

Proven here, against a real node:

  PASS   the payee claims with the right preimage, paid at their pinned program
  PASS   a v0 (browser wallet) payout program works as well as a taproot one
  PASS   the sender refunds after the deadline
  PASS   anyone may broadcast the claim; it still pays only the payee
  REJECT a claim with the wrong preimage
  REJECT a claim with no preimage at all
  REJECT a claim that pays the right amount to somebody else
  REJECT a claim that pays the payee less than the whole input
  REJECT a claim paying the payee in a different asset
  REJECT the refund before its deadline
"""

import hashlib

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, satoshi_round, BITCOIN_ASSET
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutValue, tx_from_hex,
)

import pignus_covenant as pig

FEE = 5000
VALUE = 1500 * COIN
DEADLINE = 400


class PignusHashlockTest(BitcoinTestFramework):

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

    def wallet_spk(self, kind="bech32"):
        a = self.nodes[0].getnewaddress("", kind)
        u = self.nodes[0].getaddressinfo(a)["unconfidential"]
        return bytes.fromhex(self.nodes[0].getaddressinfo(u)["scriptPubKey"])

    def raw_v1_spk(self, fill=b"\xee"):
        """A version-1 output that is nobody's in particular. What matters here
        is the covenant's rule about WHERE it pays, not who can spend after."""
        return b"\x51\x20" + fill * 32

    def asset_out(self, h):
        return b"\x01" + bytes.fromhex(h)[::-1]

    def ctxout(self, amount, spk, aout):
        return CTxOut(nValue=CTxOutValue(amount), scriptPubKey=spk,
                      nAsset=CTxOutAsset(aout))

    def fresh(self, amount, asset=None):
        node = self.nodes[0]
        unconf = node.getaddressinfo(node.getnewaddress("", "bech32"))["unconfidential"]
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

    def fund(self, spk, value, asset):
        """Put `value` of `asset` into a covenant output and return its outpoint."""
        node = self.nodes[0]
        d = self.fresh(value // COIN + 10, asset)
        d_amt = int(satoshi_round(d["amount"]) * COIN)
        btc = self.fresh(1)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(d["txid"], 16), d["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"])))
        tx.vout.append(self.ctxout(value, spk, self.asset_out(asset)))
        if d_amt - value > 0:
            tx.vout.append(self.ctxout(d_amt - value, self.wallet_spk(),
                                       self.asset_out(asset)))
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(),
                                   self.asset_out(BITCOIN_ASSET)))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        txid = node.sendrawtransaction(signed["hex"])
        self.generate(node, 1)
        return txid, 0

    def spend(self, tap, leaves, leaf, outpoint, *, value, asset, payee_spk,
              preimage=None, locktime=0, pay=None, pay_asset=None,
              witness=None):
        """A spend of the hashlock output. Every keyword exists so one rejection
        case can break exactly one rule and leave everything else honest."""
        node = self.nodes[0]
        btc = self.fresh(1)
        btc_amt = int(satoshi_round(btc["amount"]) * COIN)
        seq_no = 0xfffffffe if locktime else 0xffffffff
        paid = value if pay is None else pay
        paid_asset = pay_asset or asset
        tx = CTransaction()
        tx.nVersion = 2
        tx.nLockTime = locktime
        tx.vin.append(CTxIn(COutPoint(int(outpoint[0], 16), outpoint[1]),
                            nSequence=seq_no))
        tx.vin.append(CTxIn(COutPoint(int(btc["txid"], 16), btc["vout"]),
                            nSequence=seq_no))
        # Paying in a different asset needs that asset as an input, or the
        # transaction is refused for not balancing and proves nothing about the
        # covenant. Same for paying less than the whole input: the remainder
        # goes to change.
        extra = None
        if paid_asset != asset:
            extra = self.fresh(paid // COIN + 10, paid_asset)
            tx.vin.append(CTxIn(COutPoint(int(extra["txid"], 16), extra["vout"]),
                                nSequence=seq_no))
        # Output 0 is the credit the covenant inspects (input index 0 -> 2k = 0).
        tx.vout.append(self.ctxout(paid, payee_spk, self.asset_out(paid_asset)))
        back = value - (paid if paid_asset == asset else 0)
        if back > 0:
            tx.vout.append(self.ctxout(back, self.wallet_spk(),
                                       self.asset_out(asset)))
        if extra is not None:
            change = int(satoshi_round(extra["amount"]) * COIN) - paid
            if change > 0:
                tx.vout.append(self.ctxout(change, self.wallet_spk(),
                                           self.asset_out(paid_asset)))
        tx.vout.append(self.ctxout(btc_amt - FEE, self.wallet_spk(),
                                   self.asset_out(BITCOIN_ASSET)))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        if witness is None:
            witness = (pig.hashlock_witness(tap, leaves, leaf, preimage)
                       if preimage is not None
                       else [bytes(leaves[leaf]), pig.control_block(tap, leaf)])
        tx.wit.vtxinwit[0].scriptWitness.stack = witness
        return tx

    # --- the test ----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 200)
        node.sendtoaddress(address=node.getnewaddress(), amount=1000000,
                           fee_asset_label=BITCOIN_ASSET)
        self.generate(node, 1)
        self.D = node.issueasset(assetamount=2000000, tokenamount=0, blind=False,
                                 fee_asset=BITCOIN_ASSET)["asset"]
        self.other = node.issueasset(assetamount=1000000, tokenamount=0,
                                     blind=False, fee_asset=BITCOIN_ASSET)["asset"]
        self.generate(node, 1)

        secret = hashlib.sha256(b"pignus origination secret").digest()
        h = hashlib.sha256(secret).digest()
        payee_spk = self.raw_v1_spk(b"\xee")            # v1, 32-byte program
        sender_spk = self.raw_v1_spk(b"\xcc")
        asset_c = bytes.fromhex(self.D)[::-1]

        tap, leaves = pig.hashlock_taptree(
            preimage_hash=h, asset=asset_c, payee_prog=payee_spk[2:],
            refund_after=DEADLINE, refund_prog=sender_spk[2:])
        spk = bytes(tap.scriptPubKey)

        self.log.info("the payee claims by publishing the secret")
        out = self.fund(spk, VALUE, self.D)
        tx = self.spend(tap, leaves, "claim", out, value=VALUE, asset=self.D,
                        payee_spk=payee_spk, preimage=secret)
        node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        self.log.info("  the secret is now public in the witness")
        raw = node.getrawtransaction(tx.rehash(), True)
        wit = raw["vin"][0]["txinwitness"]
        assert_equal(wit[0], secret.hex())

        self.log.info("a claim with the WRONG preimage is refused")
        out = self.fund(spk, VALUE, self.D)
        self.assert_rejected(self.spend(
            tap, leaves, "claim", out, value=VALUE, asset=self.D,
            payee_spk=payee_spk, preimage=hashlib.sha256(b"not it").digest()))

        self.log.info("a claim with NO preimage is refused")
        self.assert_rejected(self.spend(
            tap, leaves, "claim", out, value=VALUE, asset=self.D,
            payee_spk=payee_spk,
            witness=[bytes(leaves["claim"]), pig.control_block(tap, "claim")]))

        self.log.info("the secret does not let the money go anywhere else")
        self.assert_rejected(self.spend(
            tap, leaves, "claim", out, value=VALUE, asset=self.D,
            payee_spk=self.raw_v1_spk(b"\xab"), preimage=secret))

        self.log.info("nor does it let the payee be short-changed")
        self.assert_rejected(self.spend(
            tap, leaves, "claim", out, value=VALUE, asset=self.D,
            payee_spk=payee_spk, preimage=secret, pay=VALUE - 1))

        self.log.info("nor paid in a different asset")
        self.assert_rejected(self.spend(
            tap, leaves, "claim", out, value=VALUE, asset=self.D,
            payee_spk=payee_spk, preimage=secret, pay_asset=self.other))

        self.log.info("the refund is refused before its deadline")
        assert node.getblockcount() < DEADLINE
        self.assert_rejected(self.spend(
            tap, leaves, "refund", out, value=VALUE, asset=self.D,
            payee_spk=sender_spk, locktime=node.getblockcount()),
            want="Locktime requirement not satisfied")

        self.log.info("and accepted after it")
        self.generate(node, DEADLINE - node.getblockcount() + 1)
        tx = self.spend(tap, leaves, "refund", out, value=VALUE, asset=self.D,
                        payee_spk=sender_spk, locktime=DEADLINE)
        node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)

        self.log.info("a segwit v0 payee -- what a browser wallet receives at")
        v0_spk = self.wallet_spk("bech32")
        assert_equal(v0_spk[:2].hex(), "0014")
        tap0, leaves0 = pig.hashlock_taptree(
            preimage_hash=h, asset=asset_c, payee_prog=v0_spk[2:], payee_ver=0,
            refund_after=DEADLINE, refund_prog=sender_spk[2:])
        out = self.fund(bytes(tap0.scriptPubKey), VALUE, self.D)
        tx = self.spend(tap0, leaves0, "claim", out, value=VALUE, asset=self.D,
                        payee_spk=v0_spk, preimage=secret)
        node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)

        self.log.info("a v0 program of the wrong length cannot be baked in")
        try:
            pig.build_hashlock_leaf(h, asset_c, self.raw_v1_spk()[2:], payee_ver=0)
            raise AssertionError("a 32-byte program was accepted at v0")
        except ValueError as e:
            assert "20 bytes" in str(e), e

        self.log.info("and a commitment that is not a SHA-256 hash is refused")
        try:
            pig.build_hashlock_leaf(b"\x00" * 20, asset_c, payee_spk[2:])
            raise AssertionError("a 20-byte commitment was accepted")
        except ValueError as e:
            assert "32 bytes" in str(e), e


if __name__ == "__main__":
    PignusHashlockTest().main()
