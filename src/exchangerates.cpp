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
    // The policy asset is the reference unit by definition (exchange_rate_scale
    // atoms == 1 reference unit), so it is always valued 1:1 and always
    // payable — independent of, and not overridable by, the rate map.
    if (asset == ::policyAsset) {
        return CValue(amount);
    }
    LOCK(m_write_mutex); // serialize against RebuildEffective's clear()/insert
    auto it = this->find(asset);
    if (it == this->end()) {
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
    // The policy asset is the reference unit, always 1:1 (see ConvertAmountToValue).
    if (asset == ::policyAsset) {
        return value.GetValue();
    }
    LOCK(m_write_mutex); // serialize against RebuildEffective's clear()/insert
    auto it = this->find(asset);
    if (it == this->end()) {
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

void ExchangeRateMap::RebuildEffective() {
    this->clear();
    int64_t now = GetTime();
    for (const auto& rate : m_dynamic_rates) {
        if (m_dynamic_max_age > 0 && now - rate.second.m_updated_at > m_dynamic_max_age) {
            continue; // stale: fail safe to "not accepted"
        }
        (*this)[rate.first] = CAssetExchangeRate(rate.second.m_scaled_value);
    }
    // Static entries take precedence over dynamic ones.
    for (const auto& rate : m_static_rates) {
        (*this)[rate.first] = rate.second;
    }
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
    UniValue json = this->StaticToJSON();
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

UniValue ExchangeRateMap::StaticToJSON() {
    LOCK(m_write_mutex);
    return RatesToJSON(m_static_rates);
}

UniValue ExchangeRateMap::DynamicToJSON() {
    LOCK(m_write_mutex);
    UniValue json = UniValue{UniValue::VOBJ};
    int64_t now = GetTime();
    for (const auto& rate : m_dynamic_rates) {
        std::string label = gAssetsDir.GetLabel(rate.first);
        if (label == "") {
            label = rate.first.GetHex();
        }
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("rate", rate.second.m_scaled_value);
        entry.pushKV("source", rate.second.m_source);
        entry.pushKV("updated_at", rate.second.m_updated_at);
        entry.pushKV("age", now - rate.second.m_updated_at);
        bool stale = m_dynamic_max_age > 0 && now - rate.second.m_updated_at > m_dynamic_max_age;
        entry.pushKV("stale", stale);
        json.pushKV(label, entry);
    }
    return json;
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
        bool is_static = m_static_rates.count(rate.first) > 0;
        entry.pushKV("origin", is_static ? "static" : "dynamic");
        if (!is_static) {
            auto it = m_dynamic_rates.find(rate.first);
            if (it != m_dynamic_rates.end()) {
                entry.pushKV("source", it->second.m_source);
                entry.pushKV("updated_at", it->second.m_updated_at);
            }
        }
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
            // unit (exchange_rate_scale). Non-positive rates are invalid — a
            // zero divides by zero and a negative saturates to a bogus huge
            // valuation that bypasses fee checks.
            if (newRateValue <= 0) {
                errors.push_back(strprintf("Rate for %s must be a positive integer", rate.first));
                hasError = true;
            } else {
                parsedRates[asset] = newRateValue;
            }
        }
    }
    if (hasError) return false;
    LOCK(m_write_mutex);
    m_static_rates.clear();
    for (auto rate : parsedRates) {
        m_static_rates[rate.first] = rate.second;
    }
    RebuildEffective();
    return true;
}

void ExchangeRateMap::SetDynamicRates(const std::map<CAsset, CAmount>& rates, const std::string& source, int64_t now) {
    LOCK(m_write_mutex);
    m_dynamic_rates.clear();
    for (const auto& rate : rates) {
        m_dynamic_rates[rate.first] = CDynamicExchangeRate(rate.second, source, now);
    }
    RebuildEffective();
}

void ExchangeRateMap::ClearDynamicRates() {
    LOCK(m_write_mutex);
    m_dynamic_rates.clear();
    RebuildEffective();
}

bool ExchangeRateMap::PurgeStaleDynamicRates(int64_t now) {
    if (m_dynamic_max_age <= 0) return false;
    LOCK(m_write_mutex);
    bool changed = false;
    for (auto it = m_dynamic_rates.begin(); it != m_dynamic_rates.end();) {
        if (now - it->second.m_updated_at > m_dynamic_max_age) {
            it = m_dynamic_rates.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        RebuildEffective();
    }
    return changed;
}
