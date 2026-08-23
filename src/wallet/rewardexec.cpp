// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/rewardexec.h>

#include <assetsdir.h>
#include <fs.h>
#include <logging.h>
#include <random.h>
#include <scheduler.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/time.h>
#include <wallet/stakingrewards.h>
#include <wallet/wallet.h>

#include <fstream>

namespace wallet {

namespace {

fs::path SettingsPath(const CWallet& wallet)
{
    return fs::PathFromString(wallet.GetDatabase().Filename()).parent_path() / "rewardconvert.json";
}

fs::path LogPath(const CWallet& wallet)
{
    return fs::PathFromString(wallet.GetDatabase().Filename()).parent_path() / "rewardconversions.json";
}

UniValue ReadJsonFile(const fs::path& path)
{
    std::ifstream f(fs::PathToString(path));
    if (!f.good()) return UniValue();
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    UniValue parsed;
    if (!parsed.read(data)) return UniValue();
    return parsed;
}

void WriteJsonFile(const fs::path& path, const UniValue& v)
{
    std::ofstream f(fs::PathToString(path), std::ios::trunc);
    f << v.write(1) << std::endl;
}

std::string OutPointToString(const COutPoint& o)
{
    return o.hash.GetHex() + ":" + ToString(o.n);
}

std::optional<COutPoint> OutPointFromString(const std::string& s)
{
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon != 64) return std::nullopt;
    const std::string txid = s.substr(0, colon);
    if (!IsHex(txid)) return std::nullopt;
    const std::string n = s.substr(colon + 1);
    if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return COutPoint(uint256S(txid), (uint32_t)atoi(n.c_str()));
}

} // namespace

RewardConvertSettings LoadRewardConvertSettings(const CWallet& wallet)
{
    RewardConvertSettings s;
    const UniValue v = ReadJsonFile(SettingsPath(wallet));
    if (!v.isObject()) return s;

    if (v.exists("enabled") && v["enabled"].isBool()) s.enabled = v["enabled"].get_bool();
    if (v.exists("target") && v["target"].isStr()) {
        const std::string t = v["target"].get_str();
        // "" / "bitcoin" mean native parent-chain BTC, which has no asset id.
        if (!t.empty() && t != "bitcoin") {
            const CAsset a = GetAssetFromString(t);
            if (!a.IsNull()) s.target = a;
        }
    }
    if (v.exists("exclude") && v["exclude"].isArray()) {
        for (size_t i = 0; i < v["exclude"].size(); ++i) {
            if (!v["exclude"][i].isStr()) continue;
            const CAsset a = GetAssetFromString(v["exclude"][i].get_str());
            if (!a.IsNull()) s.exclude.insert(a);
        }
    }
    if (v.exists("min_receive") && v["min_receive"].isNum()) s.min_receive = v["min_receive"].get_int64();
    if (v.exists("max_slippage_bp") && v["max_slippage_bp"].isNum()) s.max_slippage_bp = (int)v["max_slippage_bp"].get_int64();
    return s;
}

void StoreRewardConvertSettings(const CWallet& wallet, const RewardConvertSettings& s)
{
    UniValue v(UniValue::VOBJ);
    v.pushKV("enabled", s.enabled);
    v.pushKV("target", s.target ? s.target->GetHex() : "bitcoin");
    UniValue ex(UniValue::VARR);
    for (const CAsset& a : s.exclude) ex.push_back(a.GetHex());
    v.pushKV("exclude", ex);
    v.pushKV("min_receive", s.min_receive);
    v.pushKV("max_slippage_bp", (int64_t)s.max_slippage_bp);
    WriteJsonFile(SettingsPath(wallet), v);
}

std::vector<RewardConversion> LoadRewardConversions(const CWallet& wallet)
{
    std::vector<RewardConversion> out;
    const UniValue v = ReadJsonFile(LogPath(wallet));
    if (!v.isArray()) return out;
    for (size_t i = 0; i < v.size(); ++i) {
        const UniValue& e = v[i];
        if (!e.isObject()) continue;
        RewardConversion c;
        if (e.exists("id") && e["id"].isStr()) c.id = e["id"].get_str();
        if (e.exists("state") && e["state"].isStr()) c.state = e["state"].get_str();
        if (e.exists("time") && e["time"].isNum()) c.time = e["time"].get_int64();
        if (e.exists("asset") && e["asset"].isStr()) c.asset = GetAssetFromString(e["asset"].get_str());
        if (e.exists("value") && e["value"].isNum()) c.value = e["value"].get_int64();
        if (e.exists("target") && e["target"].isStr() && e["target"].get_str() != "bitcoin") {
            const CAsset a = GetAssetFromString(e["target"].get_str());
            if (!a.IsNull()) c.target = a;
        }
        if (e.exists("expected") && e["expected"].isNum()) c.expected = e["expected"].get_int64();
        if (e.exists("received") && e["received"].isNum()) c.received = e["received"].get_int64();
        if (e.exists("txid") && e["txid"].isStr()) c.txid = e["txid"].get_str();
        if (e.exists("error") && e["error"].isStr()) c.error = e["error"].get_str();
        if (e.exists("inputs") && e["inputs"].isArray()) {
            for (size_t k = 0; k < e["inputs"].size(); ++k) {
                if (!e["inputs"][k].isStr()) continue;
                if (auto o = OutPointFromString(e["inputs"][k].get_str())) c.inputs.push_back(*o);
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

void StoreRewardConversions(const CWallet& wallet, const std::vector<RewardConversion>& log)
{
    UniValue arr(UniValue::VARR);
    // A wallet does not need an unbounded history of its own housekeeping.
    const size_t kKeep = 200;
    const size_t start = log.size() > kKeep ? log.size() - kKeep : 0;
    for (size_t i = start; i < log.size(); ++i) {
        const RewardConversion& c = log[i];
        UniValue e(UniValue::VOBJ);
        e.pushKV("id", c.id);
        e.pushKV("state", c.state);
        e.pushKV("time", c.time);
        e.pushKV("asset", c.asset.GetHex());
        e.pushKV("value", c.value);
        e.pushKV("target", c.target ? c.target->GetHex() : "bitcoin");
        e.pushKV("expected", c.expected);
        e.pushKV("received", c.received);
        if (!c.txid.empty()) e.pushKV("txid", c.txid);
        if (!c.error.empty()) e.pushKV("error", c.error);
        UniValue ins(UniValue::VARR);
        for (const COutPoint& o : c.inputs) ins.push_back(OutPointToString(o));
        e.pushKV("inputs", ins);
        arr.push_back(e);
    }
    WriteJsonFile(LogPath(wallet), arr);
}

std::set<COutPoint> RewardConvertedOutpoints(const CWallet& wallet)
{
    std::set<COutPoint> out;
    for (const RewardConversion& c : LoadRewardConversions(wallet)) {
        // pending AND done both hold their coins. Only a definite refusal
        // releases them; see the note on RewardConversion::state.
        if (c.state != "pending" && c.state != "done") continue;
        for (const COutPoint& o : c.inputs) out.insert(o);
    }
    return out;
}

RewardPassReport RunRewardConversionPass(CWallet& wallet, bool dry_run)
{
    RewardPassReport report;
    const RewardConvertSettings settings = LoadRewardConvertSettings(wallet);
    if (!settings.enabled && !dry_run) return report;

    std::vector<RewardCoin> coins;
    {
        LOCK(wallet.cs_wallet);
        for (const StakingReward& r : FindWalletStakingRewards(wallet, /*include_spent=*/false)) {
            RewardCoin c;
            c.outpoint = r.outpoint;
            c.asset = r.asset;
            c.amount = r.amount;
            c.mature = r.Mature();
            c.spent = r.spent;
            coins.push_back(std::move(c));
        }
    }

    const std::set<COutPoint> already = RewardConvertedOutpoints(wallet);
    const std::vector<RewardBatch> batches = RewardBatches(coins, settings, already);
    report.ran = true;

    for (const RewardBatch& batch : batches) {
        RewardPassRow row;
        row.batch = batch;

        // What the book offers. A book we could not READ is not a book that
        // said no: treat it as "no market for now" rather than an error the
        // staker has to act on.
        try {
            const CAsset want = settings.target ? *settings.target : CAsset();
            if (settings.TargetIsNativeBtc()) {
                // Native BTC is the parent chain's own coin: the sale is a
                // cross-chain HTLC, not a book walk. Priced and executed by the
                // cross-chain path below.
                row.quote = std::nullopt;
            } else {
                const auto book = SeqobFetchBook(batch.asset, want);
                const auto walk = SeqobWalkBook(book, batch.asset, want, batch.value);
                if (walk) row.quote = RewardQuote{walk->receives, walk->reference};
            }
        } catch (const std::exception& e) {
            row.quote = std::nullopt;
            report.errors.push_back(std::string("book unreadable: ") + e.what());
        }

        row.decision = DecideRewardConversion(batch, row.quote, settings);
        report.considered.push_back(row);
    }

    return report;
}

void StartRewardConversionScheduler()
{
    // Placeholder until execution lands: wiring a timer that can only decide
    // and never act would be a timer that does nothing.
}

} // namespace wallet
