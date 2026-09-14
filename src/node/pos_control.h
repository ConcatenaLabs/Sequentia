// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_POS_CONTROL_H
#define BITCOIN_NODE_POS_CONTROL_H

#include <interfaces/chain.h>
#include <key.h>

#include <string>
#include <vector>

namespace node {
struct NodeContext;

/** Enable autonomous Proof-of-Stake block production for `keys`, at runtime,
 *  with no restart: starts the producer if this node is not producing yet, or
 *  adds the keys to the running one, and persists the merged key set in the
 *  datadir's settings.json so production resumes after a restart. The single
 *  entry point behind the startposproducer RPC and the wallet's startstaking,
 *  so both persist and report the same way. Returns false with `error` set
 *  only when nothing could be done at all (no valid key, or PoS is off). */
bool StartPosProducerWithKeys(NodeContext& node, const std::vector<CKey>& keys,
                              interfaces::PosProducerStart& out, std::string& error);
} // namespace node

#endif // BITCOIN_NODE_POS_CONTROL_H
