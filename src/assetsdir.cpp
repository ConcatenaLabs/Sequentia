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
    // SEQUENTIA: a label must name an actual asset. The null asset is not one --
    // it is the "no asset" sentinel that CAsset() and a failed lookup both
    // produce -- so binding a label to it creates an entry that resolves to
    // nothing while still occupying the name. GetAssetFromString() then returns a
    // null CAsset for a label it did find, which every caller reads as "unknown",
    // and the resulting diagnostics blame the caller's spelling for a defect in
    // the chain's configuration. Refuse the binding at the source instead.
    if (asset.IsNull())
        throw std::runtime_error(strprintf("label '%s' cannot be assigned to the null asset", metadata.GetLabel()));

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

void CAssetsDir::SetAlias(const std::string& alias, const CAsset& asset)
{
    LOCK(cs);
    // A second NAME for an asset that already has a canonical one. Only mapAssets
    // is written, so the alias resolves on the way in while GetLabel() keeps
    // answering with the canonical name on the way out.
    if (asset.IsNull() || alias.empty() || mapAssets.count(alias)) return;
    mapAssets[alias] = asset;
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
    // Set "bitcoin" to the pegged asset for tests.
    //
    // SEQUENTIA: only where the chain HAS a pegged asset. A chain running with
    // elements mode off (main, signet, regtest) declares none, leaving
    // consensus.pegged_asset default-constructed, i.e. null. Registering the name
    // anyway produced a label that resolved to nothing, and the node then wrote an
    // exchangerates.json naming that label which it could not read back on the
    // next start. On such a chain the honest state is that the name is simply
    // absent: there is one implicit asset and nothing to disambiguate, so nothing
    // needs a label.
    const CAsset& pegged_asset = Params().GetConsensus().pegged_asset;
    if (!pegged_asset.IsNull()) {
        Set(pegged_asset, AssetMetadata(pegged_asset_name));
        // SEQUENTIA: on the Sequentia chains the native asset is named for the
        // chain (SEQ / tSEQ), not "bitcoin" as Elements defaults it -- calling it
        // bitcoin in dumpassetlabels, exchangerates.json and the fee-policy window
        // reads as PARENT-CHAIN bitcoin, an asset that lives on another network and
        // cannot pay a fee here at all. Existing datadirs hold an exchangerates.json
        // written under the old name, and a name the directory cannot resolve is a
        // hard startup failure (see the InitError around LoadFromDefaultJSONFile),
        // so the old name keeps resolving as an alias. The file is rewritten with
        // the canonical name on the same startup, so this is one release of
        // tolerance rather than a permanent second name.
        if (pegged_asset_name != "bitcoin") {
            SetAlias("bitcoin", pegged_asset);
        }
    }

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
        // SEQUENTIA: an all-zero id is valid hex of the right length but is the
        // null asset, i.e. no asset at all. Merge() writes mapAssets directly and
        // so does not pass through Set()'s guard; drop such an entry here, or the
        // registry could claim a label that resolves to nothing.
        if (uint256S(assetHex).IsNull()) continue;
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
            // After any reassignment above, so the flag is not dropped with the
            // old record: the registry publishes this asset whatever its label
            // ended up being.
            it->second.MarkRegistryListed();
            continue;
        }
        // Label collisions: an already-mapped label always wins. The asset is
        // still on the registry, though, and losing a name race is no reason to
        // forget that — record it label-less rather than dropping the entry, so
        // it keeps the registry's precision and does not look unpublished.
        if (mapAssets.count(label)) {
            AssetMetadata& meta = mapAssetMetadata[asset];
            meta.SetPrecision(precision, AssetMetadata::PrecisionSource::Registry);
            meta.MarkRegistryListed();
            continue;
        }
        mapAssetMetadata[asset] = AssetMetadata(label, precision, AssetMetadata::PrecisionSource::Registry);
        mapAssetMetadata[asset].MarkRegistryListed();
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

bool CAssetsDir::HasLabel(const std::string& label) const
{
    LOCK(cs);
    return mapAssets.count(label) > 0;
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

bool CAssetsDir::IsRegistryListed(const CAsset& asset) const
{
    return GetMetadata(asset).IsRegistryListed();
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

