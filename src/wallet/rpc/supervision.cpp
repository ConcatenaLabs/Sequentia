// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key_io.h>
#include <rpc/util.h>
#include <script/standard.h>
#include <util/strencodings.h>
#include <wallet/rpc/util.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

/* SEQUENTIA: the one wallet-side operation supervised assets need (src/supervision.h).
 *
 * Everything else about supervision is in src/rpc/supervision.cpp, where nothing
 * signs: the node says what to sign, the key signs wherever it lives, and the node
 * assembles the result. That separation is the point, and this RPC does not weaken
 * it -- it signs only with a key the wallet ALREADY holds, and it can produce
 * nothing for a key it does not. An issuer whose authority lives in an HSM or
 * behind FROST never comes here; their sighash goes out to the signer and the
 * signature comes back, exactly as before.
 *
 * It exists because the wallet can also be the place the keys were made: the Assets
 * page offers to derive both supervision keys from the wallet at issuance, for an
 * issuer who has no signing setup of their own. For them the freeze flow would
 * otherwise be blocked on a signature the wallet was perfectly able to make but had
 * no way to be asked for -- BIP340 over a bare 32-byte message is not something any
 * existing wallet RPC does (signmessage is ECDSA over a prefixed, hashed string).
 */

namespace wallet {

//! The private key behind an x-only public key, from whichever ScriptPubKeyMan
//! holds it. Twin of GetStakerKey in rpc/spend.cpp, and for the same reason: the
//! generic signing path is driven by scripts, and this key is being asked to sign
//! something that is not a script at all. The wallet must be unlocked.
//!
//! x-only means the parity is not in the key, so both candidates are tried; the
//! resulting signature verifies under the x-only key either way, because BIP340
//! signing negates an odd-Y secret for us.
static bool GetSupervisionKey(const CWallet& wallet, const XOnlyPubKey& xonly, CKey& key_out)
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

RPCHelpMan signsupervisionhash()
{
    return RPCHelpMan{"signsupervisionhash",
        "\nSign a supervision message with a key this wallet holds.\n"
        "\nTakes the sighash from getsupervisionrecordhash or getsupervisionunfreezehash and signs it\n"
        "with BIP340 Schnorr under the named x-only key, which must be one this wallet can spend with.\n"
        "\nThis is for the issuer whose supervision keys were derived from this wallet at issuance. An\n"
        "issuer whose keys live in an HSM, or behind a FROST quorum, signs the sighash there instead and\n"
        "never calls this: no supervision RPC has ever needed a private key, and none takes one.\n",
        {
            {"sighash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The 32-byte message to sign."},
            {"xonlykey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The x-only public key that must sign it, as getsupervisionrecordhash's signwith field indicates."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "signature", "The 64-byte BIP340 signature."},
        }},
        RPCExamples{
            HelpExampleCli("signsupervisionhash", "\"<sighash>\" \"<operationalkey>\"")
          + HelpExampleRpc("signsupervisionhash", "\"<sighash>\", \"<operationalkey>\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
        if (!wallet) return NullUniValue;

        const std::string sighash_hex = request.params[0].get_str();
        if (!IsHex(sighash_hex) || sighash_hex.size() != 64) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "sighash must be 32 bytes of hex");
        }
        const std::string key_hex = request.params[1].get_str();
        if (!IsHex(key_hex) || key_hex.size() != 64) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "xonlykey must be 32 bytes of hex (a BIP340 x-only public key)");
        }
        const XOnlyPubKey xonly(ParseHex(key_hex));
        if (!xonly.IsFullyValid()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "xonlykey is not a valid x-only public key");
        }
        // The sighash is a message, not a hash to be displayed: take the bytes in
        // the order the supervision RPCs hand them out (HexStr, not GetHex), or
        // every signature made here would be rejected for reasons that look like
        // anything but a reversed message.
        uint256 sighash;
        const std::vector<unsigned char> raw = ParseHex(sighash_hex);
        std::copy(raw.begin(), raw.end(), sighash.begin());

        LOCK(wallet->cs_wallet);
        EnsureWalletIsUnlocked(*wallet);

        CKey key;
        if (!GetSupervisionKey(*wallet, xonly, key)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                "this wallet does not hold the private key for that x-only key; sign the sighash "
                "wherever the key lives and pass the signature to buildsupervisionrecord");
        }

        std::vector<unsigned char> sig(64);
        if (!key.SignSchnorr(sighash, sig, /*merkle_root=*/nullptr, /*aux=*/uint256())) {
            throw JSONRPCError(RPC_WALLET_ERROR, "signing failed");
        }

        UniValue result(UniValue::VOBJ);
        result.pushKV("signature", HexStr(sig));
        return result;
    }};
}

} // namespace wallet
