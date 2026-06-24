// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <referenceprices.h>

#include <logging.h>
#include <scheduler.h>
#include <support/events.h>
#include <sync.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace {

Mutex g_prices_mutex;
std::map<std::string, double> g_prices GUARDED_BY(g_prices_mutex);

/** Reply structure for the request-done callback to fill in. */
struct HTTPReply {
    HTTPReply() : status(0), error(-1) {}
    int status;
    int error;
    std::string body;
};

void http_request_done(struct evhttp_request* req, void* ctx)
{
    HTTPReply* reply = static_cast<HTTPReply*>(ctx);
    if (req == nullptr) { reply->status = 0; return; }
    reply->status = evhttp_request_get_response_code(req);
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (buf) {
        size_t size = evbuffer_get_length(buf);
        const char* data = (const char*)evbuffer_pullup(buf, size);
        if (data) reply->body = std::string(data, size);
        evbuffer_drain(buf, size);
    }
}

#if LIBEVENT_VERSION_NUMBER >= 0x02010300
void http_error_cb(enum evhttp_request_error err, void* ctx)
{
    static_cast<HTTPReply*>(ctx)->error = err;
}
#endif

/** Minimal synchronous http:// GET. The feed is served as plain HTTP behind the
 *  explorer origin; HTTPS would need bufferevent_openssl and is out of scope. */
bool HttpGet(const std::string& url, std::string& out_body, std::string& out_err)
{
    const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0) { out_err = "only http:// price-feed URLs are supported"; return false; }
    const std::string rest = url.substr(scheme.size());
    std::string hostport, path;
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) { hostport = rest; path = "/"; }
    else { hostport = rest.substr(0, slash); path = rest.substr(slash); }

    std::string host = hostport;
    int port = 80;
    const size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = std::atoi(hostport.substr(colon + 1).c_str());
    }
    if (host.empty() || port <= 0 || port > 65535) { out_err = "malformed price-feed URL"; return false; }

    raii_event_base base = obtain_event_base();
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), host, port);
    evhttp_connection_set_timeout(evcon.get(), gArgs.GetIntArg("-referencepricestimeout", 15));

    HTTPReply response;
    raii_evhttp_request req = obtain_evhttp_request(http_request_done, (void*)&response);
    if (req == nullptr) { out_err = "create http request failed"; return false; }
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    evhttp_request_set_error_cb(req.get(), http_error_cb);
#endif

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req.get());
    if (output_headers) {
        evhttp_add_header(output_headers, "Host", hostport.c_str());
        evhttp_add_header(output_headers, "Connection", "close");
    }

    const int r = evhttp_make_request(evcon.get(), req.get(), EVHTTP_REQ_GET, path.c_str());
    req.release(); // ownership moved to evcon in the call above
    if (r != 0) { out_err = "send http request failed"; return false; }

    event_base_dispatch(base.get());

    if (response.status == 0) { out_err = "could not connect to the price feed"; return false; }
    if (response.status != 200) { out_err = strprintf("price feed returned HTTP %d", response.status); return false; }
    out_body = response.body;
    return true;
}

} // namespace

int RefreshReferencePrices()
{
    const std::string url = gArgs.GetArg("-referencepricesurl", "");
    if (url.empty()) return 0;

    std::string body, err;
    if (!HttpGet(url, body, err)) {
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

void StartReferencePrices(CScheduler& scheduler)
{
    const std::string url = gArgs.GetArg("-referencepricesurl", "");
    if (url.empty()) return;
    LogPrintf("ReferencePrices: enabled, source %s\n", url);

    // Initial fetch a few seconds after startup, on the scheduler thread, so it
    // never blocks init even if the feed is slow or unreachable.
    scheduler.scheduleFromNow([] { RefreshReferencePrices(); }, std::chrono::seconds{3});

    const int poll = gArgs.GetIntArg("-referencepricespoll", 120);
    if (poll > 0) {
        scheduler.scheduleEvery([] { RefreshReferencePrices(); }, std::chrono::seconds{poll});
    }
}

std::map<std::string, double> GetReferencePrices()
{
    LOCK(g_prices_mutex);
    return g_prices;
}
