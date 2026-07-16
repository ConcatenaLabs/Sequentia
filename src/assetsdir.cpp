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

int CAssetsDir::Merge(const std::vector<AssetRegistryEntry>& entries)
{
    LOCK(cs);
    int added = 0;
    static const std::vector<std::string> protectedLabels = {"", "*", "bitcoin", "Bitcoin", "btc"};
    for (const auto& entry : entries) {
        const std::string& assetHex = entry.id_hex;
        const std::string& label = entry.label;
        if (!IsHex(assetHex) || assetHex.size() != 64) continue;
        bool prot = false;
        for (const auto& p : protectedLabels) if (label == p) { prot = true; break; }
        if (prot) continue;
        const uint8_t precision = entry.precision > MAX_ASSET_PRECISION ? DEFAULT_ASSET_PRECISION : entry.precision;
        const CAsset asset(uint256S(assetHex));

        auto it = mapAssetMetadata.find(asset);
        if (it != mapAssetMetadata.end()) {
            // Asset already known (operator -assetdir entry, native label, a
            // prior merge, or a chain-precision-only record). Let the registry
            // supply precision if nothing more authoritative (chain) claimed it.
            it->second.SetPrecision(precision, AssetMetadata::PrecisionSource::Registry);
            // A chain-precision-only record has no label yet (the wallet may have
            // registered the asset's denomination before this merge ran). Adopt
            // the registry label now, preserving the recorded precision, unless
            // the label is already taken by another asset.
            if (it->second.GetLabel().empty() && !mapAssets.count(label)) {
                it->second = AssetMetadata(label, it->second.GetPrecision(), it->second.GetPrecisionSource());
                mapAssets[label] = asset;
                added++;
            }
            continue;
        }
        // Label collisions: an already-mapped label always wins.
        if (mapAssets.count(label)) continue;
        mapAssetMetadata[asset] = AssetMetadata(label, precision, AssetMetadata::PrecisionSource::Registry);
        mapAssets[label] = asset;
        added++;
    }
    return added;
}

void CAssetsDir::SetChainPrecision(const CAsset& asset, uint8_t precision)
{
    LOCK(cs);
    if (precision > MAX_ASSET_PRECISION) precision = DEFAULT_ASSET_PRECISION;
    // Creates a label-less metadata record if the asset is otherwise unknown, so
    // an asset held only by hex still formats with the right number of decimals.
    mapAssetMetadata[asset].SetPrecision(precision, AssetMetadata::PrecisionSource::Chain);
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

uint8_t CAssetsDir::GetPrecision(const CAsset& asset) const
{
    return GetMetadata(asset).GetPrecision();
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

int MergeGlobalAssetDir(const std::vector<AssetRegistryEntry>& entries)
{
    return _gAssetsDir.Merge(entries);
}

void RegisterGlobalChainAssetPrecision(const CAsset& asset, uint8_t precision)
{
    _gAssetsDir.SetChainPrecision(asset, precision);
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

