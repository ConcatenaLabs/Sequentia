// Copyright (c) 2017-2017 The Elements Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetsdir.h>
#include <chainparams.h>
#include <chainparamsbase.h>

#include <tinyformat.h>
#include <util/strencodings.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

void CAssetsDir::Set(const CAsset& asset, const AssetMetadata& metadata)
{
    LOCK(cs);
    // No asset or label repetition
    if (GetLabel(asset) != "")
        throw std::runtime_error(strprintf("duplicated asset '%s'", asset.GetHex()));
    if (GetAsset(metadata.GetLabel()) != CAsset())
        throw std::runtime_error(strprintf("duplicated label '%s'", metadata.GetLabel()));

    mapAssetMetadata[asset] = metadata;
    mapAssets[metadata.GetLabel()] = asset;
}

void CAssetsDir::SetHex(const std::string& assetHex, const std::string& label)
{
    LOCK(cs);
    if (!IsHex(assetHex) || assetHex.size() != 64)
        throw std::runtime_error("The asset must be hex string of length 64");

    const std::vector<std::string> protectedLabels = {"", "*", "bitcoin", "Bitcoin", "btc"};
    for (std::string proLabel : protectedLabels) {
        if (label == proLabel) {
            throw std::runtime_error(strprintf("'%s' label is protected", proLabel));
        }
    }
    Set(CAsset(uint256S(assetHex)), AssetMetadata(label));
}

void CAssetsDir::InitFromStrings(const std::vector<std::string>& assetsToInit, const std::string& pegged_asset_name)
{
    LOCK(cs);
    for (std::string strToSplit : assetsToInit) {
        std::vector<std::string> vAssets;
        boost::split(vAssets, strToSplit, boost::is_any_of(":"));
        if (vAssets.size() != 2) {
            throw std::runtime_error("-assetdir parameters malformed, expecting asset:label");
        }
        SetHex(vAssets[0], vAssets[1]);
    }
    // Set "bitcoin" to the pegged asset for tests
    Set(Params().GetConsensus().pegged_asset, AssetMetadata(pegged_asset_name));

    // SEQUENTIA testnet: built-in tickers for the public demo assets, so the GUI
    // shows USDX/GOLD/etc. with no -assetdir config. Any user-supplied -assetdir
    // entry (parsed above) or the pegged "bitcoin" label takes precedence — we
    // skip anything whose asset or label is already mapped.
    if (Params().NetworkIDString() == CBaseChainParams::TESTNET) {
        static const std::pair<const char*, const char*> kSeqTestnetAssets[] = {
            {"dc7f45fcfeb17c8ae74e284472d85543395f50e88f4a36cb652e8102703b7027", "USDX"},
            {"f7a756b4e966623065543e52b754324629295c895046a0916a939898ad373667", "EURX"},
            {"c28fc933ce41f7a9188da029c6f7377fc961e2d58588372ef4073438610b9283", "GOLD"},
            {"3e30ad0ebd13cc7ac1bbd12df1414b213708a6048b745d185fe935d9624024db", "WBTC"},
            {"50a00211d7074d5f857a3dec6cb84a1f3fefb26e56a94a954a299b28ac9f32df", "SILVR"},
            {"f9b069ac00f4dc57381a304704fac93301f90d3d509d207cfbddc8367d4e9cfb", "OILX"},
        };
        for (const auto& [assetHex, label] : kSeqTestnetAssets) {
            const CAsset asset(uint256S(assetHex));
            if (GetLabel(asset).empty() && GetAsset(label) == CAsset()) {
                Set(asset, AssetMetadata(label));
            }
        }
    }
}

int CAssetsDir::Merge(const std::vector<std::pair<std::string, std::string>>& id_label_pairs)
{
    LOCK(cs);
    int added = 0;
    static const std::vector<std::string> protectedLabels = {"", "*", "bitcoin", "Bitcoin", "btc"};
    for (const auto& [assetHex, label] : id_label_pairs) {
        if (!IsHex(assetHex) || assetHex.size() != 64) continue;
        bool prot = false;
        for (const auto& p : protectedLabels) if (label == p) { prot = true; break; }
        if (prot) continue;
        const CAsset asset(uint256S(assetHex));
        // Additive + idempotent: a user -assetdir entry, the native label, or an
        // already-registered asset/label always wins — skip if either is mapped.
        if (mapAssetMetadata.count(asset) || mapAssets.count(label)) continue;
        mapAssetMetadata[asset] = AssetMetadata(label);
        mapAssets[label] = asset;
        added++;
    }
    return added;
}

CAsset CAssetsDir::GetAsset(const std::string& label) const
{
    LOCK(cs);
    auto it = mapAssets.find(label);
    if (it != mapAssets.end())
        return it->second;
    return CAsset();
}

AssetMetadata CAssetsDir::GetMetadata(const CAsset& asset) const
{
    LOCK(cs);
    auto it = mapAssetMetadata.find(asset);
    if (it != mapAssetMetadata.end())
        return it->second;
    return AssetMetadata("");
}

std::string CAssetsDir::GetLabel(const CAsset& asset) const
{
    return GetMetadata(asset).GetLabel();
}

std::string CAssetsDir::GetIdentifier(const CAsset& asset) const
{
    const std::string label = GetMetadata(asset).GetLabel();
    if (!label.empty()) return label;
    return asset.GetHex();
}

std::vector<CAsset> CAssetsDir::GetKnownAssets() const
{
    LOCK(cs);
    std::vector<CAsset> knownAssets;
    for (auto it = mapAssets.begin(); it != mapAssets.end(); it++) {
        knownAssets.push_back(it->second);
    }
    return knownAssets;
}

CAsset GetAssetFromString(const std::string& strasset) {
    CAsset asset = gAssetsDir.GetAsset(strasset);
    if (asset.IsNull() && strasset.size() == 64 && IsHex(strasset)) {
        asset = CAsset(uint256S(strasset));
    }
    return asset;
}

// GLOBAL:
CAssetsDir _gAssetsDir;
const CAssetsDir& gAssetsDir = _gAssetsDir;

void InitGlobalAssetDir(const std::vector<std::string>& assetsToInit, const std::string& pegged_asset_name)
{
    _gAssetsDir.InitFromStrings(assetsToInit, pegged_asset_name);
}

int MergeGlobalAssetDir(const std::vector<std::pair<std::string, std::string>>& id_label_pairs)
{
    return _gAssetsDir.Merge(id_label_pairs);
}

void CAssetsDir::Clear()
{
    LOCK(cs);
    mapAssetMetadata.clear();
    mapAssets.clear();
}

// Used in testing
void ClearGlobalAssetDir()
{
    _gAssetsDir.Clear();
}

