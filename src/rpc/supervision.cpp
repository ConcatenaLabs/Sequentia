// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <core_io.h>
#include <issuance.h>
#include <key_io.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <supervision.h>
#include <util/strencodings.h>

/* SEQUENTIA: RPCs for supervised assets (src/supervision.h).
 *
 * Every one of these is a pure function of its arguments or of the registry,
 * and NONE of them signs anything. That is the point, not an omission. The
 * authority over a supervised stablecoin is the thing an issuer guards hardest:
 * Alberto's point 1 chose BIP340 precisely so that key can live behind FROST,
 * split across officers and geographies, and never exist in one place. An RPC
 * that took a private key would undo that by inviting the key onto a node.
 *
 * So the flow is: ask the node what to sign (getsupervisionrecordhash), sign it
 * wherever the key lives, and hand the signature back to be assembled
 * (buildsupervisionrecord). A node that is compromised leaks nothing but public
 * data.
 */

static XOnlyPubKey ParseXOnlyKey(const std::string& hex, const std::string& name)
{
    if (!IsHex(hex) || hex.size() != 64) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, name + " must be 32 bytes of hex (a BIP340 x-only public key)");
    }
    const XOnlyPubKey key(ParseHex(hex));
    if (!key.IsFullyValid()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, name + " is not a valid x-only public key");
    }
    return key;
}

static SupervisionDescriptor ParseDescriptorArgs(const std::string& operational,
                                                 const std::string& recovery)
{
    SupervisionDescriptor desc;
    desc.operational_key = ParseXOnlyKey(operational, "operationalkey");
    desc.recovery_key = ParseXOnlyKey(recovery, "recoverykey");
    std::string err;
    if (!ValidateSupervisionDescriptor(desc, err)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, err);
    }
    return desc;
}

static CAsset ParseAssetArg(const std::string& hex)
{
    if (!IsHex(hex) || hex.size() != 64) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "asset must be 32 bytes of hex");
    }
    return CAsset(uint256S(hex));
}

//! The script a freeze names, from either an address or a raw scriptPubKey.
static CScript ParseTargetScript(const std::string& target)
{
    if (IsHex(target)) {
        const std::vector<unsigned char> raw = ParseHex(target);
        return CScript(raw.begin(), raw.end());
    }
    const CTxDestination dest = DecodeDestination(target);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "target is neither an address nor a scriptPubKey");
    }
    return GetScriptForDestination(dest);
}

static SupervisionRecordKind ParseRecordKind(const std::string& kind)
{
    if (kind == "freeze") return SupervisionRecordKind::FREEZE;
    if (kind == "rotateoperational") return SupervisionRecordKind::ROTATE_OPERATIONAL;
    if (kind == "rotaterecovery") return SupervisionRecordKind::ROTATE_RECOVERY;
    throw JSONRPCError(RPC_INVALID_PARAMETER,
                       "kind must be one of freeze, rotateoperational, rotaterecovery");
}

static std::string RecordKindName(SupervisionRecordKind kind)
{
    switch (kind) {
    case SupervisionRecordKind::FREEZE: return "freeze";
    case SupervisionRecordKind::ROTATE_OPERATIONAL: return "rotateoperational";
    case SupervisionRecordKind::ROTATE_RECOVERY: return "rotaterecovery";
    }
    return "unknown";
}

static void RequireSupervisionScheduled()
{
    if (g_supervision_height <= 0) {
        throw JSONRPCError(RPC_MISC_ERROR,
            "supervised assets are not scheduled on this chain (supervised_assets_height is 0)");
    }
}

//! Assemble the record described by the arguments, without its signature. Both
//! the sighash RPC and the build RPC go through here, so the bytes an issuer
//! signs and the bytes that reach the chain cannot drift apart.
static SupervisionRecord RecordFromArgs(const JSONRPCRequest& request)
{
    SupervisionRecord record;
    record.kind = ParseRecordKind(request.params[0].get_str());
    record.asset = ParseAssetArg(request.params[1].get_str());

    if (record.IsRotation()) {
        record.SetNewKey(ParseXOnlyKey(request.params[2].get_str(), "newkey"));
        record.old_key = ParseXOnlyKey(request.params[3].get_str(), "oldkey");
        if (record.NewKey() == record.old_key) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "newkey and oldkey are the same key");
        }
    } else {
        record.target = SupervisionTargetHash(ParseTargetScript(request.params[2].get_str()));
    }
    return record;
}

static RPCHelpMan getsupervisedassetid()
{
    return RPCHelpMan{"getsupervisedassetid",
        "\nDerive the asset id a supervised issuance would create, and the declaration output it must carry.\n"
        "\nThe descriptor is committed IN the asset id, which is what makes supervision impossible to add to an\n"
        "asset later, remove, or alter: change any field here and you are describing a different asset. Derive\n"
        "the id before issuing, so the issuance transaction can name it in its declaration output.\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The txid of the outpoint the issuance spends."},
            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The index of that outpoint."},
            {"operationalkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "x-only key that will sign freezes and unfreezes."},
            {"recoverykey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "x-only key that will sign rotations, and nothing else. Keep it cold."},
            {"contracthash", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Contract hash committing to the asset's terms (default: zero)."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "asset", "The supervised asset id."},
            {RPCResult::Type::STR_HEX, "token", "Its reissuance token. A supervised issuance must create these."},
            {RPCResult::Type::STR_HEX, "entropy", "The issuance entropy, which a later reissuance must quote."},
            {RPCResult::Type::STR_HEX, "declarationscript", "scriptPubKey of the declaration output the issuance must carry, holding zero of the asset."},
        }},
        RPCExamples{
            HelpExampleCli("getsupervisedassetid", "\"<txid>\" 0 \"<operationalkey>\" \"<recoverykey>\"")
          + HelpExampleRpc("getsupervisedassetid", "\"<txid>\", 0, \"<operationalkey>\", \"<recoverykey>\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        RequireSupervisionScheduled();
        const COutPoint prevout(ParseHashV(request.params[0], "txid"), request.params[1].get_int());
        const SupervisionDescriptor desc =
            ParseDescriptorArgs(request.params[2].get_str(), request.params[3].get_str());
        uint256 contract;
        if (!request.params[4].isNull()) contract = ParseHashV(request.params[4], "contracthash");

        uint256 entropy;
        GenerateSupervisedAssetEntropy(entropy, prevout, contract, desc);
        CAsset asset, token;
        CalculateAsset(asset, entropy);
        CalculateReissuanceToken(token, entropy, false);

        UniValue result(UniValue::VOBJ);
        result.pushKV("asset", asset.GetHex());
        result.pushKV("token", token.GetHex());
        result.pushKV("entropy", entropy.GetHex());
        const CScript declaration = BuildSupervisionScript(asset, desc);
        result.pushKV("declarationscript", HexStr(declaration));
        return result;
    }};
}

static RPCHelpMan getsupervisionrecordhash()
{
    return RPCHelpMan{"getsupervisionrecordhash",
        "\nThe message an issuer must sign to have a supervision record admitted.\n"
        "\nSign it with BIP340 Schnorr under the asset's CURRENT operational key (for a freeze) or CURRENT\n"
        "recovery key (for a rotation), wherever that key lives, and pass the signature to buildsupervisionrecord.\n"
        "This node never sees the key.\n"
        "\nThe message binds the transaction's first input, so the signature works in that transaction and no\n"
        "other. Without that, a freeze signature could be lifted off the chain and replayed by anyone to\n"
        "re-freeze a target the issuer had deliberately unfrozen.\n",
        {
            {"kind", RPCArg::Type::STR, RPCArg::Optional::NO, "freeze, rotateoperational, or rotaterecovery."},
            {"asset", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The supervised asset."},
            {"target", RPCArg::Type::STR, RPCArg::Optional::NO, "For freeze: the address or scriptPubKey to freeze. For a rotation: the new x-only key."},
            {"oldkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Rotations only: the x-only key being replaced, which must be the current one."},
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "txid of the first input the record's transaction will spend."},
            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "vout of that input."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "sighash", "The 32-byte message to sign with BIP340 Schnorr."},
            {RPCResult::Type::STR, "signwith", "Which key must sign: operational or recovery."},
        }},
        RPCExamples{
            HelpExampleCli("getsupervisionrecordhash", "\"freeze\" \"<asset>\" \"<address>\" null \"<txid>\" 0")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        RequireSupervisionScheduled();
        const SupervisionRecord record = RecordFromArgs(request);
        const COutPoint first_input(ParseHashV(request.params[4], "txid"),
                                    request.params[5].get_int());

        UniValue result(UniValue::VOBJ);
        // HexStr, not GetHex: uint256::GetHex reverses the bytes for display,
        // and what an issuer must sign is the message consensus verifies over,
        // which is the internal order. Returning the display form would hand
        // out a message that reads backwards, and every signature made from it
        // would be rejected for reasons that look like anything but this.
        result.pushKV("sighash", HexStr(SupervisionRecordSigHash(record, first_input)));
        result.pushKV("signwith", record.IsRotation() ? "recovery" : "operational");
        return result;
    }};
}

static RPCHelpMan buildsupervisionrecord()
{
    return RPCHelpMan{"buildsupervisionrecord",
        "\nAssemble a supervision record's output script from a signature produced offline.\n"
        "\nInclude the returned script as an output carrying ZERO of the record's own asset. A freeze record\n"
        "freezes when it confirms, and spending it later is the unfreeze; a rotation record can never be spent.\n"
        "\nA freeze binds only single-owner scripts: ordinary key-controlled addresses. Shared scripts, which is\n"
        "to say Lightning channels, HTLCs and covenants, are out of reach by design, because the frozen party is\n"
        "not the only party to those funds.\n",
        {
            {"kind", RPCArg::Type::STR, RPCArg::Optional::NO, "freeze, rotateoperational, or rotaterecovery."},
            {"asset", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The supervised asset."},
            {"target", RPCArg::Type::STR, RPCArg::Optional::NO, "For freeze: the address or scriptPubKey. For a rotation: the new x-only key."},
            {"oldkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Rotations only: the x-only key being replaced."},
            {"signature", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The 64-byte BIP340 signature over getsupervisionrecordhash."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "script", "scriptPubKey of the record output."},
            {RPCResult::Type::STR_HEX, "targethash", "The script hash this record names, as the registry stores it."},
        }},
        RPCExamples{
            HelpExampleCli("buildsupervisionrecord", "\"freeze\" \"<asset>\" \"<address>\" null \"<signature>\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        RequireSupervisionScheduled();
        SupervisionRecord record = RecordFromArgs(request);

        const std::string sig = request.params[4].get_str();
        if (!IsHex(sig) || sig.size() != 128) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "signature must be a 64-byte BIP340 signature in hex");
        }
        record.signature = ParseHex(sig);

        UniValue result(UniValue::VOBJ);
        result.pushKV("script", HexStr(BuildSupervisionRecordScript(record)));
        result.pushKV("targethash", record.target.GetHex());
        return result;
    }};
}

static RPCHelpMan getsupervisionunfreezehash()
{
    return RPCHelpMan{"getsupervisionunfreezehash",
        "\nThe message that authorises spending a freeze record, which is how a freeze is lifted.\n"
        "\nSign it with the asset's CURRENT operational key and put the signature, as a single push, in the\n"
        "spending input's scriptSig.\n"
        "\nThe current key, not the one the record was created under: after a rotation a stolen old key lifts\n"
        "nothing, which is the whole reason for having a separate recovery key.\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "txid of the freeze record output."},
            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "vout of the freeze record output."},
            {"asset", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The record's asset."},
            {"targethash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The record's target hash."},
        },
        RPCResult{RPCResult::Type::STR_HEX, "sighash", "The 32-byte message to sign with BIP340 Schnorr."},
        RPCExamples{
            HelpExampleCli("getsupervisionunfreezehash", "\"<txid>\" 0 \"<asset>\" \"<targethash>\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        RequireSupervisionScheduled();
        const COutPoint outpoint(ParseHashV(request.params[0], "txid"),
                                 request.params[1].get_int());
        const CAsset asset = ParseAssetArg(request.params[2].get_str());
        const uint256 target = ParseHashV(request.params[3], "targethash");
        // HexStr, not GetHex; see getsupervisionrecordhash.
        return HexStr(SupervisionUnfreezeSigHash(outpoint, asset, target));
    }};
}

static RPCHelpMan getsupervisedassets()
{
    return RPCHelpMan{"getsupervisedassets",
        "\nEvery supervised asset this chain knows, with the terms it was issued under and its keys as they\n"
        "stand now.\n"
        "\nThe descriptor is what the asset id commits to and can never change. The current keys start there and\n"
        "move only by rotation records.\n",
        {},
        RPCResult{RPCResult::Type::ARR, "", "", {
            {RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR_HEX, "asset", "The asset id."},
                {RPCResult::Type::NUM, "version", "Supervision descriptor version."},
                {RPCResult::Type::NUM, "featurebits", "Reserved; always 0 in this release."},
                {RPCResult::Type::STR_HEX, "issuedoperationalkey", "Operational key committed at issuance."},
                {RPCResult::Type::STR_HEX, "issuedrecoverykey", "Recovery key committed at issuance."},
                {RPCResult::Type::STR_HEX, "operationalkey", "Operational key now in force."},
                {RPCResult::Type::STR_HEX, "recoverykey", "Recovery key now in force."},
                {RPCResult::Type::NUM, "frozen", "How many distinct scripts are frozen for this asset."},
            }},
        }},
        RPCExamples{HelpExampleCli("getsupervisedassets", "") + HelpExampleRpc("getsupervisedassets", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        const SupervisionRegistry& registry = SupervisionRegistry::GetInstance();
        UniValue out(UniValue::VARR);
        for (const auto& entry : registry.Assets()) {
            UniValue item(UniValue::VOBJ);
            item.pushKV("asset", entry.first.GetHex());
            item.pushKV("version", (int)entry.second.version);
            item.pushKV("featurebits", (int)entry.second.feature_bits);
            item.pushKV("issuedoperationalkey", HexStr(entry.second.operational_key));
            item.pushKV("issuedrecoverykey", HexStr(entry.second.recovery_key));
            const auto operational = registry.OperationalKey(entry.first);
            const auto recovery = registry.RecoveryKey(entry.first);
            if (operational) item.pushKV("operationalkey", HexStr(*operational));
            if (recovery) item.pushKV("recoverykey", HexStr(*recovery));
            item.pushKV("frozen", (int)registry.FrozenTargets(entry.first).size());
            out.push_back(item);
        }
        return out;
    }};
}

static RPCHelpMan getassetfreezes()
{
    return RPCHelpMan{"getassetfreezes",
        "\nThe scripts currently frozen for a supervised asset.\n"
        "\nScripts are named by hash, because a record is fixed-width whatever it freezes. Use\n"
        "isassetfrozen to ask about a particular address. The count is the number of unspent freeze records\n"
        "naming that script: two records may name one target, and the freeze lifts only when the last is spent.\n",
        {
            {"asset", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The supervised asset."},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {
            {RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR_HEX, "targethash", "Hash of the frozen scriptPubKey."},
                {RPCResult::Type::NUM, "records", "Unspent freeze records naming it."},
            }},
        }},
        RPCExamples{HelpExampleCli("getassetfreezes", "\"<asset>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        const CAsset asset = ParseAssetArg(request.params[0].get_str());
        const SupervisionRegistry& registry = SupervisionRegistry::GetInstance();
        if (!registry.IsSupervised(asset)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "asset is not supervised");
        }
        UniValue out(UniValue::VARR);
        for (const auto& entry : registry.FrozenTargets(asset)) {
            UniValue item(UniValue::VOBJ);
            item.pushKV("targethash", entry.first.GetHex());
            item.pushKV("records", (int)entry.second);
            out.push_back(item);
        }
        return out;
    }};
}

static RPCHelpMan isassetfrozen()
{
    return RPCHelpMan{"isassetfrozen",
        "\nWhether a given address or script is frozen for a given asset, and whether a freeze could reach it\n"
        "at all.\n"
        "\nThe second answer is the one that surprises people. A freeze binds only SINGLE-OWNER scripts. A\n"
        "Lightning channel's funding output, an HTLC, a covenant, any shared script is out of reach, because\n"
        "the frozen party is not the only party to those funds and blocking the spend would strand a\n"
        "counterparty who did nothing. Consensus cannot freeze a Lightning balance; breaking a blacklisted\n"
        "holder's channels is the Lightning implementation's job.\n",
        {
            {"asset", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The supervised asset."},
            {"target", RPCArg::Type::STR, RPCArg::Optional::NO, "An address, or a scriptPubKey in hex."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::BOOL, "frozen", "Whether a freeze record currently names this script."},
            {RPCResult::Type::BOOL, "freezable", "Whether a freeze could bind it, i.e. whether it is single-owner."},
            {RPCResult::Type::STR_HEX, "targethash", "The hash a record would name."},
        }},
        RPCExamples{HelpExampleCli("isassetfrozen", "\"<asset>\" \"<address>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        const CAsset asset = ParseAssetArg(request.params[0].get_str());
        const CScript script = ParseTargetScript(request.params[1].get_str());
        const uint256 target = SupervisionTargetHash(script);

        // Freezability is a property of the SPEND, so it is judged here from
        // the script's shape alone: a witness that would make it single-owner
        // is synthesised. That matches what the spend rule will decide for the
        // only witness these script types accept.
        std::vector<std::vector<unsigned char>> vSolutions;
        const TxoutType type = Solver(script, vSolutions);
        const bool freezable = type == TxoutType::PUBKEY || type == TxoutType::PUBKEYHASH ||
                               type == TxoutType::WITNESS_V0_KEYHASH ||
                               type == TxoutType::WITNESS_V1_TAPROOT ||
                               type == TxoutType::SCRIPTHASH;

        UniValue result(UniValue::VOBJ);
        result.pushKV("frozen", SupervisionRegistry::GetInstance().IsFrozen(asset, target));
        result.pushKV("freezable", freezable);
        result.pushKV("targethash", target.GetHex());
        return result;
    }};
}

static RPCHelpMan decodesupervisionscript()
{
    return RPCHelpMan{"decodesupervisionscript",
        "\nRead a supervision declaration or record out of an output script.\n"
        "\nUseful for auditing: every supervised asset and every freeze on this chain is a retained output, so\n"
        "the whole supervision state can be read off the UTXO set and checked independently of any node's\n"
        "in-memory registry.\n",
        {
            {"script", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The scriptPubKey."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR, "type", "declaration, record, or none."},
            {RPCResult::Type::STR_HEX, "asset", /*optional=*/true, "The asset it concerns."},
            {RPCResult::Type::STR, "kind", /*optional=*/true, "For a record: freeze, rotateoperational or rotaterecovery."},
            {RPCResult::Type::STR_HEX, "targethash", /*optional=*/true, "For a freeze record: the frozen script's hash."},
            {RPCResult::Type::STR_HEX, "newkey", /*optional=*/true, "For a rotation record: the key installed."},
            {RPCResult::Type::STR_HEX, "oldkey", /*optional=*/true, "For a rotation record: the key replaced."},
            {RPCResult::Type::STR_HEX, "operationalkey", /*optional=*/true, "For a declaration: the key committed at issuance."},
            {RPCResult::Type::STR_HEX, "recoverykey", /*optional=*/true, "For a declaration: the recovery key committed at issuance."},
        }},
        RPCExamples{HelpExampleCli("decodesupervisionscript", "\"<scripthex>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        const std::string hex = request.params[0].get_str();
        if (!IsHex(hex)) throw JSONRPCError(RPC_INVALID_PARAMETER, "script must be hex");
        const std::vector<unsigned char> raw = ParseHex(hex);
        const CScript script(raw.begin(), raw.end());

        UniValue result(UniValue::VOBJ);
        if (const auto decl = ParseSupervisionScript(script)) {
            result.pushKV("type", "declaration");
            result.pushKV("asset", decl->asset.GetHex());
            result.pushKV("operationalkey", HexStr(decl->descriptor.operational_key));
            result.pushKV("recoverykey", HexStr(decl->descriptor.recovery_key));
            return result;
        }
        if (const auto record = ParseSupervisionRecordScript(script)) {
            result.pushKV("type", "record");
            result.pushKV("kind", RecordKindName(record->kind));
            result.pushKV("asset", record->asset.GetHex());
            if (record->IsRotation()) {
                result.pushKV("newkey", HexStr(record->NewKey()));
                result.pushKV("oldkey", HexStr(record->old_key));
            } else {
                result.pushKV("targethash", record->target.GetHex());
            }
            return result;
        }
        result.pushKV("type", "none");
        return result;
    }};
}


static RPCHelpMan addsupervisionrecordoutput()
{
    return RPCHelpMan{"addsupervisionrecordoutput",
        "\nAppend a supervision record output to a raw transaction.\n"
        "\nA record's script is bare: no address encodes it, and no address should, because it is not somewhere\n"
        "value can be sent. The output carries zero of the asset the record governs, which keeps an issuer from\n"
        "burning value into a script only consensus can release and keeps the output clear of the dust rule.\n",
        {
            {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The raw transaction."},
            {"script", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The record script from buildsupervisionrecord."},
            {"asset", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The asset the record governs."},
        },
        RPCResult{RPCResult::Type::STR_HEX, "hex", "The transaction with the record output appended."},
        RPCExamples{HelpExampleCli("addsupervisionrecordoutput", "\"<rawtx>\" \"<script>\" \"<asset>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, request.params[0].get_str())) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        }
        const std::string script_hex = request.params[1].get_str();
        if (!IsHex(script_hex)) throw JSONRPCError(RPC_INVALID_PARAMETER, "script must be hex");
        const std::vector<unsigned char> raw = ParseHex(script_hex);
        const CScript script(raw.begin(), raw.end());
        if (!ParseSupervisionRecordScript(script) && !ParseSupervisionScript(script)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "script is not a supervision record or declaration");
        }
        const CAsset asset = ParseAssetArg(request.params[2].get_str());

        // Before any fee output, which consensus requires to be last.
        auto at = mtx.vout.end();
        while (at != mtx.vout.begin() && std::prev(at)->IsFee()) --at;
        mtx.vout.insert(at, CTxOut(asset, 0, script));
        return EncodeHexTx(CTransaction(mtx));
    }};
}

static RPCHelpMan setsupervisionunfreezesig()
{
    return RPCHelpMan{"setsupervisionunfreezesig",
        "\nPut an unfreeze signature into the input that spends a freeze record.\n"
        "\nThe signature goes in the scriptSig as a single push, and the record's own script ignores it: the\n"
        "script is deliberately not the spend authority, because a script is fixed when its output is created\n"
        "and could therefore only ever name the key that was current then. Consensus checks the signature\n"
        "against the asset's key as it stands NOW, so a rotation retires the old key for unfreezing too.\n"
        "\nNo wallet will produce this, which is why it is here.\n",
        {
            {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The raw transaction."},
            {"vin", RPCArg::Type::NUM, RPCArg::Optional::NO, "Index of the input spending the freeze record."},
            {"signature", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The 64-byte BIP340 signature over getsupervisionunfreezehash."},
        },
        RPCResult{RPCResult::Type::STR_HEX, "hex", "The transaction with that input's scriptSig set."},
        RPCExamples{HelpExampleCli("setsupervisionunfreezesig", "\"<rawtx>\" 0 \"<signature>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
    {
        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, request.params[0].get_str())) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        }
        const int index = request.params[1].get_int();
        if (index < 0 || (size_t)index >= mtx.vin.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "vin out of range");
        }
        const std::string sig = request.params[2].get_str();
        if (!IsHex(sig) || sig.size() != 128) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "signature must be a 64-byte BIP340 signature in hex");
        }
        mtx.vin[index].scriptSig = CScript() << ParseHex(sig);
        return EncodeHexTx(CTransaction(mtx));
    }};
}

void RegisterSupervisionRPCCommands(CRPCTable& t)
{
// clang-format off
static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "supervision",        &getsupervisedassetid,          },
    { "supervision",        &getsupervisionrecordhash,      },
    { "supervision",        &buildsupervisionrecord,        },
    { "supervision",        &getsupervisionunfreezehash,    },
    { "supervision",        &getsupervisedassets,           },
    { "supervision",        &getassetfreezes,               },
    { "supervision",        &isassetfrozen,                 },
    { "supervision",        &decodesupervisionscript,       },
    { "supervision",        &addsupervisionrecordoutput,    },
    { "supervision",        &setsupervisionunfreezesig,     },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
