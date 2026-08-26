// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <anchor.h>

#include <chain.h>
#include <logging.h>
#include <mainchainrpc.h>
#include <pos.h>
#include <primitives/bitcoin/block.h>
#include <script/script.h>
#include <shutdown.h>
#include <streams.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <validation.h>

#include <atomic>
#include <chrono>
#include <map>
#include <set>

bool g_validate_anchor = true;
std::atomic<bool> g_anchor_unvalidated_by_prompt{false};
std::atomic<bool> g_anchor_parent_back_online{false};

//! Sub-quorum blocks accepted without the parent-chain time evidence
//! (NoteUnverifiedEscapingStall). Atomic rather than GUARDED_BY(g_anchor_mutex):
//! written from block validation with cs_main held, read by the GUI status
//! poller and getanchorstatus — neither may take locks for this. The three
//! fields are updated independently, so a concurrent reader can see a fresh
//! count with a stale height for an instant; harmless for a status display.
std::atomic<int> g_unverified_escape_stall_count{0};
std::atomic<int> g_unverified_escape_stall_height{-1};
std::atomic<int64_t> g_unverified_escape_stall_time{0};

void NoteUnverifiedEscapingStall(int height)
{
    g_unverified_escape_stall_height.store(height, std::memory_order_relaxed);
    g_unverified_escape_stall_time.store(GetTime(), std::memory_order_relaxed);
    g_unverified_escape_stall_count.fetch_add(1, std::memory_order_relaxed);
    LogPrintf("WARNING: accepted sub-quorum (escaping-stall) block at height %d without checking the parent-chain evidence: this node is not watching Bitcoin (-validateanchor=0), so it takes its peers' word that the committee really had stalled\n", height);
}

UnverifiedEscapingStalls GetUnverifiedEscapingStalls()
{
    UnverifiedEscapingStalls out;
    out.count = g_unverified_escape_stall_count.load(std::memory_order_relaxed);
    out.last_height = g_unverified_escape_stall_height.load(std::memory_order_relaxed);
    out.last_time = g_unverified_escape_stall_time.load(std::memory_order_relaxed);
    return out;
}

//! Wall-clock time of the last finality-gate rejection of a rival branch
//! (NotePosFinalForkRejection). Atomic, not GUARDED_BY(g_anchor_mutex):
//! written from block validation with cs_main held, read by the GUI status
//! poller and getanchorstatus — neither may take locks for this.
static std::atomic<int64_t> g_last_posfinal_fork_reject{0};

void NotePosFinalForkRejection()
{
    g_last_posfinal_fork_reject.store(GetTime(), std::memory_order_relaxed);
}

int64_t GetLastPosFinalForkRejectionTime()
{
    return g_last_posfinal_fork_reject.load(std::memory_order_relaxed);
}

namespace {

Mutex g_anchor_mutex;
//! Checkpoints observed on the parent chain, keyed by Sequentia block hash;
//! only the earliest commitment per block is kept.
std::map<uint256, PosCheckpoint> g_pos_checkpoints GUARDED_BY(g_anchor_mutex);
//! The current finality point (highest checkpointed-and-buried block on the
//! active chain). Height -1 = none.
int g_pos_finalized_height GUARDED_BY(g_anchor_mutex) = -1;
uint256 g_pos_finalized_hash GUARDED_BY(g_anchor_mutex);
//! Last parent-chain block already scanned for checkpoints.
uint256 g_last_checkpoint_scan_tip GUARDED_BY(g_anchor_mutex);
//! Last known parent-chain tip height (for finality updates on quiet ticks).
int g_last_btc_tip_height GUARDED_BY(g_anchor_mutex) = -1;
//! Buried, parent-canonical checkpoints whose block is NOT on our active
//! chain even though our chain has reached the claimed height — the signature
//! of being on the losing side of a long-range fork (or of a bogus
//! checkpoint; the node cannot tell alone, which is exactly why it must warn).
std::vector<PosCheckpoint> g_pos_checkpoint_conflicts GUARDED_BY(g_anchor_mutex);
// Operator-configured static checkpoints (-poscheckpoint=height:hash) live in
// the common layer (pos.cpp) so chainparams.cpp / sequentia-tx can link them.

const unsigned char POS_CKPT_TAG[7] = {'S', 'E', 'Q', 'C', 'K', 'P', 'T'};
//! Anchors confirmed to be on the parent chain's best chain, keyed
//! {parent height, parent hash} so the height orders the set (see
//! DropAnchorCachesAbove).
std::set<std::pair<uint32_t, uint256>> g_anchor_ok_cache GUARDED_BY(g_anchor_mutex);
//! Anchors confirmed DEFINITIVELY off the parent chain's best chain
//! (STALE/NOT_FOUND/HEIGHT_MISMATCH — never NO_CONNECTION, which is
//! indeterminate). This lets the recovery loop run every tick without
//! re-hitting bitcoind for the same permanently-orphaned anchors, avoiding a
//! self-inflicted RPC storm.
//!
//! Both caches are invalidated by DropAnchorCachesAbove when the parent tip
//! moves: only the part that the move could have changed, never the whole set
//! (see MainchainUnchangedHeight for why that is sound, and for what it costs
//! when it is not).
std::set<std::pair<uint32_t, uint256>> g_anchor_stale_cache GUARDED_BY(g_anchor_mutex);
//! Ceiling on the OK cache. It is no longer emptied every parent tip change, so
//! it grows by roughly one entry per parent-chain block for the life of the
//! chain (~52k/year against Bitcoin, a few MB). The cap only exists so the
//! growth is bounded rather than unbounded; reaching it (two decades of Bitcoin
//! blocks) does not break anything — the cache stops accepting new entries and
//! the excess anchors go back to costing one RPC per walk, i.e. the behaviour
//! this cache exists to avoid. If a chain ever gets there, the fix is to stop
//! re-walking provably-unchanged history at all, not a bigger set.
constexpr size_t ANCHOR_OK_CACHE_MAX = 1u << 20;
//! Blocks invalidated by the anchor watcher, so they can be reconsidered if
//! the parent chain reorganizes back.
std::set<uint256> g_anchor_invalidated GUARDED_BY(g_anchor_mutex);
//! Last seen parent chain tip.
uint256 g_last_mainchain_tip GUARDED_BY(g_anchor_mutex);
//! Median-time-past of parent-chain blocks, keyed by block hash. A block's MTP
//! is a pure function of the hash (its own and its ancestors' timestamps), so
//! entries are immutable and the cache is never invalidated — only size-capped.
std::map<uint256, int64_t> g_anchor_mtp_cache GUARDED_BY(g_anchor_mutex);
constexpr size_t ANCHOR_MTP_CACHE_MAX = 65536;
//! Reconciliation monitor snapshot for getanchorstatus (anchor.h).
PosReconcileStatus g_reconcile_status GUARDED_BY(g_anchor_mutex);

//! Query the parent chain daemon for its best block hash.
bool GetMainchainBestBlockHash(uint256& hash)
{
    try {
        UniValue reply = CallMainChainRPC("getbestblockhash", UniValue(UniValue::VARR));
        UniValue errval = find_value(reply, "error");
        if (!errval.isNull()) {
            LogPrintf("WARNING: error from mainchain getbestblockhash: %s\n", errval.write());
            return false;
        }
        UniValue result = find_value(reply, "result");
        if (!result.isStr()) return false;
        hash = uint256S(result.get_str());
        return true;
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "Could not reach mainchain daemon for getbestblockhash: %s\n", e.what());
        return false;
    }
}

//! Query the parent chain daemon for the block hash at the given height on
//! its best chain.
bool GetMainchainBlockHashAt(int height, uint256& hash)
{
    try {
        UniValue params(UniValue::VARR);
        params.push_back(height);
        UniValue reply = CallMainChainRPC("getblockhash", params);
        UniValue errval = find_value(reply, "error");
        if (!errval.isNull()) return false;
        UniValue result = find_value(reply, "result");
        if (!result.isStr()) return false;
        hash = uint256S(result.get_str());
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

//! Query the parent chain daemon for its block count.
bool GetMainchainBlockCount(int& count)
{
    try {
        UniValue reply = CallMainChainRPC("getblockcount", UniValue(UniValue::VARR));
        UniValue errval = find_value(reply, "error");
        if (!errval.isNull()) return false;
        UniValue result = find_value(reply, "result");
        if (!result.isNum()) return false;
        count = result.get_int();
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

//! Bounds the backwards walk that locates the fork point after a parent-chain
//! reorganization (MainchainUnchangedHeight). The walk costs one RPC per block
//! of reorg depth, so this is the depth past which finding the fork point stops
//! being cheaper than simply re-checking every anchor. Bitcoin reorganizations
//! are one or two blocks deep; anything past this bound falls back to
//! discarding both caches, which is always safe.
constexpr int ANCHOR_FORK_WALK_MAX = 2016;

//! One parent-chain header, read WITHOUT consulting the anchor caches.
struct MainchainHeaderInfo {
    int height{-1};
    bool on_best_chain{false};
    uint256 prev; //!< predecessor's hash; null at the genesis block
};

//! Fetch a parent-chain block header. Deliberately does NOT go through
//! CheckMainchainAnchor: its whole job is to decide whether the cached anchor
//! verdicts are still valid, and CheckMainchainAnchor would answer out of the
//! very cache under judgement — a cached OK for the old tip would then "prove"
//! that the old tip is still canonical no matter what the parent chain did.
//! Returns false when the daemon is unreachable or does not know the hash: both
//! are indeterminate, and neither may be read as "off the best chain".
bool GetMainchainHeaderInfo(const uint256& hash, MainchainHeaderInfo& out)
{
    try {
        UniValue params(UniValue::VARR);
        params.push_back(hash.GetHex());
        UniValue reply = CallMainChainRPC("getblockheader", params);
        if (!find_value(reply, "error").isNull()) return false;
        UniValue result = find_value(reply, "result");
        if (!result.isObject()) return false;
        UniValue height = find_value(result.get_obj(), "height");
        if (!height.isNum()) return false;
        UniValue confirmations = find_value(result.get_obj(), "confirmations");
        if (!confirmations.isNum()) return false;
        UniValue prev = find_value(result.get_obj(), "previousblockhash");
        out.height = height.get_int();
        out.on_best_chain = confirmations.get_int64() >= 1;
        out.prev = prev.isStr() ? uint256S(prev.get_str()) : uint256();
        return true;
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "Could not reach mainchain daemon for header %s: %s\n", hash.ToString(), e.what());
        return false;
    }
}

//! The highest parent-chain height whose best-chain block is PROVABLY the same
//! after the parent tip moved away from `old_tip` as it was before. Returns -1
//! when that cannot be established, which the caller must read as "nothing is
//! provably unchanged".
//!
//! Why this is the right question. A cached anchor verdict for (h, hash) says
//! something about the parent chain's block at height h. It can only become
//! wrong if the parent's best chain at height h changed. So: find how far up
//! the parent chain is untouched, and keep exactly the verdicts below that.
//!
//! Why the answer is sound. Let Y be a block that was on the parent's best
//! chain at height f before the move and is still on it at height f after. A
//! block's ancestors are fixed by its own header chain, not by which chain it
//! sits in, so for every h <= f the best-chain block at height h is Y's
//! ancestor at height h both before and after — the same block. Hence every
//! verdict at height <= f survives the move, positive and negative alike:
//!   - an anchor cached OK at h <= f is still the best-chain block at h;
//!   - an anchor cached STALE/NOT_FOUND/HEIGHT_MISMATCH at h <= f still is,
//!     because the only thing that could rescue it is the best chain at h
//!     becoming that block, and the best chain at h did not change.
//! Nothing here weakens the walk in section 2: verdicts above f are dropped and
//! re-fetched from the parent daemon, and the walk still descends to height 1
//! every tick. This drops re-VERIFICATION of what cannot have changed; it never
//! introduces a depth below which anchors stop being checked.
//!
//! Finding Y. If `old_tip` itself is still on the parent's best chain, the move
//! was a plain extension — the common case, one Bitcoin block every ~10 minutes
//! against a chain that produces a block every 30 seconds — and Y is the old
//! tip, for the cost of ONE header RPC no matter how many blocks were appended.
//! Otherwise the parent reorganized, and Y is the fork point: walk back from the
//! old tip until a block that is on the best chain again, one RPC per block of
//! reorg depth.
int MainchainUnchangedHeight(const uint256& old_tip)
{
    // First tick after startup (or after a failed classification): there is no
    // old tip to reason from. The caches are re-derived from the parent daemon,
    // which is exactly right — nothing about Bitcoin should be believed across
    // a restart on the strength of memory that did not survive it.
    if (old_tip.IsNull()) return -1;

    uint256 cursor = old_tip;
    for (int depth = 0; depth <= ANCHOR_FORK_WALK_MAX; ++depth) {
        MainchainHeaderInfo info;
        if (!GetMainchainHeaderInfo(cursor, info)) {
            // Unreachable daemon, or a hash it does not know (e.g. the node was
            // re-pointed at a different parent daemon). Indeterminate: keep
            // nothing.
            LogPrint(BCLog::NET, "Anchor: could not classify the parent chain tip move at %s; re-checking every anchor\n", cursor.ToString());
            return -1;
        }
        if (info.on_best_chain) {
            if (depth > 0) {
                LogPrintf("Anchor: parent chain reorganized %d block(s) deep, forking at height %d; anchor verdicts at or below that height are unaffected and are kept, everything above is re-checked\n",
                          depth, info.height);
            }
            return info.height;
        }
        if (info.prev.IsNull()) return -1; // walked off the bottom of the chain
        cursor = info.prev;
    }
    LogPrintf("Anchor: parent chain reorganized more than %d blocks deep; re-checking every anchor from scratch\n",
              ANCHOR_FORK_WALK_MAX);
    return -1;
}

//! Discard the anchor verdicts that the parent tip move could have changed:
//! those strictly above `unchanged_height`. A negative value discards both
//! caches entirely — the behaviour this file had for every tip change, now the
//! fallback for the moves that could not be classified.
void DropAnchorCachesAbove(int unchanged_height) EXCLUSIVE_LOCKS_REQUIRED(g_anchor_mutex)
{
    if (unchanged_height < 0) {
        g_anchor_ok_cache.clear();
        g_anchor_stale_cache.clear();
        return;
    }
    // Both caches are sets of {parent height, parent hash}, ordered by height
    // first, so the affected suffix is one contiguous range at the end.
    const std::pair<uint32_t, uint256> first_affected{(uint32_t)unchanged_height + 1, uint256()};
    g_anchor_ok_cache.erase(g_anchor_ok_cache.lower_bound(first_affected), g_anchor_ok_cache.end());
    g_anchor_stale_cache.erase(g_anchor_stale_cache.lower_bound(first_affected), g_anchor_stale_cache.end());
}

//! Query (cache-first) the parent chain daemon for a block's median-time-past.
//! MTP is immutable per hash, so a cached entry is served without RPC forever.
//! Returns OK with mtp set, NOT_FOUND when the daemon does not know the hash,
//! or NO_CONNECTION when the daemon is unreachable.
AnchorCheckResult GetMainchainMedianTime(const uint256& hash, int64_t& mtp)
{
    {
        LOCK(g_anchor_mutex);
        auto it = g_anchor_mtp_cache.find(hash);
        if (it != g_anchor_mtp_cache.end()) {
            mtp = it->second;
            return AnchorCheckResult::OK;
        }
    }
    try {
        UniValue params(UniValue::VARR);
        params.push_back(hash.GetHex());
        UniValue reply = CallMainChainRPC("getblockheader", params);
        UniValue errval = find_value(reply, "error");
        if (!errval.isNull()) return AnchorCheckResult::NOT_FOUND;
        UniValue result = find_value(reply, "result");
        if (!result.isObject()) return AnchorCheckResult::NOT_FOUND;
        UniValue mediantime = find_value(result.get_obj(), "mediantime");
        if (!mediantime.isNum()) return AnchorCheckResult::NOT_FOUND;
        mtp = mediantime.get_int64();
        LOCK(g_anchor_mutex);
        if (g_anchor_mtp_cache.size() >= ANCHOR_MTP_CACHE_MAX) g_anchor_mtp_cache.clear();
        g_anchor_mtp_cache.emplace(hash, mtp);
        return AnchorCheckResult::OK;
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "Could not reach mainchain daemon for mediantime of %s: %s\n", hash.ToString(), e.what());
        return AnchorCheckResult::NO_CONNECTION;
    }
}

//! Highest parent-chain height not contested by any live competing branch, via
//! getchaintips (block-producer anchor policy, Fix A). Parses the tips (skipping
//! our own active chain and daemon-rejected/invalid branches — neither is a
//! reorg threat; valid-fork/valid-headers/headers-only branches could still win)
//! and defers the selection math to AnchorUncontestedHeight. Returns false if
//! getchaintips is unavailable (caller then keeps the plain -anchorminconf
//! target). Never lowers below the previous anchor: that clamp is the caller's
//! (monotonicity).
bool GetMainchainUncontestedHeight(int active_tip_height, int& uncontested_height)
{
    const int window = (int)gArgs.GetIntArg("-anchorcontestwindow", DEFAULT_ANCHOR_CONTEST_WINDOW);
    try {
        UniValue reply = CallMainChainRPC("getchaintips", UniValue(UniValue::VARR));
        UniValue errval = find_value(reply, "error");
        if (!errval.isNull()) return false;
        UniValue result = find_value(reply, "result");
        if (!result.isArray()) return false;

        std::vector<std::pair<int, int>> competing; // {tip height, branchlen}
        for (size_t i = 0; i < result.size(); ++i) {
            const UniValue& tip = result[i];
            const UniValue& status = find_value(tip, "status");
            if (status.isStr() && (status.get_str() == "active" || status.get_str() == "invalid")) continue;
            const UniValue& h = find_value(tip, "height");
            const UniValue& bl = find_value(tip, "branchlen");
            if (!h.isNum() || !bl.isNum()) continue;
            competing.emplace_back(h.get_int(), bl.get_int());
        }
        uncontested_height = AnchorUncontestedHeight(active_tip_height, window, competing);
        return true;
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "Could not reach mainchain daemon for getchaintips: %s\n", e.what());
        return false;
    }
}

//! Defined below: walk newly-arrived parent blocks for checkpoints.
void ScanNewMainchainBlocks(ChainstateManager& chainman, const uint256& new_tip);
//! Defined below: recompute the checkpoint finality point and conflicts.
void UpdatePosFinality(ChainstateManager& chainman, int btc_tip_height);

} // namespace

int64_t g_pos_escape_stall_mtp_gap = DEFAULT_POS_ESCAPE_STALL_MTP_GAP;
bool g_pos_reconcile = true;
int64_t g_pos_reconcile_patience = DEFAULT_POS_RECONCILE_PATIENCE;
int g_pos_reconcile_min_depth = DEFAULT_POS_RECONCILE_MIN_DEPTH;

EscapeStallTimeVerdict CheckEscapingStallMtpGap(const uint256& parent_anchor_hash,
                                                const uint256& block_anchor_hash,
                                                int height,
                                                bool record_unverified)
{
    if (g_pos_escape_stall_mtp_gap <= 0) return EscapeStallTimeVerdict::ALLOWED;
    // Activation gate. The rule postdates part of some chains' history, so
    // below the activation height there is nothing to verify and nothing is
    // being delegated — return before the -validateanchor branch, or a node
    // would count acceptances for a rule that does not apply yet. Enforced
    // here rather than at the call sites so a future caller cannot omit it
    // and silently re-apply the rule retroactively (anchor.h).
    if (!PosEscapeStallMtpHeightActive(height)) {
        return EscapeStallTimeVerdict::ALLOWED;
    }
    // -validateanchor=0 delegates anchor validation to the network (the R3
    // skip); the MTP evidence rides on the same daemon, so it is delegated too.
    // Record it: this is a block certified by as little as one committee
    // member, accepted purely on the network's word (see UnverifiedEscapingStalls).
    if (!g_validate_anchor) {
        if (record_unverified && height >= 0) NoteUnverifiedEscapingStall(height);
        return EscapeStallTimeVerdict::ALLOWED;
    }
    // Chain bring-up: no anchored parent to measure from.
    if (parent_anchor_hash.IsNull() || block_anchor_hash.IsNull()) return EscapeStallTimeVerdict::ALLOWED;
    int64_t mtp_parent = 0, mtp_block = 0;
    switch (GetMainchainMedianTime(parent_anchor_hash, mtp_parent)) {
    case AnchorCheckResult::OK: break;
    default: return EscapeStallTimeVerdict::UNKNOWN;
    }
    switch (GetMainchainMedianTime(block_anchor_hash, mtp_block)) {
    case AnchorCheckResult::OK: break;
    default: return EscapeStallTimeVerdict::UNKNOWN;
    }
    return (mtp_block - mtp_parent >= g_pos_escape_stall_mtp_gap)
        ? EscapeStallTimeVerdict::ALLOWED : EscapeStallTimeVerdict::TOO_SOON;
}

bool MainchainReachable()
{
    int count{0};
    return GetMainchainBlockCount(count);
}

PosReconcileStatus GetPosReconcileStatus()
{
    LOCK(g_anchor_mutex);
    return g_reconcile_status;
}

//! Record a verdict, honouring the cache ceiling.
static void CacheAnchorVerdict(uint32_t height, const uint256& hash, AnchorCheckResult res)
    EXCLUSIVE_LOCKS_REQUIRED(!g_anchor_mutex)
{
    LOCK(g_anchor_mutex);
    if (res == AnchorCheckResult::OK) {
        if (g_anchor_ok_cache.size() < ANCHOR_OK_CACHE_MAX) {
            g_anchor_ok_cache.emplace(height, hash);
        } else {
            static bool warned = false;
            if (!warned) {
                warned = true;
                LogPrintf("WARNING: the canonical-anchor cache reached its %u-entry ceiling; anchors beyond it are re-checked against the parent chain daemon on every watcher tick, which is slow and noisy for that daemon (see ANCHOR_OK_CACHE_MAX in anchor.cpp)\n",
                          (unsigned)ANCHOR_OK_CACHE_MAX);
            }
        }
    } else if (res != AnchorCheckResult::NO_CONNECTION) {
        // A definitive off-best-chain verdict. NO_CONNECTION is not a verdict
        // and must never be memoized as one.
        g_anchor_stale_cache.emplace(height, hash);
    }
}

//! What one `getblockheader` reply says about one anchor.
//!
//! Shared by the single-call and batched paths so that the two can never come
//! to different conclusions about the same reply. Pure: it reads a reply and
//! returns a verdict, touching neither the network nor the caches.
AnchorCheckResult InterpretAnchorHeaderReply(const UniValue& reply, uint32_t height)
{
    const UniValue errval = find_value(reply, "error");
    if (!errval.isNull()) return AnchorCheckResult::NOT_FOUND;
    const UniValue result = find_value(reply, "result");
    if (!result.isObject()) return AnchorCheckResult::NOT_FOUND;
    const UniValue confirmations = find_value(result.get_obj(), "confirmations");
    // confirmations == -1 means the block is not on the best chain
    if (!confirmations.isNum() || confirmations.get_int64() < 1) return AnchorCheckResult::STALE;
    const UniValue blockheight = find_value(result.get_obj(), "height");
    if (!blockheight.isNum() || blockheight.get_int64() != (int64_t)height) {
        return AnchorCheckResult::HEIGHT_MISMATCH;
    }
    return AnchorCheckResult::OK;
}

void PrefetchAnchorVerdicts(const std::vector<std::pair<uint32_t, uint256>>& refs)
{
    // Ask the parent daemon about many anchors per round trip instead of one.
    //
    // Nothing about WHAT is verified changes here, which is the point: every
    // anchor is still asked about, from ground truth, on every tick, to any
    // depth. Only the number of TCP connections it takes to ask changes -- from
    // one per anchor to one per few hundred. On a cold cache over a long chain
    // that is the difference between minutes and seconds.
    //
    // Best-effort throughout. Anything that goes wrong here leaves the caches
    // exactly as they were and the per-anchor path below re-asks the daemon the
    // old way, so a batching failure costs speed and never correctness.
    constexpr size_t BATCH = 256;

    std::vector<std::pair<uint32_t, uint256>> want;
    {
        LOCK(g_anchor_mutex);
        for (const auto& r : refs) {
            if (g_anchor_ok_cache.count(r) || g_anchor_stale_cache.count(r)) continue;
            want.push_back(r);
        }
    }
    if (want.size() < 2) return;   // one question does not need a batch

    size_t done = 0;
    for (size_t i = 0; i < want.size(); i += BATCH) {
        if (ShutdownRequested()) return;
        const size_t n = std::min(BATCH, want.size() - i);
        std::vector<UniValue> params_list;
        params_list.reserve(n);
        for (size_t k = 0; k < n; ++k) {
            UniValue p(UniValue::VARR);
            p.push_back(want[i + k].second.GetHex());
            params_list.push_back(p);
        }
        try {
            const std::vector<UniValue> replies = CallMainChainRPCBatch("getblockheader", params_list);
            for (size_t k = 0; k < n; ++k) {
                CacheAnchorVerdict(want[i + k].first, want[i + k].second,
                                   InterpretAnchorHeaderReply(replies[k], want[i + k].first));
            }
            done += n;
        } catch (const std::exception& e) {
            LogPrintf("WARNING: batched anchor check failed after %u of %u (%s); falling back to one call per anchor\n",
                      (unsigned)done, (unsigned)want.size(), e.what());
            return;
        }
    }
    LogPrintf("Anchor watcher: fetched %u anchor verdicts in %u request(s)\n",
              (unsigned)done, (unsigned)((done + BATCH - 1) / BATCH));
}

AnchorCheckResult CheckMainchainAnchor(uint32_t height, const uint256& hash)
{
    {
        LOCK(g_anchor_mutex);
        if (g_anchor_ok_cache.count({height, hash})) return AnchorCheckResult::OK;
        // Negative cache: a definitively-off-best-chain anchor stays off until a
        // parent reorganization forking below it puts it back on the best chain,
        // and such a reorganization drops this entry (DropAnchorCachesAbove), so
        // while the entry is here it is still true and needs no RPC.
        if (g_anchor_stale_cache.count({height, hash})) return AnchorCheckResult::STALE;
    }
    try {
        UniValue params(UniValue::VARR);
        params.push_back(hash.GetHex());
        const UniValue reply = CallMainChainRPC("getblockheader", params);
        const AnchorCheckResult res = InterpretAnchorHeaderReply(reply, height);
        if (res != AnchorCheckResult::OK) {
            // Memoize a DEFINITIVE off-best-chain verdict so the every-tick
            // recovery loop does not re-ask about the same orphaned anchor for
            // as long as the parent chain leaves it orphaned.
            CacheAnchorVerdict(height, hash, res);
            return res;
        }
        CacheAnchorVerdict(height, hash, AnchorCheckResult::OK);
        return AnchorCheckResult::OK;
    } catch (const CConnectionFailed&) {
        LogPrintf("WARNING: lost connection to mainchain daemon while checking anchor %s\n", hash.ToString());
        return AnchorCheckResult::NO_CONNECTION;
    } catch (const std::exception& e) {
        LogPrintf("WARNING: error checking anchor %s against mainchain daemon: %s\n", hash.ToString(), e.what());
        return AnchorCheckResult::NO_CONNECTION;
    }
}

int AnchorUncontestedHeight(int active_tip_height, int window,
                            const std::vector<std::pair<int, int>>& competing_branches)
{
    const int w = std::max(0, window);
    int uncontested = active_tip_height;
    for (const auto& [tip_height, branchlen] : competing_branches) {
        if (branchlen <= 0) continue;                    // shares the active chain: not a fork
        if (tip_height + w < active_tip_height) continue; // further than the window behind: losing the race
        const int fork_point = tip_height - branchlen;    // last block still shared with the active chain
        if (fork_point < uncontested) uncontested = fork_point;
    }
    return uncontested;
}

bool GetAnchorForNewBlock(uint32_t prev_anchor_height, const uint256& prev_anchor_hash,
                          uint32_t& anchor_height, uint256& anchor_hash)
{
    const int min_conf = std::max<int64_t>(1, gArgs.GetIntArg("-anchorminconf", DEFAULT_ANCHOR_MIN_CONF));
    int count = 0;
    if (GetMainchainBlockCount(count)) {
        int target = count - (min_conf - 1);
        // Fix A (producer-side anti-contested-anchor policy): do not advance the
        // anchor onto a parent-chain height a competing branch is currently
        // contesting. Back the target down to the last block common to all live
        // rival branches, so a new Sequentia block anchors to Bitcoin ground
        // every current contender agrees on and needs no Sequentia reorg when the
        // parent fork resolves. Only ever LOWERS the target (never past the
        // previous anchor, enforced below), so it cannot break anchor
        // monotonicity; with no live fork the uncontested height equals the tip
        // and the target is unchanged (full anchor freshness). If getchaintips is
        // unavailable we keep the plain -anchorminconf target.
        if (gArgs.GetBoolArg("-anchoravoidcontested", DEFAULT_ANCHOR_AVOID_CONTESTED)) {
            int uncontested = -1;
            if (GetMainchainUncontestedHeight(count, uncontested) && uncontested >= 0 && uncontested < target) {
                LogPrintf("Anchor: parent chain height %d is contested; backing the new block's anchor down to the last uncontested height %d\n",
                          target, uncontested);
                target = uncontested;
            }
        }
        if (target >= 0 && (uint32_t)target >= prev_anchor_height) {
            uint256 hash;
            if (GetMainchainBlockHashAt(target, hash)) {
                anchor_height = (uint32_t)target;
                anchor_hash = hash;
                return true;
            }
        }
    }
    // Parent chain daemon unreachable (or behind the previous anchor, e.g.
    // while it is still syncing): fall back to the previous block's anchor,
    // which is monotone by construction and already validated.
    if (!prev_anchor_hash.IsNull()) {
        LogPrintf("WARNING: could not query mainchain daemon for a new anchor; reusing previous anchor %s (height %d)\n",
                  prev_anchor_hash.ToString(), prev_anchor_height);
        anchor_height = prev_anchor_height;
        anchor_hash = prev_anchor_hash;
        return true;
    }
    return false;
}

void SeedAnchorInvalidated(const std::vector<uint256>& block_hashes)
{
    if (block_hashes.empty()) return;
    LOCK(g_anchor_mutex);
    for (const uint256& h : block_hashes) g_anchor_invalidated.insert(h);
    LogPrintf("Anchor: seeded %u previously-invalidated block(s) from the block index for reorg-of-reorg recovery\n",
              (unsigned)block_hashes.size());
}

std::optional<uint256> AnchorCertifiedSiblingPending(ChainstateManager& chainman,
                                                     const uint256& tip_hash, int child_height)
{
    if (!g_con_pos || !g_con_bitcoin_anchor || !g_validate_anchor) return std::nullopt;
    AssertLockNotHeld(cs_main);
    AssertLockNotHeld(g_anchor_mutex);
    // Snapshot under g_anchor_mutex WITHOUT cs_main held (this file never nests
    // the two in that order; both sets are tiny). A stale snapshot is benign:
    // worst case one extra short hold, re-evaluated on the producer's next poll.
    std::set<uint256> invalidated;
    std::set<std::pair<uint32_t, uint256>> stale;
    {
        LOCK(g_anchor_mutex);
        if (g_anchor_invalidated.empty()) return std::nullopt;
        invalidated = g_anchor_invalidated;
        stale = g_anchor_stale_cache;
    }
    // Mirror UpdateTip's immediate-finality quorum exactly (incl. the
    // degenerate-size floor), so "guarded" == "could have been final".
    const int quorum = PosSlotQuorum(StakeRegistry::GetInstance());
    LOCK(cs_main);
    // Roots: recovery-set entries that could still be restored at/below our
    // height. Skip manual/consensus invalidations (failed WITHOUT the
    // watcher's provenance marker: they stay invalid, a rival there is
    // legitimate) and anchors confirmed off the parent's best chain and still
    // off it (a genuine departure: the height is truly vacant and production
    // must proceed). An un-failed root (verdict OK, reconnect still pending)
    // stays a root: the set holds entries until their branch actually
    // reconnects, so the whole un-fail -> reconnect window stays guarded.
    std::vector<const CBlockIndex*> roots;
    for (const uint256& hash : invalidated) {
        const CBlockIndex* p = chainman.m_blockman.LookupBlockIndex(hash);
        if (!p || p->nHeight > child_height) continue;
        const bool failed = p->nStatus & BLOCK_FAILED_MASK;
        if (failed && !(p->nStatus & BLOCK_FAILED_ANCHOR)) continue;
        if (stale.count({p->m_anchor_height, p->m_anchor_hash})) continue;
        roots.push_back(p);
    }
    if (roots.empty()) return std::nullopt;
    // The branch block that would occupy our height: a child of the current
    // tip descending from a root. Matching through the root covers the whole
    // recovery window: while the root awaits its verdict the child IS the
    // root; once the watcher un-fails the branch and ActivateBestChain is
    // reconnecting it block by block, the next branch block is a clean
    // (un-failed) child of the advancing tip and must stay protected until
    // it connects. Both index scans below run only inside a recovery window
    // (non-empty, non-stale set), never on the steady-state Step path.
    const CBlockIndex* target = nullptr;
    for (const auto& [hash, p] : chainman.m_blockman.m_block_index) {
        if (p->nHeight != child_height || !p->pprev || p->pprev->GetBlockHash() != tip_hash) continue;
        for (const CBlockIndex* root : roots) {
            if (p->GetAncestor(root->nHeight) == root) { target = p; break; }
        }
        if (target) break;
    }
    if (!target) return std::nullopt;
    // The certification that matters is the strongest at/above the vacant
    // height on the branch: the lowest orphaned block may itself be a
    // sub-quorum escaping-stall block with a quorum-certified DESCENDANT, and
    // that descendant is what recovery must protect (it held finality and may
    // carry e.g. an atomic-swap leg). A branch that is sub-quorum throughout
    // is deliberately not guarded: it never held finality, and the
    // countersignature comparator arbitrates rivals there.
    int best = target->m_pos_countersigs;
    if (best < quorum) {
        for (const auto& [hash, p] : chainman.m_blockman.m_block_index) {
            if (p->nHeight <= child_height || (int)p->m_pos_countersigs <= best) continue;
            if (p->GetAncestor(child_height) == target) best = p->m_pos_countersigs;
        }
    }
    if (best < quorum) return std::nullopt;
    return target->GetBlockHash();
}

//! Section 3 of the watcher: the PoS finality reconciliation monitor
//! (anchor.h; design doc anchor-reorg-of-reorg-recovery-design.md Change 4b;
//! incident 2026-07-17). Detects the "finality partition" state — the local
//! finalized branch abandoned by the committee while a rival quorum-certified,
//! anchor-settled branch grows — and releases the local finalized point for
//! that rival branch, letting ordinary fork choice adopt it. Runs in the
//! watcher thread: RPC verdicts are gathered outside cs_main, and the local
//! blocks are never invalidated (they were valid; they merely lost — they
//! become valid-but-inactive history).
static void MaybeReconcileFinality(ChainstateManager& chainman)
{
    if (!g_con_pos || !g_pos_reconcile) return;

    auto set_status = [](const char* state, int cert_h, const uint256& cert_hash, int64_t patience_left) {
        LOCK(g_anchor_mutex);
        g_reconcile_status.enabled = true;
        g_reconcile_status.state = state;
        g_reconcile_status.rival_cert_height = cert_h;
        g_reconcile_status.rival_cert_hash = cert_hash;
        g_reconcile_status.patience_remaining = patience_left;
    };
    const auto steady_now = []() {
        return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    // First pass after startup: arm the patience clock, so a restarted
    // (possibly already-pinned) node still waits the full patience.
    if (PosGetFinalAdvanceSteadyTime() == 0) {
        PosStampFinalAdvanceNow();
        set_status("inactive", -1, uint256(), 0);
        return;
    }

    int final_height = -1;
    uint256 final_hash;
    int cert_height = -1;
    uint256 cert_hash;
    uint32_t cert_anchor_height = 0;
    uint256 cert_anchor_hash;
    bool inactive = false;
    {
        LOCK(cs_main);
        if (!PosGetImmediateFinalPoint(final_height, final_hash)) {
            inactive = true; // no finality to release
        } else {
            const CBlockIndex* pf = chainman.m_blockman.LookupBlockIndex(final_hash);
            if (!pf || (pf->nStatus & BLOCK_FAILED_MASK) || !chainman.ActiveChain().Contains(pf)) {
                inactive = true; // the Bitcoin valve (sections 1-2) owns this case
            } else {
                const CBlockIndex* best = pindexBestHeader;
                if (!best || best->nHeight <= final_height) {
                    inactive = true;
                } else {
                    const CBlockIndex* anc = best->GetAncestor(final_height);
                    if (anc && anc->GetBlockHash() == final_hash) {
                        inactive = true; // best known header extends our own finalized chain
                    } else {
                        // Rival branch: its highest quorum-certified, non-failed
                        // block strictly above our finalized height.
                        const int quorum = PosSlotQuorum(StakeRegistry::GetInstance());
                        for (const CBlockIndex* p = best; p && p->nHeight > final_height; p = p->pprev) {
                            if ((p->nStatus & BLOCK_FAILED_MASK) || (int)p->m_pos_countersigs < quorum) continue;
                            cert_height = p->nHeight;
                            cert_hash = p->GetBlockHash();
                            cert_anchor_height = p->m_anchor_height;
                            cert_anchor_hash = p->m_anchor_hash;
                            break;
                        }
                    }
                }
            }
        }
    }
    if (inactive || cert_height < final_height + g_pos_reconcile_min_depth) {
        set_status("inactive", -1, uint256(), 0);
        return;
    }
    // Condition 4: our branch is provably abandoned — no quorum-certified
    // block has extended it for the whole patience window.
    const int64_t elapsed = steady_now() - PosGetFinalAdvanceSteadyTime();
    if (elapsed < g_pos_reconcile_patience) {
        set_status("tracking", cert_height, cert_hash, g_pos_reconcile_patience - elapsed);
        return;
    }
    // Condition 2, outside cs_main: the rival's certifying block must be
    // anchored on OUR parent best chain, at/below the currently uncontested
    // parent height — a release can never fire into a live parent-chain fork.
    if (CheckMainchainAnchor(cert_anchor_height, cert_anchor_hash) != AnchorCheckResult::OK) {
        set_status("tracking", cert_height, cert_hash, 0);
        return;
    }
    int btc_height = 0;
    if (!GetMainchainBlockCount(btc_height)) {
        set_status("tracking", cert_height, cert_hash, 0);
        return;
    }
    int uncontested = btc_height;
    if (!GetMainchainUncontestedHeight(btc_height, uncontested)) {
        set_status("tracking", cert_height, cert_hash, 0);
        return;
    }
    if ((int)cert_anchor_height > uncontested) {
        LogPrintf("PoS finality reconciliation: rival certified block %s (height %d) anchors at contested parent height %d (uncontested %d); waiting for the parent chain to settle\n",
                  cert_hash.ToString(), cert_height, cert_anchor_height, uncontested);
        set_status("tracking", cert_height, cert_hash, 0);
        return;
    }
    LogPrintf("PoS finality reconciliation: local finalized branch (height %d, %s) received no quorum-certified extension for %d s while a rival quorum-certified branch reached height %d (%s) with settled anchors; releasing local finality for the rival branch\n",
              final_height, final_hash.ToString(), (int)elapsed, cert_height, cert_hash.ToString());
    {
        LOCK(cs_main);
        PosSetReconcileRelease(cert_height, cert_hash);
        chainman.ActiveChainstate().ReaddBlockIndexCandidates();
    }
    set_status("released", cert_height, cert_hash, 0);
    BlockValidationState state;
    if (!chainman.ActiveChainstate().ActivateBestChain(state)) {
        LogPrintf("WARNING: ActivateBestChain failed after finality reconciliation release: %s\n", state.ToString());
    }
}

void AnchorWatchTask(ChainstateManager& chainman)
{
    if (!g_con_bitcoin_anchor || !g_validate_anchor) return;
    // Nothing worth starting on the way down; see the phase-2 verdict loop for
    // why abandoning a tick costs nothing.
    if (ShutdownRequested()) return;

    uint256 best;
    if (!GetMainchainBestBlockHash(best)) return;
    uint256 old_tip;
    bool tip_changed;
    {
        LOCK(g_anchor_mutex);
        old_tip = g_last_mainchain_tip;
        tip_changed = best != old_tip;
    }
    if (tip_changed) {
        // The parent chain moved, so some anchor verdicts may be stale: anchors
        // confirmed canonical may have been reorganized away, and orphaned ones
        // may be canonical again. Which ones, though, depends entirely on HOW
        // the parent moved, and the two cases are not remotely alike:
        //
        //   - a plain extension (the parent appended blocks; the overwhelming
        //     majority of moves, since Bitcoin produces a block every ~10
        //     minutes and never reorganizes most of them) changes nothing about
        //     the chain below the old tip, so every verdict already held is
        //     still correct and NOTHING needs re-checking;
        //   - a reorganization invalidates only what sits above the fork point.
        //
        // Discarding both caches wholesale on every move treated the first case
        // as if it were the second, and made every distinct anchor on the chain
        // cost a fresh RPC on the next tick — a per-parent-block cost growing
        // linearly with the age of the chain, on a walk that deliberately has no
        // depth floor. MainchainUnchangedHeight tells the two apart for one RPC
        // and keeps the verdicts the move provably cannot have touched; see
        // there for why that is sound, and note it drops re-verification only —
        // section 2 below still descends to height 1 every tick.
        //
        // The RPC is issued with no lock held: this file never calls the parent
        // daemon while holding g_anchor_mutex (or cs_main).
        const int unchanged_height = MainchainUnchangedHeight(old_tip);
        LOCK(g_anchor_mutex);
        g_last_mainchain_tip = best;
        DropAnchorCachesAbove(unchanged_height);
    }

    // PoS checkpoints (paper §11): scan new parent blocks for committed
    // Sequentia checkpoints when the parent moves, and re-evaluate
    // finality/conflicts on *every* tick — our own chain may have changed
    // (e.g. new blocks, a peer-fed fork) even when the parent has not.
    if (g_con_pos) {
        if (tip_changed) {
            ScanNewMainchainBlocks(chainman, best);
        } else {
            int last_height;
            {
                LOCK(g_anchor_mutex);
                last_height = g_last_btc_tip_height;
            }
            if (last_height >= 0) UpdatePosFinality(chainman, last_height);
        }
    }
    // 1) Reconsider blocks we invalidated earlier whose anchors are canonical
    //    again (the parent chain reorganized back — a reorg-of-reorg, common on
    //    testnet4). This runs whenever the recovery set is non-empty, NOT only on
    //    tip_changed: gating on tip_changed missed (a) a coalesced/missed parent
    //    flap where the parent went off then back within one poll so the tip
    //    looks unchanged, and (b) a restart, where the set is re-seeded from the
    //    persisted block index (SeedAnchorInvalidated, called by LoadBlockIndex)
    //    but the very first post-restart tick may or may not register as a tip
    //    change. The set holds only directly-invalidated blocks awaiting restore,
    //    so it is small and an empty set skips the work entirely. Reconsidering
    //    only ever CLEARS a block whose anchor returns OK on a live parent-chain
    //    check, so this never un-finalizes anything the canonical Bitcoin chain
    //    does not back; section 2 below re-derives bad-ness from ground truth
    //    every tick, so a block reconsidered just before its anchor re-orphans is
    //    re-invalidated on the next tick.
    {
        std::set<uint256> invalidated;
        {
            LOCK(g_anchor_mutex);
            invalidated = g_anchor_invalidated;
        }
        bool any_reconsidered = false;
        for (const uint256& hash : invalidated) {
            // Leaving early here is the same as the NO_CONNECTION break below:
            // the set is re-read from ground truth next tick, and anything not
            // reconsidered now is reconsidered then.
            if (ShutdownRequested()) break;
            CBlockIndex* pindex = nullptr;
            uint32_t anchor_height = 0;
            uint256 anchor_hash;
            bool connected = false;
            bool still_failed = false;
            {
                LOCK(cs_main);
                pindex = chainman.m_blockman.LookupBlockIndex(hash);
                if (pindex) {
                    anchor_height = pindex->m_anchor_height;
                    anchor_hash = pindex->m_anchor_hash;
                    connected = chainman.ActiveChain().Contains(pindex);
                    still_failed = pindex->nStatus & (BLOCK_FAILED_MASK | BLOCK_FAILED_ANCHOR);
                }
            }
            if (!pindex) {
                // The block index entry is gone; drop the stale hint. Done outside
                // cs_main so g_anchor_mutex and cs_main are never nested.
                LOCK(g_anchor_mutex);
                g_anchor_invalidated.erase(hash);
                continue;
            }
            if (connected) {
                // Fully recovered: the branch reconnected to the active chain.
                // Only NOW drop the hint — the certified-sibling guard (Change
                // 4a) keys on this set, so dropping it earlier (at reconsider
                // time) would unguard the still-vacant height for the window
                // between un-failing the branch and reconnecting its bodies.
                LOCK(g_anchor_mutex);
                g_anchor_invalidated.erase(hash);
                continue;
            }
            if (!still_failed) {
                // Already un-failed, awaiting reconnect (bodies may still be in
                // flight): nothing to re-check this tick. The entry deliberately
                // stays so the guard keeps holding until the branch connects.
                continue;
            }
            AnchorCheckResult res = CheckMainchainAnchor(anchor_height, anchor_hash);
            if (res == AnchorCheckResult::NO_CONNECTION) {
                // Parent daemon unreachable: cannot judge, retry next tick. Stop
                // rather than hammer a down/overloaded daemon (the documented
                // 'Work queue depth exceeded' stall vector).
                break;
            }
            if (res != AnchorCheckResult::OK) {
                // Still orphaned. The negative cache serves this without an RPC
                // for as long as it stays orphaned, so the every-tick scan stays
                // cheap; the reorganization that would rescue it is also the one
                // that drops the entry.
                continue;
            }
            LogPrintf("Anchor %s (height %d) of block %s is canonical again; reconsidering\n",
                      anchor_hash.ToString(), anchor_height, hash.ToString());
            {
                LOCK(cs_main);
                // ResetBlockFailureFlags also clears the BLOCK_FAILED_ANCHOR marker.
                chainman.ActiveChainstate().ResetBlockFailureFlags(pindex);
            }
            // The hint is NOT erased here: it lives until the branch is seen
            // connected (above), so the certified-sibling guard covers the
            // whole un-fail -> reconnect window.
            any_reconsidered = true;
        }
        if (any_reconsidered) {
            BlockValidationState state;
            if (!chainman.ActiveChainstate().ActivateBestChain(state)) {
                LogPrintf("WARNING: ActivateBestChain failed after anchor reconsideration: %s\n", state.ToString());
            }
            // Raise pindexBestHeader onto the recovered branch (once per batch) so
            // a branch known only as headers re-requests its bodies and reconnects,
            // re-finalizing the original blocks rather than minting fresh on top.
            LOCK(cs_main);
            chainman.ActiveChainstate().RecalculateBestHeader();
        }
    }

    // 2) Walk the active chain down from the tip looking for blocks whose
    //    anchors were reorganized away, and invalidate the LOWEST such block.
    //
    //    Anchor *heights* are monotone along the chain, but anchor *canonicality*
    //    is NOT: a low block can anchor to a Bitcoin block that is later orphaned
    //    while a higher block anchors to a still-canonical Bitcoin block on a
    //    different/newer parent branch (heights stay monotone, e.g. 140803 then
    //    140838). So a canonical tip does NOT imply the blocks below it are
    //    anchored canonically — we must NOT stop the walk at the first OK block.
    //    Doing so left the chain permanently wedged on a stale base: SEQ 1..4
    //    anchored to an orphaned parent block with canonical SEQ 5+ built on top,
    //    where the down-walk saw the canonical tip, broke, and never reached the
    //    stale low blocks. We instead descend to height 1 and track the lowest
    //    block whose anchor is off Bitcoin's best chain, then invalidate it:
    //    InvalidateBlock + ActivateBestChain disconnects it AND every block above
    //    it (including the canonical-anchor blocks built on the stale base), and
    //    the chain rebuilds on the parent's best chain.
    //
    //    This runs on EVERY tick, not only when the parent tip just changed: a
    //    block is invalid iff its anchor is off Bitcoin's best chain (doc 03
    //    §intro, §3), and the active tip could be stale on a tick where the
    //    parent tip did not change since the previous tick (e.g. the parent
    //    reorg was missed/coalesced, the node restarted onto an already-reorged
    //    parent, or a transiently-canonical anchor was cached OK and then went
    //    stale). Gating the walk on tip_changed left such a tip stuck forever.
    //
    //    Cost. Per the invariant the walk must reach ANY depth (doc 03 §intro/§3,
    //    doc 04 §6): there is deliberately NO depth floor and no reorg horizon,
    //    so it examines the whole active chain on every tick and always will.
    //    What it must NOT do is pay the parent chain daemon for that. Two things
    //    keep the price flat as the chain grows:
    //
    //      - runs of blocks sharing one anchor collapse below (~20 blocks per
    //        anchor at a 30-second block against a 10-minute parent), so the
    //        number of verdicts needed per tick is the number of DISTINCT
    //        anchors, not the number of blocks;
    //      - each distinct anchor's verdict is served from g_anchor_ok_cache,
    //        and that cache now survives a parent-chain extension instead of
    //        being emptied by it (see AnchorWatchTask's tip-change handling and
    //        MainchainUnchangedHeight). Only verdicts a parent reorganization
    //        could actually have changed are dropped and re-fetched.
    //
    //    So a steady-state tick costs no parent-chain RPC at all, and a tick on
    //    which the parent appended a block costs one. What it used to cost was
    //    one RPC per distinct anchor on the whole chain, every time the parent
    //    moved — thousands, growing linearly with the age of the chain, all of
    //    them re-asking about Bitcoin history that had not changed.
    //
    //    Two costs remain linear in chain length and are in-memory only: the
    //    index walk in phase 1 and the set lookups in phase 2. Should a chain
    //    ever grow long enough for those to matter, the way out is a watermark
    //    of "verified below here against a parent prefix that is still intact",
    //    invalidated exactly like the caches — again re-verification skipped,
    //    never verification. A depth floor remains forbidden.
    uint256 lowest_bad;
    while (true) {
        // Top of the loop only. Never between the InvalidateBlock and the
        // ActivateBestChain below: those two are one step, and a shutdown
        // wedged between them would leave a block invalidated with the chain
        // never reactivated onto its replacement.
        if (ShutdownRequested()) return;
        lowest_bad.SetNull();
        // Phase 1: snapshot the (immutable) anchor of each candidate block under
        // cs_main, top-down, down to height 1 — there is NO finality floor on
        // this walk. A block is valid iff its anchor is on Bitcoin's best chain,
        // to ANY depth (doc 03 §intro/§3, doc 04 §6); a checkpoint-finalized
        // block whose anchor was reorged away is just as invalid as any other,
        // and finality is always modulo a Bitcoin reorg. (The checkpoint floor
        // is a defense against SEQ-INTERNAL long-range forks, enforced at
        // accept time in ContextualCheckBlockHeader — it must never keep a block
        // whose Bitcoin anchor is off the best chain.) We do NOT call bitcoind
        // here: the RPC must not run under cs_main, or a slow/hung parent daemon
        // would stall the whole node (block processing, RPC, net) for the RPC
        // timeout.
        struct AnchorRef { uint256 block_hash; uint32_t anchor_height; uint256 anchor_hash; };
        std::vector<AnchorRef> to_check;
        {
            LOCK(cs_main);
            const CBlockIndex* pindex = chainman.ActiveChain().Tip();
            for (; pindex && pindex->nHeight > 0; pindex = pindex->pprev) {
                if (pindex->m_anchor_hash.IsNull()) break; // pre-anchor blocks
                // Collapse each run of blocks sharing one anchor down to the
                // LOWEST block of the run. Anchor heights are monotone along the
                // chain, so blocks with the same anchor are contiguous, and
                // descending means the last one seen in a run is its lowest.
                // Nothing is lost: the verdict is a property of the anchor, not
                // of the block, and the block this walk wants when an anchor is
                // bad is exactly the lowest one carrying it — invalidating that
                // one disconnects it and every block above. This is what keeps
                // the phase-2 verdict loop proportional to the number of
                // distinct anchors (~1 per parent block) rather than to the
                // number of Sequentia blocks (~20 per parent block).
                if (!to_check.empty() &&
                    to_check.back().anchor_height == pindex->m_anchor_height &&
                    to_check.back().anchor_hash == pindex->m_anchor_hash) {
                    to_check.back().block_hash = pindex->GetBlockHash();
                    continue;
                }
                to_check.push_back({pindex->GetBlockHash(), pindex->m_anchor_height, pindex->m_anchor_hash});
            }
        }
        // Phase 2: query bitcoind OUTSIDE cs_main. anchor_height/anchor_hash are
        // fixed per block, so the snapshot stays valid even if the SEQ tip moves
        // meanwhile; InvalidateBlock below re-looks-up by hash and the loop
        // re-evaluates, so the (pre-existing) snapshot→act gap is harmless.
        //
        // to_check is top-down (tip first, height 1 last), one entry per run of
        // blocks sharing an anchor. Descend the WHOLE chain — do NOT break on OK
        // (canonicality is not monotone, see above) — and remember the lowest
        // (deepest) block whose anchor is definitively off Bitcoin's best chain
        // (STALE/NOT_FOUND/HEIGHT_MISMATCH); since we overwrite lowest_bad as we
        // descend, the last bad we record is the lowest one.
        //
        // g_anchor_ok_cache can only ever mark an anchor OK, never bad, so the
        // question is whether it can hide a now-orphaned anchor from this walk.
        // It cannot. For an anchor at parent height h to stop being canonical,
        // the parent's best chain AT height h must change, which is a parent
        // reorganization forking strictly BELOW h — so the fork height computed
        // on the tip change is < h and DropAnchorCachesAbove removes that entry,
        // leaving the walk to re-ask the daemon. When the move cannot be
        // classified at all the whole cache goes. In every case a stale low
        // block reaches this loop as a live RPC.
        //
        // NO_CONNECTION partway is indeterminate, not a verdict, so we stop
        // descending (cannot judge the NO_CONNECTION block or anything below it).
        // Any stale block already recorded sits ABOVE the NO_CONNECTION block
        // (the walk is top-down, so it was found earlier/higher), and invalidating
        // a definitively off-best-chain block is always correct: InvalidateBlock
        // disconnects that block and everything ABOVE it, while the NO_CONNECTION
        // block (below it) stays connected and is simply re-judged next tick. If no
        // stale block was found before the NO_CONNECTION, there is nothing to act
        // on — bail and retry next tick.
        // One round trip per few hundred anchors instead of one per anchor.
        // Purely a transport change: the loop below still asks about every
        // entry, it simply finds most of the answers already in hand.
        {
            std::vector<std::pair<uint32_t, uint256>> refs;
            refs.reserve(to_check.size());
            for (const AnchorRef& ref : to_check) refs.emplace_back(ref.anchor_height, ref.anchor_hash);
            PrefetchAnchorVerdicts(refs);
        }

        for (const AnchorRef& ref : to_check) {
            // Abandon the tick if the node is going down. This loop is the one
            // place the watcher runs for minutes at a time: every verdict it
            // cannot serve from g_anchor_ok_cache is a round trip to the parent
            // daemon, and that cache lives in memory only -- so the FIRST tick
            // after any start asks about every distinct anchor on the whole
            // chain, thousands of them once the chain has any age.
            //
            // Shutdown joins this thread, so with no check here `sequentiad
            // stop` sat holding the datadir lock until the walk finished:
            // five minutes, measured, on a freshly synced 100k-block chain,
            // during which the GUI cannot open the datadir it just closed.
            //
            // Abandoning is safe, and is not even a new behaviour. The tick
            // re-derives every verdict from ground truth each time and carries
            // nothing across the point where it stops -- which is exactly why
            // the next line already abandons the walk on NO_CONNECTION. All that
            // is lost is work the next tick, or the next start, does again.
            if (ShutdownRequested()) return;
            AnchorCheckResult res = CheckMainchainAnchor(ref.anchor_height, ref.anchor_hash);
            if (res == AnchorCheckResult::NO_CONNECTION) break; // cannot judge deeper
            if (res != AnchorCheckResult::OK) lowest_bad = ref.block_hash;
        }
        if (lowest_bad.IsNull()) break; // quiet tick: fall through to section 3

        LogPrintf("Parent chain reorganization detected: invalidating block %s (and descendants) whose anchor is no longer canonical\n",
                  lowest_bad.ToString());
        CBlockIndex* pindex_bad;
        {
            LOCK(cs_main);
            pindex_bad = chainman.m_blockman.LookupBlockIndex(lowest_bad);
            if (!pindex_bad) return;
        }
        BlockValidationState state;
        if (!chainman.ActiveChainstate().InvalidateBlock(state, pindex_bad)) {
            LogPrintf("WARNING: failed to invalidate block %s after parent chain reorganization: %s\n",
                      lowest_bad.ToString(), state.ToString());
            return;
        }
        {
            LOCK(g_anchor_mutex);
            g_anchor_invalidated.insert(lowest_bad);
        }
        // Persist the provenance: tag this block as ANCHOR-invalidated (distinct
        // from `invalidateblock` / consensus failures) so the recovery worklist
        // can be re-seeded from the block index after a restart and ONLY anchor-
        // orphaned blocks are reconsidered. pindex_bad is stable across the lock gap.
        {
            LOCK(cs_main);
            chainman.ActiveChainstate().MarkAnchorInvalid(pindex_bad);
        }
        BlockValidationState abc_state;
        if (!chainman.ActiveChainstate().ActivateBestChain(abc_state)) {
            LogPrintf("WARNING: ActivateBestChain failed after anchor invalidation: %s\n", abc_state.ToString());
            return;
        }
        // Loop: the new tip may itself have a stale anchor (e.g. a competing
        // branch that anchored to the same reorganized-away parent block).
    }

    // 3) Finality reconciliation monitor (anchor.h, Change 4b): detect a local
    //    finalized branch abandoned by the committee and release it for the
    //    network's certified branch. Reached on quiet ticks (sections 1-2 found
    //    nothing to invalidate), which is exactly the partition's signature.
    MaybeReconcileFinality(chainman);

    // 4) Retry driver for parent-chain stalls (incident 2026-07-26). When
    //    ConnectTip cannot verify a block against the parent chain (bitcoind
    //    unreachable: pos-escape-stall-unverifiable), it deliberately makes no
    //    progress and sets fStall instead of failing the block — the block stays a
    //    clean, unmarked candidate so it can simply be retried once bitcoind
    //    answers again (validation.cpp). But that stall needs something to RE-DRIVE
    //    ActivateBestChain afterwards, and sections 1-3 above only call it when
    //    they themselves changed something. Without this, recovery would wait for
    //    the next block/header to arrive from a peer — which on a quiet or
    //    temporarily isolated node may be a long time, and is exactly the state a
    //    stalled staker is in (it is producing nothing and its peers are ahead).
    //    Calling it here, every tick, bounds recovery latency to
    //    -anchorpollinterval (default 5s) instead of leaving it to gossip timing.
    //    Cheap when there is nothing to do: ActivateBestChain returns immediately
    //    once pindexMostWork == m_chain.Tip(). Also safe against a hot loop: a
    //    still-unverifiable block re-stalls (fStall breaks both activation loops)
    //    rather than spinning, so the RPC load on a down/slow parent daemon is one
    //    attempt per tick, not one per CPU cycle.
    {
        BlockValidationState state;
        if (!chainman.ActiveChainstate().ActivateBestChain(state)) {
            LogPrintf("WARNING: ActivateBestChain failed on anchor watcher retry tick: %s\n", state.ToString());
        }
    }
}

// --- Bitcoin checkpoints against PoS long-range attacks (paper §11) ---

std::vector<unsigned char> BuildCheckpointPayload(const uint256& block_hash, uint32_t height)
{
    std::vector<unsigned char> payload(POS_CKPT_TAG, POS_CKPT_TAG + sizeof(POS_CKPT_TAG));
    payload.insert(payload.end(), block_hash.begin(), block_hash.end());
    payload.push_back((unsigned char)(height & 0xff));
    payload.push_back((unsigned char)((height >> 8) & 0xff));
    payload.push_back((unsigned char)((height >> 16) & 0xff));
    payload.push_back((unsigned char)((height >> 24) & 0xff));
    return payload;
}

std::optional<std::pair<uint256, uint32_t>> ParseCheckpointPayload(const std::vector<unsigned char>& payload)
{
    if (payload.size() != sizeof(POS_CKPT_TAG) + 32 + 4) return std::nullopt;
    if (!std::equal(POS_CKPT_TAG, POS_CKPT_TAG + sizeof(POS_CKPT_TAG), payload.begin())) return std::nullopt;
    uint256 hash;
    std::copy(payload.begin() + sizeof(POS_CKPT_TAG), payload.begin() + sizeof(POS_CKPT_TAG) + 32, hash.begin());
    const unsigned char* h = payload.data() + sizeof(POS_CKPT_TAG) + 32;
    uint32_t height = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    return std::make_pair(hash, height);
}

bool GetPosFinalizedCheckpoint(int& height, uint256& hash)
{
    LOCK(g_anchor_mutex);
    if (g_pos_finalized_height < 0) return false;
    height = g_pos_finalized_height;
    hash = g_pos_finalized_hash;
    return true;
}

std::vector<PosCheckpoint> GetPosCheckpoints()
{
    LOCK(g_anchor_mutex);
    std::vector<PosCheckpoint> out;
    out.reserve(g_pos_checkpoints.size());
    for (const auto& e : g_pos_checkpoints) out.push_back(e.second);
    return out;
}

std::vector<PosCheckpoint> GetPosCheckpointConflicts()
{
    LOCK(g_anchor_mutex);
    return g_pos_checkpoint_conflicts;
}

// ClearConfiguredPosCheckpoints / AddConfiguredPosCheckpoint /
// GetConfiguredPosCheckpoints now live in pos.cpp (common layer).

namespace {
//! SEQUENTIA: what a wallet asked to hear about on the parent chain, and how often
//! it has been touched. Guarded rather than atomic-per-container because the sets
//! are replaced wholesale after each successful scan, while the walk reads them for
//! every output of every new block.
Mutex g_parent_watch_mutex;
std::set<std::vector<unsigned char>> g_watched_scripts GUARDED_BY(g_parent_watch_mutex);
std::set<std::pair<uint256, uint32_t>> g_watched_outpoints GUARDED_BY(g_parent_watch_mutex);
std::atomic<uint64_t> g_parent_watch_touches{0};

bool IsWatchedScript(const CScript& script)
{
    const std::vector<unsigned char> raw(script.begin(), script.end());
    LOCK(g_parent_watch_mutex);
    return g_watched_scripts.count(raw) > 0;
}

bool IsWatchedOutpoint(const uint256& txid, uint32_t vout)
{
    LOCK(g_parent_watch_mutex);
    return g_watched_outpoints.count(std::make_pair(txid, vout)) > 0;
}
} // namespace

void SetWatchedParentOutputs(std::set<std::vector<unsigned char>> scripts,
                             std::set<std::pair<uint256, uint32_t>> outpoints)
{
    LOCK(g_parent_watch_mutex);
    g_watched_scripts = std::move(scripts);
    g_watched_outpoints = std::move(outpoints);
}

uint64_t GetParentWatchTouches() { return g_parent_watch_touches.load(); }

void NoteParentWatchTouch() { g_parent_watch_touches.fetch_add(1); }

//! Record the checkpoint in one output script, if it holds one.
//!
//! The single place that decides what a checkpoint is, so that the raw and JSON
//! paths cannot come to different answers about the same output.
void RecordCheckpointIfPresent(const CScript& script, int btc_height, const uint256& btc_hash)
{
    // SEQUENTIA: a wallet balance on the parent chain rides on this same walk. The
    // node reads every new parent block already; asking whether an output pays a
    // watched script is a set lookup on work being done anyway, and it belongs
    // HERE, at the one place both the raw and the JSON path agree on -- put in
    // either path alone it would answer differently depending on which daemon is
    // on the other end. Before the OP_RETURN test, because a payment to us is an
    // ordinary output and that test would discard it.
    if (IsWatchedScript(script)) g_parent_watch_touches.fetch_add(1);

    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) return;
    if (!script.GetOp(pc, opcode, data)) return;
    auto parsed = ParseCheckpointPayload(data);
    if (!parsed) return;

    LOCK(g_anchor_mutex);
    // Keep the earliest commitment for a given block.
    auto it = g_pos_checkpoints.find(parsed->first);
    if (it == g_pos_checkpoints.end() || it->second.btc_height > btc_height) {
        g_pos_checkpoints[parsed->first] = PosCheckpoint{parsed->first, parsed->second, btc_height, btc_hash};
        LogPrintf("PoS: observed checkpoint for block %s (height %u) committed in parent block %s (height %d)\n",
                  parsed->first.ToString(), parsed->second, btc_hash.ToString(), btc_height);
    }
}

//! Record every tagged checkpoint OP_RETURN in one already-parsed parent block.
//!
//! At namespace scope, and declared in the header, because the parsing that
//! feeds it moved out of the parent daemon and into this file -- which is worth
//! a test that can hand it a real block directly.
void ScanRawBlockForCheckpoints(const Sidechain::Bitcoin::CBlock& block, int btc_height,
                                const uint256& btc_hash)
{
    // Unchanged from the day it was written: walk every output, take the ones
    // that are OP_RETURN followed by a push, and keep whatever parses as a
    // checkpoint. Only the route the script took to get here is new.
    for (const auto& tx : block.vtx) {
        if (!tx) continue;
        // SEQUENTIA: spending a watched coin moves a wallet balance as much as
        // receiving one, and nothing else can see it happen -- the output simply
        // stops existing.
        for (const auto& in : tx->vin) {
            if (IsWatchedOutpoint(in.prevout.hash, in.prevout.n)) g_parent_watch_touches.fetch_add(1);
        }
        for (const auto& out : tx->vout) RecordCheckpointIfPresent(out.scriptPubKey, btc_height, btc_hash);
    }
}
namespace {

//! Does this parent daemon serve blocks THIS node can parse?
//!
//! Not every parent is bitcoind. The functional tests drive an Elements-mode
//! node as the parent, and its raw block serialization is not Bitcoin's, so
//! taking one apart here fails outright. One failed parse is enough to know
//! that for the rest of the run: it is a property of which daemon is on the
//! other end, and that does not change under us.
std::atomic<bool> g_parent_blocks_are_bitcoin{true};

//! The old path, and still the general one: ask the daemon to decode the block
//! and read the output scripts out of the JSON. Works against ANY
//! Bitcoin-RPC-compatible daemon whatever its serialization, which is exactly
//! why it is kept.
bool ScanBlockViaJson(const uint256& btc_hash, int btc_height, uint256& prev_hash_out)
{
    try {
        UniValue params(UniValue::VARR);
        params.push_back(btc_hash.GetHex());
        params.push_back(2);
        const UniValue reply = CallMainChainRPC("getblock", params);
        if (!find_value(reply, "error").isNull()) return false;
        const UniValue result = find_value(reply, "result");
        if (!result.isObject()) return false;
        const UniValue prev = find_value(result, "previousblockhash");
        prev_hash_out = prev.isStr() ? uint256S(prev.get_str()) : uint256();

        const UniValue& txs = find_value(result, "tx").get_array();
        for (size_t i = 0; i < txs.size(); ++i) {
            const UniValue& vouts = find_value(txs[i], "vout").get_array();
            for (size_t j = 0; j < vouts.size(); ++j) {
                const UniValue& spk = find_value(vouts[j], "scriptPubKey");
                if (!spk.isObject()) continue;
                const UniValue& hexval = find_value(spk, "hex");
                if (!hexval.isStr()) continue;
                const std::vector<unsigned char> raw = ParseHex(hexval.get_str());
                RecordCheckpointIfPresent(CScript(raw.begin(), raw.end()), btc_height, btc_hash);
            }
        }
        return true;
    } catch (const std::exception& e) {
        LogPrintf("WARNING: checkpoint scan of parent block %s failed: %s\n", btc_hash.ToString(), e.what());
        return false;
    }
}

//! The cheap path: fetch the block raw and take it apart here.
//!
//! Verbosity 2 makes the parent daemon decode a whole block -- every input,
//! every witness, every address -- so that this can look at the one thing it
//! cares about, the output scripts. Measured on testnet4 the JSON runs over
//! three times the size of the block itself, and the daemon does that work per
//! block, on a scan that pulls a hundred of them the first time it runs.
//!
//! The block's own hash is what makes this safe to prefer. A block that
//! deserializes AND hashes to the hash we asked for was parsed correctly; one
//! that does not, however plausibly it parsed, is thrown away and the JSON path
//! answers instead. A wrong parse cannot quietly become a wrong checkpoint,
//! because nothing is recorded until after that check.
//!
//! What the raw block does not carry is its height -- that is chain context,
//! not block data. The caller has it: the walk is contiguous ancestry, so one
//! header lookup at the tip fixes every height below it.
bool ScanBlockViaRaw(const uint256& btc_hash, int btc_height, uint256& prev_hash_out)
{
    if (!g_parent_blocks_are_bitcoin.load()) return false;
    UniValue result;
    try {
        UniValue params(UniValue::VARR);
        params.push_back(btc_hash.GetHex());
        params.push_back(0);
        const UniValue reply = CallMainChainRPC("getblock", params);
        if (!find_value(reply, "error").isNull()) return false;   // an RPC problem, not a format one
        result = find_value(reply, "result");
        if (!result.isStr() || !IsHex(result.get_str())) return false;
    } catch (const std::exception&) {
        return false;   // ditto: let the caller fall back, but keep trying raw
    }

    try {
        const std::vector<unsigned char> raw = ParseHex(result.get_str());
        CDataStream ss(raw, SER_NETWORK, PROTOCOL_VERSION);
        Sidechain::Bitcoin::CBlock block;
        ss >> block;
        if (block.GetHash() != btc_hash) throw std::runtime_error("hash mismatch");

        prev_hash_out = block.hashPrevBlock;
        ScanRawBlockForCheckpoints(block, btc_height, btc_hash);
        return true;
    } catch (const std::exception& e) {
        // A parse failure says what the parent daemon IS, so say it once and
        // stop asking. Everything from here uses the JSON path.
        if (g_parent_blocks_are_bitcoin.exchange(false)) {
            LogPrintf(/* Continued */ "The parent daemon does not serve Bitcoin-serialized blocks (%s); "
                      "reading parent blocks as JSON from now on\n", e.what());
        }
        return false;
    }
}

bool ScanMainchainBlockForCheckpoints(const uint256& btc_hash, int btc_height, uint256& prev_hash_out)
{
    if (ScanBlockViaRaw(btc_hash, btc_height, prev_hash_out)) return true;
    return ScanBlockViaJson(btc_hash, btc_height, prev_hash_out);
}

//! Recompute the finality point: the highest checkpointed block that is on
//! our active chain at the claimed height, whose commitment is still on the
//! parent chain's best chain and buried at least -poscheckpointdepth deep.
void UpdatePosFinality(ChainstateManager& chainman, int btc_tip_height)
{
    const int depth = (int)gArgs.GetIntArg("-poscheckpointdepth", DEFAULT_POS_CHECKPOINT_DEPTH);
    if (depth <= 0) return;

    // Snapshot the current finality point so a transient parent-RPC outage
    // (NO_CONNECTION) does not spuriously retreat it below: the floor must only
    // retreat when a commitment is DEFINITIVELY off Bitcoin's best chain
    // (STALE/NOT_FOUND/HEIGHT_MISMATCH), not when we merely cannot reach Bitcoin
    // to judge it right now.
    int prev_fin_height;
    uint256 prev_fin_hash;
    {
        LOCK(g_anchor_mutex);
        prev_fin_height = g_pos_finalized_height;
        prev_fin_hash = g_pos_finalized_hash;
    }

    std::vector<PosCheckpoint> candidates = GetPosCheckpoints();
    int best_height = -1;
    uint256 best_hash;
    bool finalized_indeterminate = false; // current floor's commitment unjudgeable now
    std::vector<PosCheckpoint> conflicts;
    for (const PosCheckpoint& ckpt : candidates) {
        if (btc_tip_height - ckpt.btc_height + 1 < depth) continue; // not buried enough
        // The commitment must still be on the parent chain's best chain.
        AnchorCheckResult commit_res = CheckMainchainAnchor((uint32_t)ckpt.btc_height, ckpt.btc_hash);
        if (commit_res != AnchorCheckResult::OK) {
            // If we cannot reach Bitcoin to judge the checkpoint that is
            // currently holding the floor, remember that so we hold (rather
            // than drop) the floor below; a definitive STALE/NOT_FOUND instead
            // correctly lets the floor retreat.
            if (commit_res == AnchorCheckResult::NO_CONNECTION &&
                prev_fin_height >= 0 && (int)ckpt.seq_height == prev_fin_height &&
                ckpt.seq_hash == prev_fin_hash) {
                finalized_indeterminate = true;
            }
            continue;
        }
        // The checkpointed block must be on *our* active chain at the claimed
        // height: checkpoints lock in validated history, never replace it.
        bool on_active_chain = false;
        bool chain_reached_height = false;
        {
            LOCK(cs_main);
            const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(ckpt.seq_hash);
            on_active_chain = pindex != nullptr && (uint32_t)pindex->nHeight == ckpt.seq_height &&
                              chainman.ActiveChain().Contains(pindex);
            const CBlockIndex* tip = chainman.ActiveChain().Tip();
            chain_reached_height = tip != nullptr && (uint32_t)tip->nHeight >= ckpt.seq_height;
        }
        if (on_active_chain) {
            if ((int)ckpt.seq_height > best_height) {
                best_height = (int)ckpt.seq_height;
                best_hash = ckpt.seq_hash;
            }
        } else if (chain_reached_height) {
            // Fresh-sync / long-range alarm: a buried, parent-canonical
            // checkpoint commits a block we do NOT have at a height our chain
            // already passed. Either we are on the losing side of a
            // long-range fork, or someone checkpointed a bogus block; the
            // node cannot distinguish these alone, so it must surface it
            // (getcheckpointinfo "conflicts") for operator attention.
            conflicts.push_back(ckpt);
        }
    }
    bool conflicts_changed;
    bool have_own_checkpoint;
    {
        LOCK(g_anchor_mutex);
        // Compare contents, not just cardinality: one conflict replaced by
        // another must re-raise the operator alarm below.
        conflicts_changed = conflicts.size() != g_pos_checkpoint_conflicts.size() ||
            !std::equal(conflicts.begin(), conflicts.end(), g_pos_checkpoint_conflicts.begin(),
                        [](const PosCheckpoint& a, const PosCheckpoint& b) {
                            return a.seq_hash == b.seq_hash && a.seq_height == b.seq_height;
                        });
        g_pos_checkpoint_conflicts = conflicts;
        // best_height/best_hash is the highest checkpoint that is STILL valid on
        // this pass: buried >= depth, its commitment still on Bitcoin's best
        // chain, and its block on our active chain. Track it exactly — the
        // finalized point may RISE (a new checkpoint buried) or RETREAT (a
        // previously-finalizing checkpoint whose own Bitcoin commitment was
        // reorged away, or whose block left our active chain because a Bitcoin
        // reorg invalidated its anchor). Finality is always modulo a Bitcoin
        // reorg (doc 04 §6): a checkpoint can never hold the floor once its
        // commitment is off Bitcoin's best chain, otherwise it would keep a
        // block whose anchor is orphaned, violating the core invariant.
        //
        // The one exception is a RETREAT that would be caused only by an
        // inability to reach Bitcoin (NO_CONNECTION) for the checkpoint that is
        // currently holding the floor: that is indeterminate, not a definitive
        // "off the best chain", so we hold the existing floor until we can judge
        // it (it self-corrects on the next reachable tick). A RISE is always
        // applied; a retreat caused by a definitive STALE/NOT_FOUND is applied.
        const bool would_retreat = best_height < g_pos_finalized_height;
        const bool hold_floor = would_retreat && finalized_indeterminate &&
                                g_pos_finalized_height == prev_fin_height &&
                                g_pos_finalized_hash == prev_fin_hash;
        if (!hold_floor && (best_height != g_pos_finalized_height || best_hash != g_pos_finalized_hash)) {
            if (best_height > g_pos_finalized_height) {
                LogPrintf("PoS: finalized block %s (height %d) via parent-chain checkpoint\n",
                          best_hash.ToString(), best_height);
            } else {
                LogPrintf("PoS: checkpoint finality retreated to height %d (was %d) — a checkpoint's parent-chain commitment is no longer canonical (Bitcoin reorg)\n",
                          best_height, g_pos_finalized_height);
            }
            g_pos_finalized_height = best_height;
            g_pos_finalized_hash = best_hash;
        }
        have_own_checkpoint = best_height >= 0 || g_pos_finalized_height >= 0;
    }
    (void)have_own_checkpoint;
    if (conflicts_changed && !conflicts.empty()) {
        for (const PosCheckpoint& c : conflicts) {
            LogPrintf("WARNING: PoS: parent-chain checkpoint commits block %s at height %u which is NOT on this node's active chain — this node may be on the losing side of a long-range fork. Investigate before trusting recent history.\n",
                      c.seq_hash.ToString(), c.seq_height);
        }
    }
    // Automatic fresh-sync chain *selection* (reorganizing a longer
    // non-checkpointed active chain onto a checkpointed branch) is deferred:
    // a node on a longer fork generally has not downloaded the shorter
    // checkpointed branch's block bodies, so selection must first drive their
    // fetch+validation — a block-download change beyond this watcher. The
    // conflict alarm above is the safe, implemented behavior: the node never
    // silently follows a checkpoint it cannot validate, and surfaces the
    // ambiguity for operator action. See doc/sequentia/06 §11.
}

//! Walk newly-arrived parent blocks (back to the last scanned tip, bounded by
//! the scan window) and feed them to the checkpoint scanner.
//! How a pass over the checkpoint window ended.
//!
//! The distinction matters more than it looks: only a COMPLETE pass may move
//! the scan cursor. Moving it after a partial one records the window as read
//! when most of it was not, and since the next tick starts at the new tip and
//! stops at the cursor, whatever was missed is missed for good.
enum class WindowScan {
    Complete,      //!< every block from the tip down to the cursor was scanned
    Interrupted,   //!< stopped part way; leave the cursor alone and try again
};

//! One block at a time, following each block's hashPrevBlock down.
//!
//! Sequential because it must be: it cannot ask for a block until the previous
//! one has said which came before it. Fetching the window by HEIGHT instead
//! would make it batchable, and that was built and measured -- against a parent
//! on loopback, which is what every node running beside its own bitcoind has,
//! a hundred blocks takes 0.83s sequentially and 0.18s batched. Neither is
//! worth the code that proves a height-addressed window really is the ancestry
//! of the tip it claims to descend from.
//!
//! (Where this DOES take tens of seconds, the constraint is bandwidth and not
//! round trips: the window is a few megabytes of block data, and a node reading
//! its parent over a slow link pays for the megabytes however they are asked
//! for. Batching does not move that.)
WindowScan ScanWindowSequential(const uint256& new_tip, int tip_height, int window,
                                const uint256& last_scanned)
{
    uint256 cursor = new_tip;
    for (int i = 0; i < window && !cursor.IsNull() && cursor != last_scanned; ++i) {
        if (ShutdownRequested()) return WindowScan::Interrupted;
        uint256 prev;
        if (!ScanMainchainBlockForCheckpoints(cursor, tip_height - i, prev)) {
            return WindowScan::Interrupted;
        }
        cursor = prev;
    }
    return WindowScan::Complete;
}

void ScanNewMainchainBlocks(ChainstateManager& chainman, const uint256& new_tip)
{
    uint256 last_scanned;
    {
        LOCK(g_anchor_mutex);
        last_scanned = g_last_checkpoint_scan_tip;
    }
    const int window = (int)gArgs.GetIntArg("-poscheckpointscan", DEFAULT_POS_CHECKPOINT_SCAN);

    // One header lookup fixes every height in the window. A raw block does not
    // carry its own height -- that is chain context, not block data -- and the
    // window descends contiguous ancestry, so subtracting is exact.
    int tip_height = -1;
    try {
        UniValue params(UniValue::VARR);
        params.push_back(new_tip.GetHex());
        const UniValue reply = CallMainChainRPC("getblockheader", params);
        if (find_value(reply, "error").isNull()) {
            const UniValue result = find_value(reply, "result");
            const UniValue h = result.isObject() ? find_value(result, "height") : NullUniValue;
            if (h.isNum()) tip_height = (int)h.get_int64();
        }
    } catch (const std::exception& e) {
        LogPrintf("WARNING: could not read the height of parent tip %s: %s\n", new_tip.ToString(), e.what());
    }
    if (tip_height < 0) return;   // cannot place the window; try again next tick

    const WindowScan outcome = ScanWindowSequential(new_tip, tip_height, window, last_scanned);

    {
        LOCK(g_anchor_mutex);
        // ONLY a complete pass moves the cursor. A pass cut short -- by
        // shutdown, by a parent that stopped answering, by a window that could
        // not be proven to be one chain -- must leave it where it was, or the
        // blocks it never reached are recorded as read and their checkpoints
        // are lost: the next tick starts at the new tip and stops here. The
        // cost of being wrong in the other direction is merely rescanning.
        if (outcome == WindowScan::Complete) g_last_checkpoint_scan_tip = new_tip;
        g_last_btc_tip_height = tip_height;
    }
    UpdatePosFinality(chainman, tip_height);
}

} // namespace
