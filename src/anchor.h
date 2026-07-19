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
/** Whether the block producer avoids anchoring a new block onto a parent-chain
 *  height that a competing branch is currently contesting (-anchoravoidcontested).
 *  Pure producer-side policy; see GetAnchorForNewBlock. */
static const bool DEFAULT_ANCHOR_AVOID_CONTESTED = true;
/** A competing parent-chain branch counts as a live contest for the anchor
 *  back-off only when its tip is within this many blocks of the active tip
 *  (-anchorcontestwindow). A branch that has fallen further behind is losing
 *  the race and is ignored, so old resolved forks never drag the anchor down. */
static const int DEFAULT_ANCHOR_CONTEST_WINDOW = 2;

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

/** Pure selection math for the anti-contested-anchor policy (Fix A), testable
 *  without a parent chain daemon. Given the active tip height, the contest
 *  window, and the competing branches (each a {tip height, branchlen} pair,
 *  with the active tip and daemon-rejected/invalid tips already filtered out),
 *  returns the highest uncontested parent-chain height: the lowest fork point
 *  (tip height - branchlen) among branches whose tip is within `window` of the
 *  active tip, or the active tip height when none qualify. A branch further
 *  than `window` behind the active tip is losing the race and ignored. */
int AnchorUncontestedHeight(int active_tip_height, int window,
                            const std::vector<std::pair<int, int>>& competing_branches);

/** Pick the anchor for a new block: the highest parent-chain block with at
 *  least -anchorminconf confirmations, but never below the previous block's
 *  anchor (monotonicity). With -anchoravoidcontested (default), the target is
 *  additionally backed down below any parent-chain height a live competing
 *  branch is contesting, so a new block anchors to Bitcoin ground every current
 *  contender agrees on and needs no Sequentia reorg when the parent fork
 *  resolves. This back-off is purely a block-producer policy — it changes only
 *  which anchor THIS node picks for blocks it produces, never which blocks it
 *  accepts — and costs anchor freshness only while a parent fork is actually
 *  live. Falls back to the previous block's anchor if the parent chain daemon
 *  is unreachable. Returns false if no valid (non-null) anchor can be
 *  determined. */
bool GetAnchorForNewBlock(uint32_t prev_anchor_height, const uint256& prev_anchor_hash,
                          uint32_t& anchor_height, uint256& anchor_hash);

/** Scheduler task: poll the parent chain for new tips. On a parent chain
 *  reorganization, invalidate blocks of this chain whose anchors were
 *  reorganized away (the chain then reorganizes onto the last validly
 *  anchored block), and reconsider blocks whose anchors became canonical
 *  again. Also scans new parent blocks for Sequentia checkpoints and
 *  updates the PoS finality point (see below). */
void AnchorWatchTask(ChainstateManager& chainman);

/** Seed the anchor-reorg recovery worklist from the block index at startup.
 *  The watcher's recovery set (blocks it invalidated when their parent-chain
 *  anchor was orphaned) is in-memory only, so a restart between the
 *  invalidation and the parent chain reorganizing back would otherwise strand
 *  those blocks BLOCK_FAILED forever (the BLOCK_FAILED flags persist, the
 *  worklist does not). LoadBlockIndex calls this with the persisted directly-
 *  invalidated blocks so the watcher reconsiders them once their anchor is
 *  canonical again (reorg-of-reorg). Reconsideration only ever clears a block
 *  whose anchor returns OK on a live parent-chain check, so a non-anchor
 *  failure that gets seeded is simply re-validated and re-failed once. */
void SeedAnchorInvalidated(const std::vector<uint256>& block_hashes);

/** SEQUENTIA committee-equivocation prevention (Change 4a): if a child of
 *  `tip_hash` at `child_height` lies on a watcher-invalidated branch that is
 *  not confirmed off the parent chain's best chain this parent-tip epoch, and
 *  that branch carries a quorum certification at or above the child (the
 *  lowest orphaned block may itself be a sub-quorum escaping-stall block with
 *  a certified descendant), return that child's hash. Such a branch is either
 *  awaiting the watcher's verdict, or already un-failed and reconnecting;
 *  while this returns a value the block producer must not treat the height as
 *  vacant (propose or countersign a rival there), or it manufactures a
 *  same-height committee equivocation: two quorum-certified siblings that
 *  immediate finality then freezes on different nodes (the live 96/4 finality
 *  split), a tie anchoring cannot break. Returns nullopt when nothing is
 *  pending, when the branch's anchor is confirmed off the best chain (a
 *  genuine parent departure: the height is truly vacant, produce as usual),
 *  when the branch never reached quorum anywhere at/above the child (it never
 *  held finality; the countersignature comparator arbitrates), or when
 *  PoS/anchor validation is off. Takes cs_main and g_anchor_mutex internally;
 *  call with neither held. */
std::optional<uint256> AnchorCertifiedSiblingPending(ChainstateManager& chainman,
                                                     const uint256& tip_hash, int child_height);

/** Record that the PoS finality gate rejected a rival branch
 *  (bad-fork-prior-to-pos-final in ContextualCheckBlockHeader). Rivals being
 *  re-offered and re-rejected every few peer retries while the tip stands
 *  still is the live signature of a contested parent-chain (Bitcoin) fork —
 *  the 2026-07-11 testnet4 incident. The GUI and getanchorstatus read this to
 *  tell the user to wait for Bitcoin instead of showing a generic sync state. */
void NotePosFinalForkRejection();

/** Unix time of the most recent NotePosFinalForkRejection (0 = none since
 *  startup). Lock-free; safe from any thread. */
int64_t GetLastPosFinalForkRejectionTime();

// --- Escaping-stall real-time evidence (incident 2026-07-17) ---
//
// The consensus escaping-stall relaxation (sub-quorum certification) requires
// the parent-chain anchor to have advanced POS_ESCAPING_STALL_ANCHOR_GAP
// heights past the parent block's anchor — sound while parent blocks take
// ~10 minutes, but during a testnet4 difficulty-1 block-storm heights advance
// in seconds, so the gap is met with the chain fully alive (block 25504 of the
// 2026-07-17 finality partition was minted leader-only 30 seconds after a
// quorum-certified parent). The parent chain carries a second, real-time
// clock: block timestamps, aggregated as median-time-past (MTP) — a pure
// function of the (immutable) block hash that every validator reads
// identically from its parent-chain daemon. Sub-quorum certification
// additionally requires the MTP of the block's anchor to be at least
// -posescapestallmtpgap seconds past the MTP of the parent's anchor: genuine
// stall evidence that a height race cannot fake without parent-chain hashrate.

/** Verdict of the escaping-stall MTP-gap check. UNKNOWN (parent daemon
 *  unreachable / header not yet known) must be treated as a soft, retriable
 *  rejection — never a permanent one — exactly like the R3 anchor check. */
enum class EscapeStallTimeVerdict { ALLOWED, TOO_SOON, UNKNOWN };

/** Whether a sub-quorum (escaping-stall) block satisfies the parent-chain
 *  real-time stall evidence: MTP(block anchor) - MTP(parent anchor) >=
 *  g_pos_escape_stall_mtp_gap. ALLOWED when the gap is disabled (0), anchor
 *  validation is off (-validateanchor=0 delegates to the network, mirroring
 *  the R3 skip), or the parent anchor is null (chain bring-up). MTPs are
 *  fetched from the parent daemon and cached forever (immutable per hash).
 *  Called with validation locks held is fine on cache hits; a cache miss does
 *  one parent-daemon RPC (only reached for actual sub-quorum blocks, which are
 *  rare by construction). */
EscapeStallTimeVerdict CheckEscapingStallMtpGap(const uint256& parent_anchor_hash,
                                                const uint256& block_anchor_hash);

/** Seconds of parent-chain MTP that must elapse between the parent block's
 *  anchor and a sub-quorum block's anchor (0 disables the check). One Bitcoin
 *  block interval by default. Set from -posescapestallmtpgap in init. */
extern int64_t g_pos_escape_stall_mtp_gap;
static const int64_t DEFAULT_POS_ESCAPE_STALL_MTP_GAP = 600;

// --- PoS finality reconciliation (design doc Change 4b; incident 2026-07-17) ---
//
// Two rival branches can both end up quorum-certified with valid anchors (a
// committee re-certifies from below its old finality after a transient parent
// view flap, then the parent converges back so BOTH branches' anchors are
// canonical — the 2026-07-17 "finality partition"). Anchoring genuinely cannot
// break that tie, and each node's immediate-finality gate freezes it on the
// side it finalized first. The reconciliation rule is the node-local release:
// a node abandons its finalized point ONLY for a rival branch that
//   (1) carries a full-quorum-certified block STRICTLY ABOVE the local
//       finalized height (never a same-height certificate comparison, which
//       would reopen the posterior-corruption channel),
//   (2) is anchor-valid on the local parent best chain, with the certifying
//       block anchored at/below the currently uncontested parent height (so a
//       release can never fire into a live parent-chain fork),
//   (3) forks no lower than the Bitcoin checkpoint floor (enforced at accept
//       time by the checkpoint gates), and
//   (4) has provably superseded the local branch: no quorum-certified block
//       has extended the local chain for -posreconcilepatience seconds.
// Forging that evidence requires a committee quorum (a stake majority), which
// already controls the chain going forward — no new attack surface. The
// majority side never releases (a minority branch cannot quorum-advance), so
// convergence is one-way. This is fork-choice/release behavior, not block
// validity: local clocks are fine and nodes may release at different moments.

/** Master switch (-posreconcile, default on). */
extern bool g_pos_reconcile;
/** Seconds without a quorum-certified extension of the local chain before the
 *  local finalized branch counts as abandoned (-posreconcilepatience). */
extern int64_t g_pos_reconcile_patience;
static const int64_t DEFAULT_POS_RECONCILE_PATIENCE = 600;
/** The rival's certified block must be at least this many heights above the
 *  local finalized height (-posreconcilemindepth). */
extern int g_pos_reconcile_min_depth;
static const int DEFAULT_POS_RECONCILE_MIN_DEPTH = 3;

/** Snapshot of the reconciliation monitor for getanchorstatus. */
struct PosReconcileStatus {
    bool enabled{false};
    //! "inactive" (no rival), "tracking" (rival certified above local finality
    //! seen; patience running), "released" (finality released for the rival).
    std::string state{"inactive"};
    int rival_cert_height{-1};
    uint256 rival_cert_hash;
    int64_t patience_remaining{0};
};
PosReconcileStatus GetPosReconcileStatus();

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

// Operator-configured static checkpoints (-poscheckpoint=height:hash) live in
// the common layer (pos.h / pos.cpp), not here: chainparams.cpp configures them
// and must link them into libbitcoin_common (and tools like elements-tx), which
// does not pull in this node-layer module.

#endif // BITCOIN_ANCHOR_H
