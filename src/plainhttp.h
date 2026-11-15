// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLAINHTTP_H
#define BITCOIN_PLAINHTTP_H

#include <cstddef>
#include <string>

/**
 * A minimal synchronous plain-HTTP GET, for the node's small public feeds (the
 * reference prices, the list of a price server the node is connected to).
 *
 * The node links no TLS library and this follows no redirects, so only http://
 * URLs work. Anyone on the network path can answer in the server's place, which
 * is why every caller treats what comes back as unauthenticated input, and why
 * the reply is capped at max_body bytes: without a cap libevent buffers the
 * whole reply in memory, however large.
 *
 * @return true with out_body set on an HTTP 200; false with out_err set
 *         otherwise (bad URL, no connection, timeout, oversized reply, any
 *         other status).
 */
bool PlainHttpGet(const std::string& url, int timeout_secs, size_t max_body,
                  std::string& out_body, std::string& out_err);

#endif // BITCOIN_PLAINHTTP_H
