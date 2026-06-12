// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: Bitcoin (parent chain) anchoring.
//
// When g_con_bitcoin_anchor is set, every block header commits to a reference
// to a parent-chain block (height + hash), with referenced heights required to
// be monotonically non-decreasing along the chain. Validators connected to a
// parent chain daemon (via the -mainchainrpc* settings) additionally verify
// that the referenced block is on the parent chain's best chain, and
// reorganize this chain whenever the parent chain reorganizes away a
// referenced block. See doc/sequentia/03-bitcoin-anchoring.md.

#ifndef BITCOIN_ANCHOR_H
#define BITCOIN_ANCHOR_H

#include <uint256.h>

#include <cstdint>

class ChainstateManager;

/** Default number of parent-chain confirmations a block needs before new
 *  blocks anchor to it. 1 anchors to the parent chain tip. */
static const int DEFAULT_ANCHOR_MIN_CONF = 1;
/** Default seconds between polls of the parent chain daemon for new blocks
 *  and reorganizations. */
static const int DEFAULT_ANCHOR_POLL_INTERVAL = 5;

/** Whether anchors are validated against the parent chain daemon
 *  (-validateanchor). When false, only the structural rules (presence and
 *  monotonicity) are enforced. */
extern bool g_validate_anchor;

/** Result of checking an anchor against the parent chain daemon. */
enum class AnchorCheckResult {
    OK,              //!< Block is on the parent chain's best chain at the claimed height
    NOT_FOUND,       //!< Parent chain daemon does not know the block
    STALE,           //!< Block known but no longer on the parent chain's best chain
    HEIGHT_MISMATCH, //!< Block on the best chain but at a different height than claimed
    NO_CONNECTION,   //!< Could not reach the parent chain daemon
};

/** Check that the parent-chain block (height, hash) is on the parent chain's
 *  best chain. Results of OK checks are cached until the parent chain tip
 *  changes (see AnchorWatchTask). */
AnchorCheckResult CheckMainchainAnchor(uint32_t height, const uint256& hash);

/** Pick the anchor for a new block: the highest parent-chain block with at
 *  least -anchorminconf confirmations, but never below the previous block's
 *  anchor (monotonicity). Falls back to the previous block's anchor if the
 *  parent chain daemon is unreachable. Returns false if no valid (non-null)
 *  anchor can be determined. */
bool GetAnchorForNewBlock(uint32_t prev_anchor_height, const uint256& prev_anchor_hash,
                          uint32_t& anchor_height, uint256& anchor_hash);

/** Scheduler task: poll the parent chain for new tips. On a parent chain
 *  reorganization, invalidate blocks of this chain whose anchors were
 *  reorganized away (the chain then reorganizes onto the last validly
 *  anchored block), and reconsider blocks whose anchors became canonical
 *  again. */
void AnchorWatchTask(ChainstateManager& chainman);

#endif // BITCOIN_ANCHOR_H
