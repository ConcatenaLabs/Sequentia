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
 * A fee whitelist is static when it is written by hand, and dynamic when the
 * node is connected to a price server and reads the list that server
 * publishes. There are no other states. The price server may be the
 * operator's own or another operator's; to the node either is an address.
 * With -feepriceserver=<address> (or setfeepriceserver) the node asks for the
 * list published there (a price server's GET /feewhitelist, or the file it
 * writes with --publish-file) at startup and every -feepriceserverpoll seconds
 * after, and makes it the node's whitelist. A price server never writes to a
 * node. The testnet is connected to the network's price server by default, so
 * a fresh node's whitelist is populated as it starts; the mainnet and custom
 * chains have no default price server, so they start static.
 *
 * Only a price server's published list is ever taken. A price server is what
 * turns a price feed API's prices into a whitelist, under its own admission
 * rules; an answer shaped like a price feed API's, or the registry's, is refused
 * as "not a price server", so nothing ever goes from a price feed API to the
 * whitelist without one.
 *
 * One writer. A hand write (setfeeexchangerates, or the desktop wallet) ends a
 * connection, and the whitelist is static from then on; setfeepriceserver
 * connects again. Operator writes are serialised, so of two the later wins.
 *
 * Who decides, at every start (see InitFeeList):
 *  1. -feepriceserver or -nofeepriceserver on the command line;
 *  2. else the operator's last choice through RPC or the desktop wallet
 *     (setfeepriceserver, a hand write), recorded in settings.json, unless the
 *     configuration file's setting has changed since that choice was made;
 *  3. else the configuration file's setting;
 *  4. else the network's default price server (the testnet's), which yields to
 *     a list someone wrote by hand. A file holding only the policy asset at the
 *     reference rate, or nothing, is what an older binary saved on its own (it
 *     saved the whitelist at every start), so on a chain that does not seed
 *     the whitelist it counts as written by hand only if this node recorded
 *     writing exactly that list. Nothing at startup writes the file.
 *
 * What the node does with an answer:
 *  - A list is taken whole or not at all. It is refused, and the list in force
 *    kept, when it is not a published list, is empty, exceeds the caps, holds an
 *    entry that is not a 64-hex asset id with a rate in (0, MAX_FEE_LIST_RATE],
 *    is dated more than MAX_FEE_LIST_FUTURE_SECS ahead of the node's clock, or
 *    is older than the list in force. An empty list is never a policy: a price
 *    server refuses to publish a round that admitted nothing, so reading one as
 *    "accept nothing" would let a single bad answer halt relay.
 *  - The last list is kept for as long as the server cannot be reached, with its
 *    age and the last error shown (getfeepriceserver). It is never cleared, and
 *    nothing ever falls back to one asset.
 *  - Each list taken is cached in <datadir>/feepriceserver.json, so a restart has
 *    it at once. A fresh node accepts no fee asset until its first list arrives.
 *
 * The requests run on the node's feeds thread, never on the scheduler that
 * block connection waits on, and each is bounded in time (name resolution
 * included), in size and by shutdown (plainhttp.h). The one made at startup
 * holds startup for at most FEE_LIST_STARTUP_TIMEOUT_SECS.
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
//! How far ahead of the node's clock a list's "updated" time may be.
static constexpr int64_t MAX_FEE_LIST_FUTURE_SECS = 600;
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

/** Parse a price server's answer strictly (see above); now is the node's
 *  clock, against which "updated" is bounded.
 *  @return false with error set if it must be refused. */
bool ParsePriceServerFeeList(const std::string& body, int64_t now, std::map<CAsset, CAmount>& rates,
                             int64_t& published, std::string& error);

/** Decide at startup who writes the fee whitelist (see above) and load it:
 *  when connected, the price server's cached list and then its answer to a
 *  first, bounded request; when not, the list written by hand.
 *  @return false with error set on a fatal configuration error. */
bool InitFeeList(std::string& error);

/** Ask the connected price server every -feepriceserverpoll seconds, on the
 *  given scheduler (the feeds thread). Idle while not connected, so
 *  setfeepriceserver can connect at any time. */
void StartFeePriceServer(CScheduler& feeds, CTxMemPool* mempool);

/** Before the mempool goes: no operator write runs after this returns. */
void StopFeePriceServer();

/** Ask the price server once and take its list. @return true if one was taken. */
bool RefreshFeePriceServer(int timeout_secs);

/** Write the whitelist by hand (setfeeexchangerates and the desktop wallet):
 *  ends a connection to a price server, saves the list to exchangerates.json
 *  and records the choice in settings.json. Serialised with setfeepriceserver.
 *  @return false with error set if the list could not be saved. */
bool WriteFeeListByHand(const std::map<CAsset, CAmount>& rates, std::string& error);

/** Connect to the price server at url ("" = disconnect, keeping the list in
 *  force as the operator's), and record that in settings.json. Connecting
 *  takes the server's list first and changes nothing if it cannot.
 *  @return false with error set otherwise. */
bool SetFeePriceServer(const std::string& url, std::string& error);

#endif // BITCOIN_FEEPRICESERVER_H
