// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetregistry.h>

#include <assetsdir.h>
#include <logging.h>
#include <scheduler.h>
#include <support/events.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/system.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

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

/** Minimal synchronous http:// GET. The registry is served as plain HTTP behind
 *  the explorer origin; HTTPS would need bufferevent_openssl and is out of scope. */
bool HttpGet(const std::string& url, std::string& out_body, std::string& out_err)
{
    const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0) { out_err = "only http:// registry URLs are supported"; return false; }
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
    if (host.empty() || port <= 0 || port > 65535) { out_err = "malformed registry URL"; return false; }

    raii_event_base base = obtain_event_base();
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), host, port);
    evhttp_connection_set_timeout(evcon.get(), gArgs.GetIntArg("-assetregistrytimeout", 15));

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

    if (response.status == 0) { out_err = "could not connect to the registry"; return false; }
    if (response.status != 200) { out_err = strprintf("registry returned HTTP %d", response.status); return false; }
    out_body = response.body;
    return true;
}

} // namespace

int RefreshAssetRegistry()
{
    const std::string url = gArgs.GetArg("-assetregistryurl", "");
    if (url.empty()) return 0;

    std::string body, err;
    if (!HttpGet(url, body, err)) {
        LogPrintf("AssetRegistry: fetch from %s failed: %s\n", url, err);
        return -1;
    }

    UniValue idx;
    if (!idx.read(body) || !idx.isObject()) {
        LogPrintf("AssetRegistry: invalid JSON index from %s\n", url);
        return -1;
    }

    // index.minimal.json shape:
    //   { "<assetid>": [domain, ticker, name, precision, verified] }
    // The 5th element (verified: 1/0) is appended by the registry. We only merge
    // chain+domain-verified entries; unverified legacy/seed labels are advisory
    // and are NOT trusted into the global asset directory. For backward
    // compatibility, an index that omits v[4] (older registry) is treated as
    // unverified and skipped, so the node never silently trusts unlabelled data.
    std::vector<AssetRegistryEntry> entries;
    int skipped_unverified = 0;
    for (const std::string& id : idx.getKeys()) {
        const UniValue& v = idx[id];
        if (!v.isArray() || v.size() < 2 || !v[1].isStr()) continue;
        const std::string ticker = v[1].get_str();
        if (id.size() != 64 || ticker.empty()) continue;
        // verified flag (v[4]); absent or falsy => not verified => skip.
        bool verified = false;
        if (v.size() >= 5) {
            const UniValue& vf = v[4];
            if (vf.isNum()) verified = vf.get_int() != 0;
            else if (vf.isBool()) verified = vf.get_bool();
        }
        if (!verified) { skipped_unverified++; continue; }
        // precision (v[3]): the asset's decimal places, matching the on-chain
        // nDenomination for assets issued by this software. Advisory (chain wins
        // when known); clamp out-of-range or non-numeric to the default of 8.
        uint8_t precision = DEFAULT_ASSET_PRECISION;
        if (v.size() >= 4 && v[3].isNum()) {
            const int p = v[3].get_int();
            if (p >= 0 && p <= MAX_ASSET_PRECISION) precision = static_cast<uint8_t>(p);
        }
        entries.push_back(AssetRegistryEntry{id, ticker, precision});
    }
    if (skipped_unverified > 0)
        LogPrintf("AssetRegistry: skipped %d unverified (advisory) label(s) from %s\n", skipped_unverified, url);

    const int n = MergeGlobalAssetDir(entries);
    if (n > 0) LogPrintf("AssetRegistry: merged %d new asset label(s) from %s\n", n, url);
    return n;
}

void StartAssetRegistry(CScheduler& scheduler)
{
    const std::string url = gArgs.GetArg("-assetregistryurl", "");
    if (url.empty()) return;
    LogPrintf("AssetRegistry: enabled, source %s\n", url);

    // Initial fetch a few seconds after startup, on the scheduler thread, so it
    // never blocks init even if the registry is slow or unreachable.
    scheduler.scheduleFromNow([] { RefreshAssetRegistry(); }, std::chrono::seconds{3});

    const int poll = gArgs.GetIntArg("-assetregistrypoll", 300);
    if (poll > 0) {
        scheduler.scheduleEvery([] { RefreshAssetRegistry(); }, std::chrono::seconds{poll});
    }
}
