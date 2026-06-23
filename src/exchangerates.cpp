#include <assetsdir.h>
#include <exchangerates.h>
#include <policy/policy.h>
#include <policy/value.h>
#include <util/settings.h>
#include <util/system.h>
#include <util/time.h>
#include <univalue.h>

#include <fstream>

CValue ExchangeRateMap::ConvertAmountToValue(const CAmount& amount, const CAsset& asset) {
    int64_t int64_max = std::numeric_limits<int64_t>::max();
    LOCK(m_write_mutex); // serialize against SetRates's clear()/insert
    auto it = this->find(asset);
    if (it == this->end()) {
        // No explicit rate set by this producer. The policy asset (SEQ) defaults
        // to 1:1 — the convenient out-of-box reference — but this is only a
        // DEFAULT: a producer may set an explicit rate for SEQ (the branch below),
        // which OVERRIDES this, may refuse SEQ (rate 0), or may make a different
        // asset the 1:1 reference (e.g. USDT). SEQ is privileged ONLY for staking
        // eligibility, never for fee acceptance — for fees it is just another
        // asset, valued and accepted at each producer's discretion. Any other
        // unlisted asset is not accepted (value 0).
        if (asset == ::policyAsset) {
            return CValue(amount);
        }
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
        // Policy asset (SEQ) defaults to 1:1 when unlisted; overridable per
        // producer (see ConvertAmountToValue). Other unlisted assets: not accepted.
        if (asset == ::policyAsset) {
            return value.GetValue();
        }
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
            errors.push_back(strprintf("Unknown label and invalid asset hex: %s", rate.first));
            hasError = true;
        } else if (!rate.second.isNum()) {
            errors.push_back(strprintf("Rate for %s is not an integer", rate.first));
            hasError = true;
        } else {
            CAmount newRateValue = rate.second.get_int64();
            // Rates are integers: atoms of the asset equal to one reference
            // unit (exchange_rate_scale). A rate of exactly 0 means "explicitly
            // refuse this asset" (Convert treats scaled_value <= 0 as
            // not-accepted, with no divide-by-zero). This is how a producer
            // declines an asset it would otherwise accept by default — notably
            // the policy asset SEQ, which is accepted 1:1 only as an unlisted
            // default and can be re-priced (any rate) or refused (0) here, like
            // any other asset; SEQ is privileged solely for staking. Negative
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
