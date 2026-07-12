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

    // SEQUENTIA: asset tickers/names (demo and user-issued) come from the Asset
    // Registry at runtime — see assetregistry.cpp and -assetregistryurl — which only
    // trusts domain+chain-verified entries. We deliberately do NOT hardcode a
    // built-in testnet list here: stale IDs would claim a label (e.g. "GOLD") and
    // then block the correct, verified registry entry at merge time (Merge() skips
    // any already-mapped label), leaving the real asset shown as a raw hex id.
    // Assets with no registry entry simply fall back to their hex identifier.
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

