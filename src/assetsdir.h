
#ifndef BITCOIN_ASSETSDIR_H
#define BITCOIN_ASSETSDIR_H

#include <asset.h>
#include <sync.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

/** SEQUENTIA: default number of decimal places for an asset whose denomination
 *  is unknown. Matches the policy asset (SEQ) and the on-chain default of
 *  CAssetIssuance::nDenomination. */
static constexpr uint8_t DEFAULT_ASSET_PRECISION = 8;
/** SEQUENTIA: an asset amount is an int64 atom count; 10^precision must stay
 *  well within int64, so cap precision at 18 (10^18 < 2^63). */
static constexpr uint8_t MAX_ASSET_PRECISION = 18;

class AssetMetadata
{
public:
    /** Where an asset's precision came from, in increasing order of authority.
     *  The chain's committed nDenomination outranks the registry's advisory
     *  precision, which outranks the built-in default. A lower-authority source
     *  never overwrites a higher one. */
    enum class PrecisionSource : uint8_t { Default = 0, Registry = 1, Chain = 2 };

private:
    std::string label;
    uint8_t precision{DEFAULT_ASSET_PRECISION};
    PrecisionSource precision_source{PrecisionSource::Default};

public:
    AssetMetadata() : label("") {};
    AssetMetadata(std::string _label) : label(_label) {};
    AssetMetadata(std::string _label, uint8_t _precision, PrecisionSource _source)
        : label(_label), precision(_precision), precision_source(_source) {};

    const std::string& GetLabel() const
    {
        return label;
    }

    uint8_t GetPrecision() const
    {
        return precision;
    }

    PrecisionSource GetPrecisionSource() const
    {
        return precision_source;
    }

    /** Set precision only if the new source is at least as authoritative as the
     *  current one (chain beats registry beats default). @return true if applied. */
    bool SetPrecision(uint8_t _precision, PrecisionSource _source)
    {
        if (_source < precision_source) return false;
        precision = _precision;
        precision_source = _source;
        return true;
    }
};

/** SEQUENTIA: one asset as published by the Sequentia Asset Registry index. */
struct AssetRegistryEntry
{
    std::string id_hex;
    std::string label;
    uint8_t precision{DEFAULT_ASSET_PRECISION};
};

class CAssetsDir
{
    // SEQUENTIA: leaf mutex so the Asset Registry fetcher (a scheduler thread) can
    // merge new labels at runtime while RPC/GUI threads read. Never held while
    // acquiring another lock, so there is no lock-order inversion.
    mutable RecursiveMutex cs;
    std::map<CAsset, AssetMetadata> mapAssetMetadata;
    std::map<std::string, CAsset> mapAssets;

    void Set(const CAsset& asset, const AssetMetadata& metadata);
    void SetHex(const std::string& assetHex, const std::string& label);
public:
    void InitFromStrings(const std::vector<std::string>& assetsToInit, const std::string& pegged_asset_name);

    /**
     * SEQUENTIA: merge registry entries (id, label, precision) at runtime, e.g.
     * from the Sequentia Asset Registry. Additive and idempotent for labels: an
     * asset or label that is already mapped keeps its label (existing operator
     * -assetdir entries and the native label always win). Registry precision is
     * still applied to an already-labelled asset unless a more authoritative
     * (chain) precision is already recorded. Thread-safe.
     * @return the number of newly-added labels.
     */
    int Merge(const std::vector<AssetRegistryEntry>& entries);

    /**
     * SEQUENTIA: record an asset's precision as read from its on-chain issuance
     * (CAssetIssuance::nDenomination). This is the authoritative source and
     * overrides any registry-supplied precision. Does NOT create a label, so an
     * asset known only by hex still formats with the right number of decimals.
     * Thread-safe.
     */
    void SetChainPrecision(const CAsset& asset, uint8_t precision);

    /** Drop all entries (used in testing). */
    void Clear();

    /**
     * @param  label A label string
     * @return asset id corresponding to the asset label
     */
    CAsset GetAsset(const std::string& label) const;

    AssetMetadata GetMetadata(const CAsset& asset) const;

    /** @return the number of decimal places to display for the asset. Defaults
     *  to DEFAULT_ASSET_PRECISION (8) when the asset's denomination is unknown. */
    uint8_t GetPrecision(const CAsset& asset) const;

    /** @return the label associated to the asset id */
    std::string GetLabel(const CAsset& asset) const;

    /** @return the label associated to the asset id, or some other identifier */
    std::string GetIdentifier(const CAsset& asset) const;

    std::vector<CAsset> GetKnownAssets() const;
};

/**
 * Returns asset id corresponding to the given asset expression, which is either an asset label or a hex value.
 * @param  strasset A label string or a hex value corresponding to an asset
 * @return       The asset ID for the given expression
 */
CAsset GetAssetFromString(const std::string& strasset);

// GLOBAL:
class CAssetsDir;

extern const CAssetsDir& gAssetsDir;

void InitGlobalAssetDir(const std::vector<std::string>& assetsToInit, const std::string& pegged_asset_name);
// SEQUENTIA: merge runtime registry entries (e.g. from the Asset Registry) into
// the global dir. Returns the number of newly-added labels.
int MergeGlobalAssetDir(const std::vector<AssetRegistryEntry>& entries);
// SEQUENTIA: record an asset's on-chain precision (nDenomination) in the global
// dir. Authoritative; overrides any registry-supplied precision.
void RegisterGlobalChainAssetPrecision(const CAsset& asset, uint8_t precision);
// Used in testing
void ClearGlobalAssetDir(void);

#endif // BITCOIN_ASSETSDIR_H
