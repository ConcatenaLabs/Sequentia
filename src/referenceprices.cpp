// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <referenceprices.h>

#include <plainhttp.h>

#include <logging.h>
#include <scheduler.h>
#include <shutdown.h>
#include <sync.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/system.h>


#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace {

//! Largest price-feed reply accepted, in bytes.
constexpr size_t MAX_REFERENCE_PRICES_BODY = 1 << 20;

Mutex g_prices_mutex;
std::map<std::string, double> g_prices GUARDED_BY(g_prices_mutex);

} // namespace

int RefreshReferencePrices()
{
    const std::string url = gArgs.GetArg("-referencepricesurl", "");
    if (url.empty()) return 0;

    std::string body, err;
    if (!PlainHttpGet(url, gArgs.GetIntArg("-referencepricestimeout", 15), MAX_REFERENCE_PRICES_BODY, body, err,
                     [] { return ShutdownRequested(); })) {
        LogPrintf("ReferencePrices: fetch from %s failed: %s\n", url, err);
        return -1;
    }

    UniValue feed;
    if (!feed.read(body) || !feed.isObject()) {
        LogPrintf("ReferencePrices: invalid JSON from %s\n", url);
        return -1;
    }

    // /prices shape: { "<TICKER>": {"price": <usd>, "market_cap":..., "volume_24h":...} }
    // (a bare number value is also tolerated). Only positive prices are cached.
    std::map<std::string, double> next;
    for (const std::string& ticker : feed.getKeys()) {
        const UniValue& v = feed[ticker];
        double price = 0.0;
        if (v.isObject() && v.exists("price") && v["price"].isNum()) price = v["price"].get_real();
        else if (v.isNum()) price = v.get_real();
        if (price > 0.0 && !ticker.empty()) next[ToUpper(ticker)] = price;
    }

    const int n = static_cast<int>(next.size());
    // Keep the last-good prices if a poll returns nothing usable (feed gap / empty body).
    if (n == 0) {
        LogPrintf("ReferencePrices: no usable prices from %s (keeping previous)\n", url);
        return 0;
    }
    {
        LOCK(g_prices_mutex);
        g_prices = std::move(next);
    }
    LogPrintf("ReferencePrices: cached %d price(s) from %s\n", n, url);
    return n;
}

namespace {
//! A feed's answer can never stop the node: a task that throws ends the
//! feeds thread's loop and the node with it (CScheduler rethrows), and a
//! number out of range makes UniValue's getters throw.
void GuardedRefreshReferencePrices()
{
    try {
        RefreshReferencePrices();
    } catch (const std::exception& e) {
        LogPrintf("ReferencePrices: the answer could not be read: %s\n", e.what());
    }
}
} // namespace

void StartReferencePrices(CScheduler& scheduler)
{
    const std::string url = gArgs.GetArg("-referencepricesurl", "");
    if (url.empty()) return;
    LogPrintf("ReferencePrices: enabled, source %s\n", url);

    // Initial fetch a few seconds after startup, on the feeds' own thread (see
    // init.cpp), so neither init nor block connection ever waits on the feed.
    scheduler.scheduleFromNow([] { GuardedRefreshReferencePrices(); }, std::chrono::seconds{3});

    const int poll = gArgs.GetIntArg("-referencepricespoll", 120);
    if (poll > 0) {
        scheduler.scheduleEvery([] { GuardedRefreshReferencePrices(); }, std::chrono::seconds{poll});
    }
}

std::map<std::string, double> GetReferencePrices()
{
    LOCK(g_prices_mutex);
    return g_prices;
}
