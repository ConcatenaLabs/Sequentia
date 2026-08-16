// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <feeassets.h>

#include <chainparams.h>
#include <exchangerates.h>
#include <logging.h>
#include <referenceprices.h>
#include <scheduler.h>
#include <txmempool.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>

std::string FeeAssetFeedTicker(const CAsset& asset)
{
    if (asset == Params().GetConsensus().pegged_asset) return "SEQ";
    return ToUpper(gAssetsDir.GetIdentifier(asset));
}

namespace {
//! Fill in everything but the price, which the callers below look up from one
//! shared snapshot rather than re-copying the price map per asset.
FeeAssetInfo BuildWithoutPrice(const CAsset& asset)
{
    FeeAssetInfo info;
    info.asset = asset;
    const AssetMetadata meta = gAssetsDir.GetMetadata(asset);
    info.identifier = meta.GetLabel().empty() ? asset.GetHex() : meta.GetLabel();
    info.precision = meta.GetPrecision();
    info.registry_listed = meta.IsRegistryListed();
    info.listed = ExchangeRateMap::GetInstance().GetRate(asset, info.rate);
    info.accepted = info.listed && info.rate > 0;
    return info;
}

void ApplyPrice(FeeAssetInfo& info, const std::map<std::string, double>& prices)
{
    const auto it = prices.find(FeeAssetFeedTicker(info.asset));
    if (it != prices.end() && it->second > 0.0) {
        info.has_market_price = true;
        info.market_price = it->second;
    }
}
} // namespace

FeeAssetInfo GetFeeAssetInfo(const CAsset& asset)
{
    FeeAssetInfo info = BuildWithoutPrice(asset);
    ApplyPrice(info, GetReferencePrices());
    return info;
}

std::vector<FeeAssetInfo> GetAllFeeAssetInfo()
{
    std::set<CAsset> assets;
    for (const auto& rate : ExchangeRateMap::GetInstance().GetRates()) {
        assets.insert(rate.first);
    }
    for (const CAsset& asset : gAssetsDir.GetKnownAssets()) {
        assets.insert(asset);
    }

    const std::map<std::string, double> prices = GetReferencePrices();
    std::vector<FeeAssetInfo> out;
    out.reserve(assets.size());
    for (const CAsset& asset : assets) {
        FeeAssetInfo info = BuildWithoutPrice(asset);
        ApplyPrice(info, prices);
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const FeeAssetInfo& a, const FeeAssetInfo& b) {
        return a.identifier < b.identifier;
    });
    return out;
}

//! One whole unit's price, in reference units, expressed the way the whitelist
//! wants it: scaled by exchange_rate_scale, and carrying a further 10^(8 -
//! precision) so the node can value a fee without knowing the asset's decimals.
//! Matches the price server's own conversion (contrib/price-server/README.md);
//! the two must agree or a sidecar taking over would move every fee.
//! Returns 0 for anything that cannot be represented, so one absurd quote is
//! dropped on its own rather than poisoning the batch.
CAmount FeeRateFromUnitPrice(double price, uint8_t precision)
{
    if (!(price > 0.0)) return 0;
    long double scaled = static_cast<long double>(price) * static_cast<long double>(exchange_rate_scale);
    for (int i = precision; i < 8; ++i) scaled *= 10.0L;
    for (int i = 8; i < precision; ++i) scaled /= 10.0L;
    if (!(scaled >= 1.0L)) return 0;
    if (scaled > static_cast<long double>(std::numeric_limits<int64_t>::max())) return 0;
    return static_cast<CAmount>(scaled + 0.5L);
}

double UnitPriceFromFeeRate(CAmount rate, uint8_t precision)
{
    if (rate <= 0) return 0.0;
    long double price = static_cast<long double>(rate) / static_cast<long double>(exchange_rate_scale);
    for (int i = precision; i < 8; ++i) price /= 10.0L;
    for (int i = 8; i < precision; ++i) price *= 10.0L;
    return static_cast<double>(price);
}

int ApplyFeedDerivedFeeRates()
{
    const std::map<std::string, double> prices = GetReferencePrices();
    if (prices.empty()) return 0;

    // Driven by the assets this chain knows, never by the feed's keys: the feed
    // also quotes things that are not assets here at all (the parent chain's
    // bitcoin, for one), and those must not become payable just because a price
    // exists for them.
    std::map<CAsset, CAmount> derived;
    for (const CAsset& asset : gAssetsDir.GetKnownAssets()) {
        const auto it = prices.find(FeeAssetFeedTicker(asset));
        if (it == prices.end()) continue;
        const CAmount rate = FeeRateFromUnitPrice(it->second, gAssetsDir.GetPrecision(asset));
        if (rate > 0) derived.emplace(asset, rate);
    }
    const int changed = ExchangeRateMap::GetInstance().MergeFeedRates(derived);
    if (changed > 0) {
        LogPrintf("FeeAssets: priced %d asset(s) for fee payment from the reference feed\n", changed);
    }
    return changed;
}

void StartFeedDerivedFeeRates(CScheduler& scheduler, CTxMemPool* mempool)
{
    if (gArgs.GetArg("-referencepricesurl", "").empty()) return;

    auto tick = [mempool] {
        if (ApplyFeedDerivedFeeRates() > 0 && mempool != nullptr) {
            // Every entry in the mempool was valued at the old rates, and the
            // miner sorts on that valuation; leaving them stale would rank the
            // queue by prices nobody quotes any more.
            mempool->RecomputeFees();
        }
    };
    // A few seconds behind the first price fetch, which StartReferencePrices
    // schedules; if it has not landed yet the next poll picks it up.
    scheduler.scheduleFromNow(tick, std::chrono::seconds{10});
    const int poll = gArgs.GetIntArg("-referencepricespoll", 300);
    if (poll > 0) scheduler.scheduleEvery(tick, std::chrono::seconds{poll});
}
