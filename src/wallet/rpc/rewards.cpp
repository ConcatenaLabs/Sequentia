// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetsdir.h>
#include <core_io.h>
#include <pos.h>
#include <rpc/util.h>
#include <seqdexclient.h>
#include <wsclient.h>
#include <util/moneystr.h>
#include <wallet/rewardconvert.h>
#include <wallet/rewardexec.h>
#include <wallet/rpc/util.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {

namespace {

//! Native parent-chain BTC has no asset id, so the RPCs spell it "bitcoin" --
//! the same word the fee-asset arguments already use for the policy asset's
//! label, and the one a staker would type.
std::string TargetToString(const std::optional<CAsset>& t)
{
    return t ? t->GetHex() : "bitcoin";
}

UniValue AssetWithLabel(const CAsset& a)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("asset", a.GetHex());
    const std::string label = gAssetsDir.GetLabel(a);
    if (!label.empty()) o.pushKV("assetlabel", label);
    return o;
}

UniValue ReportToUniValue(const RewardPassReport& rep, const RewardConvertSettings& settings)
{
    UniValue rows(UniValue::VARR);
    for (const RewardPassRow& r : rep.considered) {
        UniValue o = AssetWithLabel(r.batch.asset);
        o.pushKV("amount", ValueFromAmount(r.batch.value));
        o.pushKV("outputs", (int64_t)r.batch.inputs.size());
        o.pushKV("converts", r.decision.Converts());
        o.pushKV("reason", r.decision.Reason());
        if (r.quote) {
            o.pushKV("would_receive", ValueFromAmount(r.quote->receives));
            o.pushKV("slippage_bp", (int64_t)r.quote->SlippageBp());
        }
        if (r.executed) o.pushKV("txid", r.txid);
        if (!r.error.empty()) o.pushKV("error", r.error);
        rows.push_back(o);
    }

    UniValue errs(UniValue::VARR);
    for (const std::string& e : rep.errors) errs.push_back(e);

    UniValue out(UniValue::VOBJ);
    out.pushKV("enabled", settings.enabled);
    out.pushKV("target", TargetToString(settings.target));
    out.pushKV("ran", rep.ran);
    out.pushKV("considered", rows);
    out.pushKV("errors", errs);
    return out;
}

} // namespace

RPCHelpMan getrewardautoconvert()
{
    return RPCHelpMan{"getrewardautoconvert",
                "\nSEQUENTIA staking: the standing instruction for converting this wallet's staking rewards,\n"
                "and what it would do right now.\n"
                "\nA staker earns the transaction fees of the blocks it produces, in whichever assets the payers\n"
                "chose -- which for most stakers is a long tail of small balances in assets they never chose to\n"
                "hold. Auto-conversion sells that tail for ONE asset you pick. Native Bitcoin is the default and\n"
                "the first thing offered, but not the only choice: outside staking no asset is privileged.\n"
                "\n`considered` is a DRY RUN over the rewards that have matured: what would be converted, and\n"
                "for anything that would not, why. \"No market for this pair\" and \"not yet worth converting\"\n"
                "are the ordinary answers on a quiet chain, and both mean the rewards simply wait.\n",
                {},
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::BOOL, "enabled", "whether rewards are being converted"},
                    {RPCResult::Type::STR, "target", "the asset they are converted into (\"bitcoin\" = native parent-chain BTC)"},
                    {RPCResult::Type::ARR, "exclude", "assets kept as they are, on top of the target itself", {
                        {RPCResult::Type::STR_HEX, "", "asset id"}}},
                    {RPCResult::Type::STR_AMOUNT, "min_receive", "the floor a batch must clear, in the target asset"},
                    {RPCResult::Type::NUM, "max_slippage_bp", "how far below the reference price a fill may land, in basis points"},
                    {RPCResult::Type::STR, "relay", "the configured SeqDEX relay, or empty when none is set"},
                    {RPCResult::Type::BOOL, "ran", "whether the dry run could be made"},
                    {RPCResult::Type::ARR, "considered", "each asset with matured, unconverted rewards", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "asset", "the asset"},
                            {RPCResult::Type::STR, "assetlabel", /*optional=*/true, "its label, when it has one"},
                            {RPCResult::Type::STR_AMOUNT, "amount", "how much has gathered"},
                            {RPCResult::Type::NUM, "outputs", "how many reward outputs it gathers"},
                            {RPCResult::Type::BOOL, "converts", "whether it would be converted now"},
                            {RPCResult::Type::STR, "reason", "why, in words"},
                            {RPCResult::Type::STR_AMOUNT, "would_receive", /*optional=*/true, "what the book offers for it"},
                            {RPCResult::Type::NUM, "slippage_bp", /*optional=*/true, "how far that is below the reference price"},
                        }},
                    }},
                    {RPCResult::Type::ARR, "errors", "anything that went wrong reading the market", {
                        {RPCResult::Type::STR, "", "a message"}}},
                    {RPCResult::Type::ARR, "recent", "the last conversions this wallet made", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR, "state", "\"pending\", \"done\" or \"failed\""},
                            {RPCResult::Type::NUM_TIME, "time", "when it started"},
                            {RPCResult::Type::STR_HEX, "asset", "what was sold"},
                            {RPCResult::Type::STR_AMOUNT, "amount", "how much"},
                            {RPCResult::Type::STR, "target", "what it was sold for"},
                            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "the settling transaction"},
                            {RPCResult::Type::STR, "error", /*optional=*/true, "why it did not complete"},
                        }},
                    }},
                }},
                RPCExamples{HelpExampleCli("getrewardautoconvert", "") + HelpExampleRpc("getrewardautoconvert", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    const RewardConvertSettings settings = LoadRewardConvertSettings(*pwallet);
    const RewardPassReport rep = RunRewardConversionPass(*pwallet, /*dry_run=*/true);

    UniValue out = ReportToUniValue(rep, settings);
    UniValue ex(UniValue::VARR);
    for (const CAsset& a : settings.exclude) ex.push_back(a.GetHex());
    out.pushKV("exclude", ex);
    out.pushKV("min_receive", ValueFromAmount(settings.min_receive));
    out.pushKV("max_slippage_bp", (int64_t)settings.max_slippage_bp);
    out.pushKV("relay", SeqdexRelayUrl());

    UniValue recent(UniValue::VARR);
    const auto log = LoadRewardConversions(*pwallet);
    for (auto it = log.rbegin(); it != log.rend() && recent.size() < 20; ++it) {
        UniValue e(UniValue::VOBJ);
        e.pushKV("state", it->state);
        e.pushKV("time", it->time);
        e.pushKV("asset", it->asset.GetHex());
        e.pushKV("amount", ValueFromAmount(it->value));
        e.pushKV("target", TargetToString(it->target));
        if (!it->txid.empty()) e.pushKV("txid", it->txid);
        if (!it->error.empty()) e.pushKV("error", it->error);
        recent.push_back(e);
    }
    out.pushKV("recent", recent);
    return out;
},
    };
}

RPCHelpMan setrewardautoconvert()
{
    return RPCHelpMan{"setrewardautoconvert",
                "\nSEQUENTIA staking: tell this wallet whether, and into what, to convert its staking rewards.\n"
                "\nOff by default, always. Converting rewards is irreversible and you may have chosen those\n"
                "assets deliberately, so nothing is sold because a version changed. Once ON, the wallet sells\n"
                "without asking again -- that is the point of it -- and it stops at the next check when you\n"
                "turn it off.\n"
                "\nWhat it will never do: convert more than staking has paid you, touch your stake, spend a\n"
                "delegation or payout record, or accept a price further from the reference than you allow.\n"
                "Rewards in the target asset are left alone, and so is anything you exclude.\n"
                "\nConversion needs a SeqDEX relay to read the book from: set -seqoburl.\n",
                {
                    {"enabled", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Whether to convert rewards at all."},
                    {"target", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "What to convert into: an asset id or label, or \"bitcoin\" for native parent-chain Bitcoin (the default)."},
                    {"min_receive", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The least a batch must be worth before it is converted, in the target asset. Smaller batches wait for the next reward rather than paying a swap's costs to convert dust."},
                    {"max_slippage_bp", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "How far below the reference price a fill may land, in basis points (200 = 2%)."},
                    {"exclude", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Assets to keep as they are. Pass an empty array to clear.",
                        {{"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "asset id or label"}}},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::BOOL, "enabled", "the setting now in force"},
                    {RPCResult::Type::STR, "target", "what rewards are converted into"},
                    {RPCResult::Type::STR_AMOUNT, "min_receive", "the floor a batch must clear"},
                    {RPCResult::Type::NUM, "max_slippage_bp", "the slippage cap"},
                    {RPCResult::Type::ARR, "exclude", "assets kept as they are", {
                        {RPCResult::Type::STR_HEX, "", "asset id"}}},
                    {RPCResult::Type::STR, "warning", /*optional=*/true, "anything that stops this working as asked"},
                }},
                RPCExamples{HelpExampleCli("setrewardautoconvert", "true")
                          + HelpExampleCli("setrewardautoconvert", "true \"USDX\" 0.5")
                          + HelpExampleRpc("setrewardautoconvert", "true, \"bitcoin\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    RewardConvertSettings s = LoadRewardConvertSettings(*pwallet);

    if (!request.params[0].isNull()) s.enabled = request.params[0].get_bool();
    if (!request.params[1].isNull()) {
        const std::string t = request.params[1].get_str();
        if (t.empty() || t == "bitcoin" || t == "BTC" || t == "btc") {
            s.target = std::nullopt;   // native parent-chain BTC
        } else {
            const CAsset a = GetAssetFromString(t);
            if (a.IsNull()) throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, "Unknown label and invalid asset hex: " + t);
            s.target = a;
        }
    }
    if (!request.params[2].isNull()) {
        const CAmount m = AmountFromValue(request.params[2]);
        if (m < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "min_receive cannot be negative");
        s.min_receive = m;
    }
    if (!request.params[3].isNull()) {
        const int bp = request.params[3].get_int();
        if (bp < 0 || bp > 10000) throw JSONRPCError(RPC_INVALID_PARAMETER, "max_slippage_bp must be between 0 and 10000");
        s.max_slippage_bp = bp;
    }
    if (!request.params[4].isNull()) {
        s.exclude.clear();
        const UniValue& arr = request.params[4].get_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            const CAsset a = GetAssetFromString(arr[i].get_str());
            if (a.IsNull()) throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, "Unknown label and invalid asset hex: " + arr[i].get_str());
            s.exclude.insert(a);
        }
    }

    StoreRewardConvertSettings(*pwallet, s);

    UniValue out(UniValue::VOBJ);
    out.pushKV("enabled", s.enabled);
    out.pushKV("target", TargetToString(s.target));
    out.pushKV("min_receive", ValueFromAmount(s.min_receive));
    out.pushKV("max_slippage_bp", (int64_t)s.max_slippage_bp);
    UniValue ex(UniValue::VARR);
    for (const CAsset& a : s.exclude) ex.push_back(a.GetHex());
    out.pushKV("exclude", ex);
    if (s.enabled && SeqdexRelayUrl().empty()) {
        out.pushKV("warning", "no SeqDEX relay is configured, so nothing can be converted: set -seqoburl=http://host:port");
    }
    return out;
},
    };
}

RPCHelpMan convertrewards()
{
    return RPCHelpMan{"convertrewards",
                "\nSEQUENTIA staking: run one reward-conversion pass now, instead of waiting for the next one.\n"
                "\nWith `dry_run` (the default) nothing is spent: the answer is what the wallet WOULD do, which\n"
                "is the same thing getrewardautoconvert reports. Pass false to actually convert.\n"
                "\nEvery \"not now\" is a WAIT, never an error. No market for the pair, a batch not yet worth\n"
                "converting, a price too far from the reference -- in each case the rewards stay exactly where\n"
                "they are and the batch is reconsidered when the next reward in that asset lands.\n",
                {
                    {"dry_run", RPCArg::Type::BOOL, RPCArg::Default{true}, "Work out what would happen without doing it."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::BOOL, "enabled", "whether conversion is switched on"},
                    {RPCResult::Type::STR, "target", "what rewards are converted into"},
                    {RPCResult::Type::BOOL, "ran", "whether the pass was made"},
                    {RPCResult::Type::ARR, "considered", "each asset with matured, unconverted rewards, and what became of it", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "asset", "the asset"},
                            {RPCResult::Type::STR_AMOUNT, "amount", "how much has gathered"},
                            {RPCResult::Type::BOOL, "converts", "whether it converted (or would)"},
                            {RPCResult::Type::STR, "reason", "why, in words"},
                            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "the settling transaction, when it converted"},
                        }},
                    }},
                    {RPCResult::Type::ARR, "errors", "anything that went wrong", {
                        {RPCResult::Type::STR, "", "a message"}}},
                }},
                RPCExamples{HelpExampleCli("convertrewards", "") + HelpExampleCli("convertrewards", "false")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_con_pos) throw JSONRPCError(RPC_MISC_ERROR, "Proof-of-Stake (con_pos) is not enabled on this chain");
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;
    pwallet->BlockUntilSyncedToCurrentChain();

    const bool dry_run = request.params[0].isNull() ? true : request.params[0].get_bool();
    const RewardConvertSettings settings = LoadRewardConvertSettings(*pwallet);
    if (!dry_run && !settings.enabled) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            "reward conversion is switched off for this wallet; turn it on with setrewardautoconvert true");
    }
    const RewardPassReport rep = RunRewardConversionPass(*pwallet, dry_run);
    return ReportToUniValue(rep, settings);
},
    };
}

RPCHelpMan getseqdexstatus()
{
    return RPCHelpMan{"getseqdexstatus",
                "\nSEQUENTIA: whether this node can reach the SeqDEX relay it converts staking rewards through,\n"
                "and what it finds there.\n"
                "\nConversion has two halves and they fail differently. Reading the book is a plain HTTP request;\n"
                "agreeing a cross-chain swap with a maker is a WebSocket conversation. A relay that answers one\n"
                "and not the other converts rewards into Sequentia assets but never into Bitcoin, which is worth\n"
                "being able to see rather than infer from nothing happening.\n",
                {},
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR, "relay", "the configured -seqoburl, or empty when none is set"},
                    {RPCResult::Type::BOOL, "book_readable", "whether the order book could be read"},
                    {RPCResult::Type::NUM, "markets", /*optional=*/true, "how many markets it is serving"},
                    {RPCResult::Type::BOOL, "courier_reachable", "whether the WebSocket a cross-chain swap needs could be opened"},
                    {RPCResult::Type::STR, "error", /*optional=*/true, "what went wrong, when something did"},
                }},
                RPCExamples{HelpExampleCli("getseqdexstatus", "") + HelpExampleRpc("getseqdexstatus", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue out(UniValue::VOBJ);
    out.pushKV("relay", SeqdexRelayUrl());

    std::string first_error;
    bool book_ok = false;
    int markets = 0;
    try {
        const UniValue j = SeqdexHttpJson("/v1/markets", "GET");
        if (j.exists("markets") && j["markets"].isArray()) {
            markets = (int)j["markets"].size();
            book_ok = true;
        } else {
            first_error = "the relay answered, but not with a market list";
        }
    } catch (const std::exception& e) {
        first_error = e.what();
    }
    out.pushKV("book_readable", book_ok);
    if (book_ok) out.pushKV("markets", (int64_t)markets);

    bool ws_ok = false;
    std::string host, prefix;
    uint16_t port = 0;
    if (SeqdexRelayEndpoint(host, port, prefix)) {
        std::string ws_error;
        auto ws = WsClient::Connect(host, port, prefix + "/v1/ws", std::chrono::seconds(15), ws_error);
        if (ws) {
            ws_ok = true;
            ws->Close();
        } else if (first_error.empty()) {
            first_error = ws_error;
        }
    } else if (first_error.empty()) {
        first_error = "no relay configured: set -seqoburl=http://host:port";
    }
    out.pushKV("courier_reachable", ws_ok);
    if (!first_error.empty()) out.pushKV("error", first_error);
    return out;
},
    };
}

} // namespace wallet
