// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/sha256.h>
#include <key_io.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <util/strencodings.h>

#include <algorithm>
#include <string>
#include <vector>

/* SEQUENTIA: the offline half of an OpenAMP holder's account.
 *
 * OpenAMP (doc/sequentia/openamp-restricted-assets.md) runs on an issuer's
 * policy server, and this node is not it. But two of the things a holder needs
 * are pure derivation from public keys, with no server and no chain state
 * involved, and doing them here is what lets a Core wallet be an OpenAMP holder
 * at all:
 *
 *  - the account id (AID) the issuer knows the holder by, which is a hash of the
 *    registered key set and nothing else; and
 *  - the enclave: the 2-of-2 taproot output a restricted asset is held in, whose
 *    address, leaf script and control block follow from the holder's key and the
 *    asset's policy key, both of which are public.
 *
 * Deriving them here rather than asking a server for them is not only
 * convenience. The policy key is committed in the asset id through the issuance
 * contract, so a holder who has the contract can check for themselves that the
 * address they are about to be paid at is the one that asset's own id implies --
 * rather than trusting the server that would be the party gaining from a lie.
 *
 * The construction is fixed by openamp's contract-v1 spec: internal key NUMS (so
 * there is no key-path spend), transfer leaf `<K_user> CHECKSIGVERIFY <K_policy>
 * CHECKSIG`, and where the asset was issued with clawback a second leaf with the
 * issuer's key in place of the holder's. Leaf version is Elements' 0xc4.
 */

namespace {

//! BIP341's nothing-up-my-sleeve point, which openamp uses as every enclave's
//! internal key so that no key-path spend exists.
const std::string OPENAMP_NUMS_HEX = "50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0";

XOnlyPubKey ParseXOnly(const UniValue& v, const std::string& name)
{
    const std::string hex = v.get_str();
    if (!IsHex(hex) || hex.size() != 64) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be 32 bytes of hex (a BIP340 x-only public key)", name));
    }
    const XOnlyPubKey key{ParseHex(hex)};
    if (!key.IsFullyValid()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s is not a valid x-only public key", name));
    }
    return key;
}

//! `<a> CHECKSIGVERIFY <b> CHECKSIG`, openamp's enclave leaf.
CScript CheckSigPair(const XOnlyPubKey& a, const XOnlyPubKey& b)
{
    return CScript() << ToByteVector(a) << OP_CHECKSIGVERIFY << ToByteVector(b) << OP_CHECKSIG;
}

//! sha256("openamp-aid-v1" || the sorted lowercase hex of each key), first 20
//! bytes. The keys are hashed as their hex TEXT, not as bytes, and sorted as
//! strings: that is what openampd does, and an AID derived any other way would
//! simply not be the holder's.
std::string ComputeAid(std::vector<std::string> keys_hex)
{
    for (std::string& k : keys_hex) k = ToLower(k);
    std::sort(keys_hex.begin(), keys_hex.end());
    CSHA256 hasher;
    const std::string tag = "openamp-aid-v1";
    hasher.Write(reinterpret_cast<const unsigned char*>(tag.data()), tag.size());
    for (const std::string& k : keys_hex) {
        hasher.Write(reinterpret_cast<const unsigned char*>(k.data()), k.size());
    }
    unsigned char out[CSHA256::OUTPUT_SIZE];
    hasher.Finalize(out);
    return HexStr(Span<const unsigned char>(out, 20));
}

RPCHelpMan getopenampaccount()
{
    return RPCHelpMan{"getopenampaccount",
        "\nDerive an OpenAMP account id, and optionally the enclave a restricted asset is held in.\n"
        "\nThe account id (AID) is a hash of the registered key set, so it can be derived here with no\n"
        "policy server involved: it is what an issuer, or a platform built on one, identifies the holder\n"
        "by. Give a policy key as well and the enclave itself is derived -- the address units arrive at,\n"
        "and the leaf script and control block needed to spend them.\n"
        "\nThe policy key is committed in the asset's id through its issuance contract, so deriving the\n"
        "enclave here lets a holder confirm the address they are paid at is the one the asset's own id\n"
        "implies, rather than taking the server's word for it.\n"
        "\nThis derives from public keys only. It reaches no network, touches no wallet, and needs no chain.\n",
        {
            {"pubkeys", RPCArg::Type::ARR, RPCArg::Optional::NO, "The x-only public keys registered for the account. The first is the active enclave key.",
                {
                    {"", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "An x-only public key."},
                }},
            {"policykey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The asset's policy key, from its issuance contract. Given this, the enclave is derived too."},
            {"issuerkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The asset's issuer key, if it was issued with clawback. Adds the disclosed clawback leaf."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "aid", "The account id."},
            {RPCResult::Type::STR, "address", /*optional=*/true, "The enclave address, when a policy key was given."},
            {RPCResult::Type::STR_HEX, "script_pubkey", /*optional=*/true, "The enclave's scriptPubKey."},
            {RPCResult::Type::STR_HEX, "transfer_leaf", /*optional=*/true, "The leaf script the holder spends through."},
            {RPCResult::Type::STR_HEX, "transfer_control", /*optional=*/true, "That leaf's control block."},
            {RPCResult::Type::STR_HEX, "claw_leaf", /*optional=*/true, "The clawback leaf, when an issuer key was given."},
            {RPCResult::Type::STR_HEX, "claw_control", /*optional=*/true, "That leaf's control block."},
        }},
        RPCExamples{
            HelpExampleCli("getopenampaccount", "\"[\\\"<xonlykey>\\\"]\"")
          + HelpExampleCli("getopenampaccount", "\"[\\\"<xonlykey>\\\"]\" \"<policykey>\"")
          + HelpExampleRpc("getopenampaccount", "[\"<xonlykey>\"], \"<policykey>\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        const UniValue& keys = request.params[0].get_array();
        if (keys.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "pubkeys is empty: an account is its key set");
        }
        std::vector<std::string> keys_hex;
        for (size_t i = 0; i < keys.size(); ++i) {
            // Validated as keys even though the AID hashes their text: a typo that
            // is not a point on the curve would otherwise become a real-looking AID
            // that no enclave could ever be derived for.
            ParseXOnly(keys[i], strprintf("pubkeys[%d]", i));
            keys_hex.push_back(keys[i].get_str());
        }

        UniValue result(UniValue::VOBJ);
        result.pushKV("aid", ComputeAid(keys_hex));

        if (request.params[1].isNull()) return result;

        const XOnlyPubKey user_key = ParseXOnly(keys[0], "pubkeys[0]");
        const XOnlyPubKey policy_key = ParseXOnly(request.params[1], "policykey");
        const bool clawback = !request.params[2].isNull();

        const CScript transfer_leaf = CheckSigPair(user_key, policy_key);
        CScript claw_leaf;
        if (clawback) {
            claw_leaf = CheckSigPair(ParseXOnly(request.params[2], "issuerkey"), policy_key);
        }

        // Both leaves sit at depth 1 when there are two, and the single leaf at
        // depth 0 when there is one -- openamp's tree, and BIP341 sorts each pair
        // when it hashes, so the order they go in does not change the result.
        TaprootBuilder builder;
        if (clawback) {
            builder.Add(1, transfer_leaf, TAPROOT_LEAF_TAPSCRIPT);
            builder.Add(1, claw_leaf, TAPROOT_LEAF_TAPSCRIPT);
        } else {
            builder.Add(0, transfer_leaf, TAPROOT_LEAF_TAPSCRIPT);
        }
        const XOnlyPubKey nums{ParseHex(OPENAMP_NUMS_HEX)};
        builder.Finalize(nums);
        if (!builder.IsComplete()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "the enclave tree did not finalize");
        }

        const WitnessV1Taproot output = builder.GetOutput();
        result.pushKV("address", EncodeDestination(output));
        result.pushKV("script_pubkey", HexStr(GetScriptForDestination(output)));

        const TaprootSpendData spend_data = builder.GetSpendData();
        auto control_for = [&](const CScript& leaf) -> std::string {
            const auto it = spend_data.scripts.find({leaf, TAPROOT_LEAF_TAPSCRIPT});
            if (it == spend_data.scripts.end() || it->second.empty()) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "the enclave leaf has no control block");
            }
            return HexStr(*it->second.begin());
        };

        result.pushKV("transfer_leaf", HexStr(transfer_leaf));
        result.pushKV("transfer_control", control_for(transfer_leaf));
        if (clawback) {
            result.pushKV("claw_leaf", HexStr(claw_leaf));
            result.pushKV("claw_control", control_for(claw_leaf));
        }
        return result;
    }};
}

} // namespace

void RegisterOpenAmpRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"openamp", &getopenampaccount},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
