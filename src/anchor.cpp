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

#include <map>
#include <set>

bool g_validate_anchor = true;

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
//! Blocks invalidated by the anchor watcher, so they can be reconsidered if
//! the parent chain reorganizes back.
std::set<uint256> g_anchor_invalidated GUARDED_BY(g_anchor_mutex);
//! Last seen parent chain tip.
uint256 g_last_mainchain_tip GUARDED_BY(g_anchor_mutex);

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

//! Defined below: walk newly-arrived parent blocks for checkpoints.
void ScanNewMainchainBlocks(ChainstateManager& chainman, const uint256& new_tip);
//! Defined below: recompute the checkpoint finality point and conflicts.
void UpdatePosFinality(ChainstateManager& chainman, int btc_tip_height);

} // namespace

AnchorCheckResult CheckMainchainAnchor(uint32_t height, const uint256& hash)
{
    {
        LOCK(g_anchor_mutex);
        if (g_anchor_ok_cache.count({height, hash})) return AnchorCheckResult::OK;
    }
    try {
        UniValue params(UniValue::VARR);
        params.push_back(hash.GetHex());
        UniValue reply = CallMainChainRPC("getblockheader", params);
        UniValue errval = find_value(reply, "error");
        if (!errval.isNull()) {
            return AnchorCheckResult::NOT_FOUND;
        }
        UniValue result = find_value(reply, "result");
        if (!result.isObject()) {
            return AnchorCheckResult::NOT_FOUND;
        }
        UniValue confirmations = find_value(result.get_obj(), "confirmations");
        if (!confirmations.isNum() || confirmations.get_int64() < 1) {
            // confirmations == -1 means the block is not on the best chain
            return AnchorCheckResult::STALE;
        }
        UniValue blockheight = find_value(result.get_obj(), "height");
        if (!blockheight.isNum() || blockheight.get_int64() != (int64_t)height) {
            return AnchorCheckResult::HEIGHT_MISMATCH;
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

bool GetAnchorForNewBlock(uint32_t prev_anchor_height, const uint256& prev_anchor_hash,
                          uint32_t& anchor_height, uint256& anchor_hash)
{
    const int min_conf = std::max<int64_t>(1, gArgs.GetIntArg("-anchorminconf", DEFAULT_ANCHOR_MIN_CONF));
    int count = 0;
    if (GetMainchainBlockCount(count)) {
        int target = count - (min_conf - 1);
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
            // The parent chain moved: previously confirmed anchors may have
            // been reorganized away, so drop the cache and re-check.
            g_anchor_ok_cache.clear();
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
    //    again (the parent chain reorganized back). This only matters when the
    //    parent moved, so it is gated on tip_changed.
    if (tip_changed) {
        std::set<uint256> invalidated;
        {
            LOCK(g_anchor_mutex);
            invalidated = g_anchor_invalidated;
        }
        for (const uint256& hash : invalidated) {
            CBlockIndex* pindex;
            uint32_t anchor_height;
            uint256 anchor_hash;
            {
                LOCK(cs_main);
                pindex = chainman.m_blockman.LookupBlockIndex(hash);
                if (!pindex) continue;
                anchor_height = pindex->m_anchor_height;
                anchor_hash = pindex->m_anchor_hash;
            }
            if (CheckMainchainAnchor(anchor_height, anchor_hash) == AnchorCheckResult::OK) {
                LogPrintf("Anchor %s (height %d) of block %s is canonical again; reconsidering\n",
                          anchor_hash.ToString(), anchor_height, hash.ToString());
                {
                    LOCK(cs_main);
                    chainman.ActiveChainstate().ResetBlockFailureFlags(pindex);
                }
                {
                    LOCK(g_anchor_mutex);
                    g_anchor_invalidated.erase(hash);
                }
                BlockValidationState state;
                if (!chainman.ActiveChainstate().ActivateBestChain(state)) {
                    LogPrintf("WARNING: ActivateBestChain failed after anchor reconsideration: %s\n", state.ToString());
                }
            }
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
        // descending (cannot judge blocks below). But it must NOT discard a lower
        // stale block we already found above it: invalidating a definitively
        // off-best-chain block is always correct, and a NO_CONNECTION block sits
        // ABOVE it (higher height) and so will be reconnected automatically once
        // it (re-)checks OK. If no stale block was found before the
        // NO_CONNECTION, there is nothing to act on — bail and retry next tick.
        for (const AnchorRef& ref : to_check) {
            AnchorCheckResult res = CheckMainchainAnchor(ref.anchor_height, ref.anchor_hash);
            if (res == AnchorCheckResult::NO_CONNECTION) break; // cannot judge deeper
            if (res != AnchorCheckResult::OK) lowest_bad = ref.block_hash;
        }
        if (lowest_bad.IsNull()) return;

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
        BlockValidationState abc_state;
        if (!chainman.ActiveChainstate().ActivateBestChain(abc_state)) {
            LogPrintf("WARNING: ActivateBestChain failed after anchor invalidation: %s\n", abc_state.ToString());
            return;
        }
        // Loop: the new tip may itself have a stale anchor (e.g. a competing
        // branch that anchored to the same reorganized-away parent block).
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
