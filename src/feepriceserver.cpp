// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <feepriceserver.h>

#include <asset.h>
#include <assetsdir.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <crypto/sha256.h>
#include <exchangerates.h>
#include <fs.h>
#include <logging.h>
#include <plainhttp.h>
#include <policy/policy.h>
#include <scheduler.h>
#include <shutdown.h>
#include <sync.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/settings.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/system.h>
#include <util/time.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>

const char* const FEE_PRICE_SERVER_CACHE_FILE = "feepriceserver.json";

namespace {

//! The testnet's price server: the list it publishes, served as a file. Plain
//! HTTP like the testnet's other defaults (the registry, the reference
//! prices), because the node links no TLS library and follows no redirects.
const char* const TESTNET_FEE_PRICE_SERVER = "http://sequentiatestnet.com/feewhitelist";

//! settings.json's records (hidden arguments, see init.cpp): the
//! configuration file's -feepriceserver when the operator last chose, and the
//! fingerprint of the list this node last wrote by hand.
const char* const SETTING_CHOICE = "feepriceserver";
const char* const SETTING_CHOICE_CONF = "feepriceserverconf";
const char* const SETTING_HAND_LIST = "feelisthand";

//! Serialises the operator's writes (setfeepriceserver, a hand write) with
//! each other, so of two the later is the one in force and in settings.json.
//! The poll never takes it, so an operator never waits on the poll's request.
Mutex g_operator_mutex;
bool g_stopped GUARDED_BY(g_operator_mutex){false};
std::atomic<CTxMemPool*> g_mempool{nullptr};
//! Serialises writes of the cache file.
Mutex g_cache_mutex;

bool Interrupted() { return ShutdownRequested(); }

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

std::string FormatTime(int64_t t) { return t > 0 ? FormatISO8601DateTime(t) : std::string{"a time not stated"}; }

/** One setting as written: "" unset, "!" negated (-nofeepriceserver), or the
 *  value itself. */
std::string SettingText(const util::SettingsValue& v)
{
    if (v.isNull()) return "";
    if (v.isFalse()) return "!";
    if (v.isStr()) return v.get_str();
    return v.write();
}

/** -feepriceserver as the command line alone, or the configuration file alone, sets it. */
util::SettingsValue SettingFrom(bool command_line)
{
    const std::string chain = gArgs.GetChainName();
    util::SettingsValue v;
    gArgs.LockSettings([&](util::Settings& s) {
        util::Settings only;
        if (command_line) only.command_line_options = s.command_line_options;
        else only.ro_config = s.ro_config;
        v = util::GetSetting(only, chain, SETTING_CHOICE, /*ignore_default_section_config=*/false, /*get_chain_name=*/false);
    });
    return v;
}

util::SettingsValue RwSetting(const std::string& name)
{
    util::SettingsValue v;
    gArgs.LockSettings([&](util::Settings& s) {
        auto it = s.rw_settings.find(name);
        if (it != s.rw_settings.end()) v = it->second;
    });
    return v;
}

void WriteRwSettings(const std::function<void(std::map<std::string, util::SettingsValue>&)>& fn)
{
    gArgs.LockSettings([&](util::Settings& s) { fn(s.rw_settings); });
    std::vector<std::string> errors;
    if (!gArgs.WriteSettingsFile(&errors)) {
        LogPrintf("FeePriceServer: could not record the choice in the settings file (%s); it holds until the node stops\n", MakeUnorderedList(errors));
    }
}

/** Record the operator's choice ("" = static) beside the configuration file's
 *  setting at this moment, so a later edit of that file reads as the newer
 *  choice. */
void RecordChoice(const std::string& url)
{
    const std::string conf = SettingText(SettingFrom(/*command_line=*/false));
    WriteRwSettings([&](std::map<std::string, util::SettingsValue>& rw) {
        rw[SETTING_CHOICE] = url.empty() ? util::SettingsValue(false) : util::SettingsValue(url);
        rw[SETTING_CHOICE_CONF] = conf;
    });
}

std::string Fingerprint(const std::map<CAsset, CAmount>& rates)
{
    std::string text;
    for (const auto& [asset, rate] : rates) text += strprintf("%s=%d;", asset.GetHex(), rate);
    uint8_t hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(reinterpret_cast<const uint8_t*>(text.data()), text.size()).Finalize(hash);
    return HexStr(hash);
}

/** Record that this node wrote exactly this list by hand. */
void RecordHandList(const std::map<CAsset, CAmount>& rates)
{
    const std::string fp = Fingerprint(rates);
    WriteRwSettings([&](std::map<std::string, util::SettingsValue>& rw) { rw[SETTING_HAND_LIST] = fp; });
}

/** Whether exchangerates.json holds a list someone chose (see feepriceserver.h). */
bool RatesFileIsChosen(const fs::path& path, std::string& what)
{
    what.clear();
    if (!fs::exists(path)) return false;
    // A chain that seeds the whitelist (custom chains) loads its file as it
    // always did, and the mainnet never seeded, so a file there is always its
    // operator's.
    if (Params().SeedFeeWhitelist() || Params().NetworkIDString() == CBaseChainParams::SEQUENTIA) return true;
    std::map<std::string, util::SettingsValue> json;
    std::vector<std::string> errors;
    std::map<CAsset, CAmount> rates;
    // Unreadable: chosen, so loading it reports what is wrong with it.
    if (!util::ReadSettings(path, json, errors)) return true;
    const std::map<std::string, UniValue> entries(json.begin(), json.end());
    if (!ExchangeRateMap::ParseJSON(entries, rates, errors)) return true;
    const bool seed_only = rates.size() == 1 && rates.begin()->first == ::policyAsset &&
                           rates.begin()->second == exchange_rate_scale;
    if (!rates.empty() && !seed_only) return true;
    what = rates.empty() ? "an empty list" : "the policy asset alone at the reference rate, the seed older binaries saved on their own";
    const util::SettingsValue recorded = RwSetting(SETTING_HAND_LIST);
    return recorded.isStr() && recorded.get_str() == Fingerprint(rates);
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
    LOCK(g_cache_mutex);
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
    // Whatever a price server sends, the node must not stop: nothing thrown
    // while asking or reading leaves this function, and such an answer is
    // refused like any other.
    try {
        std::string body;
        return PlainHttpGet(url, timeout_secs, MAX_FEE_LIST_BODY, body, error, Interrupted) &&
               ParsePriceServerFeeList(body, GetTime(), rates, published, error);
    } catch (const std::exception& e) {
        error = strprintf("the answer could not be read (%s)", e.what());
    } catch (...) {
        error = "the answer could not be read";
    }
    rates.clear();
    return false;
}

void RecomputeMempoolFees()
{
    if (CTxMemPool* mempool = g_mempool.load()) mempool->RecomputeFees();
}

/** Who decides, and what (see feepriceserver.h). */
struct Choice {
    std::string url; // empty: static
    std::string decided_by{"none"};
    bool by_default{false};
};

Choice Decide(const std::string& initial_file, const fs::path& rates_file)
{
    const auto as_url = [](const util::SettingsValue& v) {
        return v.isStr() ? v.get_str() : std::string{};
    };
    Choice c;
    const util::SettingsValue cmd = SettingFrom(/*command_line=*/true);
    const util::SettingsValue conf = SettingFrom(/*command_line=*/false);
    const util::SettingsValue rw = RwSetting(SETTING_CHOICE);
    const util::SettingsValue rw_conf = RwSetting(SETTING_CHOICE_CONF);
    if (!cmd.isNull()) {
        c.url = as_url(cmd);
        c.decided_by = "command line";
        return c;
    }
    if (!rw.isNull()) {
        // The operator's last choice through RPC or the desktop wallet, unless
        // the configuration file has been edited since it was made, which is
        // then the newer choice.
        if (rw_conf.isStr() && rw_conf.get_str() == SettingText(conf)) {
            c.url = as_url(rw);
            c.decided_by = "settings";
            return c;
        }
        LogPrintf("FeePriceServer: the configuration file's feepriceserver setting changed after the choice settings.json records, so the configuration file's is in force\n");
    }
    if (!conf.isNull()) {
        c.url = as_url(conf);
        c.decided_by = "configuration file";
        return c;
    }
    const std::string network = DefaultFeePriceServer(gArgs);
    if (network.empty()) return c;
    // The network's default yields to a list someone wrote by hand.
    std::string what;
    if (!initial_file.empty() || RatesFileIsChosen(rates_file, what)) {
        c.decided_by = "whitelist file";
        LogPrintf("FeePriceServer: this node keeps the static fee whitelist written by hand (%s) and is not connected to the network's price server at %s; setfeepriceserver connects it\n",
                  initial_file.empty() ? fs::PathToString(rates_file) : initial_file, network);
        return c;
    }
    c.url = network;
    c.decided_by = "default";
    c.by_default = true;
    return c;
}

} // namespace

std::string DefaultFeePriceServer(const ArgsManager& args)
{
    const std::string chain = args.GetChainName();
    if (chain == CBaseChainParams::TESTNET) return TESTNET_FEE_PRICE_SERVER;
    if (IsCustomChain(chain)) return args.GetArg("-con_default_fee_price_server", "");
    return "";
}

bool ParsePriceServerFeeList(const std::string& body, int64_t now, std::map<CAsset, CAmount>& rates,
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
    if (r.isObject() && updated.isNull() && doc.exists("updated")) {
        // A price server whose first poll has not completed: a price server,
        // with nothing to publish yet.
        error = "the price server has not published a list yet";
        return false;
    }
    if (!r.isObject() || !updated.isNum()) {
        error = "the address is not a price server: its answer is not a published fee list "
                "(\"updated\" and \"rates\"), and a price feed API's answer is never taken as a whitelist";
        return false;
    }
    // Bounded before anything else is read: a list dated in the future would
    // make every honest list after it read as older, and so freeze the
    // whitelist until the operator reconnected, across restarts by its cache.
    // get_real() throws on a number out of a double's range (1e9999): read
    // here, so such an answer is refused like any other bad answer.
    double when{0};
    try {
        when = updated.get_real();
    } catch (const std::exception&) {
        when = std::nan("");
    }
    if (!std::isfinite(when) || when < 0) {
        error = strprintf("the list's time (\"updated\": %s) is not a time", updated.getValStr().substr(0, 40));
        return false;
    }
    if (when > static_cast<double>(now + MAX_FEE_LIST_FUTURE_SECS)) {
        error = strprintf("the list is dated (\"updated\": %s) more than %d s ahead of this node's clock (%s)",
                          updated.getValStr().substr(0, 40), MAX_FEE_LIST_FUTURE_SECS, FormatISO8601DateTime(now));
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
    published = static_cast<int64_t>(when);
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
    const Choice choice = Decide(initial_file, rates_file);

    if (choice.url.empty()) {
        FeeListSource source;
        source.decided_by = choice.decided_by;
        SetFeeListSource(source);
        std::vector<std::string> errors;
        if (!initial_file.empty()) {
            // The operator names the list to start from: it is the list
            // written by hand, kept in exchangerates.json from now on.
            const fs::path file_path = GetConfigFile(initial_file);
            if (!map.LoadFromJSONFile(file_path, errors)) {
                error = strprintf("Unable to load exchange rates from JSON file %s: \n%s\n", initial_file, MakeUnorderedList(errors));
                return false;
            }
            errors.clear();
            if (!map.SaveToJSONFile(errors)) {
                error = strprintf("Unable to save exchange rates to JSON file %s: \n%s\n", exchange_rates_config_file, MakeUnorderedList(errors));
                return false;
            }
            RecordHandList(map.GetRates());
        } else {
            std::string what;
            if (RatesFileIsChosen(rates_file, what)) {
                if (!map.LoadFromDefaultJSONFile(errors)) {
                    error = strprintf("Unable to load exchange rates from default JSON file %s: \n%s\n", exchange_rates_config_file, MakeUnorderedList(errors));
                    return false;
                }
            } else if (!what.empty()) {
                // Left where it is: nothing at startup writes the file.
                LogPrintf("%s holds %s, which no one chose, so it is not loaded: the fee whitelist is empty until it is written or the node is connected to a price server\n",
                          fs::PathToString(rates_file), what);
            }
        }
        LogPrintf("FeePriceServer: the fee whitelist is static (decided by: %s), %u assets\n", choice.decided_by, map.GetRates().size());
        return true;
    }

    if (choice.url.rfind("http://", 0) != 0) {
        error = strprintf("-feepriceserver=%s: only plain-HTTP addresses are supported", choice.url);
        return false;
    }

    // Connected: the list read from the price server is the whitelist's only
    // source, so the operator's file is neither loaded nor rewritten.
    FeeListSource source;
    source.connected = true;
    source.url = choice.url;
    source.by_default = choice.by_default;
    source.decided_by = choice.decided_by;
    SetFeeListSource(source);
    const std::string whose = strprintf(" (decided by: %s%s)", choice.decided_by,
                                        choice.by_default ? ", the network's price server" : "");

    std::string cached, why;
    std::map<CAsset, CAmount> rates;
    int64_t published{0};
    UniValue doc;
    bool cache_ok{false};
    int64_t written{0};
    try {
        cache_ok = ReadFile(CachePath(), cached) && doc.read(cached) && doc.isObject() &&
                   doc["url"].isStr() && doc["url"].get_str() == choice.url && doc["written"].isNum() &&
                   ParsePriceServerFeeList(cached, GetTime(), rates, published, why);
        if (cache_ok) written = doc["written"].get_int64();
    } catch (const std::exception& e) {
        cache_ok = false;
        why = e.what();
    }
    if (cache_ok) {
        ApplyPriceServerFeeList(choice.url, rates, published, written);
        LogPrintf("FeePriceServer: connected to %s%s; its last list (%u assets, taken %s) is in force\n",
                  choice.url, whose, rates.size(), FormatISO8601DateTime(written));
    } else {
        if (!why.empty()) LogPrintf("FeePriceServer: the cached list is not used: %s\n", why);
        LogPrintf("FeePriceServer: connected to %s%s\n", choice.url, whose);
    }
    // Asked now, before the wallet can be asked to send, so a reachable server's
    // list is in force from the first moment. Bounded: the whole request ends
    // within the startup timeout however the server behaves.
    RefreshFeePriceServer(std::min<int64_t>(FEE_LIST_STARTUP_TIMEOUT_SECS, gArgs.GetIntArg("-feepriceservertimeout", 15)));
    return true;
}

bool RefreshFeePriceServer(int timeout_secs)
{
    const FeeListSource src = GetFeeListSource();
    if (!src.connected) return false;
    const int64_t now = GetTime();
    UpdateFeeListSource([&](FeeListSource& s) { if (s.url == src.url) s.last_attempt = now; });

    std::map<CAsset, CAmount> rates;
    int64_t published{0};
    std::string err;
    bool ok = Request(src.url, timeout_secs, rates, published, err);
    if (ok) {
        const std::map<CAsset, CAmount> before = ExchangeRateMap::GetInstance().GetRates();
        // The cache is written under the same lock as the list, so it always
        // holds the last list taken, never an older one that lost a race.
        switch (ApplyPriceServerFeeList(src.url, rates, published, now,
                                        [&] { WriteCache(src.url, rates, published, now); })) {
        case FeeListApply::NOT_CONNECTED:
            LogPrintf("FeePriceServer: a list from %s arrived after this node disconnected from it; discarded\n", src.url);
            return false;
        case FeeListApply::OLDER:
            ok = false;
            err = strprintf("the list is older (%s) than the one in force (%s)",
                            FormatTime(published), FormatTime(GetFeeListSource().list_published));
            break;
        case FeeListApply::TAKEN: {
            RecomputeMempoolFees();
            bool same_assets = before.size() == rates.size();
            for (auto a = before.cbegin(), b = rates.cbegin(); same_assets && a != before.cend(); ++a, ++b) {
                same_assets = a->first == b->first;
            }
            if (!src.have_list || !src.last_error.empty() || !same_assets) {
                LogPrintf("FeePriceServer: took the list from %s: %u assets (%s)\n", src.url, rates.size(), Describe(rates));
            }
            return true;
        }
        }
    }
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

void StartFeePriceServer(CScheduler& feeds, CTxMemPool* mempool)
{
    g_mempool = mempool;
    const int poll = std::max<int64_t>(1, gArgs.GetIntArg("-feepriceserverpoll", 60));
    const int timeout = gArgs.GetIntArg("-feepriceservertimeout", 15);
    // One self-rescheduling task: quick retries until the first list is in
    // force, the poll interval after that, and idle while not connected.
    static std::function<void()> tick;
    tick = [&feeds, poll, timeout] {
        int next = poll;
        if (GetFeeListSource().connected) {
            // A task that throws ends the feeds thread's loop and the node
            // with it (CScheduler rethrows), so nothing leaves this one.
            try {
                RefreshFeePriceServer(timeout);
            } catch (const std::exception& e) {
                LogPrintf("FeePriceServer: the request failed: %s\n", e.what());
            }
            if (!GetFeeListSource().have_list) next = std::min(poll, FEE_LIST_RETRY_SECS);
        }
        feeds.scheduleFromNow([] { tick(); }, std::chrono::seconds{next});
    };
    const int first = GetFeeListSource().have_list ? poll : std::min(poll, FEE_LIST_RETRY_SECS);
    feeds.scheduleFromNow([] { tick(); }, std::chrono::seconds{first});
}

void StopFeePriceServer()
{
    LOCK(g_operator_mutex);
    g_stopped = true;
    g_mempool = nullptr;
}

bool WriteFeeListByHand(const std::map<CAsset, CAmount>& rates, std::string& error)
{
    LOCK(g_operator_mutex);
    if (g_stopped) { error = "the node is shutting down"; return false; }
    const FeeListSource src = GetFeeListSource();
    if (WriteHandFeeList(rates, "the fee whitelist was written by hand")) {
        LogPrintf("FeePriceServer: the fee whitelist was written by hand, so it is static and this node is no longer connected to the price server at %s; setfeepriceserver connects it again\n", src.url);
    }
    UpdateFeeListSource([](FeeListSource& s) { s.decided_by = "settings"; });
    RecordChoice("");
    std::vector<std::string> errors;
    if (!ExchangeRateMap::GetInstance().SaveToJSONFile(errors)) {
        error = strprintf("Error saving exchange rates to JSON file %s: \n%s\n", exchange_rates_config_file, MakeUnorderedList(errors));
        return false;
    }
    RecordHandList(rates);
    return true;
}

bool SetFeePriceServer(const std::string& url, std::string& error)
{
    LOCK(g_operator_mutex);
    if (g_stopped) { error = "the node is shutting down"; return false; }
    if (url.empty()) {
        const FeeListSource src = GetFeeListSource();
        if (DisconnectFeePriceServer("disconnected with setfeepriceserver")) {
            LogPrintf("FeePriceServer: disconnected from %s; the list in force is kept as a static whitelist\n", src.url);
        }
        UpdateFeeListSource([](FeeListSource& s) { s.decided_by = "settings"; });
        RecordChoice("");
        // The list in force becomes the operator's static whitelist, kept
        // across a restart like any list written by hand.
        std::vector<std::string> errors;
        ExchangeRateMap& map = ExchangeRateMap::GetInstance();
        if (!map.SaveToJSONFile(errors)) {
            error = strprintf("Disconnected, but could not save the whitelist to %s: %s", exchange_rates_config_file, MakeUnorderedList(errors));
            return false;
        }
        RecordHandList(map.GetRates());
        return true;
    }
    if (url.rfind("http://", 0) != 0) {
        error = "only plain-HTTP addresses are supported";
        return false;
    }
    // Ask first and change nothing unless the answer is a price server's list,
    // so pointing the node at a price feed API, or anything else, writes nothing.
    std::map<CAsset, CAmount> rates;
    int64_t published{0};
    std::string err;
    if (!Request(url, gArgs.GetIntArg("-feepriceservertimeout", 15), rates, published, err)) {
        error = strprintf("Not connected to %s: %s", url, err);
        return false;
    }
    const int64_t now = GetTime();
    FeeListSource source;
    source.connected = true;
    source.url = url;
    source.last_attempt = now;
    source.decided_by = "settings";
    source.by_default = url == DefaultFeePriceServer(gArgs);
    // Connected and the list written as one step. Already connected to this
    // address, a poll may have taken a newer list while this request was in
    // flight: that list stays, and this older answer is neither taken nor
    // cached. Any other writer waits on g_operator_mutex.
    const FeeListApply applied = ConnectPriceServerFeeList(source, rates, published, now,
                                                           [&] { WriteCache(url, rates, published, now); });
    RecomputeMempoolFees();
    RecordChoice(url);
    if (applied == FeeListApply::TAKEN) {
        LogPrintf("FeePriceServer: connected to %s with setfeepriceserver; took its list: %u assets (%s)\n",
                  url, rates.size(), Describe(rates));
    } else {
        LogPrintf("FeePriceServer: connected to %s with setfeepriceserver; its answer (published %s) is older than the list in force (published %s), which stays\n",
                  url, FormatTime(published), FormatTime(GetFeeListSource().list_published));
    }
    return true;
}
