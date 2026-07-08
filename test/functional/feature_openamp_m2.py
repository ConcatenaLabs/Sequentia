#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""OpenAMP M2: the Tier B containment covenant, proven by real spends.

Issues a restricted asset into Tier B enclaves (taproot {L_cov, C(K)}) and
demonstrates that consensus itself enforces containment:

  PASS  enclave -> enclave transfer (recipient + change), fee in the ordinary asset
  PASS  enclave -> OP_RETURN burn (burn_allowed asset)
  FAIL  enclave -> bare/other-witness address (out-of-enclave)
  FAIL  fee output denominated in the restricted asset (Rule 1, by consensus)

Every FAIL is rejected by the node's script interpreter, not by any server.

The covenant self-proves its own leaf hash via OP_TWEAKVERIFY against the
spending input's scriptPubKey, binds the holder key from an unspendable
commitment sibling, and checks every asset-A output is enclave-shaped or an
OP_RETURN burn. See openamp_covenant.py and the design doc (§6).

Note: enclave transfers use ordinary-asset SEGWIT inputs for the fee, which is
all openampd ever produces. (A legacy P2PKH co-input perturbs the taproot
sighash in this regtest harness; it never occurs in production.)
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, satoshi_round, BITCOIN_ASSET
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr
from test_framework.messages import (
    COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset,
    CTxOutValue, uint256_from_str, tx_from_hex,
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

    def any_utxo(self, min_btc):
        for u in self.nodes[0].listunspent():
            if u["asset"] == BITCOIN_ASSET and u["amount"] >= min_btc and u["spendable"]:
                return u
        raise AssertionError("no wallet utxo")

    def fresh_segwit_fee_utxo(self, amount=10):
        """A dedicated bech32 (segwit) utxo to use as an ordinary-asset fee input."""
        node = self.nodes[0]
        bech = node.getnewaddress("", "bech32")
        unconf = node.getaddressinfo(bech)["unconfidential"]
        node.sendtoaddress(unconf, amount)
        self.generate(node, 1)
        for u in node.listunspent():
            if (u["asset"] == BITCOIN_ASSET and abs(u["amount"] - amount) < 1e-9
                    and u["scriptPubKey"].startswith("0014")):
                return u
        raise AssertionError("no fresh segwit utxo")

    def ctxout(self, amount, spk, asset_out=None):
        if asset_out is None:
            asset_out = b"\x01" + bytes.fromhex(BITCOIN_ASSET)[::-1]
        return CTxOut(nValue=CTxOutValue(amount), scriptPubKey=spk, nAsset=CTxOutAsset(asset_out))

    def sign_enclave_input(self, tx, spent, idx, lcov, tap, user_x, user_sec, policy_sec, out_keys):
        msg = TaprootSignatureHash(tx, spent, 0, self.genesis, idx, scriptpath=True, script=lcov)
        sig_user = sign_schnorr(user_sec, msg)
        sig_policy = sign_schnorr(policy_sec, msg)
        stack = cov.cov_witness(tx, spent, idx, lcov, tap, user_x, sig_user, sig_policy,
                                out_keys, self.genesis, MAXO)
        control = bytes([tap.leaves["cov"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["cov"].merklebranch
        tx.wit.vtxinwit[idx].scriptWitness.stack = stack + [bytes(lcov), control]

    def spend_enclave(self, enclave_txid, enclave_vout, enclave_amount, src_tap, outs):
        """Build a transaction spending an enclave utxo plus a fresh segwit fee
        input. `outs` is a list of (amount, spk, asset_out, out_key). Returns
        (tx, spent, out_keys) with the wallet fee input already signed and the
        enclave input's witness left for sign_enclave_input."""
        node = self.nodes[0]
        fee_u = self.fresh_segwit_fee_utxo()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(enclave_txid, 16), enclave_vout)))
        tx.vin.append(CTxIn(COutPoint(int(fee_u["txid"], 16), fee_u["vout"])))
        out_keys = []
        for (amt, spk, aout, key) in outs:
            tx.vout.append(self.ctxout(amt, spk, aout))
            out_keys.append(key)
        fee_in = int(satoshi_round(fee_u["amount"]) * COIN)
        tx.vout.append(self.ctxout(fee_in - FEE, self.wallet_spk()))   # fee change (bitcoin)
        out_keys.append(None)
        tx.vout.append(CTxOut(CTxOutValue(FEE)))                        # fee (bitcoin)
        out_keys.append(None)
        spent = [self.ctxout(enclave_amount, src_tap.scriptPubKey, self.asset_out),
                 self.ctxout(fee_in, bytes.fromhex(fee_u["scriptPubKey"]),
                             b"\x01" + bytes.fromhex(fee_u["asset"])[::-1])]
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        return tx, spent, out_keys

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(node.getnewaddress(), 50)
        self.generate(node, 1)
        self.genesis = uint256_from_str(bytes.fromhex(node.getblockhash(0))[::-1])

        policy_sec = generate_privkey()
        policy_x = compute_xonly_pubkey(policy_sec)[0]
        alice_sec = generate_privkey()
        alice_x = compute_xonly_pubkey(alice_sec)[0]
        bob_sec = generate_privkey()
        bob_x = compute_xonly_pubkey(bob_sec)[0]

        self.log.info("issue into Tier B enclave (asset id commits to policy key)")
        funding = self.any_utxo(2)
        prevout = COutPoint(int(funding["txid"], 16), funding["vout"])
        contract = {"openamp": {"tier": "B", "policy_pubkey": policy_x.hex()}}
        import json, hashlib
        digest = hashlib.sha256(json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()).digest()
        from feature_openamp_m0 import derive_issuance_ids
        entropy, asset_i, token_i = derive_issuance_ids(prevout, digest)
        self.asset_display = asset_i[::-1].hex()
        self.asset_out = b"\x01" + asset_i

        alice_tap, lcov = cov.enclave_taptree(alice_x, policy_x, asset_i, MAXO, burn_allowed=True)
        bob_tap, _ = cov.enclave_taptree(bob_x, policy_x, asset_i, MAXO, burn_allowed=True)
        # L_cov is asset-wide: identical bytes for every holder.
        assert_equal(bytes(lcov), bytes(cov.build_lcov(policy_x, asset_i, MAXO, True)))

        in_sats = int(satoshi_round(funding["amount"]) * COIN)
        itx = CTransaction()
        itx.nVersion = 2
        itx.vin.append(CTxIn(prevout))
        itx.vin[0].assetIssuance.assetEntropy = uint256_from_str(digest)
        itx.vin[0].assetIssuance.nAmount = CTxOutValue(100 * COIN)
        itx.vin[0].assetIssuance.nInflationKeys = CTxOutValue(COIN)
        itx.vout.append(self.ctxout(100 * COIN, alice_tap.scriptPubKey, self.asset_out))
        itx.vout.append(self.ctxout(COIN, self.wallet_spk(), b"\x01" + token_i))
        itx.vout.append(self.ctxout(in_sats - FEE, self.wallet_spk()))
        itx.vout.append(CTxOut(CTxOutValue(FEE)))
        signed = node.signrawtransactionwithwallet(itx.serialize().hex())
        assert signed["complete"]
        itxid = node.sendrawtransaction(signed["hex"])
        self.generate(node, 1)
        assert_equal(node.gettxout(itxid, 0)["scriptPubKey"]["hex"], bytes(alice_tap.scriptPubKey).hex())

        # PASS: enclave -> enclave (Bob 60 + Alice change 40)
        self.log.info("PASS: enclave -> enclave transfer")
        tx, spent, out_keys = self.spend_enclave(itxid, 0, 100 * COIN, alice_tap, [
            (60 * COIN, bob_tap.scriptPubKey, self.asset_out, bob_x),
            (40 * COIN, alice_tap.scriptPubKey, self.asset_out, alice_x),
        ])
        self.sign_enclave_input(tx, spent, 0, lcov, alice_tap, alice_x, alice_sec, policy_sec, out_keys)
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(node.gettxout(txid, 0)["scriptPubKey"]["hex"], bytes(bob_tap.scriptPubKey).hex())
        assert_equal(node.gettxout(txid, 1)["scriptPubKey"]["hex"], bytes(alice_tap.scriptPubKey).hex())

        # PASS: enclave -> OP_RETURN burn (Bob's 60: burn 30 + Bob enclave 30)
        self.log.info("PASS: enclave -> OP_RETURN burn")
        tx, spent, out_keys = self.spend_enclave(txid, 0, 60 * COIN, bob_tap, [
            (30 * COIN, CScript([OP_RETURN]), self.asset_out, None),   # OP_RETURN burn
            (30 * COIN, bob_tap.scriptPubKey, self.asset_out, bob_x),
        ])
        self.sign_enclave_input(tx, spent, 0, lcov, bob_tap, bob_x, bob_sec, policy_sec, out_keys)
        burn_txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert_equal(node.gettxout(burn_txid, 1)["scriptPubKey"]["hex"], bytes(bob_tap.scriptPubKey).hex())

        # Bob now holds 30 in enclave (burn_txid vout 1) for the FAIL cases.
        bob_utxo = (burn_txid, 1, 30 * COIN)

        # FAIL: enclave -> bare wallet address (out of enclave)
        self.log.info("FAIL: out-of-enclave destination")
        tx, spent, out_keys = self.spend_enclave(bob_utxo[0], bob_utxo[1], bob_utxo[2], bob_tap, [
            (30 * COIN, self.wallet_spk(), self.asset_out, None),      # NOT an enclave
        ])
        self.sign_enclave_input(tx, spent, 0, lcov, bob_tap, bob_x, bob_sec, policy_sec, out_keys)
        assert_raises_rpc_error(-26, None, node.sendrawtransaction, tx.serialize().hex())

        # FAIL: fee output denominated in the restricted asset (Rule 1)
        self.log.info("FAIL: restricted asset in a fee output")
        fee_u = self.fresh_segwit_fee_utxo()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(bob_utxo[0], 16), bob_utxo[1])))
        tx.vin.append(CTxIn(COutPoint(int(fee_u["txid"], 16), fee_u["vout"])))
        tx.vout.append(self.ctxout(29 * COIN, bob_tap.scriptPubKey, self.asset_out))
        tx.vout.append(self.ctxout(COIN, b"", self.asset_out))          # FEE OUTPUT IN ASSET A
        fee_in = int(satoshi_round(fee_u["amount"]) * COIN)
        tx.vout.append(self.ctxout(fee_in - FEE, self.wallet_spk()))
        tx.vout.append(CTxOut(CTxOutValue(FEE)))
        out_keys = [bob_x, None, None, None]
        spent = [self.ctxout(30 * COIN, bob_tap.scriptPubKey, self.asset_out),
                 self.ctxout(fee_in, bytes.fromhex(fee_u["scriptPubKey"]), b"\x01" + bytes.fromhex(fee_u["asset"])[::-1])]
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        self.sign_enclave_input(tx, spent, 0, lcov, bob_tap, bob_x, bob_sec, policy_sec, out_keys)
        assert_raises_rpc_error(-26, None, node.sendrawtransaction, tx.serialize().hex())

        self.log.info("M2 complete: consensus enforces containment; a restricted "
                      "asset cannot leave its enclave or become a fee")


if __name__ == "__main__":
    OpenAmpM2Test().main()
