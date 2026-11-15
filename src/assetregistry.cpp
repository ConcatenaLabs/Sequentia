// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetregistry.h>

#include <assetsdir.h>
#include <logging.h>
#include <scheduler.h>
#include <plainhttp.h>
#include <shutdown.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

//! The cap on a registry reply's body, in bytes: the index of every asset
//! with its labels, which is small, with room to grow.
constexpr size_t MAX_REGISTRY_BODY = 8 << 20;

/** One plain-HTTP request to the registry, bounded in time and size however
 *  the other end behaves (see plainhttp.h). The registry is served as plain
 *  HTTP behind the explorer origin. `body` empty means GET, otherwise POST
 *  with that JSON body.
 *
 *  Reports the status rather than judging it, so callers can tell a registry that
 *  refused (and said why) from one that never answered. */
bool HttpRequest(const std::string& url, const std::string& body, int& out_status,
                 std::string& out_body, std::string& out_err)
{
    PlainHttpRequest request;
    request.url = url;
    request.body = body;
    request.timeout_secs = gArgs.GetIntArg("-assetregistrytimeout", 15);
    request.max_body = MAX_REGISTRY_BODY;
    request.interrupted = [] { return ShutdownRequested(); };
    if (!PlainHttpFetch(request, out_status, out_body, out_err)) {
        out_err = "the registry: " + out_err;
        return false;
    }
    return true;
}

bool HttpGet(const std::string& url, std::string& out_body, std::string& out_err)
{
    int status = 0;
    if (!HttpRequest(url, /*body=*/"", status, out_body, out_err)) return false;
    if (status != 200) { out_err = strprintf("registry returned HTTP %d", status); return false; }
    return true;
}

} // namespace

std::string AssetRegistryBaseUrl()
{
    const std::string url = gArgs.GetArg("-assetregistryurl", "");
    if (url.empty()) return "";
    // The configured URL names the index file; submissions go to the directory
    // holding it (.../registry/index.minimal.json -> .../registry/). Only drop a
    // last segment that looks like a filename, so a URL already naming a
    // directory is left alone.
    const size_t slash = url.rfind('/');
    if (slash == std::string::npos) return url + "/";
    if (url.find('.', slash) == std::string::npos) {
        return slash + 1 == url.size() ? url : url + "/";
    }
    return url.substr(0, slash + 1);
}

bool AssetRegistryPost(const std::string& json_body, int& out_status, std::string& out_body, std::string& out_err)
{
    const std::string base = AssetRegistryBaseUrl();
    if (base.empty()) {
        out_err = "no asset registry is configured; set -assetregistryurl to the registry you want to use";
        return false;
    }
    return HttpRequest(base, json_body, out_status, out_body, out_err);
}

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

namespace {
//! A feed's answer can never stop the node: a task that throws ends the
//! feeds thread's loop and the node with it (CScheduler rethrows), and a
//! number out of range makes UniValue's getters throw.
void GuardedRefreshAssetRegistry()
{
    try {
        RefreshAssetRegistry();
    } catch (const std::exception& e) {
        LogPrintf("AssetRegistry: the answer could not be read: %s\n", e.what());
    }
}
} // namespace

void StartAssetRegistry(CScheduler& scheduler)
{
    const std::string url = gArgs.GetArg("-assetregistryurl", "");
    if (url.empty()) return;
    LogPrintf("AssetRegistry: enabled, source %s\n", url);

    // Initial fetch a few seconds after startup, on the feeds' own thread (see
    // init.cpp), so neither init nor block connection ever waits on the registry.
    scheduler.scheduleFromNow([] { GuardedRefreshAssetRegistry(); }, std::chrono::seconds{3});

    const int poll = gArgs.GetIntArg("-assetregistrypoll", 300);
    if (poll > 0) {
        scheduler.scheduleEvery([] { GuardedRefreshAssetRegistry(); }, std::chrono::seconds{poll});
    }
}
