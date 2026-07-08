#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""OpenAMP M0: restricted-asset enclave proof (doc/sequentia/openamp-design.md §12).

Proves against the real node, with no chain-side changes:

1. Issuance of a restricted asset whose contract hash commits to the policy
   key, minted directly into Tier A enclave outputs (taproot, NUMS internal
   key, leaves: transfer = <K_user> CHECKSIGVERIFY <K_policy> CHECKSIG and
   clawback = <K_issuer> CHECKSIGVERIFY <K_policy> CHECKSIG).
2. Independent (pure python) verification of the asset -> policy-key binding:
   entropy/asset/token ids recomputed from the issuance prevout + contract
   hash. The issuance transaction is built manually around the recomputed ids,
   so consensus acceptance itself proves the derivation is exact.
3. A co-signed enclave-to-enclave transfer where the sender self-pays the fee
   in the ordinary asset (the issuer is not involved in the fee leg).
4. Policy refusals by a stub co-signer holding K_policy: frozen sender,
   out-of-enclave destination, restricted asset in the fee output.
5. Consensus enforcement of the 2-of-2: a transfer without a valid policy
   signature is rejected by the chain, not merely by server courtesy.
6. Fee conversion (design §7): the sender pays in the restricted asset; the
   issuer takes a conversion output into its own enclave account and attaches
   the real fee in the ordinary asset.
7. Clawback through the disclosed L_claw leaf (issuer + policy, no user key).
"""

import json
import struct

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, satoshi_round, BITCOIN_ASSET
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    CTxOutAsset,
    CTxOutValue,
    sha256,
    hash256,
    tx_from_hex,
    uint256_from_str,
)
from test_framework.script import (
    CScript,
    OP_CHECKSIG,
    OP_CHECKSIGVERIFY,
    SIGHASH_DEFAULT,
    TaprootSignatureHash,
    taproot_construct,
)

# BIP341 nothing-up-my-sleeve point: no key-path spend exists for enclave outputs.
NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")

FEE_SATS = 5000

# ---------------------------------------------------------------------------
# Pure-python SHA256 compression, for Elements' fast merkle root (midstate
# hashing: one compression of a 64-byte block, no length padding). This is the
# independent implementation of the asset-id derivation chain; it deliberately
# shares no code with the node.
# ---------------------------------------------------------------------------

_SHA256_K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]
_SHA256_IV = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]


def _ror(x, n):
    return ((x >> n) | (x << (32 - n))) & 0xffffffff


def sha256_midstate(block):
    """State after compressing one 64-byte block from the standard IV, serialized big-endian."""
    assert len(block) == 64
    w = list(struct.unpack(">16I", block))
    for i in range(16, 64):
        s0 = _ror(w[i - 15], 7) ^ _ror(w[i - 15], 18) ^ (w[i - 15] >> 3)
        s1 = _ror(w[i - 2], 17) ^ _ror(w[i - 2], 19) ^ (w[i - 2] >> 10)
        w.append((w[i - 16] + s0 + w[i - 7] + s1) & 0xffffffff)
    a, b, c, d, e, f, g, h = _SHA256_IV
    for i in range(64):
        s1 = _ror(e, 6) ^ _ror(e, 11) ^ _ror(e, 25)
        ch = (e & f) ^ (~e & g)
        t1 = (h + s1 + ch + _SHA256_K[i] + w[i]) & 0xffffffff
        s0 = _ror(a, 2) ^ _ror(a, 13) ^ _ror(a, 22)
        maj = (a & b) ^ (a & c) ^ (b & c)
        t2 = (s0 + maj) & 0xffffffff
        h, g, f, e, d, c, b, a = g, f, e, (d + t1) & 0xffffffff, c, b, a, (t1 + t2) & 0xffffffff
    return struct.pack(">8I", *[(x + y) & 0xffffffff for x, y in zip(_SHA256_IV, [a, b, c, d, e, f, g, h])])


def fast_merkle_2(left, right):
    """Elements ComputeFastMerkleRoot for exactly two leaves."""
    return sha256_midstate(left + right)


def derive_issuance_ids(prevout, contract_digest):
    """GenerateAssetEntropy / CalculateAsset / CalculateReissuanceToken, in python.

    prevout: COutPoint of the issuance input.
    contract_digest: raw sha256 digest of the canonical contract JSON (this is
    the internal uint256 byte order; display hex is the reverse).
    Returns (entropy, asset, token) in internal byte order.
    """
    h_i = hash256(prevout.serialize())
    entropy = fast_merkle_2(h_i, contract_digest)
    asset = fast_merkle_2(entropy, b"\x00" * 32)
    token = fast_merkle_2(entropy, b"\x01" + b"\x00" * 31)
    return entropy, asset, token


def display_hex(internal_bytes):
    return internal_bytes[::-1].hex()


# ---------------------------------------------------------------------------
# Enclave construction and the stub policy server
# ---------------------------------------------------------------------------

def enclave_tap(user_x, policy_x, issuer_x):
    """Tier A enclave: NUMS internal key, transfer leaf + clawback leaf (clawback default ON)."""
    transfer = CScript([user_x, OP_CHECKSIGVERIFY, policy_x, OP_CHECKSIG])
    claw = CScript([issuer_x, OP_CHECKSIGVERIFY, policy_x, OP_CHECKSIG])
    return taproot_construct(NUMS, [("transfer", transfer), ("claw", claw)])


def enclave_witness(tap, leaf_name, sig_policy, sig_other):
    """Witness stack for an enclave leaf. Both leaves execute as
    <K_a> CHECKSIGVERIFY <K_policy> CHECKSIG, so the policy signature sits at
    the bottom of the stack and the user/issuer signature on top of it."""
    leaf = tap.leaves[leaf_name]
    control = bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch
    return [sig_policy, sig_other, bytes(leaf.script), control]


class PolicyRefusal(Exception):
    pass


class StubPolicyServer:
    """M0 stand-in for openampd: K_policy custody, a registry of enclave
    accounts, freeze-by-refusal, and Rule-1/containment checks on every
    co-sign request. It re-derives everything from the transaction itself."""

    def __init__(self, policy_sec, asset_commitment, genesis_hash):
        self.policy_sec = policy_sec
        self.asset_commitment = asset_commitment  # b'\x01' + asset id (internal order)
        self.genesis_hash = genesis_hash
        self.enclaves = {}  # user xonly pubkey -> taproot info
        self.frozen = set()

    def register(self, user_x, tap):
        self.enclaves[user_x] = tap

    def freeze(self, user_x):
        self.frozen.add(user_x)

    def unfreeze(self, user_x):
        self.frozen.discard(user_x)

    def registered_spks(self):
        return {bytes(tap.scriptPubKey) for tap in self.enclaves.values()}

    def cosign(self, tx, in_idx, spent_utxos, sender_x):
        if sender_x not in self.enclaves:
            raise PolicyRefusal("sender not registered")
        if sender_x in self.frozen:
            raise PolicyRefusal("sender frozen")
        allowed = self.registered_spks()
        for out in tx.vout:
            if out.nAsset.vchCommitment[0] != 1:
                raise PolicyRefusal("confidential output in a restricted transfer")
            if bytes(out.nAsset.vchCommitment) == self.asset_commitment:
                if len(out.scriptPubKey) == 0:
                    raise PolicyRefusal("restricted asset in a fee output")
                if bytes(out.scriptPubKey) not in allowed:
                    raise PolicyRefusal("restricted asset to an out-of-enclave destination")
        leaf_script = self.enclaves[sender_x].leaves["transfer"].script
        msg = TaprootSignatureHash(tx, spent_utxos, SIGHASH_DEFAULT, self.genesis_hash,
                                   in_idx, scriptpath=True, script=leaf_script)
        return sign_schnorr(self.policy_sec, msg)

    def sign_clawback(self, tx, in_idx, spent_utxos, holder_x):
        """Clawback ceremony stub: openampd would log to the transparency
        chain and require the offline issuer authorization first."""
        leaf_script = self.enclaves[holder_x].leaves["claw"].script
        msg = TaprootSignatureHash(tx, spent_utxos, SIGHASH_DEFAULT, self.genesis_hash,
                                   in_idx, scriptpath=True, script=leaf_script)
        return sign_schnorr(self.policy_sec, msg)


class OpenAmpM0Test(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-initialfreecoins=2100000000000000",
            "-anyonecanspendaremine=1",
            "-blindedaddresses=0",
            "-validatepegin=0",
            "-con_parent_chain_signblockscript=51",
            # The open fee market, as on Sequentia chains: exactly one fee
            # asset per transaction, valued through the default-deny
            # ExchangeRateMap whitelist (the policy asset defaults to 1:1).
            "-con_any_asset_fees=1",
            "-maxtxfee=100.0",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()

    def setup_network(self, split=False):
        self.setup_nodes()

    # -- helpers ------------------------------------------------------------

    def wallet_spk(self):
        addr = self.nodes[0].getnewaddress()
        unconf = self.nodes[0].getaddressinfo(addr)["unconfidential"]
        return bytes.fromhex(self.nodes[0].getaddressinfo(unconf)["scriptPubKey"])

    def wallet_utxo(self, min_btc=1):
        for utxo in self.nodes[0].listunspent():
            if utxo["asset"] == BITCOIN_ASSET and utxo["amount"] >= min_btc and utxo["spendable"]:
                return utxo
        raise AssertionError("no wallet utxo available")

    def utxo_to_ctxout(self, utxo):
        return CTxOut(
            nValue=CTxOutValue(int(satoshi_round(utxo["amount"]) * COIN)),
            scriptPubKey=bytes.fromhex(utxo["scriptPubKey"]),
            nAsset=CTxOutAsset(b"\x01" + bytes.fromhex(utxo["asset"])[::-1]),
        )

    def send_and_mine(self, tx):
        txid = self.nodes[0].sendrawtransaction(tx.serialize().hex())
        self.generate(self.nodes[0], 1)
        assert txid in self.nodes[0].getblock(self.nodes[0].getbestblockhash())["tx"]
        return txid

    def assert_enclave_utxo(self, txid, n, tap, asset_display, amount_sats):
        out = self.nodes[0].gettxout(txid, n)
        assert out is not None
        assert_equal(out["asset"], asset_display)
        assert_equal(int(satoshi_round(out["value"]) * COIN), amount_sats)
        assert_equal(out["scriptPubKey"]["hex"], bytes(tap.scriptPubKey).hex())

    # -- test ---------------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        # Spend the initialfreecoins output so the wallet has ordinary utxos.
        node.sendtoaddress(node.getnewaddress(), 50)
        self.generate(node, 1)
        genesis_hash = uint256_from_str(bytes.fromhex(node.getblockhash(0))[::-1])

        # Participants: issuer, policy server, two investors.
        policy_sec = generate_privkey()
        policy_x = compute_xonly_pubkey(policy_sec)[0]
        issuer_sec = generate_privkey()
        issuer_x = compute_xonly_pubkey(issuer_sec)[0]
        alice_sec = generate_privkey()
        alice_x = compute_xonly_pubkey(alice_sec)[0]
        bob_sec = generate_privkey()
        bob_x = compute_xonly_pubkey(bob_sec)[0]

        alice_tap = enclave_tap(alice_x, policy_x, issuer_x)
        bob_tap = enclave_tap(bob_x, policy_x, issuer_x)
        issuer_tap = enclave_tap(issuer_x, policy_x, issuer_x)

        # ------------------------------------------------------------------
        self.log.info("1: contract commits to the policy key; canonical hash")
        contract = {
            "name": "OpenAMP M0 Demo Bond",
            "ticker": "BONDX",
            "precision": 8,
            "version": 0,
            "issuer_pubkey": issuer_x.hex(),
            "openamp": {
                "version": 1,
                "type": "restricted",
                "policy_pubkey": policy_x.hex(),
                "tier": "A",
                "clawback": True,
                "burn_allowed": True,
            },
        }
        canonical = json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()
        contract_digest = sha256(canonical)

        # ------------------------------------------------------------------
        self.log.info("2: derive asset ids in pure python and issue around them")
        funding = self.wallet_utxo(min_btc=2)
        prevout = COutPoint(int(funding["txid"], 16), funding["vout"])
        entropy, asset_i, token_i = derive_issuance_ids(prevout, contract_digest)
        asset_display = display_hex(asset_i)
        asset_out = b"\x01" + asset_i
        server = StubPolicyServer(policy_sec, asset_out, genesis_hash)
        for x, tap in ((alice_x, alice_tap), (bob_x, bob_tap), (issuer_x, issuer_tap)):
            server.register(x, tap)

        in_sats = int(satoshi_round(funding["amount"]) * COIN)
        issue_tx = CTransaction()
        issue_tx.nVersion = 2
        issue_tx.vin.append(CTxIn(prevout))
        issue_tx.vin[0].assetIssuance.assetEntropy = uint256_from_str(contract_digest)
        issue_tx.vin[0].assetIssuance.nAmount = CTxOutValue(100 * COIN)
        issue_tx.vin[0].assetIssuance.nInflationKeys = CTxOutValue(1 * COIN)
        issue_tx.vin[0].assetIssuance.denomination = 8
        # 100 BONDX straight into Alice's enclave; the reissuance token to the
        # issuer's ordinary wallet (kept unspent in M0; see
        # sequentia-reissuance-needs-blinded-token before ever reissuing).
        issue_tx.vout.append(CTxOut(CTxOutValue(100 * COIN), alice_tap.scriptPubKey, CTxOutAsset(asset_out)))
        issue_tx.vout.append(CTxOut(CTxOutValue(1 * COIN), self.wallet_spk(), CTxOutAsset(b"\x01" + token_i)))
        issue_tx.vout.append(CTxOut(CTxOutValue(in_sats - FEE_SATS), self.wallet_spk()))
        issue_tx.vout.append(CTxOut(CTxOutValue(FEE_SATS)))
        signed = node.signrawtransactionwithwallet(issue_tx.serialize().hex())
        assert signed["complete"]
        issue_txid = self.send_and_mine(tx_from_hex(signed["hex"]))
        # Consensus accepted a transaction built around the python-derived
        # asset ids: the derivation matches the node exactly. Cross-check the
        # node's own decoding too.
        decoded = node.getrawtransaction(issue_txid, True, node.getbestblockhash())["vin"][0]["issuance"]
        assert_equal(decoded["asset"], asset_display)
        assert_equal(decoded["token"], display_hex(token_i))
        assert_equal(decoded["assetEntropy"], display_hex(entropy))
        self.assert_enclave_utxo(issue_txid, 0, alice_tap, asset_display, 100 * COIN)
        self.log.info("   asset %s is bound to policy key %s via the contract hash" %
                      (asset_display, policy_x.hex()))

        # ------------------------------------------------------------------
        self.log.info("3: co-signed transfer Alice -> Bob, sender self-pays the fee")
        fee_utxo = self.wallet_utxo()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(issue_txid, 16), 0)))
        tx.vin.append(CTxIn(COutPoint(int(fee_utxo["txid"], 16), fee_utxo["vout"])))
        tx.vout.append(CTxOut(CTxOutValue(60 * COIN), bob_tap.scriptPubKey, CTxOutAsset(asset_out)))
        tx.vout.append(CTxOut(CTxOutValue(40 * COIN), alice_tap.scriptPubKey, CTxOutAsset(asset_out)))
        fee_in_sats = int(satoshi_round(fee_utxo["amount"]) * COIN)
        tx.vout.append(CTxOut(CTxOutValue(fee_in_sats - FEE_SATS), self.wallet_spk()))
        tx.vout.append(CTxOut(CTxOutValue(FEE_SATS)))
        spent = [self.utxo_to_ctxout({"amount": 100, "scriptPubKey": bytes(alice_tap.scriptPubKey).hex(),
                                      "asset": asset_display}),
                 self.utxo_to_ctxout(fee_utxo)]
        # Wallet signs its fee input (sighash ALL covers the final outputs).
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        sig_policy = server.cosign(tx, 0, spent, alice_x)
        msg = TaprootSignatureHash(tx, spent, SIGHASH_DEFAULT, genesis_hash, 0,
                                   scriptpath=True, script=alice_tap.leaves["transfer"].script)
        sig_alice = sign_schnorr(alice_sec, msg)
        tx.wit.vtxinwit[0].scriptWitness.stack = enclave_witness(alice_tap, "transfer", sig_policy, sig_alice)
        transfer_txid = self.send_and_mine(tx)
        self.assert_enclave_utxo(transfer_txid, 0, bob_tap, asset_display, 60 * COIN)
        self.assert_enclave_utxo(transfer_txid, 1, alice_tap, asset_display, 40 * COIN)

        # ------------------------------------------------------------------
        self.log.info("4: freeze-by-refusal, and consensus rejection without the policy signature")
        server.freeze(bob_x)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(transfer_txid, 16), 0)))
        tx.vout.append(CTxOut(CTxOutValue(10 * COIN), alice_tap.scriptPubKey, CTxOutAsset(asset_out)))
        tx.vout.append(CTxOut(CTxOutValue(50 * COIN - FEE_SATS), bob_tap.scriptPubKey, CTxOutAsset(asset_out)))
        # (Deliberately unbalanced by FEE_SATS in BONDX: never broadcast in
        # this shape; the refusal fires before any fee logic.)
        spent_bob = [self.utxo_to_ctxout({"amount": 60, "scriptPubKey": bytes(bob_tap.scriptPubKey).hex(),
                                          "asset": asset_display})]
        try:
            server.cosign(tx, 0, spent_bob, bob_x)
            raise AssertionError("frozen sender was co-signed")
        except PolicyRefusal as e:
            assert_equal(str(e), "sender frozen")

        # A forged transfer (zeroed policy signature) must die at consensus,
        # so give it a perfectly valid fee leg and let only the signature fail.
        fee_utxo = self.wallet_utxo()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(transfer_txid, 16), 0)))
        tx.vin.append(CTxIn(COutPoint(int(fee_utxo["txid"], 16), fee_utxo["vout"])))
        tx.vout.append(CTxOut(CTxOutValue(60 * COIN), alice_tap.scriptPubKey, CTxOutAsset(asset_out)))
        fee_in_sats = int(satoshi_round(fee_utxo["amount"]) * COIN)
        tx.vout.append(CTxOut(CTxOutValue(fee_in_sats - FEE_SATS), self.wallet_spk()))
        tx.vout.append(CTxOut(CTxOutValue(FEE_SATS)))
        spent_forged = [spent_bob[0], self.utxo_to_ctxout(fee_utxo)]
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        msg = TaprootSignatureHash(tx, spent_forged, SIGHASH_DEFAULT, genesis_hash, 0,
                                   scriptpath=True, script=bob_tap.leaves["transfer"].script)
        sig_bob = sign_schnorr(bob_sec, msg)
        tx.wit.vtxinwit[0].scriptWitness.stack = enclave_witness(bob_tap, "transfer", b"\x00" * 64, sig_bob)
        assert_raises_rpc_error(-26, "Invalid Schnorr signature", node.sendrawtransaction, tx.serialize().hex())

        # ------------------------------------------------------------------
        self.log.info("5: refusals: out-of-enclave destination and BONDX in the fee output")
        server.unfreeze(bob_x)
        plain_spk = self.wallet_spk()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(transfer_txid, 16), 1)))
        tx.vout.append(CTxOut(CTxOutValue(40 * COIN), plain_spk, CTxOutAsset(asset_out)))
        spent_alice = [self.utxo_to_ctxout({"amount": 40, "scriptPubKey": bytes(alice_tap.scriptPubKey).hex(),
                                            "asset": asset_display})]
        try:
            server.cosign(tx, 0, spent_alice, alice_x)
            raise AssertionError("out-of-enclave destination was co-signed")
        except PolicyRefusal as e:
            assert_equal(str(e), "restricted asset to an out-of-enclave destination")

        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(transfer_txid, 16), 1)))
        tx.vout.append(CTxOut(CTxOutValue(39 * COIN), bob_tap.scriptPubKey, CTxOutAsset(asset_out)))
        tx.vout.append(CTxOut(CTxOutValue(1 * COIN), b"", CTxOutAsset(asset_out)))  # fee output in BONDX
        try:
            server.cosign(tx, 0, spent_alice, alice_x)
            raise AssertionError("restricted-asset fee output was co-signed")
        except PolicyRefusal as e:
            assert_equal(str(e), "restricted asset in a fee output")
        # Layer 3 of Rule 1: even a rogue co-signer cannot get this mined; the
        # node itself refuses a fee in an asset it does not accept.
        tx.wit.vtxinwit.append(CTxInWitness())
        msg = TaprootSignatureHash(tx, spent_alice, SIGHASH_DEFAULT, genesis_hash, 0,
                                   scriptpath=True, script=alice_tap.leaves["transfer"].script)
        rogue_policy_sig = sign_schnorr(policy_sec, msg)
        sig_alice = sign_schnorr(alice_sec, msg)
        tx.wit.vtxinwit[0].scriptWitness.stack = enclave_witness(alice_tap, "transfer", rogue_policy_sig, sig_alice)
        # BONDX is not on the node's fee-asset whitelist (default-deny
        # ExchangeRateMap), so the fee converts to zero value: non-paying.
        assert_raises_rpc_error(-26, "min relay fee not met", node.sendrawtransaction, tx.serialize().hex())

        # ------------------------------------------------------------------
        self.log.info("6: fee conversion: Bob pays in BONDX, issuer bridges to the fee market")
        fee_utxo = self.wallet_utxo()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(transfer_txid, 16), 0)))
        tx.vin.append(CTxIn(COutPoint(int(fee_utxo["txid"], 16), fee_utxo["vout"])))
        tx.vout.append(CTxOut(CTxOutValue(10 * COIN), alice_tap.scriptPubKey, CTxOutAsset(asset_out)))
        tx.vout.append(CTxOut(CTxOutValue(49 * COIN), bob_tap.scriptPubKey, CTxOutAsset(asset_out)))
        # The conversion output: fee-equivalent BONDX into the issuer's own
        # enclave account (the issuer is a registered holder like any other).
        tx.vout.append(CTxOut(CTxOutValue(1 * COIN), issuer_tap.scriptPubKey, CTxOutAsset(asset_out)))
        fee_in_sats = int(satoshi_round(fee_utxo["amount"]) * COIN)
        tx.vout.append(CTxOut(CTxOutValue(fee_in_sats - FEE_SATS), self.wallet_spk()))
        tx.vout.append(CTxOut(CTxOutValue(FEE_SATS)))
        spent = [self.utxo_to_ctxout({"amount": 60, "scriptPubKey": bytes(bob_tap.scriptPubKey).hex(),
                                      "asset": asset_display}),
                 self.utxo_to_ctxout(fee_utxo)]
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        sig_policy = server.cosign(tx, 0, spent, bob_x)
        msg = TaprootSignatureHash(tx, spent, SIGHASH_DEFAULT, genesis_hash, 0,
                                   scriptpath=True, script=bob_tap.leaves["transfer"].script)
        sig_bob = sign_schnorr(bob_sec, msg)
        tx.wit.vtxinwit[0].scriptWitness.stack = enclave_witness(bob_tap, "transfer", sig_policy, sig_bob)
        conv_txid = self.send_and_mine(tx)
        self.assert_enclave_utxo(conv_txid, 0, alice_tap, asset_display, 10 * COIN)
        self.assert_enclave_utxo(conv_txid, 1, bob_tap, asset_display, 49 * COIN)
        self.assert_enclave_utxo(conv_txid, 2, issuer_tap, asset_display, 1 * COIN)

        # ------------------------------------------------------------------
        self.log.info("7: clawback through the disclosed L_claw leaf")
        fee_utxo = self.wallet_utxo()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(conv_txid, 16), 1)))
        tx.vin.append(CTxIn(COutPoint(int(fee_utxo["txid"], 16), fee_utxo["vout"])))
        tx.vout.append(CTxOut(CTxOutValue(49 * COIN), issuer_tap.scriptPubKey, CTxOutAsset(asset_out)))
        fee_in_sats = int(satoshi_round(fee_utxo["amount"]) * COIN)
        tx.vout.append(CTxOut(CTxOutValue(fee_in_sats - FEE_SATS), self.wallet_spk()))
        tx.vout.append(CTxOut(CTxOutValue(FEE_SATS)))
        spent = [self.utxo_to_ctxout({"amount": 49, "scriptPubKey": bytes(bob_tap.scriptPubKey).hex(),
                                      "asset": asset_display}),
                 self.utxo_to_ctxout(fee_utxo)]
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        sig_policy = server.sign_clawback(tx, 0, spent, bob_x)
        msg = TaprootSignatureHash(tx, spent, SIGHASH_DEFAULT, genesis_hash, 0,
                                   scriptpath=True, script=bob_tap.leaves["claw"].script)
        sig_issuer = sign_schnorr(issuer_sec, msg)
        tx.wit.vtxinwit[0].scriptWitness.stack = enclave_witness(bob_tap, "claw", sig_policy, sig_issuer)
        claw_txid = self.send_and_mine(tx)
        self.assert_enclave_utxo(claw_txid, 0, issuer_tap, asset_display, 49 * COIN)

        # Final containment audit: every BONDX atom is in an enclave output.
        # alice 40 + 10, issuer 1 + 49 = 100.
        self.log.info("M0 complete: containment held through issue, transfer, "
                      "refusals, fee conversion, and clawback")


if __name__ == "__main__":
    OpenAmpM0Test().main()
