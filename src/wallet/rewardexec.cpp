// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/rewardexec.h>

#include <assetsdir.h>
#include <core_io.h>
#include <fs.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <policy/policy.h>
#include <script/standard.h>
#include <logging.h>
#include <random.h>
#include <scheduler.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/time.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/covenantfill.h>
#include <wallet/xchainconvert.h>
#include <wallet/spend.h>
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


namespace {

//! The maker's credit output: a v1 Taproot output paying `maker_prog`. The leaf
//! compares these bytes exactly, so this is not a choice of address type.
CScript MakerCreditSpk(const std::vector<unsigned char>& maker_prog)
{
    return CScript() << OP_1 << maker_prog;
}

struct FillOutcome {
    bool ok{false};
    std::string txid;
    CAmount received{0};
    std::string error;
};

/** Take `filled` atoms of asset A out of one resting covenant offer.
 *
 *  One transaction, broadcast by this node, with the maker offline. The
 *  covenant sits at input 0 -- which is what makes the credit output index 0
 *  and the remainder index 1, since the leaf derives both from its own input
 *  index -- and the wallet funds the maker's credit and the network fee.
 */
FillOutcome ExecuteCovenantFill(CWallet& wallet, const SeqobOffer& offer, CAmount filled)
{
    FillOutcome out;
    if (!offer.covenant) { out.error = "offer is not covenant-backed"; return out; }
    const SeqobCovenant& cov = *offer.covenant;

    const auto scripts = BuildSeqobFillScripts(cov);
    if (!scripts) { out.error = "covenant terms are not self-consistent"; return out; }

    // The coin the offer claims to rest on, as the CHAIN has it. This is the
    // whole trust model: the relay is not believed about the terms, the value,
    // or even that the offer is still there. A node has the UTXO set, so it can
    // simply look.
    const COutPoint cov_out(cov.txid, cov.vout);
    std::map<COutPoint, Coin> coins{{cov_out, Coin()}};
    wallet.chain().findCoins(coins);
    const Coin& coin = coins[cov_out];
    if (coin.IsSpent()) { out.error = "the offer's covenant output is already spent"; return out; }
    if (coin.out.scriptPubKey != scripts->spk) {
        out.error = "the offer's terms do not rebuild the covenant it claims to rest on";
        return out;
    }
    if (!coin.out.nValue.IsExplicit() || !coin.out.nAsset.IsExplicit()) {
        out.error = "the covenant output is blinded, which no covenant this node fills ever is";
        return out;
    }
    const CAmount locked = coin.out.nValue.GetAmount();
    if (coin.out.nAsset.GetAsset() != cov.asset_a) { out.error = "covenant asset mismatch"; return out; }

    const auto plan = PlanSeqobFill(cov, locked, filled);
    if (!plan) { out.error = "that size is not a fill this covenant accepts"; return out; }

    // Outputs, in the order the leaf demands: credit at 2k, remainder at 2k+1.
    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.vin.emplace_back(cov_out);
    mtx.vout.emplace_back(cov.asset_b, plan->credit, MakerCreditSpk(cov.maker_prog));
    if (plan->partial) {
        // Slot 1 (2k+1) is the self-replicating remainder covenant.
        mtx.vout.emplace_back(cov.asset_a, plan->remainder, scripts->spk);
    }
    // A FULL fill has no remainder, but the leaf still READS slot 1 whenever one
    // exists: it only takes the "remainder = 0" branch when what it finds there
    // is not asset A. So a full fill needs some other-asset output sitting in
    // that slot, which is arranged after funding, once there is one to move.

    // What we came for: the filled asset A, to this wallet.
    CTxDestination dest;
    {
        LOCK(wallet.cs_wallet);
        bilingual_str err;
        if (!wallet.GetNewDestination(OutputType::BECH32, "reward conversion", dest, err)) {
            out.error = "could not get a receiving address: " + err.original;
            return out;
        }
    }
    mtx.vout.emplace_back(cov.asset_a, plan->filled, GetScriptForDestination(dest));

    // Fund the maker's credit and the fee. The covenant input is EXTERNAL: the
    // wallet does not own it, so it has to be told what it is worth or it will
    // try to fund asset A as well.
    CCoinControl cc;
    cc.m_add_inputs = true;
    cc.fAllowOtherInputs = true;
    cc.Select(cov_out);
    cc.SelectExternal(cov_out, coin.out);

    CAmount fee = 0;
    int change_pos = (int)mtx.vout.size();   // never 0 or 1: the leaf reads those
    bilingual_str error;
    if (!FundTransaction(wallet, mtx, fee, change_pos, error, /*lockUnspents=*/false,
                         /*setSubtractFeeFromOutputs=*/{}, cc)) {
        out.error = "could not fund the fill: " + error.original;
        return out;
    }

    // Put the outputs back in the order the leaf reads them. Funding appends,
    // so slot 0 and slot 1 should be untouched -- but "should be" is not a
    // thing to broadcast money on, so check rather than assume.
    if (mtx.vout.size() < 2 || mtx.vout[0].scriptPubKey != MakerCreditSpk(cov.maker_prog)) {
        out.error = "funding disturbed the maker credit output";
        return out;
    }
    if (plan->partial) {
        if (mtx.vout[1].scriptPubKey != scripts->spk) {
            out.error = "funding disturbed the remainder output";
            return out;
        }
    } else {
        // Move a non-asset-A output into slot 1. Change in the asset we paid
        // with is the usual candidate; the fee output serves when there is no
        // change at all. Signing happens AFTER this, so reordering is free.
        size_t gap = 0;
        for (size_t i = 1; i < mtx.vout.size(); ++i) {
            if (mtx.vout[i].nAsset.IsExplicit() && mtx.vout[i].nAsset.GetAsset() == cov.asset_a) continue;
            gap = i;
            break;
        }
        if (gap == 0) {
            out.error = "a full fill needs one output that is not the asset being bought to sit "
                        "at the remainder slot, and this transaction has none";
            return out;
        }
        if (gap != 1) std::swap(mtx.vout[1], mtx.vout[gap]);
    }

    // The covenant input's witness is the leaf and its control block, and
    // nothing else: the leaf has no CHECKSIG, so there is nothing to sign.
    for (size_t i = 0; i < mtx.vin.size(); ++i) {
        if (mtx.vin[i].prevout != cov_out) continue;
        if (i != 0) { out.error = "funding moved the covenant off input 0"; return out; }
        CScriptWitness wit;
        wit.stack.emplace_back(scripts->fill_leaf.begin(), scripts->fill_leaf.end());
        wit.stack.push_back(scripts->control_block);
        mtx.witness.vtxinwit.resize(mtx.vin.size());
        mtx.witness.vtxinwit[i].scriptWitness = wit;
    }

    // Sign OUR inputs. The covenant input is signed by nobody, so it is handed
    // in as a known coin and its "failure" to sign is the expected outcome.
    {
        LOCK(wallet.cs_wallet);
        std::map<COutPoint, Coin> sign_coins;
        sign_coins[cov_out] = coin;
        std::map<int, bilingual_str> input_errors;
        wallet.SignTransaction(mtx, sign_coins, SIGHASH_ALL, input_errors);
        for (const auto& e : input_errors) {
            if (mtx.vin[e.first].prevout == cov_out) continue;   // expected
            out.error = "could not sign the fill: " + e.second.original;
            return out;
        }
    }

    CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    std::string rejected;
    if (!wallet.chain().broadcastTransaction(tx, wallet.m_default_max_tx_fee, /*relay=*/true, rejected)) {
        out.error = "broadcast refused: " + rejected;
        return out;
    }

    out.ok = true;
    out.txid = tx->GetHash().GetHex();
    out.received = plan->filled;
    return out;
}

} // namespace

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
        std::optional<SeqobWalk> walk;
        std::optional<SeqobOffer> btc_offer;
        CAmount btc_slice = 0;
        try {
            if (settings.TargetIsNativeBtc()) {
                // Native Bitcoin is the parent chain's own coin, so this is a
                // cross-chain swap against a maker rather than a walk down a
                // book of covenants. The rail rests WHOLE offers, so the price
                // comes from the best one that can take our size, and the slice
                // is clamped to it -- never the other way round, which would
                // sell coins staking never paid.
                const auto bids = SeqobFetchBtcBids(batch.asset);
                for (const SeqobOffer& o : bids) {
                    const CAmount take = RewardSliceForWholeHtlc(o.want_amount, batch.value);
                    if (take <= 0) continue;
                    const CAmount pays = (CAmount)(((__int128)o.offer_amount * take) / o.want_amount);
                    if (pays <= 0) continue;
                    btc_offer = o;
                    btc_slice = take;
                    // The reference is what the whole batch would fetch at this
                    // (best) price; `receives` is what this offer can actually
                    // take, so a partial shows up as slippage rather than as a
                    // silently smaller sale.
                    const CAmount reference = (CAmount)(((__int128)o.offer_amount * batch.value) / o.want_amount);
                    row.quote = RewardQuote{pays, reference};
                    break;
                }
            } else {
                const CAsset want = *settings.target;
                const auto book = SeqobFetchBook(batch.asset, want);
                walk = SeqobWalkBook(book, batch.asset, want, batch.value);
                if (walk) row.quote = RewardQuote{walk->receives, walk->reference};
            }
        } catch (const std::exception& e) {
            row.quote = std::nullopt;
            report.errors.push_back(std::string("book unreadable: ") + e.what());
        }

        row.decision = DecideRewardConversion(batch, row.quote, settings);

        if (!dry_run && row.decision.Converts() && (walk || btc_offer)) {
            // Commit to the coins BEFORE spending them. If the node dies between
            // here and the broadcast returning, they stay claimed by a `pending`
            // record rather than being offered to a second conversion -- the sale
            // may well have happened, and there is no way to know.
            RewardConversion rec;
            rec.id = batch.asset.GetHex() + ":" + ToString(batch.value) + ":" + ToString(GetTime());
            rec.state = "pending";
            rec.time = GetTime();
            rec.asset = batch.asset;
            rec.value = batch.value;
            rec.target = settings.target;
            rec.expected = row.quote->receives;
            rec.inputs = batch.inputs;
            auto log = LoadRewardConversions(wallet);
            log.push_back(rec);
            StoreRewardConversions(wallet, log);

            CAmount got = 0;
            bool committed = false;
            std::string last_error;
            if (btc_offer) {
                // Cross-chain. Long-running by nature -- it waits on Bitcoin
                // confirmations -- and it reports separately whether our asset
                // was committed, because a swap that got that far must never
                // have its coins offered to a second attempt.
                const XchainOutcome r = RunXchainConversion(wallet, *btc_offer, btc_slice);
                committed = r.committed;
                if (r.ok) {
                    got += r.received;
                    row.executed = true;
                } else {
                    last_error = r.error;
                }
            } else {
                for (const SeqobWalkLeg& leg : walk->legs) {
                    const FillOutcome r = ExecuteCovenantFill(wallet, leg.offer, leg.receive);
                    if (r.ok) {
                        got += r.received;
                        row.txid = r.txid;
                        row.executed = true;
                    } else {
                        // One offer failing is not the sale failing: a covenant
                        // someone else just took, or an offer that moved, is
                        // ordinary. Walk on; nothing was committed for a fill
                        // that did not broadcast.
                        last_error = r.error;
                    }
                }
            }

            log = LoadRewardConversions(wallet);
            for (auto it = log.rbegin(); it != log.rend(); ++it) {
                if (it->id != rec.id) continue;
                if (got > 0) {
                    it->state = "done";
                    it->received = got;
                    it->txid = row.txid;
                } else if (committed) {
                    // The asset WAS committed even though the swap did not
                    // finish: it is either about to settle or refundable by
                    // timelock, and either way those coins are spoken for.
                    // Releasing them here is how a wallet sells twice.
                    it->error = last_error.empty() ? "cross-chain swap in flight" : last_error;
                    row.error = it->error;
                } else {
                    // Nothing was sold, definitively: release the coins so the
                    // next pass can reconsider them.
                    it->state = "failed";
                    it->error = last_error.empty() ? "no fill completed" : last_error;
                    row.error = it->error;
                    report.errors.push_back(it->error);
                }
                break;
            }
            StoreRewardConversions(wallet, log);
            LogPrintf("[rewards] %s conversion of %d %s: %s\n",
                      got > 0 ? "completed" : "abandoned", batch.value,
                      batch.asset.GetHex(), got > 0 ? row.txid : row.error);
        }

        report.considered.push_back(row);
    }

    return report;
}

void StartRewardConversionScheduler(WalletContext& context, CScheduler& scheduler)
{
    // Rewards arrive at block pace at best, and every pass reads the book, so
    // looking more often would only cost the relay requests. The resume pass is
    // separate and quicker: it is what finishes a cross-chain swap whose maker
    // has just revealed the secret, or refunds one whose maker never did.
    scheduler.scheduleEvery([&context]{
        for (const std::shared_ptr<CWallet>& wallet : GetWallets(context)) {
            if (!wallet) continue;
            try {
                ResumeXchainSwaps(*wallet);
            } catch (const std::exception& e) {
                LogPrintf("[rewards] resume failed for %s: %s\n", wallet->GetName(), e.what());
            }
        }
    }, std::chrono::minutes{2});

    scheduler.scheduleEvery([&context]{
        for (const std::shared_ptr<CWallet>& wallet : GetWallets(context)) {
            if (!wallet) continue;
            try {
                if (!LoadRewardConvertSettings(*wallet).enabled) continue;
                const RewardPassReport rep = RunRewardConversionPass(*wallet, /*dry_run=*/false);
                for (const std::string& e : rep.errors) {
                    LogPrintf("[rewards] %s: %s\n", wallet->GetName(), e);
                }
            } catch (const std::exception& e) {
                LogPrintf("[rewards] conversion pass failed for %s: %s\n", wallet->GetName(), e.what());
            }
        }
    }, std::chrono::minutes{10});
}

} // namespace wallet
