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
    {
        LOCK(g_anchor_mutex);
        if (best == g_last_mainchain_tip) return; // no parent chain change
        g_last_mainchain_tip = best;
        // The parent chain moved: previously confirmed anchors may have been
        // reorganized away, so drop the cache and re-check.
        g_anchor_ok_cache.clear();
    }

    // PoS checkpoints (paper §11): scan the new parent blocks for committed
    // Sequentia checkpoints and update the finality point.
    if (g_con_pos) {
        ScanNewMainchainBlocks(chainman, best);
    }

    // 1) Reconsider blocks we invalidated earlier whose anchors are canonical
    //    again (the parent chain reorganized back).
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

    // 2) Walk the active chain down from the tip looking for blocks whose
    //    anchors were reorganized away. Anchor heights are monotone along the
    //    chain, so once a block's anchor is confirmed canonical, all blocks
    //    below it are anchored to canonical blocks too.
    uint256 lowest_bad;
    while (true) {
        lowest_bad.SetNull();
        int finalized_height = -1;
        uint256 finalized_hash;
        (void)GetPosFinalizedCheckpoint(finalized_height, finalized_hash);
        {
            LOCK(cs_main);
            const CBlockIndex* pindex = chainman.ActiveChain().Tip();
            // Never invalidate at or below a checkpoint-finalized block: the
            // checkpoint's burial depth bounds how deep we follow parent
            // reorganizations (a deeper parent reorg needs operator action).
            for (; pindex && pindex->nHeight > 0 && pindex->nHeight > finalized_height; pindex = pindex->pprev) {
                if (pindex->m_anchor_hash.IsNull()) break; // pre-anchor blocks
                // Skip the RPC when the parent shares the same anchor and has
                // already been deemed bad (we only need the lowest).
                AnchorCheckResult res;
                {
                    // CheckMainchainAnchor takes g_anchor_mutex internally and
                    // performs network I/O; we accept doing this under cs_main
                    // because the watcher is the only caller and results are
                    // cached per parent-chain tip.
                    res = CheckMainchainAnchor(pindex->m_anchor_height, pindex->m_anchor_hash);
                }
                if (res == AnchorCheckResult::OK) break;
                if (res == AnchorCheckResult::NO_CONNECTION) return; // cannot judge now
                lowest_bad = pindex->GetBlockHash();
            }
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

    std::vector<PosCheckpoint> candidates = GetPosCheckpoints();
    int best_height = -1;
    uint256 best_hash;
    for (const PosCheckpoint& ckpt : candidates) {
        if (btc_tip_height - ckpt.btc_height + 1 < depth) continue; // not buried enough
        // The commitment must still be on the parent chain's best chain.
        if (CheckMainchainAnchor((uint32_t)ckpt.btc_height, ckpt.btc_hash) != AnchorCheckResult::OK) continue;
        // The checkpointed block must be on *our* active chain at the claimed
        // height: checkpoints lock in validated history, never replace it.
        {
            LOCK(cs_main);
            const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(ckpt.seq_hash);
            if (!pindex || (uint32_t)pindex->nHeight != ckpt.seq_height) continue;
            if (!chainman.ActiveChain().Contains(pindex)) continue;
        }
        if ((int)ckpt.seq_height > best_height) {
            best_height = (int)ckpt.seq_height;
            best_hash = ckpt.seq_hash;
        }
    }
    if (best_height >= 0) {
        LOCK(g_anchor_mutex);
        if (best_height > g_pos_finalized_height) {
            g_pos_finalized_height = best_height;
            g_pos_finalized_hash = best_hash;
            LogPrintf("PoS: finalized block %s (height %d) via parent-chain checkpoint\n",
                      best_hash.ToString(), best_height);
        }
    }
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
    }
    if (tip_height >= 0) {
        UpdatePosFinality(chainman, tip_height);
    }
}

} // namespace
