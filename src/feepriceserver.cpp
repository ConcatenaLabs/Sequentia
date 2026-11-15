// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <feepriceserver.h>

#include <asset.h>
#include <assetsdir.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <exchangerates.h>
#include <fs.h>
#include <logging.h>
#include <plainhttp.h>
#include <policy/policy.h>
#include <scheduler.h>
#include <sync.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/settings.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/system.h>
#include <util/time.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>

const char* const FEE_PRICE_SERVER_CACHE_FILE = "feepriceserver.json";

namespace {

//! The testnet's price server: the list it pushes to the public nodes,
//! published as a file. Plain HTTP like the testnet's other defaults (the
//! registry, the reference prices), because the node links no TLS library and
//! follows no redirects.
const char* const TESTNET_FEE_PRICE_SERVER = "http://sequentiatestnet.com/feewhitelist";

bool g_fee_price_server_by_default{false};

Mutex g_request_mutex;
CTxMemPool* g_mempool{nullptr};

bool IsCustomChain(const std::string& chain)
{
    for (const std::string* known : {&CBaseChainParams::MAIN, &CBaseChainParams::SEQUENTIA,
                                     &CBaseChainParams::TESTNET, &CBaseChainParams::SIGNET,
                                     &CBaseChainParams::REGTEST, &CBaseChainParams::LIQUID1,
                                     &CBaseChainParams::LIQUID1TEST, &CBaseChainParams::LIQUIDTESTNET}) {
        if (chain == *known) return false;
    }
    return true;
}

fs::path CachePath() { return AbsPathForConfigVal(fs::PathFromString(FEE_PRICE_SERVER_CACHE_FILE)); }
fs::path RatesFilePath() { return AbsPathForConfigVal(fs::PathFromString(exchange_rates_config_file)); }

bool ReadFile(const fs::path& path, std::string& out)
{
    std::ifstream f{path};
    if (!f.good()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/** A file holding the policy asset at the reference rate and nothing else is
 *  what every earlier start of a seeding node saved on its own (it saved the
 *  whitelist at every start). It records no choice anybody made. */
bool IsOldSeedFile(const fs::path& path)
{
    std::map<std::string, util::SettingsValue> json;
    std::vector<std::string> errors;
    if (!util::ReadSettings(path, json, errors) || json.size() != 1) return false;
    const auto& [key, value] = *json.begin();
    return GetAssetFromString(key) == ::policyAsset && value.isNum() &&
           value.getValStr() == ToString(exchange_rate_scale);
}

void RecordChoice(const std::string& url)
{
    gArgs.LockSettings([&](util::Settings& settings) {
        settings.rw_settings["feepriceserver"] = url.empty() ? util::SettingsValue(false) : util::SettingsValue(url);
    });
    std::vector<std::string> errors;
    if (!gArgs.WriteSettingsFile(&errors)) {
        LogPrintf("FeePriceServer: could not record the choice in the settings file (%s); "
                  "it holds until the node stops\n", MakeUnorderedList(errors));
    }
}

void WriteCache(const std::string& url, const std::map<CAsset, CAmount>& rates, int64_t published, int64_t written)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("url", url);
    obj.pushKV("written", written);
    obj.pushKV("updated", published);
    UniValue r(UniValue::VOBJ);
    for (const auto& [asset, rate] : rates) r.pushKV(asset.GetHex(), rate);
    obj.pushKV("rates", r);
    const fs::path path = CachePath();
    const fs::path tmp = fs::PathFromString(fs::PathToString(path) + ".new");
    {
        std::ofstream f{tmp, std::ios::out | std::ios::trunc};
        f << obj.write(1) << "\n";
        if (!f.good()) { LogPrintf("FeePriceServer: could not write %s\n", fs::PathToString(tmp)); return; }
    }
    if (!RenameOver(tmp, path)) LogPrintf("FeePriceServer: could not replace %s\n", fs::PathToString(path));
}

std::string Describe(const std::map<CAsset, CAmount>& rates)
{
    std::vector<std::string> names;
    for (const auto& kv : rates) {
        const std::string label = gAssetsDir.GetLabel(kv.first);
        names.push_back(label.empty() ? kv.first.GetHex().substr(0, 16) : label);
    }
    return Join(names, ", ");
}

/** Ask url for its list. @return true with rates set, or false with error. */
bool Request(const std::string& url, int timeout_secs, std::map<CAsset, CAmount>& rates,
             int64_t& published, std::string& error)
{
    std::string body;
    return PlainHttpGet(url, timeout_secs, MAX_FEE_LIST_BODY, body, error) &&
           ParsePriceServerFeeList(body, rates, published, error);
}

} // namespace

std::string DefaultFeePriceServer(const ArgsManager& args)
{
    const std::string chain = args.GetChainName();
    if (chain == CBaseChainParams::TESTNET) return TESTNET_FEE_PRICE_SERVER;
    if (IsCustomChain(chain)) return args.GetArg("-con_default_fee_price_server", "");
    return "";
}

void NoteFeePriceServerIsDefault(bool by_default) { g_fee_price_server_by_default = by_default; }

bool ParsePriceServerFeeList(const std::string& body, std::map<CAsset, CAmount>& rates,
                             int64_t& published, std::string& error)
{
    rates.clear();
    published = 0;
    UniValue doc;
    if (!doc.read(body) || !doc.isObject()) { error = "the answer is not a JSON object"; return false; }
    // A price server publishes its list as {"updated": <time>, "rates": {...}}.
    // Anything else, a price feed API's prices or the registry's index above
    // all, is not a whitelist and must never become one: a price server is
    // what applies admission rules to a feed, and skipping it is not possible.
    const UniValue& r = doc["rates"];
    const UniValue& updated = doc["updated"];
    if (!r.isObject() || !updated.isNum()) {
        error = "the address is not a price server: its answer is not a published fee list "
                "(\"updated\" and \"rates\"), and a price feed API's answer is never taken as a whitelist";
        return false;
    }
    if (r.empty()) { error = "the list is empty (a price server never publishes an empty list)"; return false; }
    if (r.size() > MAX_FEE_LIST_ENTRIES) {
        error = strprintf("the list has %u entries, more than the %u accepted", r.size(), MAX_FEE_LIST_ENTRIES);
        return false;
    }
    const std::vector<std::string>& keys = r.getKeys();
    const std::vector<UniValue>& values = r.getValues();
    for (size_t i = 0; i < keys.size(); ++i) {
        // Ids only, never labels: a label resolves through the asset directory,
        // which is itself filled from an unauthenticated feed.
        if (keys[i].size() != 64 || !IsHex(keys[i])) {
            error = strprintf("entry %s is not a 64-hex asset id", keys[i].substr(0, 70));
            return false;
        }
        const CAsset asset(uint256S(keys[i]));
        if (asset.IsNull()) { error = "the list names the null asset"; return false; }
        int64_t rate{0};
        if (!values[i].isNum() || !ParseInt64(values[i].getValStr(), &rate) || rate <= 0 || rate > MAX_FEE_LIST_RATE) {
            error = strprintf("the rate for %s is not an integer in (0, %d]", keys[i], MAX_FEE_LIST_RATE);
            return false;
        }
        rates[asset] = rate;
    }
    published = static_cast<int64_t>(updated.get_real());
    return true;
}

bool InitFeeList(std::string& error)
{
    ExchangeRateMap& map = ExchangeRateMap::GetInstance();
    // On a chain that does not seed the whitelist (the mainnet and the testnet)
    // nothing is accepted until it is written. Cleared before anything is
    // loaded, so a list that is written is kept.
    if (!Params().SeedFeeWhitelist()) map.ClearRates();

    const std::string initial_file = gArgs.GetArg("-initialexchangeratesjsonfile", "");
    const fs::path rates_file = RatesFilePath();

    // A chain that once seeded the whitelist and no longer does (the testnet)
    // has data directories whose exchangerates.json holds nothing but that
    // seed: the node saved its whitelist at every start, so the seed was
    // written there without anyone choosing it. Loading it would keep the
    // one-asset list the open fee market withholds, so it is moved aside. The
    // mainnet never seeded, so a file there is always its operator's.
    //
    // Checked once per data directory, and the check recorded in
    // settings.json: from then on the file holds only what is written to it,
    // so an operator who lists the policy asset alone at the reference rate
    // keeps that list, which would otherwise read as the seed at every start.
    if (!Params().SeedFeeWhitelist() && Params().NetworkIDString() != CBaseChainParams::SEQUENTIA &&
        !gArgs.GetBoolArg("-feelistseedchecked", false)) {
        if (initial_file.empty() && fs::exists(rates_file) && IsOldSeedFile(rates_file)) {
            const fs::path aside = fs::PathFromString(fs::PathToString(rates_file) + ".old-seed");
            if (!RenameOver(rates_file, aside)) {
                error = strprintf("Unable to move the old seeded fee list %s aside", fs::PathToString(rates_file));
                return false;
            }
            LogPrintf("%s held only the old seeded fee list (the policy asset alone, which nobody chose); "
                      "moved it to %s rather than loading it\n", fs::PathToString(rates_file), fs::PathToString(aside));
        }
        gArgs.LockSettings([](util::Settings& settings) { settings.rw_settings["feelistseedchecked"] = true; });
        std::vector<std::string> errors;
        if (!gArgs.WriteSettingsFile(&errors)) {
            LogPrintf("Could not record the fee whitelist's seed check in the settings file (%s)\n", MakeUnorderedList(errors));
        }
    }

    std::string url = gArgs.IsArgNegated("-feepriceserver") ? "" : gArgs.GetArg("-feepriceserver", "");
    // The chain's default yields to a list the operator wrote; an address the
    // operator chose does not, since that choice is the newer one.
    if (!url.empty() && g_fee_price_server_by_default && (!initial_file.empty() || fs::exists(rates_file))) {
        LogPrintf("FeePriceServer: this node keeps the static fee whitelist its operator wrote (%s) and is not "
                  "connected to the price server at %s; setfeepriceserver connects it\n",
                  initial_file.empty() ? fs::PathToString(rates_file) : initial_file, url);
        url.clear();
    }

    if (url.empty()) {
        SetFeeListSource(FeeListSource{});
        std::vector<std::string> errors;
        if (!initial_file.empty()) {
            const fs::path file_path = GetConfigFile(initial_file);
            if (!map.LoadFromJSONFile(file_path, errors)) {
                error = strprintf("Unable to load exchange rates from JSON file %s: \n%s\n", initial_file, MakeUnorderedList(errors));
                return false;
            }
        } else if (!map.LoadFromDefaultJSONFile(errors)) {
            error = strprintf("Unable to load exchange rates from default JSON file %s: \n%s\n", exchange_rates_config_file, MakeUnorderedList(errors));
            return false;
        }
        errors.clear();
        if (!map.SaveToJSONFile(errors)) {
            error = strprintf("Unable to save exchange rates to JSON file %s: \n%s\n", exchange_rates_config_file, MakeUnorderedList(errors));
            return false;
        }
        return true;
    }

    if (url.rfind("http://", 0) != 0) {
        error = strprintf("-feepriceserver=%s: only http:// addresses are supported", url);
        return false;
    }

    // Connected: the price server is the only writer, so the operator's file is
    // neither loaded nor rewritten.
    FeeListSource source;
    source.connected = true;
    source.url = url;
    source.by_default = g_fee_price_server_by_default;
    SetFeeListSource(source);
    const std::string whose = source.by_default ? " (the network's price server)" : "";

    std::string cached, why;
    std::map<CAsset, CAmount> rates;
    int64_t published{0};
    UniValue doc;
    if (ReadFile(CachePath(), cached) && doc.read(cached) && doc.isObject() &&
        doc["url"].isStr() && doc["url"].get_str() == url && doc["written"].isNum() &&
        ParsePriceServerFeeList(cached, rates, published, why)) {
        const int64_t written = doc["written"].get_int64();
        ApplyPriceServerFeeList(url, rates, published, written);
        LogPrintf("FeePriceServer: connected to %s%s; its last list (%u assets, taken %s) is in force\n",
                  url, whose, rates.size(), FormatISO8601DateTime(written));
    } else {
        LogPrintf("FeePriceServer: connected to %s%s\n", url, whose);
    }
    // Asked now, before the wallet can be asked to send, so a reachable server's
    // list is in force from the first moment: the wait is the request itself.
    RefreshFeePriceServer(std::min<int64_t>(FEE_LIST_STARTUP_TIMEOUT_SECS, gArgs.GetIntArg("-feepriceservertimeout", 15)));
    return true;
}

bool RefreshFeePriceServer(int timeout_secs)
{
    LOCK(g_request_mutex);
    const FeeListSource src = GetFeeListSource();
    if (!src.connected) return false;
    const int64_t now = GetTime();
    UpdateFeeListSource([&](FeeListSource& s) { if (s.url == src.url) s.last_attempt = now; });

    std::map<CAsset, CAmount> rates;
    int64_t published{0};
    std::string err;
    bool ok = Request(src.url, timeout_secs, rates, published, err);
    if (ok && src.have_list && published < src.list_published) {
        ok = false;
        err = strprintf("the list is older (%s) than the one in force (%s)",
                        FormatISO8601DateTime(published), FormatISO8601DateTime(src.list_published));
    }
    if (!ok) {
        bool changed{false};
        UpdateFeeListSource([&](FeeListSource& s) {
            if (s.url != src.url) return;
            changed = s.last_error != err;
            s.last_error = err;
        });
        if (changed) {
            LogPrintf("FeePriceServer: %s: %s; %s\n", src.url, err,
                      src.have_list ? "the list in force is kept" : "no list from it yet, so no fee asset is accepted");
        }
        return false;
    }

    const std::map<CAsset, CAmount> before = ExchangeRateMap::GetInstance().GetRates();
    if (!ApplyPriceServerFeeList(src.url, rates, published, now)) {
        LogPrintf("FeePriceServer: a list from %s arrived after this node disconnected from it; discarded\n", src.url);
        return false;
    }
    if (g_mempool) g_mempool->RecomputeFees();
    WriteCache(src.url, rates, published, now);

    bool same_assets = before.size() == rates.size();
    for (auto a = before.cbegin(), b = rates.cbegin(); same_assets && a != before.cend(); ++a, ++b) {
        same_assets = a->first == b->first;
    }
    if (!src.have_list || !src.last_error.empty() || !same_assets) {
        LogPrintf("FeePriceServer: took the list from %s: %u assets (%s)\n", src.url, rates.size(), Describe(rates));
    }
    return true;
}

void StartFeePriceServer(CScheduler& scheduler, CTxMemPool* mempool)
{
    g_mempool = mempool;
    const int poll = std::max<int64_t>(1, gArgs.GetIntArg("-feepriceserverpoll", 60));
    const int timeout = gArgs.GetIntArg("-feepriceservertimeout", 15);
    // One self-rescheduling task: quick retries until the first list is in
    // force, the poll interval after that, and idle while not connected.
    static std::function<void()> tick;
    tick = [&scheduler, poll, timeout] {
        int next = poll;
        if (GetFeeListSource().connected) {
            RefreshFeePriceServer(timeout);
            if (!GetFeeListSource().have_list) next = std::min(poll, FEE_LIST_RETRY_SECS);
        }
        scheduler.scheduleFromNow([] { tick(); }, std::chrono::seconds{next});
    };
    const int first = GetFeeListSource().have_list ? poll : std::min(poll, FEE_LIST_RETRY_SECS);
    scheduler.scheduleFromNow([] { tick(); }, std::chrono::seconds{first});
}

void NoteOperatorFeeListWrite(const std::string& how)
{
    const FeeListSource src = GetFeeListSource();
    if (!DisconnectFeePriceServer(strprintf("the fee whitelist was written by %s", how))) return;
    RecordChoice("");
    LogPrintf("FeePriceServer: the fee whitelist was written by %s, so this node is no longer connected to the "
              "price server at %s. From now on the whitelist is what is written to it; setfeepriceserver "
              "connects it again\n", how, src.url);
}

bool SetFeePriceServer(const std::string& url, std::string& error)
{
    if (url.empty()) {
        const FeeListSource src = GetFeeListSource();
        if (DisconnectFeePriceServer("disconnected with setfeepriceserver")) {
            LogPrintf("FeePriceServer: disconnected from %s; the list in force is kept as a static whitelist\n", src.url);
        }
        RecordChoice("");
        // The list in force becomes the operator's static whitelist, kept
        // across a restart like any list written by hand.
        std::vector<std::string> errors;
        if (!ExchangeRateMap::GetInstance().SaveToJSONFile(errors)) {
            error = strprintf("Disconnected, but could not save the whitelist to %s: %s", exchange_rates_config_file, MakeUnorderedList(errors));
            return false;
        }
        return true;
    }
    if (url.rfind("http://", 0) != 0) {
        error = "only http:// addresses are supported";
        return false;
    }
    // Ask first and change nothing unless the answer is a price server's list,
    // so pointing the node at a price feed API, or anything else, writes nothing.
    LOCK(g_request_mutex);
    std::map<CAsset, CAmount> rates;
    int64_t published{0};
    std::string err;
    if (!Request(url, gArgs.GetIntArg("-feepriceservertimeout", 15), rates, published, err)) {
        error = strprintf("Not connected to %s: %s", url, err);
        return false;
    }
    const int64_t now = GetTime();
    UpdateFeeListSource([&](FeeListSource& s) {
        s = FeeListSource{};
        s.connected = true;
        s.url = url;
        s.last_attempt = now;
    });
    ApplyPriceServerFeeList(url, rates, published, now);
    if (g_mempool) g_mempool->RecomputeFees();
    WriteCache(url, rates, published, now);
    RecordChoice(url);
    LogPrintf("FeePriceServer: connected to %s with setfeepriceserver; took its list: %u assets (%s)\n",
              url, rates.size(), Describe(rates));
    return true;
}
