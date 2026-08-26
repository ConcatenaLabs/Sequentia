#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The node-side half of being an OpenAMP holder.

OpenAMP keeps a restricted asset in a 2-of-2 taproot enclave -- the holder's key
and the issuer's policy key -- so a holder needs an account id the issuer knows
them by, the enclave address their units arrive at, and a BIP340 signature over
each sighash the policy server hands back. `getopenampaccount` derives the first
two offline and `signopenamptransfer` produces the third; together they are what
lets a Core wallet hold an issuer-governed asset at all.

What is checked here:

1. `getopenampaccount` reproduces openampd's own golden vectors exactly -- the
   account id, the enclave scriptPubKey, both leaves and both control blocks. A
   derivation that disagreed with the reference implementation by one byte would
   send a holder's units to an address nobody can spend.
2. It agrees with the test framework's independent taproot construction for
   freshly generated keys, so the vector match is not a coincidence of one case.
3. A transfer out of an enclave whose holder key belongs to the wallet is signed
   through `signopenamptransfer` and ACCEPTED BY CONSENSUS. That is the whole
   round trip, and only a correct signature over a correctly derived sighash
   survives it.
4. The refusals that make the RPC safe to point at somebody else's server: a
   sighash that does not match the transaction, a leaf and control block that do
   not commit to the input being spent, and a key the wallet does not hold. The
   first is the point of the RPC existing rather than a bare "sign these bytes".
"""

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
    CTxOutWitness,
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

FEE_SATS = 10000

# BIP341's nothing-up-my-sleeve point: openamp's enclave internal key, so that no
# key-path spend of an enclave exists.
NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")

# openampd's own vectors, from openampd/internal/elements/testdata/vectors.json
# ("enclave"), which were themselves generated against this test framework. They
# are pinned here so that a change to either side has to be deliberate.
VECTOR = {
    "user_x": "1b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f",
    "policy_x": "4d4b6cd1361032ca9bd2aeb9d900aa4d45d9ead80ac9423374c451a7254d0766",
    "issuer_x": "531fe6068134503d2723133227c867ac8fa6c83c537e9a44c3c5bdbdcb1fe337",
    "aid": "7a117ddc0d98c9756ac1586e80970924691a2117",
    "spk": "51208349d6e1cca7a163a8f68d896cea8e67bcb48cd01d1e3fd43a84b333e8b05313",
    "transfer_script": "201b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f"
                       "ad204d4b6cd1361032ca9bd2aeb9d900aa4d45d9ead80ac9423374c451a7254d0766ac",
    "claw_script": "20531fe6068134503d2723133227c867ac8fa6c83c537e9a44c3c5bdbdcb1fe337"
                   "ad204d4b6cd1361032ca9bd2aeb9d900aa4d45d9ead80ac9423374c451a7254d0766ac",
    "transfer_control": "c450929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0"
                        "7c6f03515d27cc7b1d13740c343648ce451ed4de790b9b677b7e1691fb44348d",
    "claw_control": "c450929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0"
                    "2eaafacdebea0578e8e9a47504ce92e2a56ff01c4926076babdd3af8750b4286",
}


def enclave_tap(user_x, policy_x, issuer_x):
    """The enclave: NUMS internal key, transfer leaf and clawback leaf."""
    transfer = CScript([user_x, OP_CHECKSIGVERIFY, policy_x, OP_CHECKSIG])
    claw = CScript([issuer_x, OP_CHECKSIGVERIFY, policy_x, OP_CHECKSIG])
    return taproot_construct(NUMS, [("transfer", transfer), ("claw", claw)])


def control_block(tap, leaf_name):
    leaf = tap.leaves[leaf_name]
    return bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch


def size_witness(tx):
    """One witness slot per input and per output before signing.

    Elements' taproot sighash commits to every output witness, and a transaction
    re-parsed from a witness-less serialization comes back with those vectors
    empty -- so signing before padding signs a message the node never reproduces.
    """
    while len(tx.wit.vtxinwit) < len(tx.vin):
        tx.wit.vtxinwit.append(CTxInWitness())
    while len(tx.wit.vtxoutwit) < len(tx.vout):
        tx.wit.vtxoutwit.append(CTxOutWitness())
    return tx


class OpenAmpWalletTest(BitcoinTestFramework):

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
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.setup_nodes()

    # -- helpers ------------------------------------------------------------

    def wallet_spk(self):
        addr = self.nodes[0].getnewaddress("", "bech32")
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

    # -- test ---------------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(address=node.getnewaddress(), amount=50, fee_asset_label='bitcoin')
        self.generate(node, 1)
        genesis_hash = uint256_from_str(bytes.fromhex(node.getblockhash(0))[::-1])

        self.log.info("1: getopenampaccount reproduces openampd's golden vectors")
        got = node.getopenampaccount([VECTOR["user_x"]], VECTOR["policy_x"], VECTOR["issuer_x"])
        assert_equal(got["aid"], VECTOR["aid"])
        assert_equal(got["script_pubkey"], VECTOR["spk"])
        assert_equal(got["transfer_leaf"], VECTOR["transfer_script"])
        assert_equal(got["transfer_control"], VECTOR["transfer_control"])
        assert_equal(got["claw_leaf"], VECTOR["claw_script"])
        assert_equal(got["claw_control"], VECTOR["claw_control"])

        # The account id is the key set's own hash, so it is derivable with no
        # policy key in sight -- which is what lets a wallet show it before the
        # holder has been told anything about a particular asset.
        assert_equal(node.getopenampaccount([VECTOR["user_x"]])["aid"], VECTOR["aid"])
        # And it does not depend on the order the keys are given in.
        pair = [VECTOR["user_x"], VECTOR["policy_x"]]
        assert_equal(node.getopenampaccount(pair)["aid"],
                     node.getopenampaccount(list(reversed(pair)))["aid"])

        self.log.info("2: and agrees with an independent construction for fresh keys")
        policy_sec = generate_privkey()
        policy_x = compute_xonly_pubkey(policy_sec)[0]
        issuer_sec = generate_privkey()
        issuer_x = compute_xonly_pubkey(issuer_sec)[0]

        # The holder key is an ordinary key of this wallet: that is the whole
        # point, since it is the wallet that has to sign with it later.
        holder_addr = node.getnewaddress("", "bech32")
        holder_info = node.getaddressinfo(holder_addr)
        holder_x = bytes.fromhex(holder_info["pubkey"])[1:]

        tap = enclave_tap(holder_x, policy_x, issuer_x)
        derived = node.getopenampaccount([holder_x.hex()], policy_x.hex(), issuer_x.hex())
        assert_equal(derived["script_pubkey"], bytes(tap.scriptPubKey).hex())
        assert_equal(derived["transfer_leaf"], bytes(tap.leaves["transfer"].script).hex())
        assert_equal(derived["transfer_control"], control_block(tap, "transfer").hex())
        assert_equal(derived["claw_control"], control_block(tap, "claw").hex())

        self.log.info("3: fund that enclave at the address the node derived for it")
        # Paying the derived address, rather than a scriptPubKey built here, is
        # what proves the address a holder would hand out is the enclave.
        fund_txid = node.sendtoaddress(address=derived["address"], amount=25,
                                       fee_asset_label='bitcoin')
        self.generate(node, 1)
        funded = tx_from_hex(node.gettransaction(fund_txid)["hex"])
        enclave_vout = None
        for n, out in enumerate(funded.vout):
            if bytes(out.scriptPubKey) == bytes(tap.scriptPubKey):
                enclave_vout = n
                break
        assert enclave_vout is not None, "the enclave address did not receive the payment"
        enclave_value = node.gettxout(fund_txid, enclave_vout)
        assert_equal(enclave_value["asset"], BITCOIN_ASSET)
        enclave_sats = int(satoshi_round(enclave_value["value"]) * COIN)

        self.log.info("4: the wallet signs a transfer out of it, and consensus accepts it")
        asset_commitment = CTxOutAsset(b"\x01" + bytes.fromhex(BITCOIN_ASSET)[::-1])
        fee_utxo = self.wallet_utxo()
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(fund_txid, 16), enclave_vout)))
        tx.vin.append(CTxIn(COutPoint(int(fee_utxo["txid"], 16), fee_utxo["vout"])))
        # The restricted units stay in the enclave; the fee rides in an ordinary
        # input, which is Rule 1 -- a restricted asset never reaches a fee output.
        tx.vout.append(CTxOut(CTxOutValue(enclave_sats), tap.scriptPubKey, asset_commitment))
        fee_in_sats = int(satoshi_round(fee_utxo["amount"]) * COIN)
        tx.vout.append(CTxOut(CTxOutValue(fee_in_sats - FEE_SATS), self.wallet_spk()))
        tx.vout.append(CTxOut(CTxOutValue(FEE_SATS)))

        spent = [
            self.utxo_to_ctxout({"amount": enclave_value["value"],
                                 "scriptPubKey": bytes(tap.scriptPubKey).hex(),
                                 "asset": BITCOIN_ASSET}),
            self.utxo_to_ctxout(fee_utxo),
        ]

        # The wallet signs its own fee input first; after that the transaction is
        # final except for the enclave witness, which is what the policy server
        # would have built and what it asks the holder to sign.
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = size_witness(tx_from_hex(partial["hex"]))
        tx_hex = tx.serialize().hex()

        server_sighash = TaprootSignatureHash(
            tx, spent, SIGHASH_DEFAULT, genesis_hash, 0,
            scriptpath=True, script=tap.leaves["transfer"].script)

        signed = node.signopenamptransfer(tx_hex, [{
            "vin": 0,
            "sighash": server_sighash.hex(),
            "xonlykey": holder_x.hex(),
            "leaf": derived["transfer_leaf"],
            "control": derived["transfer_control"],
        }])
        assert_equal(len(signed["signatures"]), 1)
        assert_equal(signed["signatures"][0]["vin"], 0)
        holder_sig = bytes.fromhex(signed["signatures"][0]["signature"])
        assert_equal(len(holder_sig), 64)

        # The refusals are exercised before the transfer is broadcast, while the
        # enclave output it spends still exists: every one of them has to get past
        # the prevout lookup to reach the check it is actually about.
        self.log.info("5: and it refuses everything that would be blind signing")

        # A sighash that is not this transaction's. This is the check that makes
        # the RPC safe to point at a server you do not control.
        wrong = bytearray(server_sighash)
        wrong[0] ^= 0xff
        assert_raises_rpc_error(-8, "the policy server asked for sighash",
                                node.signopenamptransfer, tx_hex, [{
                                    "vin": 0,
                                    "sighash": bytes(wrong).hex(),
                                    "xonlykey": holder_x.hex(),
                                    "leaf": derived["transfer_leaf"],
                                    "control": derived["transfer_control"],
                                }])

        # A leaf and control block from somebody else's enclave: they do not
        # commit to the output being spent, so this input is not that enclave.
        other_sec = generate_privkey()
        other_x = compute_xonly_pubkey(other_sec)[0]
        other = node.getopenampaccount([other_x.hex()], policy_x.hex(), issuer_x.hex())
        assert_raises_rpc_error(-8, "do not commit to the output being spent",
                                node.signopenamptransfer, tx_hex, [{
                                    "vin": 0,
                                    "sighash": server_sighash.hex(),
                                    "xonlykey": holder_x.hex(),
                                    "leaf": other["transfer_leaf"],
                                    "control": other["transfer_control"],
                                }])

        # A key this wallet does not hold produces nothing, so the RPC hands out
        # no authority the wallet did not already have. The policy key is the
        # sharpest case: it really is in this leaf and really is a signer of it,
        # so every structural check passes and only custody stops it.
        assert_raises_rpc_error(-4, "does not hold the private key",
                                node.signopenamptransfer, tx_hex, [{
                                    "vin": 0,
                                    "sighash": server_sighash.hex(),
                                    "xonlykey": policy_x.hex(),
                                    "leaf": derived["transfer_leaf"],
                                    "control": derived["transfer_control"],
                                }])

        # A key that is not in the leaf at all, even though the wallet holds it:
        # signing under it could only ever produce a signature nothing checks.
        stranger = node.getaddressinfo(node.getnewaddress("", "bech32"))["pubkey"][2:]
        assert_raises_rpc_error(-8, "does not appear in the leaf script",
                                node.signopenamptransfer, tx_hex, [{
                                    "vin": 0,
                                    "sighash": server_sighash.hex(),
                                    "xonlykey": stranger,
                                    "leaf": derived["transfer_leaf"],
                                    "control": derived["transfer_control"],
                                }])

        # And an input that is not an input.
        assert_raises_rpc_error(-8, "is not an input of this transaction",
                                node.signopenamptransfer, tx_hex, [{
                                    "vin": 9,
                                    "sighash": server_sighash.hex(),
                                    "xonlykey": holder_x.hex(),
                                    "leaf": derived["transfer_leaf"],
                                    "control": derived["transfer_control"],
                                }])

        self.log.info("6: the signed transfer is accepted by consensus")
        policy_sig = sign_schnorr(policy_sec, server_sighash)
        # <policy sig> <holder sig> <leaf> <control>, bottom to top: both leaves
        # run <K_a> CHECKSIGVERIFY <K_policy> CHECKSIG.
        tx.wit.vtxinwit[0].scriptWitness.stack = [
            policy_sig, holder_sig,
            bytes(tap.leaves["transfer"].script),
            control_block(tap, "transfer"),
        ]
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        assert txid in node.getblock(node.getbestblockhash())["tx"]
        # Consensus accepted the wallet's signature: the enclave moved, intact.
        moved = node.gettxout(txid, 0)
        assert_equal(moved["scriptPubKey"]["hex"], bytes(tap.scriptPubKey).hex())
        assert_equal(int(satoshi_round(moved["value"]) * COIN), enclave_sats)

        self.log.info("   a Core wallet is an OpenAMP holder: account, enclave, signature")


if __name__ == '__main__':
    OpenAmpWalletTest().main()
