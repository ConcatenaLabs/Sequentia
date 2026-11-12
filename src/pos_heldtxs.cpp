// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_heldtxs.h>

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <fs.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <pos.h>
#include <primitives/block.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <txmempool.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <set>

using node::ReadBlockFromDisk;

namespace {

struct HeldEntry {
    CTransactionRef tx;
    int fork_height{0};         //!< height of the highest block the disconnect kept
    uint32_t release_anchor{0}; //!< released once the tip anchors at or above this
};

struct ParkedBlock {
    int height{0};
    int64_t since_ms{0};
};

Mutex g_held_mutex;
std::atomic<bool> g_held_active{false};
std::map<uint256, HeldEntry> g_held GUARDED_BY(g_held_mutex);
std::map<COutPoint, uint256> g_spent_by GUARDED_BY(g_held_mutex);
std::map<uint256, ParkedBlock> g_parked GUARDED_BY(g_held_mutex);
std::set<uint256> g_given_up GUARDED_BY(g_held_mutex);
//! Per block hash: the conflict it carries, if any. Cleared whenever the held set changes.
std::map<uint256, std::optional<std::string>> g_block_conflict GUARDED_BY(g_held_mutex);
//! Blocks of the active chain already examined for releases.
std::set<uint256> g_release_scanned GUARDED_BY(g_held_mutex);
uint256 g_mempool_enforced_tip GUARDED_BY(g_held_mutex);
bool g_dirty_mempool GUARDED_BY(g_held_mutex){false};

static constexpr uint32_t HELD_FILE_VERSION = 1;

int64_t SteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

fs::path HeldPath() { return gArgs.GetDataDirNet() / "heldtxs.dat"; }

int MinForkHeight() EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    int m = std::numeric_limits<int>::max();
    for (const auto& [txid, e] : g_held) m = std::min(m, e.fork_height);
    for (const auto& [hash, p] : g_parked) m = std::min(m, p.height - 1);
    return m;
}

void AddEntry(const CTransactionRef& tx, int fork_height, uint32_t release_anchor) EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    const uint256 txid = tx->GetHash();
    auto it = g_held.find(txid);
    if (it != g_held.end()) {
        it->second.fork_height = std::min(it->second.fork_height, fork_height);
        it->second.release_anchor = std::max(it->second.release_anchor, release_anchor);
        return;
    }
    g_held[txid] = HeldEntry{tx, fork_height, release_anchor};
    for (const CTxIn& in : tx->vin) {
        if (in.m_is_pegin) continue;
        g_spent_by[in.prevout] = txid;
    }
}

void EraseEntry(const uint256& txid) EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    auto it = g_held.find(txid);
    if (it == g_held.end()) return;
    for (const CTxIn& in : it->second.tx->vin) {
        auto s = g_spent_by.find(in.prevout);
        if (s != g_spent_by.end() && s->second == txid) g_spent_by.erase(s);
    }
    g_held.erase(it);
}

void SaveLocked() EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    // Written whole on every change: changes are rare (a disconnect, a
    // release, a block set aside) and the file is small, and a node killed
    // between the disconnect and the vote must still hold the record.
    const fs::path tmp = gArgs.GetDataDirNet() / "heldtxs.dat.new";
    FILE* f = fsbridge::fopen(tmp, "wb");
    if (!f) {
        LogPrintf("Held transactions: cannot write %s\n", fs::PathToString(tmp));
        return;
    }
    CAutoFile file(f, SER_DISK, CLIENT_VERSION);
    try {
        file << HELD_FILE_VERSION;
        file << (uint64_t)g_held.size();
        for (const auto& [txid, e] : g_held) {
            file << e.tx << e.fork_height << e.release_anchor;
        }
        file << (uint64_t)g_parked.size();
        for (const auto& [hash, p] : g_parked) file << hash << p.height;
        file << (uint64_t)g_given_up.size();
        for (const uint256& hash : g_given_up) file << hash;
        if (!FileCommit(file.Get())) throw std::runtime_error("FileCommit failed");
        file.fclose();
        if (!RenameOver(tmp, HeldPath())) throw std::runtime_error("rename failed");
    } catch (const std::exception& e) {
        LogPrintf("Held transactions: failed to write the record: %s\n", e.what());
    }
}

void Changed() EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    g_block_conflict.clear();
    g_dirty_mempool = true;
    SaveLocked();
}

std::optional<uint256> ConflictLocked(const CTransaction& tx) EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    if (g_spent_by.empty()) return std::nullopt;
    const uint256 txid = tx.GetHash();
    for (const CTxIn& in : tx.vin) {
        if (in.m_is_pegin) continue;
        auto it = g_spent_by.find(in.prevout);
        if (it != g_spent_by.end() && it->second != txid) return it->second;
    }
    return std::nullopt;
}

std::optional<std::string> TxsConflictLocked(const CBlock& block) EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsCoinBase()) continue;
        if (auto held = ConflictLocked(*tx)) {
            return strprintf("transaction %s respends an input of held transaction %s",
                             tx->GetHash().ToString(), held->ToString());
        }
    }
    return std::nullopt;
}

//! The conflict a block of the index carries, read from disk once and cached.
std::optional<std::string> IndexConflictLocked(const CBlockIndex* pindex)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main, g_held_mutex)
{
    const uint256 hash = pindex->GetBlockHash();
    auto it = g_block_conflict.find(hash);
    if (it != g_block_conflict.end()) return it->second;
    std::optional<std::string> res;
    CBlock block;
    if ((pindex->nStatus & BLOCK_HAVE_DATA) && ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
        res = TxsConflictLocked(block);
    }
    g_block_conflict[hash] = res;
    return res;
}

} // namespace

bool HeldTxsActive() { return g_held_active; }

static std::atomic<bool> g_held_filling{false};
void HeldTxsSetFilling(bool filling) { g_held_filling = filling && g_held_active; }
bool HeldTxsFilling() { return g_held_filling; }

void HeldTxsInit()
{
    g_held_active = g_con_pos && g_con_bitcoin_anchor && !gArgs.GetBoolArg("-posdebugignoreheld", false);
    if (!g_held_active) {
        if (gArgs.GetBoolArg("-posdebugignoreheld", false)) {
            LogPrintf("Held transactions: rule turned off (-posdebugignoreheld)\n");
        }
        return;
    }
    FILE* f = fsbridge::fopen(HeldPath(), "rb");
    if (!f) return;
    CAutoFile file(f, SER_DISK, CLIENT_VERSION);
    LOCK(g_held_mutex);
    try {
        uint32_t version;
        file >> version;
        if (version != HELD_FILE_VERSION) throw std::runtime_error("unknown version");
        uint64_t n;
        file >> n;
        for (uint64_t i = 0; i < n; ++i) {
            CMutableTransaction mtx;
            int fork_height;
            uint32_t release_anchor;
            file >> mtx >> fork_height >> release_anchor;
            AddEntry(MakeTransactionRef(std::move(mtx)), fork_height, release_anchor);
        }
        file >> n;
        const int64_t now = SteadyMs();
        for (uint64_t i = 0; i < n; ++i) {
            uint256 hash;
            int height;
            file >> hash >> height;
            g_parked[hash] = ParkedBlock{height, now};
        }
        file >> n;
        for (uint64_t i = 0; i < n; ++i) {
            uint256 hash;
            file >> hash;
            g_given_up.insert(hash);
        }
        g_dirty_mempool = true;
        LogPrintf("Held transactions: loaded %u held, %u set-aside block(s)\n", g_held.size(), g_parked.size());
    } catch (const std::exception& e) {
        LogPrintf("Held transactions: failed to read the record: %s\n", e.what());
    }
}

void HeldTxsRecordDisconnect(ChainstateManager& chainman, const std::vector<const CBlockIndex*>& disconnected)
{
    if (!g_held_active || disconnected.empty()) return;
    const int64_t start = GetTimeMicros();
    // A block is as final as the highest certified block above it: hold every
    // block up to the highest one that carried its parent's quorum.
    int top = -1;
    uint32_t release_anchor = 0;
    std::vector<std::shared_ptr<CBlock>> blocks;
    {
        LOCK(cs_main);
        for (size_t i = 0; i < disconnected.size(); ++i) {
            if (disconnected[i]->PosCarriesQuorum()) top = (int)i;
            release_anchor = std::max(release_anchor, disconnected[i]->m_anchor_height);
        }
        for (int i = 0; i <= top; ++i) {
            auto block = std::make_shared<CBlock>();
            if (!ReadBlockFromDisk(*block, disconnected[i], Params().GetConsensus())) {
                LogPrintf("Held transactions: cannot read disconnected block %s\n", disconnected[i]->GetBlockHash().ToString());
                continue;
            }
            blocks.push_back(block);
        }
    }
    if (top < 0) {
        LogPrintf("Held transactions: none of the %u disconnected block(s) was certified; nothing held\n", disconnected.size());
        return;
    }
    release_anchor += POS_HELD_WINDOW;
    const int fork_height = disconnected.front()->nHeight - 1;
    // A transaction that spends the coinbase of a disconnected block can never
    // be mined on the new chain (that coinbase is not mined again), nor can one
    // that spends its outputs: holding them would only freeze their other inputs.
    std::set<uint256> dead;
    size_t n = 0;
    LOCK(g_held_mutex);
    for (const auto& block : blocks) {
        for (const CTransactionRef& tx : block->vtx) {
            if (tx->IsCoinBase()) {
                dead.insert(tx->GetHash());
                continue;
            }
            bool is_dead = false;
            for (const CTxIn& in : tx->vin) {
                if (!in.m_is_pegin && dead.count(in.prevout.hash)) is_dead = true;
            }
            if (is_dead) {
                dead.insert(tx->GetHash());
                continue;
            }
            AddEntry(tx, fork_height, release_anchor);
            ++n;
        }
    }
    Changed();
    LogPrintf("Held transactions: holding %u transaction(s) of %d disconnected block(s) above height %d "
              "(release by anchor %u), %u held in all, filled in %.2fms\n",
              n, top + 1, fork_height, release_anchor, g_held.size(), (GetTimeMicros() - start) * 0.001);
}

std::optional<uint256> HeldTxsConflict(const CTransaction& tx)
{
    if (!g_held_active) return std::nullopt;
    LOCK(g_held_mutex);
    return ConflictLocked(tx);
}

std::optional<std::string> HeldTxsBlockConflict(const CBlock& block, const CBlockIndex* prev)
{
    AssertLockHeld(cs_main);
    if (!g_held_active) return std::nullopt;
    LOCK(g_held_mutex);
    if (g_held.empty()) return std::nullopt;
    if (auto c = TxsConflictLocked(block)) return c;
    const int floor = MinForkHeight();
    for (const CBlockIndex* p = prev; p && p->nHeight > floor; p = p->pprev) {
        if (auto c = IndexConflictLocked(p)) {
            return strprintf("ancestor %s (height %d): %s", p->GetBlockHash().ToString(), p->nHeight, *c);
        }
    }
    return std::nullopt;
}

bool HeldTxsMaintain(ChainstateManager& chainman, CTxMemPool& mempool, bool park)
{
    if (!g_held_active) return false;
    {
        LOCK(g_held_mutex);
        if (g_held.empty() && g_parked.empty()) {
            g_block_conflict.clear();
            g_release_scanned.clear();
            return false;
        }
    }

    CBlockIndex* to_park{nullptr};
    std::vector<uint256> to_restore;
    std::vector<CTransactionRef> to_readmit;
    {
        LOCK(cs_main);
        LOCK(g_held_mutex);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        if (!tip) return false;
        bool changed = false;
        const int floor = MinForkHeight();

        // The highest block above the disconnect point that carries its
        // parent's quorum: everything at or below it is as final as Sequentia
        // makes anything, so a held transaction mined there is done, and one
        // that a conflicting spend there displaced cannot come back on this chain.
        const CBlockIndex* cert_top{nullptr};
        for (const CBlockIndex* p = tip; p && p->nHeight > floor; p = p->pprev) {
            if (p->PosCarriesQuorum()) {
                cert_top = p;
                break;
            }
        }
        for (const CBlockIndex* p = cert_top; p && p->nHeight > floor; p = p->pprev) {
            if (!g_release_scanned.insert(p->GetBlockHash()).second) break;
            CBlock block;
            if (!ReadBlockFromDisk(block, p, Params().GetConsensus())) continue;
            for (const CTransactionRef& tx : block.vtx) {
                if (tx->IsCoinBase()) continue;
                if (g_held.count(tx->GetHash())) {
                    LogPrintf("Held transactions: %s mined in certified block %s; released\n",
                              tx->GetHash().ToString(), p->GetBlockHash().ToString());
                    EraseEntry(tx->GetHash());
                    changed = true;
                } else if (auto held = ConflictLocked(*tx)) {
                    LogPrintf("Held transactions: %s is respent by %s in certified block %s; released\n",
                              held->ToString(), tx->GetHash().ToString(), p->GetBlockHash().ToString());
                    EraseEntry(*held);
                    changed = true;
                }
            }
        }
        // The window: Bitcoin has moved far enough past the disconnected blocks.
        for (auto it = g_held.begin(); it != g_held.end();) {
            if (tip->m_anchor_height >= it->second.release_anchor) {
                LogPrintf("Held transactions: %s released at anchor %u (window)\n", it->first.ToString(), tip->m_anchor_height);
                const uint256 txid = it->first;
                ++it;
                EraseEntry(txid);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) Changed();

        // Restore set-aside blocks when they no longer conflict, when a
        // certified block at their height or above has displaced them, or when
        // the patience runs out (the member then only refuses to sign on them).
        const int64_t now = SteadyMs();
        for (auto it = g_parked.begin(); it != g_parked.end(); ++it) {
            const CBlockIndex* b = chainman.m_blockman.LookupBlockIndex(it->first);
            bool restore = !b;
            if (b && cert_top && cert_top->nHeight >= it->second.height) restore = true;
            if (b && !restore && !IndexConflictLocked(b)) restore = true;
            if (b && !restore && now - it->second.since_ms >= POS_HELD_PARK_MS) {
                restore = true;
                g_given_up.insert(it->first);
                LogPrintf("Held transactions: no certified block displaced %s within %ds; restoring it, signing nothing on it\n",
                          it->first.ToString(), (int)(POS_HELD_PARK_MS / 1000));
            }
            if (restore) to_restore.push_back(it->first);
        }

        // Arm two: the lowest sub-quorum block above cert_top on the active
        // chain that carries a conflict is set aside, once.
        if (park && to_restore.empty()) {
            const int stop = cert_top ? cert_top->nHeight : floor;
            CBlockIndex* lowest{nullptr};
            for (CBlockIndex* p = chainman.ActiveChain().Tip(); p && p->nHeight > stop; p = p->pprev) {
                if (IndexConflictLocked(p)) lowest = p;
            }
            if (lowest && !g_given_up.count(lowest->GetBlockHash()) && !g_parked.count(lowest->GetBlockHash())) {
                to_park = lowest;
            }
        }

        // Held transactions go back into the mempool whenever the tip moved
        // (a block that evicted them as conflicts may have been set aside).
        if (g_dirty_mempool || g_mempool_enforced_tip != tip->GetBlockHash()) {
            g_dirty_mempool = false;
            g_mempool_enforced_tip = tip->GetBlockHash();
            for (const auto& [txid, e] : g_held) to_readmit.push_back(e.tx);
        }
    }

    bool chain_changed = false;
    for (const uint256& hash : to_restore) {
        LOCK(cs_main);
        LOCK(g_held_mutex);
        g_parked.erase(hash);
        SaveLocked();
        CBlockIndex* b = chainman.m_blockman.LookupBlockIndex(hash);
        if (!b) continue;
        // ResetBlockFailureFlags also clears every failed ancestor: never clear
        // one that is not ours, such as an anchor invalidation.
        bool foreign = false;
        for (const CBlockIndex* a = b->pprev; a && !foreign; a = a->pprev) foreign = a->nStatus & BLOCK_FAILED_MASK;
        if (foreign) {
            LogPrintf("Held transactions: not restoring %s: an ancestor is invalid for another reason\n", hash.ToString());
            continue;
        }
        LogPrintf("Held transactions: restoring set-aside block %s\n", hash.ToString());
        chainman.ActiveChainstate().ResetBlockFailureFlags(b);
        chain_changed = true;
    }
    if (to_park) {
        LogPrintf("Held transactions: setting aside sub-quorum block %s (height %d), which respends a held transaction\n",
                  to_park->GetBlockHash().ToString(), to_park->nHeight);
        {
            LOCK(g_held_mutex);
            g_parked[to_park->GetBlockHash()] = ParkedBlock{to_park->nHeight, SteadyMs()};
            SaveLocked();
        }
        BlockValidationState state;
        if (!chainman.ActiveChainstate().InvalidateBlock(state, to_park)) {
            LogPrintf("Held transactions: could not set aside %s: %s\n", to_park->GetBlockHash().ToString(), state.ToString());
        }
        chain_changed = true;
    }
    if (chain_changed) {
        BlockValidationState state;
        chainman.ActiveChainstate().ActivateBestChain(state);
        LOCK(g_held_mutex);
        g_dirty_mempool = true;
        return true;
    }

    if (!to_readmit.empty()) {
        LOCK(cs_main);
        LOCK(mempool.cs);
        size_t added = 0;
        for (const CTransactionRef& tx : to_readmit) {
            if (mempool.exists(GenTxid::Txid(tx->GetHash()))) continue;
            // Whatever spends its inputs in the mempool is a conflict this node
            // refuses now; make room for the held transaction.
            for (const CTxIn& in : tx->vin) {
                if (const CTransaction* c = mempool.GetConflictTx(in.prevout)) {
                    if (c->GetHash() != tx->GetHash()) mempool.removeRecursive(*c, MemPoolRemovalReason::REPLACED);
                }
            }
            const MempoolAcceptResult r = AcceptToMemoryPool(chainman.ActiveChainstate(), tx, GetTime(),
                                                             /*bypass_limits=*/false, /*test_accept=*/false);
            if (r.m_result_type == MempoolAcceptResult::ResultType::VALID) {
                ++added;
            } else {
                LogPrint(BCLog::MEMPOOL, "Held transactions: %s not readmitted: %s\n", tx->GetHash().ToString(),
                         r.m_state.ToString());
            }
        }
        if (added) LogPrintf("Held transactions: readmitted %u held transaction(s) to the mempool\n", added);
    }
    return false;
}

HeldTxsInfo HeldTxsGetInfo()
{
    HeldTxsInfo info;
    LOCK(g_held_mutex);
    for (const auto& [txid, e] : g_held) {
        info.txids.push_back(txid);
        info.release_anchor = std::max(info.release_anchor, e.release_anchor);
    }
    for (const auto& [hash, p] : g_parked) info.parked.push_back(hash);
    for (const uint256& hash : g_given_up) info.given_up.push_back(hash);
    if (!g_held.empty() || !g_parked.empty()) info.fork_height = MinForkHeight();
    return info;
}
