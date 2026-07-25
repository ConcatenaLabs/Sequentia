#ifndef BITCOIN_EXCHANGERATES_H
#define BITCOIN_EXCHANGERATES_H

#include <fs.h>
#include <policy/policy.h>
#include <policy/value.h>
#include <sync.h>
#include <univalue.h>

constexpr const CAmount exchange_rate_scale = COIN; // 100,000,000
const std::string exchange_rates_config_file = "exchangerates.json";

class CAssetExchangeRate
{
public:
    /** Fee rate: atoms of the asset equal to one reference unit (exchange_rate_scale). */
    CAmount m_scaled_value;

    CAssetExchangeRate() : m_scaled_value(0) { }
    CAssetExchangeRate(CAmount amount) : m_scaled_value(amount) { }
    CAssetExchangeRate(uint64_t amount) : m_scaled_value(amount) { }
};

/**
 * The fee-asset whitelist: a single map from asset to exchange rate (in atoms
 * per `exchange_rate_scale` reference fee atoms), read by the mempool/miner/wallet
 * when valuing any-asset fees.
 *
 * SEQUENTIA: there is exactly ONE whitelist — the node does not distinguish a
 * "static" operator layer from a "dynamic" price-server layer. Whether the
 * whitelist is effectively static or dynamic is determined entirely OUTSIDE the
 * node, by whether a price-server sidecar is running and pushing changes. The
 * node just holds whatever was last set, by an operator (exchangerates.json /
 * setfeeexchangerates) or by a sidecar (which uses the same setter); it does not
 * track provenance or freshness. The sidecar is responsible for the admission
 * thresholds and for adding/removing assets.
 */
class ExchangeRateMap : public std::map<CAsset, CAssetExchangeRate>
{
private:
    //! Serializes writers (RPC threads, config load) against readers
    //! (ConvertAmountToValue/ConvertValueToAmount).
    Mutex m_write_mutex;

    ExchangeRateMap() { ResetToBootstrapRates(); }

public:
    static ExchangeRateMap& GetInstance() {
        static ExchangeRateMap instance; // Guaranteed to be destroyed and instantiated only once
        return instance;
    }

    /**
     * Seed the out-of-box whitelist: the policy asset alone, priced at
     * exchange_rate_scale.
     *
     * SEQUENTIA: this SEED is the entire bootstrap mechanism. It is what lets a
     * node that has never been configured accept fees at all, and it is why the
     * converters need no special case for the policy asset: an unlisted asset is
     * unlisted, whichever asset it is. The reference unit stays an abstract
     * factor and is never itself a token; the seed merely states an opening
     * price for one asset, which an operator or price-server sidecar is free to
     * change, set to 0 (refuse), or drop entirely by replacing the whitelist.
     */
    void ResetToBootstrapRates();

    /**
     * Convert an amount denominated in some asset to reference fee atoms.
     * An asset absent from the whitelist is not accepted and values to 0, with
     * no exception for the policy asset.
     * @param[in]   amount       Corresponds to CTxMemPoolEntry.nFee
     * @param[in]   asset        Corresponds to CTxMemPoolEntry.nFeeAsset
     * @return the value at the current exchange rate. Corresponds to CTxMemPoolEntry.nFeeValue
     */
    CValue ConvertAmountToValue(const CAmount& amount, const CAsset& asset);

    /**
     * Convert an amount denominated in reference fee atoms into some asset.
     * An asset absent from the whitelist is not accepted and converts to 0, with
     * no exception for the policy asset.
     * @param[in]   value        Corresponds to CTxMemPoolEntry.nFeeValue
     * @param[in]   asset        Corresponds to CTxMemPoolEntry.nFeeAsset
     * @return the amount at the current exchange rate. Corresponds to CTxMemPoolEntry.nFee
     */
    CAmount ConvertValueToAmount(const CValue& value, const CAsset& asset);

    /** Load the whitelist from <datadir>/exchangerates.json (no-op if absent). */
    bool LoadFromDefaultJSONFile(std::vector<std::string>& errors);

    /** Load the whitelist from a JSON config file (keys are asset labels/hex, values are rates). */
    bool LoadFromJSONFile(fs::path file_path, std::vector<std::string>& errors);

    /** Persist the current whitelist to <datadir>/exchangerates.json. */
    bool SaveToJSONFile(std::vector<std::string>& errors);

    //! The whitelist as JSON (asset label/hex -> rate).
    UniValue ToJSON();

    //! The whitelist as JSON with one nested object per asset ({ "rate": n }).
    //! This is exactly the accept set: there is no implicit entry to materialise.
    UniValue AcceptancePolicyToJSON();

    //! Replace the whole whitelist from parsed JSON (asset label/hex -> rate).
    bool LoadFromJSON(std::map<std::string, UniValue> json, std::vector<std::string>& error);

    /** Replace the whole whitelist. Used by both an operator and a price-server
     *  sidecar — the node treats them identically. */
    void SetRates(const std::map<CAsset, CAmount>& rates);

    /** Empty the whitelist. Nothing survives: an empty whitelist accepts NO fee
     *  asset, the policy asset included. Use ResetToBootstrapRates() to get back
     *  to the out-of-box state instead. */
    void ClearRates();
};

#endif // BITCOIN_EXCHANGERATES_H
