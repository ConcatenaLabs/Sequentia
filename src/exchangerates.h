#ifndef BITCOIN_EXCHANGERATES_H
#define BITCOIN_EXCHANGERATES_H

#include <fs.h>
#include <policy/policy.h>
#include <policy/value.h>
#include <sync.h>
#include <univalue.h>

constexpr const CAmount exchange_rate_scale = COIN; // 100,000,000
const std::string exchange_rates_config_file = "exchangerates.json";

/** Default maximum age (in seconds) of a dynamic exchange rate before it is
 *  considered stale and dropped from the effective whitelist. 0 disables expiry. */
static const int64_t DEFAULT_DYNAMIC_RATE_MAX_AGE = 0;

class CAssetExchangeRate
{
public:
    /** Fee rate. */
    CAmount m_scaled_value;

    CAssetExchangeRate() : m_scaled_value(0) { }
    CAssetExchangeRate(CAmount amount) : m_scaled_value(amount) { }
    CAssetExchangeRate(uint64_t amount) : m_scaled_value(amount) { }
};

/** A dynamically published exchange rate (e.g. from a locally run price
 *  server), carrying provenance and freshness metadata. */
class CDynamicExchangeRate
{
public:
    CAmount m_scaled_value{0};
    //! Identifier of the publisher (e.g. price server name), informational only.
    std::string m_source;
    //! Unix time at which this rate was last published.
    int64_t m_updated_at{0};

    CDynamicExchangeRate() {}
    CDynamicExchangeRate(CAmount value, const std::string& source, int64_t updated_at)
        : m_scaled_value(value), m_source(source), m_updated_at(updated_at) {}
};

/**
 * The effective fee-asset whitelist: a map from asset to exchange rate
 * (in atoms per `exchange_rate_scale` reference fee atoms).
 *
 * The effective map (the std::map base, read by the mempool/miner/wallet) is
 * built from two layers:
 *  - a *static* layer, configured by the operator via exchangerates.json or the
 *    `setfeeexchangerates` RPC, and
 *  - a *dynamic* layer, published at runtime (e.g. by a price server sidecar)
 *    via the `setdynamicfeerates` RPC, with per-entry freshness metadata.
 *
 * A static entry always takes precedence over a dynamic entry for the same
 * asset, so an operator-pinned valuation can never be overridden by a price
 * feed. Dynamic entries older than the configured max age are excluded from
 * the effective map (failing safe to "asset not accepted").
 */
class ExchangeRateMap : public std::map<CAsset, CAssetExchangeRate>
{
private:
    //! Operator-configured rates (exchangerates.json / setfeeexchangerates).
    std::map<CAsset, CAssetExchangeRate> m_static_rates;
    //! Runtime-published rates (setdynamicfeerates), e.g. from a price server.
    std::map<CAsset, CDynamicExchangeRate> m_dynamic_rates;
    //! Maximum age in seconds for dynamic rates; 0 = no expiry.
    int64_t m_dynamic_max_age{DEFAULT_DYNAMIC_RATE_MAX_AGE};
    //! Serializes writers (RPC threads, scheduler purge task). Readers of the
    //! effective map are not synchronized here; see RecomputeFees() callers.
    Mutex m_write_mutex;

    ExchangeRateMap() {}
    ExchangeRateMap(const CAmount& default_asset_rate) {
        m_static_rates[::policyAsset] = default_asset_rate;
        RebuildEffective();
    }

    //! Rebuild the effective (base std::map) view from the two layers.
    void RebuildEffective();

public:
    static ExchangeRateMap& GetInstance() {
        static ExchangeRateMap instance(exchange_rate_scale); // Guaranteed to be destroyed and instantiated only once
        return instance;
    }

    /**
     * Convert an amount denominated in some asset to reference fee atoms
     *
     * @param[in]   amount       Corresponds to CTxMemPoolEntry.nFee
     * @param[in]   asset        Corresponds to CTxMemPoolEntry.nFeeAsset
     * @return the value at current exchange rate. Corresponds to CTxMemPoolEntry.nFeeValue
     */
    CValue ConvertAmountToValue(const CAmount& amount, const CAsset& asset);

    /**
     * Convert an amount denominated in reference fee atoms into some asset
     *
     * @param[in]   value        Corresponds to CTxMemPoolEntry.nFeeValue
     * @param[in]   asset        Corresponds to CTxMemPoolEntry.nFeeAsset
     * @return the amount at current exchange rate. Corresponds to CTxMemPoolEntry.nFee
     */
    CAmount ConvertValueToAmount(const CValue& value, const CAsset& asset);

    /**
     * Load the exchange rate map from the default JSON config file in <datadir>/exchangerates.json.
     *
     * @param[in]   errors        Vector for storing error messages, if there are any.
     * @return true on success
     */
    bool LoadFromDefaultJSONFile(std::vector<std::string>& errors);

    /**
     * Load the exchange rate map from a JSON config file.
     *
     * @param[in]   file_path     File path to JSON config file where keys are asset labels and values are exchange rates.
     * @param[in]   errors        Vector for storing error messages, if there are any.
     * @return true on success
     */
    bool LoadFromJSONFile(fs::path file_path, std::vector<std::string>& errors);

    /**
     * Save the (static layer of the) exchange rate map to a JSON config file
     * in the node's data directory.
     *
     * @param[in]   errors        Vector for storing error messages, if there are any.
     * @return true on success
     */
    bool SaveToJSONFile(std::vector<std::string>& errors);

    //! Effective (merged) rates as JSON.
    UniValue ToJSON();

    //! Static layer as JSON (this is what is persisted to exchangerates.json).
    UniValue StaticToJSON();

    //! Dynamic layer as JSON, including source and freshness metadata.
    UniValue DynamicToJSON();

    //! Effective acceptance policy: every accepted asset with its rate and provenance.
    UniValue AcceptancePolicyToJSON();

    //! Replace the static layer from parsed JSON (asset label/hex -> rate).
    bool LoadFromJSON(std::map<std::string, UniValue> json, std::vector<std::string>& error);

    /**
     * Replace the dynamic layer.
     *
     * @param[in]   rates         New dynamic rates (asset -> scaled rate).
     * @param[in]   source        Identifier of the publisher, stored per entry.
     * @param[in]   now           Current unix time, stored as publish time.
     */
    void SetDynamicRates(const std::map<CAsset, CAmount>& rates, const std::string& source, int64_t now);

    //! Drop all dynamic rates.
    void ClearDynamicRates();

    /**
     * Drop dynamic entries older than the configured max age.
     *
     * @param[in]   now           Current unix time.
     * @return true if any entry was dropped (callers should RecomputeFees()).
     */
    bool PurgeStaleDynamicRates(int64_t now);

    void SetDynamicMaxAge(int64_t max_age) { m_dynamic_max_age = max_age; }
    int64_t GetDynamicMaxAge() const { return m_dynamic_max_age; }
};

#endif // BITCOIN_EXCHANGERATES_H
