// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <anchor.h>

#include <chain.h>
#include <logging.h>
#include <mainchainrpc.h>
#include <sync.h>
#include <util/system.h>
#include <validation.h>

#include <map>
#include <set>

bool g_validate_anchor = true;

namespace {

Mutex g_anchor_mutex;
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
        {
            LOCK(cs_main);
            const CBlockIndex* pindex = chainman.ActiveChain().Tip();
            for (; pindex && pindex->nHeight > 0; pindex = pindex->pprev) {
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
