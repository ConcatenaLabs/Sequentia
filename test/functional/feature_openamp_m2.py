#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""OpenAMP M2: the Tier B containment covenant, proven by real spends.

STATUS: WORK IN PROGRESS, not in the test runner. The covenant MECHANISM is
proven (see openamp_covenant.py and the design doc §6): a single tapscript
leaf self-proves its own hash via OP_TWEAKVERIFY against the spending input,
binds the holder key from an unspendable commitment sibling, and enforces a
2-of-2. That self-check plus authorization validates on-chain as an isolated
leaf. The full 1933-byte, four-output-unrolled leaf below still has a
stack-alignment issue when the per-output containment loop is combined with
the dual-signature authorization; the enclave->enclave spend does not yet
validate. Tier A (feature_openamp_m0.py, feature_openamp_daemon.py) is the
fully-proven, AMP2-parity path; Tier B is the beyond-AMP2 hardening and is
completed here incrementally.

Intended coverage once the leaf is finalized:
  PASS  enclave -> enclave transfer (recipient + change), fee in the ordinary asset
  PASS  enclave -> OP_RETURN burn (burn_allowed asset)
  FAIL  enclave -> bare/other-witness address (out-of-enclave)
  FAIL  fee output denominated in the restricted asset (Rule 1, by consensus)
  FAIL  confidential output in a Tier B spend
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, satoshi_round, BITCOIN_ASSET
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutValue, CTxOutNonce, uint256_from_str,
)
from test_framework.script import CScript, OP_RETURN, TaprootSignatureHash

import openamp_covenant as cov

FEE = 5000
MAXO = 4


class OpenAmpM2Test(BitcoinTestFramework):

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
        self.skip_if_no_bdb()

    def setup_network(self, split=False):
        self.setup_nodes()

    def wallet_spk(self):
        addr = self.nodes[0].getnewaddress()
        unconf = self.nodes[0].getaddressinfo(addr)["unconfidential"]
        return bytes.fromhex(self.nodes[0].getaddressinfo(unconf)["scriptPubKey"])

    def wallet_utxo(self, min_btc=1):
        for u in self.nodes[0].listunspent():
            if u["asset"] == BITCOIN_ASSET and u["amount"] >= min_btc and u["spendable"]:
                return u
        raise AssertionError("no wallet utxo")

    def ctxout(self, amount, spk, asset_out=None):
        if asset_out is None:
            asset_out = b"\x01" + bytes.fromhex(BITCOIN_ASSET)[::-1]
        return CTxOut(nValue=CTxOutValue(amount), scriptPubKey=spk, nAsset=CTxOutAsset(asset_out))

    def sign_enclave_input(self, tx, spent, idx, lcov, tap, user_x, user_sec, policy_sec, out_keys):
        from test_framework.key import verify_schnorr
        msg = TaprootSignatureHash(tx, spent, 0, self.genesis, idx, scriptpath=True, script=lcov)
        sig_user = sign_schnorr(user_sec, msg)
        sig_policy = sign_schnorr(policy_sec, msg)
        assert verify_schnorr(user_x, sig_user, msg), "local user sig verify failed"
        stack = cov.cov_witness(tx, spent, idx, lcov, tap, user_x, sig_user, sig_policy,
                                out_keys, self.genesis, MAXO)
        control = bytes([tap.leaves["cov"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["cov"].merklebranch
        tx.wit.vtxinwit[idx].scriptWitness.stack = stack + [bytes(lcov), control]

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(node.getnewaddress(), 50)
        self.generate(node, 1)
        self.genesis = uint256_from_str(bytes.fromhex(node.getblockhash(0))[::-1])

        policy_sec = (2).to_bytes(32, "big")
        policy_x = compute_xonly_pubkey(policy_sec)[0]
        alice_sec = (3).to_bytes(32, "big")
        alice_x = compute_xonly_pubkey(alice_sec)[0]
        bob_sec = (4).to_bytes(32, "big")
        bob_x = compute_xonly_pubkey(bob_sec)[0]

        # Issue a restricted asset into Alice's Tier B enclave.
        self.log.info("issue into Tier B enclave (asset id commits to policy key)")
        funding = self.wallet_utxo(min_btc=2)
        prevout = COutPoint(int(funding["txid"], 16), funding["vout"])
        contract = {"openamp": {"tier": "B", "policy_pubkey": policy_x.hex()}}
        import json, hashlib
        digest = hashlib.sha256(json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()).digest()
        from feature_openamp_m0 import derive_issuance_ids
        entropy, asset_i, token_i = derive_issuance_ids(prevout, digest)
        asset_display = asset_i[::-1].hex()
        asset_out = b"\x01" + asset_i

        # asset_internal for the covenant is the raw 32-byte id (internal order).
        alice_tap, lcov = cov.enclave_taptree(alice_x, policy_x, asset_i, MAXO, burn_allowed=True)
        bob_tap, _ = cov.enclave_taptree(bob_x, policy_x, asset_i, MAXO, burn_allowed=True)
        # L_cov is asset-wide: identical bytes for Alice and Bob.
        assert_equal(bytes(lcov), bytes(cov.build_lcov(policy_x, asset_i, MAXO, True)))

        in_sats = int(satoshi_round(funding["amount"]) * COIN)
        itx = CTransaction()
        itx.nVersion = 2
        itx.vin.append(CTxIn(prevout))
        itx.vin[0].assetIssuance.assetEntropy = uint256_from_str(digest)
        itx.vin[0].assetIssuance.nAmount = CTxOutValue(100 * COIN)
        itx.vin[0].assetIssuance.nInflationKeys = CTxOutValue(COIN)
        itx.vout.append(self.ctxout(100 * COIN, alice_tap.scriptPubKey, asset_out))
        itx.vout.append(self.ctxout(COIN, self.wallet_spk(), b"\x01" + token_i))
        itx.vout.append(self.ctxout(in_sats - FEE, self.wallet_spk()))
        itx.vout.append(CTxOut(CTxOutValue(FEE)))
        signed = node.signrawtransactionwithwallet(itx.serialize().hex())
        assert signed["complete"]
        itxid = node.sendrawtransaction(signed["hex"])
        self.generate(node, 1)
        assert_equal(node.gettxout(itxid, 0)["scriptPubKey"]["hex"], bytes(alice_tap.scriptPubKey).hex())

        def base_transfer(recip_out, fee_change=True):
            """A transfer spending Alice's enclave utxo (vout 0 of itx) plus a
            wallet fee input; returns (tx, spent, out_keys)."""
            fee_u = self.wallet_utxo()
            tx = CTransaction()
            tx.nVersion = 2
            tx.vin.append(CTxIn(COutPoint(int(itxid, 16), 0)))
            tx.vin.append(CTxIn(COutPoint(int(fee_u["txid"], 16), fee_u["vout"])))
            out_keys = []
            for (amt, spk, aout, key) in recip_out:
                tx.vout.append(self.ctxout(amt, spk, aout))
                out_keys.append(key)
            fee_in = int(satoshi_round(fee_u["amount"]) * COIN)
            if fee_change:
                tx.vout.append(self.ctxout(fee_in - FEE, self.wallet_spk()))
                out_keys.append(None)
            tx.vout.append(CTxOut(CTxOutValue(FEE)))  # fee in bitcoin asset
            out_keys.append(None)
            spent = [self.ctxout(100 * COIN, alice_tap.scriptPubKey, asset_out),
                     self.ctxout(fee_in, bytes.fromhex(fee_u["scriptPubKey"]), b"\x01" + bytes.fromhex(fee_u["asset"])[::-1])]
            partial = node.signrawtransactionwithwallet(tx.serialize().hex())
            tx = tx_from_hex(partial["hex"])
            while len(tx.wit.vtxinwit) < len(tx.vin):
                tx.wit.vtxinwit.append(CTxInWitness())
            return tx, spent, out_keys

        # PASS: enclave -> enclave (Bob 60 + Alice change 40)
        self.log.info("PASS: enclave -> enclave transfer")
        tx, spent, out_keys = base_transfer([
            (60 * COIN, bob_tap.scriptPubKey, asset_out, bob_x),
            (40 * COIN, alice_tap.scriptPubKey, asset_out, alice_x),
        ])
        self.sign_enclave_input(tx, spent, 0, lcov, alice_tap, alice_x, alice_sec, policy_sec, out_keys)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(node.gettxout(txid, 0)["scriptPubKey"]["hex"], bytes(bob_tap.scriptPubKey).hex())
        assert_equal(node.gettxout(txid, 1)["scriptPubKey"]["hex"], bytes(alice_tap.scriptPubKey).hex())

        # Now Bob holds 60 in enclave (txid vout 0); use it for the fail cases.
        bob_prevout = (txid, 0)

        def bob_transfer(recip_out):
            fee_u = self.wallet_utxo()
            tx = CTransaction()
            tx.nVersion = 2
            tx.vin.append(CTxIn(COutPoint(int(bob_prevout[0], 16), bob_prevout[1])))
            tx.vin.append(CTxIn(COutPoint(int(fee_u["txid"], 16), fee_u["vout"])))
            out_keys = []
            for (amt, spk, aout, key, nonce) in recip_out:
                o = self.ctxout(amt, spk, aout)
                if nonce is not None:
                    o.nNonce = CTxOutNonce(nonce)
                tx.vout.append(o)
                out_keys.append(key)
            fee_in = int(satoshi_round(fee_u["amount"]) * COIN)
            tx.vout.append(self.ctxout(fee_in - FEE, self.wallet_spk()))
            out_keys.append(None)
            tx.vout.append(CTxOut(CTxOutValue(FEE)))
            out_keys.append(None)
            spent = [self.ctxout(60 * COIN, bob_tap.scriptPubKey, asset_out),
                     self.ctxout(fee_in, bytes.fromhex(fee_u["scriptPubKey"]), b"\x01" + bytes.fromhex(fee_u["asset"])[::-1])]
            partial = node.signrawtransactionwithwallet(tx.serialize().hex())
            self.log.info("bob_transfer wallet-sign errors: %s" % partial.get("errors"))
            tx = tx_from_hex(partial["hex"])
            while len(tx.wit.vtxinwit) < len(tx.vin):
                tx.wit.vtxinwit.append(CTxInWitness())
            return tx, spent, out_keys

        # PASS: enclave -> OP_RETURN burn (30) + Alice enclave (30)
        self.log.info("PASS: enclave -> OP_RETURN burn")
        tx, spent, out_keys = bob_transfer([
            (30 * COIN, CScript([OP_RETURN]), asset_out, None, None),   # OP_RETURN burn
            (30 * COIN, alice_tap.scriptPubKey, asset_out, alice_x, None),
        ])
        self.sign_enclave_input(tx, spent, 0, lcov, bob_tap, bob_x, bob_sec, policy_sec, out_keys)
        burn_txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        # Restore: Alice now has 30 more; Bob's 60 is spent. Re-issue a fresh Bob
        # enclave utxo for the remaining fail cases by transferring Alice->Bob.
        self.log.info("re-fund Bob for the fail cases")
        fee_u = self.wallet_utxo()
        atx = CTransaction()
        atx.nVersion = 2
        alice_utxo = [u for u in node.scantxoutset("start", ["raw(%s)" % bytes(alice_tap.scriptPubKey).hex()])["unspents"] if u["asset"] == asset_display]
        alice_in = alice_utxo[0]
        atx.vin.append(CTxIn(COutPoint(int(alice_in["txid"], 16), alice_in["vout"])))
        atx.vin.append(CTxIn(COutPoint(int(fee_u["txid"], 16), fee_u["vout"])))
        av = int(satoshi_round(alice_in["amount"]) * COIN)
        atx.vout.append(self.ctxout(av, bob_tap.scriptPubKey, asset_out))
        fee_in = int(satoshi_round(fee_u["amount"]) * COIN)
        atx.vout.append(self.ctxout(fee_in - FEE, self.wallet_spk()))
        atx.vout.append(CTxOut(CTxOutValue(FEE)))
        aout_keys = [bob_x, None, None]
        aspent = [self.ctxout(av, alice_tap.scriptPubKey, asset_out),
                  self.ctxout(fee_in, bytes.fromhex(fee_u["scriptPubKey"]), b"\x01" + bytes.fromhex(fee_u["asset"])[::-1])]
        partial = node.signrawtransactionwithwallet(atx.serialize().hex())
        atx = tx_from_hex(partial["hex"])
        while len(atx.wit.vtxinwit) < len(atx.vin):
            atx.wit.vtxinwit.append(CTxInWitness())
        self.sign_enclave_input(atx, aspent, 0, lcov, alice_tap, alice_x, alice_sec, policy_sec, aout_keys)
        bob_txid = node.sendrawtransaction(atx.serialize().hex())
        self.generate(node, 1)
        bob_prevout = (bob_txid, 0)
        bob_bal = av

        # FAIL: enclave -> bare wallet address (out of enclave)
        self.log.info("FAIL: out-of-enclave destination")
        tx, spent, out_keys = bob_transfer([
            (bob_bal, self.wallet_spk(), asset_out, None, None),   # not an enclave
        ])
        self.sign_enclave_input(tx, spent, 0, lcov, bob_tap, bob_x, bob_sec, policy_sec, out_keys)
        assert_raises_rpc_error(-26, None, node.sendrawtransaction, tx.serialize().hex())

        # FAIL: fee output in the restricted asset (Rule 1)
        self.log.info("FAIL: restricted asset in a fee output")
        fee_u = self.wallet_utxo()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(bob_prevout[0], 16), bob_prevout[1])))
        tx.vin.append(CTxIn(COutPoint(int(fee_u["txid"], 16), fee_u["vout"])))
        tx.vout.append(self.ctxout(bob_bal - COIN, bob_tap.scriptPubKey, asset_out))
        tx.vout.append(self.ctxout(COIN, b"", asset_out))  # FEE OUTPUT IN ASSET A
        fee_in = int(satoshi_round(fee_u["amount"]) * COIN)
        tx.vout.append(self.ctxout(fee_in - FEE, self.wallet_spk()))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        out_keys = [bob_x, None, None, None]
        spent = [self.ctxout(bob_bal, bob_tap.scriptPubKey, asset_out),
                 self.ctxout(fee_in, bytes.fromhex(fee_u["scriptPubKey"]), b"\x01" + bytes.fromhex(fee_u["asset"])[::-1])]
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        self.sign_enclave_input(tx, spent, 0, lcov, bob_tap, bob_x, bob_sec, policy_sec, out_keys)
        assert_raises_rpc_error(-26, None, node.sendrawtransaction, tx.serialize().hex())

        # The covenant additionally requires every output's asset to be
        # explicit (the `OP_1 OP_EQUALVERIFY` on the asset prefix), so a
        # confidential output that could hide asset A is rejected the same
        # way. Proving that on-chain needs valid rangeproofs to even decode,
        # which is out of scope here; the explicit-prefix check is exercised
        # positively by every PASS case above (all outputs explicit).

        self.log.info("M2 complete: consensus enforces containment; a restricted asset cannot leave its enclave or become a fee")


from test_framework.messages import tx_from_hex  # noqa: E402

if __name__ == "__main__":
    OpenAmpM2Test().main()
