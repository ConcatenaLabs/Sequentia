// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLAINHTTP_H
#define BITCOIN_PLAINHTTP_H

#include <cstddef>
#include <functional>
#include <string>

/**
 * A minimal synchronous plain-HTTP client, for the node's small public feeds
 * (the asset registry, the reference prices, the list of a price server the
 * node is connected to).
 *
 * The node links no TLS library and this follows no redirects, so only http://
 * URLs work. Anyone on the network path can answer in the server's place, which
 * is why every caller treats what comes back as unauthenticated input, and why
 * every request is bounded however the other end behaves:
 *  - in time: the whole request, name resolution included, ends after
 *    timeout_secs. libevent's own timeout restarts at every byte, so a server
 *    sending one byte at a time would otherwise hold the caller indefinitely;
 *  - in memory: the reply's status line and headers are capped at
 *    PLAIN_HTTP_MAX_HEADERS bytes and its body at max_body bytes; without the
 *    caps libevent buffers the whole reply, however large;
 *  - by shutdown: interrupted() is asked several times a second, and the request
 *    ends as soon as it returns true.
 */

//! The cap on a reply's status line and headers, in bytes.
static constexpr size_t PLAIN_HTTP_MAX_HEADERS = 16 * 1024;

struct PlainHttpRequest {
    std::string url;
    //! Empty: GET. Otherwise POST with this body, as application/json.
    std::string body;
    int timeout_secs{15};
    size_t max_body{1 << 20};
    //! Asked while the request runs; returning true ends it.
    std::function<bool()> interrupted;
};

/** Make the request.
 *  @return true with out_status and out_body set when the server answered
 *          (any status); false with out_err set otherwise (bad URL, no
 *          connection, the deadline passed, an oversized reply, interrupted). */
bool PlainHttpFetch(const PlainHttpRequest& request, int& out_status, std::string& out_body, std::string& out_err);

/** A GET that succeeds only on HTTP 200. */
bool PlainHttpGet(const std::string& url, int timeout_secs, size_t max_body,
                  std::string& out_body, std::string& out_err,
                  const std::function<bool()>& interrupted = {});

#endif // BITCOIN_PLAINHTTP_H
