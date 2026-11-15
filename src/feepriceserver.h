// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_FEEPRICESERVER_H
#define BITCOIN_FEEPRICESERVER_H

#include <consensus/amount.h>

#include <cstdint>
#include <map>
#include <string>

class ArgsManager;
class CScheduler;
class CTxMemPool;
struct CAsset;

/**
 * SEQUENTIA: a node connected to a price server.
 *
 * A fee whitelist is static when it is written by hand, and dynamic when a
 * price server updates it. With -feepriceserver=<address> the node is
 * connected to a price server, its operator's own or another operator's: it
 * asks for the list that server publishes (the body of the price server's
 * GET /api/whitelist, or the file it writes with --publish-file) at startup and
 * every -feepriceserverpoll seconds after, and makes it the node's whitelist.
 * The testnet is connected to the network's price server by default, so a fresh
 * node's whitelist is populated as it starts; the mainnet and custom chains have
 * no default price server.
 *
 * Only a price server's published list is ever taken. A price server is what
 * turns a price feed API's prices into a whitelist, under its own admission
 * rules; an answer shaped like a price feed API's, or the registry's, is refused
 * as "not a price server", so nothing ever goes from a price feed API to the
 * whitelist without one.
 *
 * One writer. While the node is connected, the price server is the whitelist's
 * only writer. The first write by anyone else, the operator by hand
 * (setfeeexchangerates, the desktop wallet's Fee acceptance dialog) or a price
 * server of the operator's pushing over RPC, disconnects the node for good: it
 * records -nofeepriceserver in its settings.json, and from then on the list is
 * exactly what is written to it, an empty one included. To connect again:
 * setfeepriceserver "<address>", or the dialog. An address given on the command
 * line is connected at every start, as any command-line setting wins.
 *
 * What the node does with an answer:
 *  - A list is taken whole or not at all. It is refused, and the list in force
 *    kept, when it is not a published list, is empty, exceeds the caps, holds an
 *    entry that is not a 64-hex asset id with a rate in (0, MAX_FEE_LIST_RATE],
 *    or is older than the list in force. An empty list is never a policy: a
 *    price server refuses to publish a round that admitted nothing, so reading
 *    one as "accept nothing" would let a single bad answer halt relay.
 *  - The last list is kept for as long as the server cannot be reached, with its
 *    age and the last error shown (getfeepriceserver). It is never cleared, and
 *    nothing ever falls back to one asset.
 *  - Each list taken is cached in <datadir>/feepriceserver.json, so a restart has
 *    it at once. A fresh node accepts no fee asset until its first list arrives.
 *
 * The request is plain HTTP (the node links no TLS), so the list is not
 * authenticated; see the issue tracker for the signed list that fixes that.
 */

//! The cap on a published list's entries.
static constexpr size_t MAX_FEE_LIST_ENTRIES = 1000;
//! The cap on a price server's answer, in bytes.
static constexpr size_t MAX_FEE_LIST_BODY = 1 << 20;
//! The largest rate a published list may carry: the price server's own MAX_RATE.
static constexpr CAmount MAX_FEE_LIST_RATE = 1'000'000'000'000'000'000LL;
//! Seconds between requests while no list from the server is in force yet.
static constexpr int FEE_LIST_RETRY_SECS = 15;
//! The longest the startup request may hold startup up, in seconds.
static constexpr int FEE_LIST_STARTUP_TIMEOUT_SECS = 5;
//! The cache of the price server's list, in the data directory.
extern const char* const FEE_PRICE_SERVER_CACHE_FILE;

/** The network's price server for the selected chain: the testnet's for
 *  chain=test, -con_default_fee_price_server on a custom chain (so the tests can
 *  stand in for the testnet), and none on every other chain. */
std::string DefaultFeePriceServer(const ArgsManager& args);

/** Remember that -feepriceserver holds the chain's default (set by
 *  InitParameterInteraction), since a default yields to a list the operator
 *  wrote and an address the operator chose does not. */
void NoteFeePriceServerIsDefault(bool by_default);

/** Parse a price server's answer strictly (see above).
 *  @return false with error set if it must be refused. */
bool ParsePriceServerFeeList(const std::string& body, std::map<CAsset, CAmount>& rates,
                             int64_t& published, std::string& error);

/** Decide at startup who writes the fee whitelist and load it: when connected,
 *  the price server's cached list and then its answer to a first request; when
 *  not, the operator's file. Also moves aside a data directory's old seed.
 *  @return false with error set on a fatal configuration error. */
bool InitFeeList(std::string& error);

/** Ask the price server again every -feepriceserverpoll seconds (a no-op loop
 *  while not connected, so setfeepriceserver can connect at any time). */
void StartFeePriceServer(CScheduler& scheduler, CTxMemPool* mempool);

/** Ask the price server once and take its list. @return true if one was taken. */
bool RefreshFeePriceServer(int timeout_secs);

/** Someone other than the price server is about to write the whitelist:
 *  disconnect, if connected, and record that in settings.json. */
void NoteOperatorFeeListWrite(const std::string& how);

/** Connect to the price server at url ("" = disconnect, keeping the list in
 *  force as the operator's), and record that in settings.json. Connecting
 *  takes the server's list first and changes nothing if it cannot.
 *  @return false with error set otherwise. */
bool SetFeePriceServer(const std::string& url, std::string& error);

#endif // BITCOIN_FEEPRICESERVER_H
