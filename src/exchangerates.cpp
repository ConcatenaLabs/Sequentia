#include <assetsdir.h>
#include <exchangerates.h>
#include <policy/policy.h>
#include <policy/value.h>
#include <util/settings.h>
#include <util/strencodings.h> // IsHex
#include <util/system.h>
#include <util/time.h>
#include <univalue.h>

#include <fstream>

CValue ExchangeRateMap::ConvertAmountToValue(const CAmount& amount, const CAsset& asset) {
    int64_t int64_max = std::numeric_limits<int64_t>::max();
    LOCK(m_write_mutex); // serialize against SetRates's clear()/insert
    auto it = this->find(asset);
    if (it == this->end()) {
        // No rate set by this producer, so the asset is not accepted (value 0).
        // This holds for EVERY asset, the policy asset (SEQ) included: the
        // reference unit is an abstract factor and is never itself a token, so
        // there is no asset the node values 1:1 by fiat. SEQ is privileged ONLY
        // for staking eligibility, never for fee acceptance.
        //
        // Nothing is lost by having no special case here: a fresh node still
        // accepts policy-asset fees out of the box because the whitelist is
        // SEEDED with the policy asset at exchange_rate_scale on construction
        // (ResetToBootstrapRates). Reaching this branch for the policy asset
        // means the seed was replaced by a whitelist that leaves it out, which
        // is an operator policy that omits it, and refusing is the honest
        // reading of it (the same policy stated as an explicit rate of 0).
        return CValue(0);
    }
    auto scaled_value = it->second.m_scaled_value;
    // A non-positive rate is never valid (rates are atoms-of-asset per
    // reference unit). Guard regardless of input validation: a negative rate
    // would otherwise saturate to a huge value (fee-check bypass) and a zero
    // rate divides by zero in ConvertValueToAmount. Treat as "not accepted".
    if (scaled_value <= 0) {
        return CValue(0);
    }
    __uint128_t result = ((__uint128_t)amount * (__uint128_t)scaled_value) / (__uint128_t)exchange_rate_scale;
    if (result > static_cast<uint64_t>(int64_max)) {
        return CValue(int64_max);
    } else {
        return CValue((int64_t) result);
    }
}

CAmount ExchangeRateMap::ConvertValueToAmount(const CValue& value, const CAsset& asset) {
    int64_t int64_max = std::numeric_limits<int64_t>::max();
    LOCK(m_write_mutex); // serialize against SetRates's clear()/insert
    auto it = this->find(asset);
    if (it == this->end()) {
        // Unlisted means not accepted, whichever asset it is (see
        // ConvertAmountToValue). Keeping this in step with that function matters:
        // if one direction refused an unlisted asset while the other still quoted
        // an amount for it, the wallet would compute a fee the mempool then
        // valued at 0 and rejected.
        return 0;
    }
    auto scaled_value = it->second.m_scaled_value;
    if (scaled_value <= 0) { // see ConvertAmountToValue; also avoids divide-by-zero
        return 0;
    }
    __uint128_t result = ((__uint128_t)value.GetValue() * (__uint128_t)exchange_rate_scale + (__uint128_t)scaled_value - 1) / (__uint128_t)scaled_value;
    if (result > static_cast<__uint128_t>(int64_max)) {
        return int64_max;
    } else {
        return (int64_t) result;
    }
}

void ExchangeRateMap::ResetToBootstrapRates() {
    LOCK(m_write_mutex);
    this->clear();
    (*this)[::policyAsset] = CAssetExchangeRate(exchange_rate_scale);
}

void ExchangeRateMap::SetRates(const std::map<CAsset, CAmount>& rates) {
    LOCK(m_write_mutex);
    this->clear();
    for (const auto& rate : rates) {
        (*this)[rate.first] = CAssetExchangeRate(rate.second);
    }
}

void ExchangeRateMap::ClearRates() {
    LOCK(m_write_mutex);
    this->clear();
}

bool ExchangeRateMap::LoadFromDefaultJSONFile(std::vector<std::string>& errors) {
    fs::path file_path = AbsPathForConfigVal(fs::PathFromString(exchange_rates_config_file));
    if (fs::exists(file_path)) {
        return LoadFromJSONFile(file_path, errors);
    } else {
        return true;
    }
}

bool ExchangeRateMap::LoadFromJSONFile(fs::path file_path, std::vector<std::string>& errors) {
    std::map <std::string, UniValue> json;
    if (!util::ReadSettings(file_path, json, errors)) {
        return false;
    }
    return this->LoadFromJSON(json, errors);
}

bool ExchangeRateMap::SaveToJSONFile(std::vector<std::string>& errors) {
    UniValue json = this->ToJSON();
    std::map<std::string, util::SettingsValue> settings;
    json.getObjMap(settings);
    fs::path file_path = AbsPathForConfigVal(fs::PathFromString(exchange_rates_config_file));
    return util::WriteSettings(file_path, settings, errors);
}

static UniValue RatesToJSON(const std::map<CAsset, CAssetExchangeRate>& rates) {
    UniValue json = UniValue{UniValue::VOBJ};
    for (const auto& rate : rates) {
        std::string label = gAssetsDir.GetLabel(rate.first);
        if (label == "") {
            label = rate.first.GetHex();
        }
        json.pushKV(label, rate.second.m_scaled_value);
    }
    return json;
}

UniValue ExchangeRateMap::ToJSON() {
    LOCK(m_write_mutex);
    return RatesToJSON(*this);
}

UniValue ExchangeRateMap::AcceptancePolicyToJSON() {
    LOCK(m_write_mutex);
    UniValue json = UniValue{UniValue::VOBJ};
    for (const auto& rate : *this) {
        std::string label = gAssetsDir.GetLabel(rate.first);
        if (label == "") {
            label = rate.first.GetHex();
        }
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("rate", rate.second.m_scaled_value);
        json.pushKV(label, entry);
    }
    return json;
}

bool ExchangeRateMap::LoadFromJSON(std::map<std::string, UniValue> json, std::vector<std::string>& errors) {
    bool hasError = false;
    std::map<CAsset, CAmount> parsedRates;
    for (auto rate : json) {
        CAsset asset = GetAssetFromString(rate.first);
        if (asset.IsNull()) {
            // SEQUENTIA: GetAssetFromString() collapses several different
            // failures into one null return, and reporting them all as "unknown
            // label and invalid hex" sends the reader looking for a typo that is
            // not there. Say which of them actually happened.
            if (gAssetsDir.HasLabel(rate.first)) {
                // The directory knows the name but it resolves to the null asset.
                // Set() and Merge() both refuse to create such a binding now, so
                // this should not arise; it is reported distinctly anyway because
                // blaming the operator's spelling for a directory defect is
                // exactly the wrong answer that cost time here.
                errors.push_back(strprintf("Label %s is registered but names the null asset, so no rate can be set for it", rate.first));
            } else if (rate.first.size() == 64 && IsHex(rate.first)) {
                errors.push_back(strprintf("Asset id %s is the null asset, which no rate can be set for", rate.first));
            } else {
                errors.push_back(strprintf("Unknown label and invalid asset hex: %s", rate.first));
            }
            hasError = true;
        } else if (!rate.second.isNum()) {
            errors.push_back(strprintf("Rate for %s is not an integer", rate.first));
            hasError = true;
        } else {
            CAmount newRateValue = rate.second.get_int64();
            // Rates are integers: atoms of the asset equal to one reference
            // unit (exchange_rate_scale). A rate of exactly 0 means "explicitly
            // refuse this asset" (Convert treats scaled_value <= 0 as
            // not-accepted, with no divide-by-zero), which is the same outcome
            // as leaving the asset out of the set entirely; listing it at 0 just
            // says so on the record. The policy asset SEQ is no different: it
            // can be re-priced (any rate), refused (0) or omitted here like any
            // other asset, and it is privileged solely for staking. Negative
            // rates are invalid (they would saturate to a bogus huge valuation
            // that bypasses fee checks).
            if (newRateValue < 0) {
                errors.push_back(strprintf("Rate for %s must be a non-negative integer (0 = refuse the asset)", rate.first));
                hasError = true;
            } else {
                parsedRates[asset] = newRateValue;
            }
        }
    }
    if (hasError) return false;
    SetRates(parsedRates);
    return true;
}
