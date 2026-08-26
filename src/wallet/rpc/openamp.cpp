// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <core_io.h>
#include <key_io.h>
#include <primitives/transaction.h>
#include <rpc/util.h>
#include <script/interpreter.h>
#include <util/strencodings.h>
#include <wallet/rpc/util.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

#include <algorithm>

/* SEQUENTIA: the wallet-side operation OpenAMP restricted assets need.
 *
 * OpenAMP (doc/sequentia/openamp-design.md) keeps a restricted asset in a 2-of-2
 * taproot "enclave": a NUMS internal key with no key path, and a leaf
 * `<K_user> CHECKSIGVERIFY <K_policy> CHECKSIG`. Every spend therefore needs the
 * holder's BIP340 signature alongside the issuer's policy server's. The server
 * builds the transaction and hands back a sighash per input; the holder signs it
 * and sends the signature back. That is the whole of the wallet's part, and no
 * existing RPC could do it: the enclave is not a script this wallet has a
 * descriptor for, so the generic signing path never sees it, and signmessage is
 * ECDSA over a prefixed string rather than BIP340 over a taproot sighash.
 *
 * The important difference from signsupervisionhash is that this one does not
 * take the server's word for what it is signing. A supervision sighash is a bare
 * tagged hash with no transaction behind it, so that RPC can only sign the 32
 * bytes it is handed. Here there IS a transaction, so the node recomputes the
 * sighash from it and refuses unless the two agree -- which means a signature
 * made here can authorise nothing but the exact transaction the caller passed
 * in, whatever the server claimed about it. It further checks, through the
 * control block, that the input really is an output committing to a leaf this
 * wallet's key appears in, so a server cannot get a holder to sign for an input
 * that is not theirs.
 *
 * A wallet holding no key for the named x-only pubkey produces nothing, so this
 * RPC hands out no authority the wallet did not already have.
 */

namespace wallet {

//! The private key behind an x-only public key. Twin of GetSupervisionKey in
//! wallet/rpc/supervision.cpp and of GetStakerKey in wallet/rpc/spend.cpp, and
//! for the same reason: the generic signing path is driven by descriptors, and
//! an enclave leaf is not something this wallet has one for.
//!
//! x-only means the parity is not in the key, so both candidates are tried; the
//! resulting signature verifies under the x-only key either way, because BIP340
//! signing negates an odd-Y secret for us.
static bool GetEnclaveKey(const CWallet& wallet, const XOnlyPubKey& xonly, CKey& key_out)
{
    for (const bool odd : {false, true}) {
        std::vector<unsigned char> compressed{static_cast<unsigned char>(odd ? 0x03 : 0x02)};
        compressed.insert(compressed.end(), xonly.begin(), xonly.end());
        const CPubKey pubkey(compressed);
        if (!pubkey.IsFullyValid()) continue;
        for (ScriptPubKeyMan* spk_man : wallet.GetAllScriptPubKeyMans()) {
            if (auto* legacy = dynamic_cast<LegacyScriptPubKeyMan*>(spk_man)) {
                if (legacy->GetKey(pubkey.GetID(), key_out)) return true;
            } else if (auto* desc = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man)) {
                if (desc->GetStakingKey(pubkey, key_out)) return true;
            }
        }
    }
    return false;
}

static std::vector<unsigned char> ParseHexField(const UniValue& v, const std::string& name, size_t expected_bytes = 0)
{
    if (!v.isStr() || !IsHex(v.get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be hex", name));
    }
    std::vector<unsigned char> out = ParseHex(v.get_str());
    if (expected_bytes != 0 && out.size() != expected_bytes) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be %d bytes of hex", name, expected_bytes));
    }
    return out;
}

RPCHelpMan signopenamptransfer()
{
    return RPCHelpMan{"signopenamptransfer",
        "\nSign OpenAMP enclave inputs of a transaction an issuer's policy server built.\n"
        "\nTakes the unsigned transaction and, for each input spending one of this wallet's enclave\n"
        "outputs, the leaf script and control block the policy server published for that enclave. The\n"
        "node recomputes each input's taproot sighash from the transaction itself and signs only when it\n"
        "matches the sighash the server asked for, so a signature made here can authorise nothing but\n"
        "the transaction you passed in.\n"
        "\nThe wallet must be unlocked, and must hold the key named for each input; it signs for no other.\n",
        {
            {"tx", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The unsigned transaction, as the policy server returned it."},
            {"inputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The enclave inputs to sign.",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"vin", RPCArg::Type::NUM, RPCArg::Optional::NO, "Index of the input to sign."},
                            {"sighash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The 32-byte sighash the policy server asked for; the signature is refused unless the node derives the same one."},
                            {"xonlykey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The x-only public key that must sign, as the server's to_sign entry names it."},
                            {"leaf", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The enclave's transfer leaf script, from the server's address endpoint."},
                            {"control", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The leaf's control block, from the server's address endpoint."},
                        }},
                }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::ARR, "signatures", "", {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::NUM, "vin", "The input this signature is for."},
                    {RPCResult::Type::STR_HEX, "signature", "The 64-byte BIP340 signature."},
                }},
            }},
        }},
        RPCExamples{
            HelpExampleCli("signopenamptransfer", "\"<txhex>\" \"[{\\\"vin\\\":0,\\\"sighash\\\":\\\"<hex>\\\",\\\"xonlykey\\\":\\\"<hex>\\\",\\\"leaf\\\":\\\"<hex>\\\",\\\"control\\\":\\\"<hex>\\\"}]\"")
          + HelpExampleRpc("signopenamptransfer", "\"<txhex>\", [{\"vin\":0,\"sighash\":\"<hex>\",\"xonlykey\":\"<hex>\",\"leaf\":\"<hex>\",\"control\":\"<hex>\"}]")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
        if (!wallet) return NullUniValue;

        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, request.params[0].get_str())) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "tx could not be decoded");
        }
        const UniValue& inputs = request.params[1].get_array();
        if (inputs.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "inputs is empty: there is nothing to sign");
        }

        // An Elements taproot sighash commits to one witness slot per input AND
        // per output, and deserialization only sizes those vectors when the
        // transaction already carried a witness. A transfer whose other inputs
        // are not signed yet arrives without one, so its vectors come back empty
        // and every sighash computed from it would be a message the node will
        // never reproduce once the transaction is complete. Size them to what the
        // finished transaction must have.
        mtx.witness.vtxinwit.resize(mtx.vin.size());
        mtx.witness.vtxoutwit.resize(mtx.vout.size());

        // Every prevout, not just the ones being signed: a taproot sighash commits
        // to the amount and scriptPubKey of all of them, the policy server's own
        // fee input included.
        std::map<COutPoint, Coin> coins;
        for (const CTxIn& txin : mtx.vin) coins[txin.prevout];
        wallet->chain().findCoins(coins);

        std::vector<CTxOut> spent_outputs;
        spent_outputs.reserve(mtx.vin.size());
        for (size_t i = 0; i < mtx.vin.size(); ++i) {
            const Coin& coin = coins.at(mtx.vin[i].prevout);
            if (coin.IsSpent()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                    "input %d spends %s, which this node cannot see. The sighash cannot be checked "
                    "without every prevout, so nothing was signed: wait for the funding transaction to "
                    "confirm or to reach this node's mempool.", i, mtx.vin[i].prevout.ToString()));
            }
            spent_outputs.push_back(coin.out);
        }
        // Kept for the control-block check below; Init takes its own copy by move.
        const std::vector<CTxOut> prevouts = spent_outputs;

        PrecomputedTransactionData txdata{Params().HashGenesisBlock()};
        txdata.Init(mtx, std::move(spent_outputs), /*force=*/true);
        if (!txdata.m_bip341_taproot_ready || !txdata.m_spent_outputs_ready) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "the transaction's taproot sighash data could not be precomputed");
        }

        LOCK(wallet->cs_wallet);
        EnsureWalletIsUnlocked(*wallet);

        UniValue signatures(UniValue::VARR);
        for (size_t n = 0; n < inputs.size(); ++n) {
            const UniValue& item = inputs[n];
            RPCTypeCheckObj(item, {
                {"vin", UniValueType(UniValue::VNUM)},
                {"sighash", UniValueType(UniValue::VSTR)},
                {"xonlykey", UniValueType(UniValue::VSTR)},
                {"leaf", UniValueType(UniValue::VSTR)},
                {"control", UniValueType(UniValue::VSTR)},
            });

            const int vin = item["vin"].get_int();
            if (vin < 0 || static_cast<size_t>(vin) >= mtx.vin.size()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("vin %d is not an input of this transaction", vin));
            }

            const std::vector<unsigned char> claimed = ParseHexField(item["sighash"], "sighash", 32);
            const std::vector<unsigned char> key_bytes = ParseHexField(item["xonlykey"], "xonlykey", 32);
            const std::vector<unsigned char> leaf_bytes = ParseHexField(item["leaf"], "leaf");
            const std::vector<unsigned char> control = ParseHexField(item["control"], "control");

            const XOnlyPubKey xonly(key_bytes);
            if (!xonly.IsFullyValid()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "xonlykey is not a valid x-only public key");
            }
            if (control.size() < TAPROOT_CONTROL_BASE_SIZE || control.size() > TAPROOT_CONTROL_MAX_SIZE ||
                ((control.size() - TAPROOT_CONTROL_BASE_SIZE) % TAPROOT_CONTROL_NODE_SIZE) != 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "control is not a well-formed taproot control block");
            }
            const uint8_t leaf_version = control[0] & TAPROOT_LEAF_MASK;
            if (leaf_version != TAPROOT_LEAF_TAPSCRIPT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                    "control names leaf version 0x%02x; an OpenAMP enclave leaf is 0x%02x",
                    leaf_version, TAPROOT_LEAF_TAPSCRIPT));
            }

            const CScript leaf_script(leaf_bytes.begin(), leaf_bytes.end());
            const uint256 tapleaf_hash = ComputeTapleafHash(leaf_version, leaf_script);

            // The leaf and control block must genuinely commit to the output being
            // spent. Without this a server could ask a holder to sign for an input
            // that is not theirs at all.
            int witness_version;
            std::vector<unsigned char> program;
            if (!prevouts[vin].scriptPubKey.IsWitnessProgram(witness_version, program) ||
                witness_version != 1 || program.size() != 32) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("input %d does not spend a taproot output", vin));
            }
            if (!VerifyTaprootCommitment(control, program, tapleaf_hash)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                    "input %d: the leaf and control block do not commit to the output being spent, so "
                    "this input is not the enclave they describe", vin));
            }
            if (std::search(leaf_bytes.begin(), leaf_bytes.end(), key_bytes.begin(), key_bytes.end()) == leaf_bytes.end()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                    "input %d: xonlykey does not appear in the leaf script it is asked to sign under", vin));
            }

            ScriptExecutionData execdata;
            execdata.m_annex_init = true;
            execdata.m_annex_present = false;
            execdata.m_codeseparator_pos_init = true;
            execdata.m_codeseparator_pos = 0xFFFFFFFF;
            execdata.m_tapleaf_hash_init = true;
            execdata.m_tapleaf_hash = tapleaf_hash;

            uint256 sighash;
            if (!SignatureHashSchnorr(sighash, execdata, mtx, vin, /*hash_type=*/SIGHASH_DEFAULT,
                                      SigVersion::TAPSCRIPT, txdata, MissingDataBehavior::FAIL)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("input %d: the sighash could not be computed", vin));
            }

            // The point of the whole RPC: the server's word is checked, not taken.
            // The bytes are compared in the order the server hands them out (HexStr,
            // not GetHex), which is the convention signsupervisionhash uses too.
            if (!std::equal(claimed.begin(), claimed.end(), sighash.begin())) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                    "input %d: the policy server asked for sighash %s but this transaction's is %s. "
                    "Nothing was signed.", vin, HexStr(claimed), HexStr(sighash)));
            }

            CKey key;
            if (!GetEnclaveKey(*wallet, xonly, key)) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
                    "input %d: this wallet does not hold the private key for %s", vin, HexStr(key_bytes)));
            }

            std::vector<unsigned char> sig(64);
            if (!key.SignSchnorr(sighash, sig, /*merkle_root=*/nullptr, /*aux=*/uint256())) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("input %d: signing failed", vin));
            }

            UniValue entry(UniValue::VOBJ);
            entry.pushKV("vin", vin);
            entry.pushKV("signature", HexStr(sig));
            signatures.push_back(entry);
        }

        UniValue result(UniValue::VOBJ);
        result.pushKV("signatures", signatures);
        return result;
    }};
}

} // namespace wallet
