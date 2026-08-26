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
#include <wallet/fees.h>
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
    const std::optional<uint32_t> vout = ToIntegral<uint32_t>(n);
    if (!vout) return std::nullopt;
    return COutPoint(uint256S(txid), *vout);
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

    // The transaction is assembled by hand rather than handed to the funder.
    //
    // The funder cannot value this input. The covenant's funding transaction is
    // in the wallet (the wallet made it), but the covenant OUTPUT is not the
    // wallet's, so the value it reads back is not the 500 asset A sitting there
    // -- and everything downstream follows from that number. Told to cover an
    // output of asset A it selects the wallet's own on top, and the transaction
    // has the asset going in twice and coming out once; told to cover nothing
    // it creates no change for the surplus, and the asset goes in and never
    // comes out. Both are rejected as unbalanced, and neither is fixable from
    // the outside.
    //
    // So: choose the coins, place the outputs, and compute the fee here, where
    // the covenant's value is known exactly.
    const CAsset& asset_a = cov.asset_a;
    const CAsset& asset_b = cov.asset_b;

    CTxDestination dest;
    {
        LOCK(wallet.cs_wallet);
        bilingual_str err;
        if (!wallet.GetNewDestination(OutputType::BECH32, "reward conversion", dest, err)) {
            out.error = "could not get a receiving address: " + err.original;
            return out;
        }
    }
    const CScript pay_to = GetScriptForDestination(dest);

    CMutableTransaction mtx;
    mtx.nVersion = 2;

    // Coins, per asset. Two lists, because the fee usually cannot come out of
    // the asset being sold: a staker converting GOLD rewards may hold no GOLD
    // but the rewards themselves, and taking the fee from those would mean the
    // batch could never be converted whole. That is not a hypothetical -- it is
    // what the live testnet's treasury looked like the first time this ran
    // against it. So the credit is paid in the asset the covenant wants, and
    // the fee in the policy asset, which is what a wallet reliably has.
    const CAsset fee_asset = ::policyAsset;
    const bool fee_in_b = (fee_asset == asset_b);

    auto explicit_coins = [&](const CAsset& want) {
        std::vector<COutput> found;
        {
            LOCK(wallet.cs_wallet);
            AvailableCoins(wallet, found, nullptr, 1, MAX_MONEY, MAX_MONEY, 0, &want);
        }
        std::vector<COutput> keep;
        for (const COutput& c : found) {
            if (!c.fSpendable) continue;
            if (c.tx->GetHash() == cov_out.hash && (uint32_t)c.i == cov_out.n) continue;
            if (!(c.tx->GetOutputAsset(wallet, c.i) == want)) continue;
            // EXPLICIT coins only. A covenant fill is an explicit construction
            // throughout -- the leaf inspects values and assets directly -- so a
            // confidential coin cannot fund one without blinding the
            // transaction, which would then fail the leaf's own checks.
            // Sequentia is transparent-by-default, so this discards nothing on a
            // real chain; a chain left on the Elements default has to be told
            // (-con_default_blinded_addresses=0).
            if (!c.tx->tx->vout[c.i].nValue.IsExplicit()) continue;
            if (!c.tx->tx->vout[c.i].nAsset.IsExplicit()) continue;
            if (c.tx->GetOutputValueOut(wallet, c.i) <= 0) continue;
            keep.push_back(c);
        }
        std::sort(keep.begin(), keep.end(), [&](const COutput& x, const COutput& y) {
            return x.tx->GetOutputValueOut(wallet, x.i) > y.tx->GetOutputValueOut(wallet, y.i);
        });
        return keep;
    };

    const std::vector<COutput> b_coins = explicit_coins(asset_b);
    const std::vector<COutput> f_coins = fee_in_b ? std::vector<COutput>() : explicit_coins(fee_asset);

    // Two passes: pick coins against an estimated fee, then settle the fee
    // against the size that actually resulted. One correction is enough --
    // adding a coin can only grow the transaction by a known input.
    CAmount fee = 2000;
    for (int pass = 0; pass < 2; ++pass) {
        mtx.vin.clear();
        mtx.vout.clear();
        mtx.witness.vtxinwit.clear();
        mtx.vin.emplace_back(cov_out);

        int inputs_added = 0;
        const CAmount need_b = plan->credit + (fee_in_b ? fee : 0);
        CAmount gathered_b = 0;
        for (const COutput& c : b_coins) {
            if (gathered_b >= need_b) break;
            gathered_b += c.tx->GetOutputValueOut(wallet, c.i);
            mtx.vin.emplace_back(COutPoint(c.tx->GetHash(), c.i));
            ++inputs_added;
        }
        if (gathered_b < need_b) {
            LogPrintf("[rewards]   funding short: %d %s candidates, gathered %d, needed %d\n",
                      (int)b_coins.size(), asset_b.GetHex(), gathered_b, need_b);
            out.error = strprintf(
                "not enough unblinded %s to pay the maker (a covenant fill cannot be funded from "
                "confidential coins)", asset_b.GetHex());
            return out;
        }

        CAmount gathered_f = 0;
        if (!fee_in_b) {
            for (const COutput& c : f_coins) {
                if (gathered_f >= fee) break;
                gathered_f += c.tx->GetOutputValueOut(wallet, c.i);
                mtx.vin.emplace_back(COutPoint(c.tx->GetHash(), c.i));
                ++inputs_added;
            }
            if (gathered_f < fee) {
                LogPrintf("[rewards]   fee short: %d %s candidates, gathered %d, needed %d\n",
                          (int)f_coins.size(), fee_asset.GetHex(), gathered_f, fee);
                out.error = strprintf("not enough unblinded %s to pay the network fee",
                                      fee_asset.GetHex());
                return out;
            }
        }

        // Slot 0 is the maker's credit and slot 1 is what the leaf reads as the
        // remainder -- an asset-A output on a partial fill, and on a full fill
        // anything that is NOT asset A, so the leaf takes its remainder-is-zero
        // branch. The change (or, failing that, the fee) does that job.
        mtx.vout.emplace_back(asset_b, plan->credit, MakerCreditSpk(cov.maker_prog));

        const CAmount change_b = gathered_b - plan->credit - (fee_in_b ? fee : 0);
        const CAmount change_f = fee_in_b ? 0 : (gathered_f - fee);

        // Everything this transaction carries that is NOT asset A. The fee
        // output is always among them, so there is always something to put in
        // the remainder slot of a full fill.
        std::vector<CTxOut> others;
        if (change_b > 0) others.emplace_back(asset_b, change_b, pay_to);
        if (change_f > 0) others.emplace_back(fee_asset, change_f, pay_to);
        others.emplace_back(fee_asset, fee, CScript());

        // Slot 1 is what the leaf reads as the remainder: the self-replicating
        // covenant on a partial fill, and on a full fill anything that is NOT
        // asset A, so the leaf takes its remainder-is-zero branch.
        if (plan->partial) {
            mtx.vout.emplace_back(asset_a, plan->remainder, scripts->spk);
            mtx.vout.emplace_back(asset_a, plan->filled, pay_to);
            for (const CTxOut& o : others) mtx.vout.push_back(o);
        } else {
            mtx.vout.push_back(others.front());
            mtx.vout.emplace_back(asset_a, plan->filled, pay_to);
            for (size_t i = 1; i < others.size(); ++i) mtx.vout.push_back(others[i]);
        }

        if (pass == 0) {
            // Size it with the witness the covenant input will carry, plus the
            // signatures the wallet's own inputs will grow.
            const size_t cov_witness = scripts->fill_leaf.size() + scripts->control_block.size() + 8;
            const size_t est = GetVirtualTransactionSize(CTransaction(mtx))
                             + cov_witness / WITNESS_SCALE_FACTOR + inputs_added * 108;
            CCoinControl cc;
            const CFeeRate rate = GetMinimumFeeRate(wallet, cc, nullptr);
            const CAmount want = rate.GetFee(est);
            fee = std::max<CAmount>(want, 1000);
        }
    }

    // The covenant input's witness is the leaf and its control block, and
    // nothing else: the FILL leaf has no CHECKSIG, so there is nothing to sign
    // for it. The leaf IS the spending condition.
    mtx.witness.vtxinwit.resize(mtx.vin.size());
    {
        CScriptWitness wit;
        wit.stack.emplace_back(scripts->fill_leaf.begin(), scripts->fill_leaf.end());
        wit.stack.push_back(scripts->control_block);
        mtx.witness.vtxinwit[0].scriptWitness = wit;
    }

    // Sign OUR inputs. Every input's coin goes in the map, not just the ones
    // the wallet owns: the map-based signer looks each one up here rather than
    // in the wallet, so a partial map makes it report the wallet's own inputs
    // as missing. The covenant input is signed by nobody, so its "failure" is
    // the expected outcome and the only one skipped.
    {
        std::map<COutPoint, Coin> sign_coins;
        for (const CTxIn& in : mtx.vin) sign_coins[in.prevout] = Coin();
        wallet.chain().findCoins(sign_coins);
        sign_coins[cov_out] = coin;

        LOCK(wallet.cs_wallet);
        std::map<int, bilingual_str> input_errors;
        wallet.SignTransaction(mtx, sign_coins, SIGHASH_ALL, input_errors);
        for (const auto& e : input_errors) {
            if (e.first >= 0 && (size_t)e.first < mtx.vin.size() && mtx.vin[e.first].prevout == cov_out) {
                continue;   // expected: nothing signs the covenant
            }
            out.error = "could not sign the fill: " + e.second.original;
            return out;
        }
    }

    CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    std::string rejected;
    if (!wallet.chain().broadcastTransaction(tx, wallet.m_default_max_tx_fee, /*relay=*/true, rejected)) {
        // The whole transaction, so a rejection can be read rather than guessed
        // at: a covenant fill is assembled by hand and every rejection is a
        // statement about which byte was wrong.
        // Sum both sides per asset before complaining: "value in != value out"
        // names no asset, and which one is wrong is the entire diagnosis.
        std::map<CAsset, CAmount> in_sum, out_sum;
        in_sum[coin.out.nAsset.GetAsset()] += coin.out.nValue.GetAmount();
        for (size_t i = 1; i < tx->vin.size(); ++i) {
            std::map<COutPoint, Coin> one{{tx->vin[i].prevout, Coin()}};
            wallet.chain().findCoins(one);
            const Coin& c = one[tx->vin[i].prevout];
            if (!c.IsSpent() && c.out.nAsset.IsExplicit() && c.out.nValue.IsExplicit()) {
                in_sum[c.out.nAsset.GetAsset()] += c.out.nValue.GetAmount();
            }
        }
        for (const CTxOut& o : tx->vout) {
            if (o.nAsset.IsExplicit() && o.nValue.IsExplicit()) {
                out_sum[o.nAsset.GetAsset()] += o.nValue.GetAmount();
            }
        }
        for (const auto& e : in_sum) {
            LogPrintf("[rewards]   %s in=%d out=%d\n", e.first.GetHex(), e.second, out_sum[e.first]);
        }
        for (const auto& e : out_sum) {
            if (!in_sum.count(e.first)) LogPrintf("[rewards]   %s in=0 out=%d\n", e.first.GetHex(), e.second);
        }
        LogPrintf("[rewards] covenant fill refused (%s): %s\n", rejected, EncodeHexTx(*tx));
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
                // A book with offers but no price for THIS size is not an empty
                // book; the quote records that with receives 0 and a non-zero
                // reference, so the staker is told the truth.
                if (!bids.empty()) row.quote = RewardQuote{0, 1};
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
                if (!book.empty()) row.quote = RewardQuote{0, 1};
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
            // Only the coins this sale accounts for are retired. A partial fill
            // must not retire the whole batch: the rest is still a reward, and
            // still due to be converted when the book can take it.
            rec.inputs = batch.inputs;
            const CAmount selling = walk ? walk->sells : batch.value;
            if (selling < batch.value) {
                std::vector<COutPoint> covered;
                CAmount acc = 0;
                for (const COutPoint& o : batch.inputs) {
                    if (acc >= selling) break;
                    for (const RewardCoin& c : coins) {
                        if (!(c.outpoint == o)) continue;
                        acc += c.amount;
                        covered.push_back(o);
                        break;
                    }
                }
                rec.inputs = covered;
                rec.value = acc;
            }
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
                LogPrintf("[rewards] filling %d of %d %s across %d offer(s)\n",
                          walk->sells, batch.value, batch.asset.GetHex(), (int)walk->legs.size());
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
