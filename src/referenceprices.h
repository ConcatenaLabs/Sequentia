// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_REFERENCEPRICES_H
#define BITCOIN_REFERENCEPRICES_H

#include <map>
#include <string>

class CScheduler;

/**
 * SEQUENTIA reference-currency price feed client.
 *
 * When -referencepricesurl is set, the node periodically fetches the market-data
 * feed (/prices: { "<TICKER>": {"price": <usd>, ...} }) and caches per-asset USD
 * base prices. The node GUI reads these (via the getreferenceprices RPC) to value
 * displayed amounts in a user-chosen reference currency (USD, BTC, or any priced
 * asset). This is DISPLAY-ONLY: it never affects consensus, fees, the mempool, or
 * the asset registry — it is purely a valuation aid for the UI.
 */

/** Fetch the feed once and replace the cached prices.
 *  @return the number of priced tickers cached, 0 if no URL is configured, or -1
 *  on a fetch/parse error (which is logged, not thrown). */
int RefreshReferencePrices();

/** Schedule an initial fetch shortly after startup plus periodic refreshes every
 *  -referencepricespoll seconds. No-op if -referencepricesurl is unset. */
void StartReferencePrices(CScheduler& scheduler);

/** A thread-safe snapshot copy of the cached { UPPER_TICKER -> USD price } map. */
std::map<std::string, double> GetReferencePrices();

#endif // BITCOIN_REFERENCEPRICES_H
