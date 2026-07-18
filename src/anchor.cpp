// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <anchor.h>

#include <chain.h>
#include <logging.h>
#include <mainchainrpc.h>
#include <pos.h>
#include <script/script.h>
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
// the common layer (pos.cpp) so chainparams.cpp / elements-tx can link them.

const unsigned char POS_CKPT_TAG[7] = {'S', 'E', 'Q', 'C', 'K', 'P', 'T'};
//! Anchors confirmed to be on the parent chain's best chain. Cleared whenever
//! the parent chain tip changes, since a reorganization can make them stale.
std::set<std::pair<uint32_t, uint256>> g_anchor_ok_cache GUARDED_BY(g_anchor_mutex);
//! Anchors confirmed DEFINITIVELY off the parent chain's best chain
//! (STALE/NOT_FOUND/HEIGHT_MISMATCH — never NO_CONNECTION, which is
//! indeterminate). Like the OK cache it is cleared on every parent tip change,
//! since only a parent move can turn a stale anchor canonical again. This lets
//! the recovery loop run every tick without re-hitting bitcoind for the same
//! permanently-orphaned anchors: it RPC-checks each anchor at most once per
//! parent-tip epoch instead of every poll, avoiding a self-inflicted RPC storm.
std::set<std::pair<uint32_t, uint256>> g_anchor_stale_cache GUARDED_BY(g_anchor_mutex);
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
                                                const uint256& block_anchor_hash)
{
    if (g_pos_escape_stall_mtp_gap <= 0) return EscapeStallTimeVerdict::ALLOWED;
    // -validateanchor=0 delegates anchor validation to the network (the R3
    // skip); the MTP evidence rides on the same daemon, so it is delegated too.
    if (!g_validate_anchor) return EscapeStallTimeVerdict::ALLOWED;
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

PosReconcileStatus GetPosReconcileStatus()
{
    LOCK(g_anchor_mutex);
    return g_reconcile_status;
}

AnchorCheckResult CheckMainchainAnchor(uint32_t height, const uint256& hash)
{
    {
        LOCK(g_anchor_mutex);
        if (g_anchor_ok_cache.count({height, hash})) return AnchorCheckResult::OK;
        // Negative cache: a definitively-off-best-chain anchor stays off until the
        // parent tip moves (which clears this cache), so serve it without an RPC.
        if (g_anchor_stale_cache.count({height, hash})) return AnchorCheckResult::STALE;
    }
    // Memoize a DEFINITIVE off-best-chain verdict (not NO_CONNECTION) so the
    // every-tick recovery loop does not re-RPC the same orphaned anchor until the
    // parent tip moves (which clears the cache).
    auto cache_stale = [&](AnchorCheckResult r) {
        LOCK(g_anchor_mutex);
        g_anchor_stale_cache.emplace(height, hash);
        return r;
    };
    try {
        UniValue params(UniValue::VARR);
        params.push_back(hash.GetHex());
        UniValue reply = CallMainChainRPC("getblockheader", params);
        UniValue errval = find_value(reply, "error");
        if (!errval.isNull()) {
            return cache_stale(AnchorCheckResult::NOT_FOUND);
        }
        UniValue result = find_value(reply, "result");
        if (!result.isObject()) {
            return cache_stale(AnchorCheckResult::NOT_FOUND);
        }
        UniValue confirmations = find_value(result.get_obj(), "confirmations");
        if (!confirmations.isNum() || confirmations.get_int64() < 1) {
            // confirmations == -1 means the block is not on the best chain
            return cache_stale(AnchorCheckResult::STALE);
        }
        UniValue blockheight = find_value(result.get_obj(), "height");
        if (!blockheight.isNum() || blockheight.get_int64() != (int64_t)height) {
            return cache_stale(AnchorCheckResult::HEIGHT_MISMATCH);
        }
        LOCK(g_anchor_mutex);
        g_anchor_ok_cache.emplace(height, hash);
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
    // legitimate) and anchors confirmed off the parent's best chain this
    // epoch (a genuine departure: the height is truly vacant and production
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

    uint256 best;
    if (!GetMainchainBestBlockHash(best)) return;
    bool tip_changed;
    {
        LOCK(g_anchor_mutex);
        tip_changed = best != g_last_mainchain_tip;
        if (tip_changed) {
            g_last_mainchain_tip = best;
            // The parent chain moved: previously confirmed anchors may have been
            // reorganized away (drop the OK cache) and previously-orphaned anchors
            // may be canonical again (drop the negative cache), so re-check both.
            g_anchor_ok_cache.clear();
            g_anchor_stale_cache.clear();
        }
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
                // until the parent tip moves, so the every-tick scan stays cheap.
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
    //    Cost: with no break-on-OK the walk examines every anchored block on the
    //    active chain each tick. Canonical anchors are served from
    //    g_anchor_ok_cache (a single in-memory set lookup, no RPC), so on a quiet
    //    tick where nothing changed every entry is a cache hit and the walk is
    //    cheap; only blocks whose anchor is not (yet) cached OK cost a bitcoind
    //    RPC. Per the invariant the walk must reach ANY depth (doc 03 §intro/§3,
    //    doc 04 §6) — there is deliberately NO depth floor or reorg horizon. For a
    //    very long chain the per-tick RPC cost of re-checking not-yet-cached
    //    entries could be bounded by re-checking only entries above the parent
    //    reorg's fork height (instead of clearing the whole cache on tip_changed);
    //    that is a future optimization and must not introduce a correctness floor.
    uint256 lowest_bad;
    while (true) {
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
                to_check.push_back({pindex->GetBlockHash(), pindex->m_anchor_height, pindex->m_anchor_hash});
            }
        }
        // Phase 2: query bitcoind OUTSIDE cs_main. anchor_height/anchor_hash are
        // fixed per block, so the snapshot stays valid even if the SEQ tip moves
        // meanwhile; InvalidateBlock below re-looks-up by hash and the loop
        // re-evaluates, so the (pre-existing) snapshot→act gap is harmless.
        //
        // to_check is top-down (tip first, height 1 last). Descend the WHOLE
        // chain — do NOT break on OK (canonicality is not monotone, see above) —
        // and remember the lowest (deepest) block whose anchor is definitively
        // off Bitcoin's best chain (STALE/NOT_FOUND/HEIGHT_MISMATCH); since we
        // overwrite lowest_bad as we descend, the last bad we record is the
        // lowest one. g_anchor_ok_cache can only ever mark an anchor OK, never
        // bad: a now-orphaned anchor is NOT in the OK cache (it is cleared on
        // tip_changed, and an anchor that was cached OK but then went stale
        // without a tip change returns STALE here on the live RPC), so the cache
        // cannot hide a stale low block from this walk.
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
        for (const AnchorRef& ref : to_check) {
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

//! Scan one parent-chain block (via getblock verbosity 2, so this works
//! against any Bitcoin-RPC-compatible daemon regardless of its transaction
//! serialization) for tagged checkpoint OP_RETURN outputs. Returns false on
//! connection problems.
bool ScanMainchainBlockForCheckpoints(const uint256& btc_hash, int& btc_height_out, uint256& prev_hash_out)
{
    try {
        UniValue params(UniValue::VARR);
        params.push_back(btc_hash.GetHex());
        params.push_back(2);
        UniValue reply = CallMainChainRPC("getblock", params);
        UniValue errval = find_value(reply, "error");
        if (!errval.isNull()) return false;
        UniValue result = find_value(reply, "result");
        if (!result.isObject()) return false;
        btc_height_out = find_value(result, "height").get_int();
        UniValue prev = find_value(result, "previousblockhash");
        prev_hash_out = prev.isStr() ? uint256S(prev.get_str()) : uint256();

        const UniValue& txs = find_value(result, "tx").get_array();
        for (size_t i = 0; i < txs.size(); ++i) {
            const UniValue& vouts = find_value(txs[i], "vout").get_array();
            for (size_t j = 0; j < vouts.size(); ++j) {
                const UniValue& spk = find_value(vouts[j], "scriptPubKey");
                if (!spk.isObject()) continue;
                const UniValue& hexval = find_value(spk, "hex");
                if (!hexval.isStr()) continue;
                std::vector<unsigned char> raw = ParseHex(hexval.get_str());
                CScript script(raw.begin(), raw.end());
                CScript::const_iterator pc = script.begin();
                opcodetype opcode;
                std::vector<unsigned char> data;
                if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) continue;
                if (!script.GetOp(pc, opcode, data)) continue;
                auto parsed = ParseCheckpointPayload(data);
                if (!parsed) continue;
                LOCK(g_anchor_mutex);
                // Keep the earliest commitment for a given block.
                auto it = g_pos_checkpoints.find(parsed->first);
                if (it == g_pos_checkpoints.end() || it->second.btc_height > btc_height_out) {
                    g_pos_checkpoints[parsed->first] = PosCheckpoint{parsed->first, parsed->second, btc_height_out, btc_hash};
                    LogPrintf("PoS: observed checkpoint for block %s (height %u) committed in parent block %s (height %d)\n",
                              parsed->first.ToString(), parsed->second, btc_hash.ToString(), btc_height_out);
                }
            }
        }
        return true;
    } catch (const std::exception& e) {
        LogPrintf("WARNING: checkpoint scan of parent block %s failed: %s\n", btc_hash.ToString(), e.what());
        return false;
    }
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
void ScanNewMainchainBlocks(ChainstateManager& chainman, const uint256& new_tip)
{
    uint256 last_scanned;
    {
        LOCK(g_anchor_mutex);
        last_scanned = g_last_checkpoint_scan_tip;
    }
    const int window = (int)gArgs.GetIntArg("-poscheckpointscan", DEFAULT_POS_CHECKPOINT_SCAN);
    uint256 cursor = new_tip;
    int tip_height = -1;
    for (int i = 0; i < window && !cursor.IsNull() && cursor != last_scanned; ++i) {
        int height = 0;
        uint256 prev;
        if (!ScanMainchainBlockForCheckpoints(cursor, height, prev)) break;
        if (tip_height < 0) tip_height = height;
        cursor = prev;
    }
    {
        LOCK(g_anchor_mutex);
        g_last_checkpoint_scan_tip = new_tip;
        if (tip_height >= 0) g_last_btc_tip_height = tip_height;
    }
    if (tip_height >= 0) {
        UpdatePosFinality(chainman, tip_height);
    }
}

} // namespace
