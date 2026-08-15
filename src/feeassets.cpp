// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <feeassets.h>

#include <chainparams.h>
#include <exchangerates.h>
#include <referenceprices.h>
#include <util/strencodings.h>

#include <algorithm>
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
