#ifndef BITCOIN_EXCHANGERATES_H
#define BITCOIN_EXCHANGERATES_H

#include <fs.h>
#include <policy/policy.h>
#include <policy/value.h>
#include <sync.h>
#include <univalue.h>

#include <functional>

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
 * SEQUENTIA: there is exactly ONE whitelist, with no layers. It is static
 * when written by hand and dynamic when the node is connected to a price
 * server and reads that server's published list (FeeListSource below): the
 * node holds whatever its one writer set last. The price server is
 * responsible for the admission thresholds and for adding and removing assets.
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
     * SEQUENTIA: this SEED is the entire bootstrap mechanism. Where it is kept
     * (custom chains) it is what lets a node that has never
     * been configured accept fees at all, and it is why the converters need no
     * special case for the policy asset: an unlisted asset is unlisted,
     * whichever asset it is. The reference unit stays an abstract factor and is
     * never itself a token; the seed merely states an opening price for one
     * asset, which an operator or a price server is free to change, set
     * to 0 (refuse), or drop entirely by replacing the whitelist.
     *
     * A chain whose CChainParams::SeedFeeWhitelist() is false (the mainnet
     * and the testnet) discards the seed at startup, so an unconfigured node
     * there accepts nothing until its whitelist is written.
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

    /**
     * Look up one asset's rate without going through a conversion.
     * @param[out] rate_out  the rate as listed; 0 means the asset is listed and
     *                       explicitly refused, which is NOT the same state as
     *                       being absent even though both refuse a fee.
     * @return false if the asset is absent from the whitelist.
     */
    bool GetRate(const CAsset& asset, CAmount& rate_out);

    /** A snapshot copy of the whole whitelist. Callers must not iterate the map
     *  itself: it is mutated under m_write_mutex by SetRates(). */
    std::map<CAsset, CAmount> GetRates();

    /** Load the whitelist from <datadir>/exchangerates.json (no-op if absent). */
    bool LoadFromDefaultJSONFile(std::vector<std::string>& errors);

    /** Load the whitelist from a JSON config file (keys are asset labels/hex, values are rates). */
    bool LoadFromJSONFile(fs::path file_path, std::vector<std::string>& errors);

    /** Persist the current whitelist to <datadir>/exchangerates.json. */
    bool SaveToJSONFile(std::vector<std::string>& errors);

    //! The whitelist as JSON (asset label/hex -> rate).
    UniValue ToJSON();

    //! The whitelist as JSON with one nested object per asset ({ "rate": n }).
    //! This is exactly the MEMBERSHIP set, with no implicit entry to materialise.
    //! Note it is not quite the ACCEPT set: an asset listed at rate 0 is reported
    //! here and still refused when valued, since a 0 rate values every amount at
    //! nothing. Membership is necessary, not sufficient.
    UniValue AcceptancePolicyToJSON();

    //! Replace the whole whitelist from parsed JSON (asset label/hex -> rate).
    bool LoadFromJSON(std::map<std::string, UniValue> json, std::vector<std::string>& error);

    //! Parse JSON (asset label/hex -> rate) into rates without writing the
    //! whitelist, so a caller can validate before it commits to the write.
    static bool ParseJSON(const std::map<std::string, UniValue>& json, std::map<CAsset, CAmount>& rates_out, std::vector<std::string>& errors);

    /** Replace the whole whitelist: the hand write, or the list of the price
     *  server the node is connected to (see FeeListSource). */
    void SetRates(const std::map<CAsset, CAmount>& rates);

    /** Whether at least one asset is accepted, i.e. listed at a positive rate.
     *  False means this node can neither create nor relay any transaction. */
    bool HasAcceptedAsset();

    /** Empty the whitelist. Nothing survives: an empty whitelist accepts NO fee
     *  asset, the policy asset included. Use ResetToBootstrapRates() to get back
     *  to the out-of-box state instead. */
    void ClearRates();
};

/**
 * SEQUENTIA: who writes the fee whitelist.
 *
 * A whitelist is static when it is written by hand (exchangerates.json,
 * setfeeexchangerates, the desktop wallet), and dynamic when the node is
 * connected to a price server, its operator's own or another operator's, and
 * reads the list that server publishes (-feepriceserver, setfeepriceserver).
 * There is no third state. A price server turns what a price feed API reports
 * into a whitelist under its own admission rules and publishes it; it never
 * writes to a node, and nothing ever goes from a price feed API to the
 * whitelist without one. A hand write ends a connection (see feepriceserver.h).
 * The display reference-price feed (-referencepricesurl) never writes the
 * whitelist.
 */
struct FeeListSource {
    //! Connected to a price server, which is then the whitelist's only writer:
    //! the whitelist is dynamic. Not connected: static, written by hand.
    bool connected{false};
    //! The address of the price server's published list (when connected).
    std::string url;
    //! The address came from the chain's default rather than from the operator.
    bool by_default{false};
    //! Where the choice in force was made: "command line", "configuration
    //! file", "settings" (the operator's last setfeepriceserver, hand write or
    //! desktop-wallet choice, recorded in settings.json), "default" (the
    //! network's price server), "whitelist file" (a list written by hand,
    //! which the network's default yields to) or "none".
    std::string decided_by{"none"};
    //! A list from that server is in force (taken this run, or its cache).
    bool have_list{false};
    //! Unix time the list in force was taken from the server; 0 = never.
    int64_t last_written{0};
    //! The server's own "updated" time for the list in force; 0 = not stated.
    int64_t list_published{0};
    //! Unix time of the last request to the server; 0 = none yet.
    int64_t last_attempt{0};
    //! Why the last request did not change the list; empty after a success.
    std::string last_error;
    //! Why the node disconnected, when it did during this run.
    std::string ended;
};

//! A snapshot of who writes the whitelist.
FeeListSource GetFeeListSource();
//! Replace the record of who writes the whitelist.
void SetFeeListSource(const FeeListSource& source);
//! Change it in place, under its lock.
void UpdateFeeListSource(const std::function<void(FeeListSource&)>& fn);

enum class FeeListApply { TAKEN, OLDER, NOT_CONNECTED };

/** Write a price server's list into the whitelist, if and only if the node is
 *  still connected to that same address and the list is not older than the
 *  one in force. Checked and written under one lock, so a list that was in
 *  flight when the operator took over is discarded rather than written over
 *  the operator's list. */
FeeListApply ApplyPriceServerFeeList(const std::string& url, const std::map<CAsset, CAmount>& rates,
                                     int64_t published, int64_t now);

/** Write the whitelist by hand: disconnect from the price server, if
 *  connected, and write rates, under the same lock as ApplyPriceServerFeeList.
 *  @return true if the node was connected. */
bool WriteHandFeeList(const std::map<CAsset, CAmount>& rates, const std::string& why);

/** Disconnect from the price server, recording why.
 *  @return true if the node was connected. */
bool DisconnectFeePriceServer(const std::string& why);

#endif // BITCOIN_EXCHANGERATES_H
