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
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
 *  again. Also scans new parent blocks for Sequentia checkpoints and
 *  updates the PoS finality point (see below). */
void AnchorWatchTask(ChainstateManager& chainman);

// --- Bitcoin checkpoints against PoS long-range attacks (paper §11) ---
//
// Anyone may commit a Sequentia block reference into the parent chain as a
// tagged OP_RETURN ("SEQCKPT" || block hash (32) || height (4, LE)). A node
// treats a checkpointed block as *finalized* once (a) the block is in its own
// active chain and (b) the committing parent-chain transaction is buried at
// least -poscheckpointdepth confirmations deep. Checkpoints therefore never
// force a chain on a node — they only lock in history it already validated —
// so conflicting or bogus checkpoints are harmless. Together with the CSV
// stake-unbonding period (which must exceed the checkpoint cadence), this
// closes the posterior-corruption / long-range attack window: keys that
// unbonded after a checkpoint cannot rewrite the history below it.

/** Parent-chain confirmations a checkpoint commitment needs before it
 *  finalizes (0 disables checkpoint processing). The whitepaper (§3.11) sets
 *  this at 2016 Bitcoin blocks (~2 weeks): the checkpoint "consolidation"
 *  window, which the stake-unbonding period must exceed so keys cannot unbond
 *  and then rewrite history below a checkpoint. */
static const int DEFAULT_POS_CHECKPOINT_DEPTH = 2016;
/** How many parent-chain blocks to scan backwards for checkpoints on the
 *  first watcher pass. */
static const int DEFAULT_POS_CHECKPOINT_SCAN = 100;

/** Build the OP_RETURN payload committing to a Sequentia block:
 *  "SEQCKPT" || hash || LE32(height). Embed it in any parent-chain
 *  transaction (e.g. a `data` output). */
std::vector<unsigned char> BuildCheckpointPayload(const uint256& block_hash, uint32_t height);

/** Parse a checkpoint payload, or nullopt if malformed. */
std::optional<std::pair<uint256, uint32_t>> ParseCheckpointPayload(const std::vector<unsigned char>& payload);

/** The current finalized block (highest checkpointed-and-buried block on the
 *  active chain), if any. */
bool GetPosFinalizedCheckpoint(int& height, uint256& hash);

/** A checkpoint observed on the parent chain. */
struct PosCheckpoint {
    uint256 seq_hash;
    uint32_t seq_height;
    int btc_height;        //!< parent-chain height of the committing block
    uint256 btc_hash;      //!< parent-chain block that contains the commitment
};

/** All checkpoints observed so far (for RPC introspection). */
std::vector<PosCheckpoint> GetPosCheckpoints();

/** Buried, parent-canonical checkpoints committing blocks that are NOT on
 *  this node's active chain at heights it has already passed — the alarm
 *  that this node may be on the losing side of a long-range fork (or that a
 *  bogus checkpoint exists; the node cannot tell alone). Surfaced via
 *  getcheckpointinfo. */
std::vector<PosCheckpoint> GetPosCheckpointConflicts();

// --- Operator-configured static checkpoints (-poscheckpoint=height:hash) ---
//
// Unlike the dynamic parent-chain checkpoints above (which only lock in history
// a node has already validated), these are supplied by the operator up front,
// so they protect a *fresh* sync against a long-range alternate history before
// any block is downloaded: a block presented at a configured checkpoint height
// must carry the configured hash, else it (and any branch built on it) is
// rejected in ContextualCheckBlockHeader. Reject-only — they never make a node
// seek or download a particular branch.

/** Drop all configured checkpoints (chain-parameter (re)load). */
void ClearConfiguredPosCheckpoints();

/** Register a configured checkpoint. Fails on a negative height or a height
 *  already configured with a different hash. */
bool AddConfiguredPosCheckpoint(int height, const uint256& hash, std::string& error);

/** All configured checkpoints, keyed by Sequentia height. */
std::map<int, uint256> GetConfiguredPosCheckpoints();

#endif // BITCOIN_ANCHOR_H
