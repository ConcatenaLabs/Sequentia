// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MAINCHAINRPC_H
#define BITCOIN_MAINCHAINRPC_H

#include <rpc/client.h>
#include <rpc/protocol.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <string>
#include <stdexcept>

#include <univalue.h>
#include <vector>

static const bool DEFAULT_NAMED=false;
static const char DEFAULT_RPCCONNECT[] = "127.0.0.1";
static const int DEFAULT_HTTP_CLIENT_TIMEOUT=900;

//
// Exception thrown on connection error.  This error is used to determine
// when to wait if -rpcwait is given.
//
class CConnectionFailed : public std::runtime_error
{
public:

    explicit inline CConnectionFailed(const std::string& msg) :
        std::runtime_error(msg)
    {}

};

UniValue CallMainChainRPC(const std::string& strMethod, const UniValue& params);

/** Many calls of one method, in a SINGLE HTTP round trip.
 *
 *  The parent-chain client opens a fresh connection per call and closes it
 *  again, so asking N questions costs N connects, N authentications and N
 *  round trips. That is invisible at N=1 and ruinous at N=10,000, which is
 *  what verifying every anchor on a long chain against a cold cache actually
 *  costs -- ten minutes of it, measured.
 *
 *  Returns one reply object per entry of `params_list`, in THE SAME ORDER,
 *  matched back by JSON-RPC id rather than by position: a batch reply may
 *  legally arrive in any order, and quietly pairing the wrong verdict with the
 *  wrong anchor would invalidate the wrong block. Anything it cannot match
 *  exactly throws rather than guessing.
 *
 *  This is one round trip: the caller decides how many calls belong in it. */
std::vector<UniValue> CallMainChainRPCBatch(const std::string& strMethod,
                                            const std::vector<UniValue>& params_list);

/** Put a batch reply back in request order, by id.
 *
 *  Separated from the call so it can be tested against the orderings a daemon
 *  is ALLOWED to send and almost never does. Pairing an answer with the wrong
 *  question is the one failure of batching that would be silent and wrong
 *  rather than loud and slow, so it is checked rather than assumed: every id
 *  must be present, in range, and used exactly once, or this throws. */
std::vector<UniValue> MatchBatchReplies(const UniValue& valReply, size_t expected,
                                        const std::string& strMethod);

/** SEQUENTIA: how many times this node has called the parent chain daemon.
 *
 *  Every call is one fresh TCP connection (the client sends `Connection:
 *  close`), so this is also the number of connections opened towards the
 *  parent daemon. It exists because the anchor watcher's cost is not visible
 *  any other way from inside the node: the count per watcher tick is the
 *  number this chain's O(chain length) anchor walk is judged by, both when
 *  measuring a live node and in feature_anchor_rpc_cost.py, which asserts the
 *  per-tick cost does not grow with the number of distinct anchors. Counted
 *  at call entry, so a call that throws still counts — it cost a connection
 *  attempt either way. Lock-free; safe from any thread. */
uint64_t GetMainchainRPCCallCount();

/** The same total, broken down by RPC method name. Takes a small internal
 *  mutex; call it from diagnostics, not from a hot path. */
std::map<std::string, uint64_t> GetMainchainRPCCallCountsByMethod();

// Verify if the block with given hash has at least the specified minimum number
// of confirmations.
// For validating merkle blocks, you can provide the nbTxs parameter to verify if
// it equals the number of transactions in the block.
bool IsConfirmedBitcoinBlock(const uint256& hash, const int nMinConfirmationDepth, const int nbTxs);

#endif // BITCOIN_MAINCHAINRPC_H

