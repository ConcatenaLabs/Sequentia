
#ifndef BITCOIN_ASSETSDIR_H
#define BITCOIN_ASSETSDIR_H

#include <asset.h>
#include <sync.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

class AssetMetadata
{
    std::string label;
public:
    AssetMetadata() : label("") {};
    AssetMetadata(std::string _label) : label(_label) {};

    const std::string& GetLabel() const
    {
        return label;
    }
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
     * SEQUENTIA: merge (asset-id-hex, label) pairs at runtime, e.g. from the
     * Sequentia Asset Registry. Additive and idempotent: an asset or label that
     * is already mapped is skipped (existing operator -assetdir entries and the
     * native label always win). Thread-safe.
     * @return the number of newly-added labels.
     */
    int Merge(const std::vector<std::pair<std::string, std::string>>& id_label_pairs);

    /** Drop all entries (used in testing). */
    void Clear();

    /**
     * @param  label A label string
     * @return asset id corresponding to the asset label
     */
    CAsset GetAsset(const std::string& label) const;

    AssetMetadata GetMetadata(const CAsset& asset) const;

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
// SEQUENTIA: merge runtime labels (e.g. from the Asset Registry) into the global
// dir. Returns the number of newly-added labels.
int MergeGlobalAssetDir(const std::vector<std::pair<std::string, std::string>>& id_label_pairs);
// Used in testing
void ClearGlobalAssetDir(void);

#endif // BITCOIN_ASSETSDIR_H
