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
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pos.h>
#include <primitives/block.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <txmempool.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
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
    uint64_t seq{0};            //!< block order across disconnects: parents before children
};

struct AsideBlock {
    const CBlockIndex* pindex{nullptr};
    int64_t since_ms{0};
};

std::atomic<bool> g_held_active{false};
std::atomic<bool> g_held_filling{false};

Mutex g_held_mutex;
std::map<uint256, HeldEntry> g_held GUARDED_BY(g_held_mutex);
std::map<COutPoint, uint256> g_spent_by GUARDED_BY(g_held_mutex);
uint64_t g_seq GUARDED_BY(g_held_mutex){0};
//! Arm two: blocks kept out of fork choice. Memory only.
std::map<uint256, AsideBlock> g_aside GUARDED_BY(g_held_mutex);
//! Blocks set aside once already; never set aside again. Memory only.
std::set<uint256> g_given_up GUARDED_BY(g_held_mutex);
//! Per block hash: the conflict it carries, if any. Cleared whenever the held set changes.
std::map<uint256, std::optional<std::string>> g_block_conflict GUARDED_BY(g_held_mutex);
//! Blocks of the active chain already examined for releases; cleared by every disconnect.
std::set<uint256> g_release_scanned GUARDED_BY(g_held_mutex);
uint256 g_mempool_enforced_tip GUARDED_BY(g_held_mutex);
bool g_dirty_mempool GUARDED_BY(g_held_mutex){false};
//! The liveness bound: when the current hold began (0: none), and the height
//! of the highest certified block of the active chain at that moment.
int64_t g_hold_since_ms GUARDED_BY(g_held_mutex){0};
//! The member declined to sign or to propose for a held transaction since the
//! last certified block; the bound's clock starts once it has an honest
//! proposal to sign or to make at the held height.
bool g_declined GUARDED_BY(g_held_mutex){false};
int g_hold_progress GUARDED_BY(g_held_mutex){-1};
int g_cert_progress GUARDED_BY(g_held_mutex){-1};
int64_t g_last_descendant_scan_ms GUARDED_BY(g_held_mutex){0};

static constexpr uint32_t HELD_FILE_VERSION = 2;

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
    for (const auto& [hash, a] : g_aside) m = std::min(m, a.pindex->nHeight - 1);
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
    g_held[txid] = HeldEntry{tx, fork_height, release_anchor, g_seq++};
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
    // Only the held transactions are written: a restarted member must still
    // refuse to sign their respends. Set-asides are never written, so no
    // record, lost or stale, can keep a block out of a node's fork choice.
    const fs::path tmp = gArgs.GetDataDirNet() / "heldtxs.dat.new";
    FILE* f = fsbridge::fopen(tmp, "wb");
    if (!f) {
        LogPrintf("Held transactions: cannot write %s\n", fs::PathToString(tmp));
        return;
    }
    CAutoFile file(f, SER_DISK, CLIENT_VERSION);
    try {
        std::vector<const HeldEntry*> order;
        for (const auto& [txid, e] : g_held) order.push_back(&e);
        std::sort(order.begin(), order.end(), [](const HeldEntry* a, const HeldEntry* b) { return a->seq < b->seq; });
        file << HELD_FILE_VERSION;
        file << (uint64_t)order.size();
        for (const HeldEntry* e : order) file << e->tx << e->fork_height << e->release_anchor;
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

//! Whether a block carries its parent's quorum, from what the index knows:
//! the judged answer, or for a block that cannot escape a stall the header's.
bool CarriesQuorum(const CBlockIndex* p) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (p->PosCarriesQuorum()) return true;
    return !p->m_pos_cert_known && p->m_pos_certified && p->pprev &&
           !PosEscapingStallAllowed(p->pprev->m_anchor_height, p->m_anchor_height);
}

CFeeRate BlockMinFeeRate()
{
    if (gArgs.IsArgSet("-blockmintxfee")) {
        if (std::optional<CAmount> parsed = ParseMoney(gArgs.GetArg("-blockmintxfee", ""))) return CFeeRate{*parsed};
    }
    return CFeeRate{DEFAULT_BLOCK_MIN_TX_FEE};
}

void ReleaseAllLocked(const std::string& why) EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    LogPrintf("Held transactions: %s; released %u held transaction(s) and %u set-aside block(s)\n",
              why, g_held.size(), g_aside.size());
    g_held.clear();
    g_spent_by.clear();
    g_given_up.clear();
    g_hold_since_ms = 0;
    g_declined = false;
    Changed();
}

} // namespace

bool HeldTxsActive() { return g_held_active; }
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
        if (version != 1 && version != HELD_FILE_VERSION) throw std::runtime_error("unknown version");
        uint64_t n;
        file >> n;
        for (uint64_t i = 0; i < n; ++i) {
            CMutableTransaction mtx;
            int fork_height;
            uint32_t release_anchor;
            file >> mtx >> fork_height >> release_anchor;
            AddEntry(MakeTransactionRef(std::move(mtx)), fork_height, release_anchor);
        }
        // Version 1 also listed set-aside blocks; they are not carried over.
        g_dirty_mempool = true;
        LogPrintf("Held transactions: loaded %u held\n", g_held.size());
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
    LOCK(g_held_mutex);
    g_release_scanned.clear();
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

void HeldTxsNoteDecline()
{
    if (!g_held_active) return;
    LOCK(g_held_mutex);
    if (!g_held.empty()) g_declined = true;
}

void HeldTxsNoteHold()
{
    if (!g_held_active) return;
    LOCK(g_held_mutex);
    if (g_hold_since_ms == 0 && !g_held.empty() && (g_declined || !g_aside.empty())) {
        g_hold_since_ms = SteadyMs();
        g_hold_progress = g_cert_progress;
    }
}

const CBlockIndex* HeldTxsSetAsideAncestor(const CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);
    if (!g_held_active || !pindex) return nullptr;
    LOCK(g_held_mutex);
    for (const auto& [hash, a] : g_aside) {
        if (pindex->nHeight >= a.pindex->nHeight && pindex->GetAncestor(a.pindex->nHeight) == a.pindex) return a.pindex;
    }
    return nullptr;
}

void HeldTxsRelease(const uint256& txid, const std::string& why)
{
    if (!g_held_active) return;
    LOCK(g_held_mutex);
    if (!g_held.count(txid)) return;
    LogPrintf("Held transactions: %s released: %s\n", txid.ToString(), why);
    EraseEntry(txid);
    Changed();
}

bool HeldTxsMaintain(ChainstateManager& chainman, CTxMemPool& mempool, bool park, int64_t bound_ms)
{
    if (!g_held_active) return false;
    {
        LOCK(g_held_mutex);
        if (g_held.empty() && g_aside.empty()) {
            g_block_conflict.clear();
            g_given_up.clear();
            g_hold_since_ms = 0;
            return false;
        }
    }

    CBlockIndex* to_park{nullptr};
    std::vector<uint256> to_restore;
    std::vector<CTransactionRef> to_check;
    {
        LOCK(cs_main);
        LOCK(g_held_mutex);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        if (!tip) return false;
        bool changed = false;
        const int64_t now = SteadyMs();

        // Progress: the highest block of the active chain that carries its
        // parent's quorum.
        const CBlockIndex* cert_any{nullptr};
        int scanned = 0;
        for (const CBlockIndex* p = tip; p && scanned < 2000; p = p->pprev, ++scanned) {
            if (CarriesQuorum(p)) {
                cert_any = p;
                break;
            }
        }
        g_cert_progress = cert_any ? cert_any->nHeight : -1;

        if (g_hold_since_ms == 0 && g_declined && g_cert_progress > g_hold_progress) g_declined = false;
        // The liveness bound. A certified block since the hold began ends the
        // hold; a hold that outlasts the bound releases everything.
        if (g_hold_since_ms != 0) {
            if (g_cert_progress > g_hold_progress) {
                g_hold_since_ms = 0;
                g_declined = false;
            } else if (bound_ms > 0 && now - g_hold_since_ms >= bound_ms) {
                for (const auto& [hash, a] : g_aside) to_restore.push_back(hash);
                ReleaseAllLocked(strprintf("no certified block for %ds while holding (the liveness bound)",
                                           (int)((now - g_hold_since_ms) / 1000)));
            }
        }

        const int floor = MinForkHeight();
        // Everything at or below the highest certified block above the
        // disconnect point is as final as Sequentia makes anything: a held
        // transaction mined there is done, and one that a conflicting spend
        // there displaced cannot come back on this chain.
        const CBlockIndex* cert_top = (cert_any && cert_any->nHeight > floor) ? cert_any : nullptr;
        for (const CBlockIndex* p = cert_top; p && p->nHeight > floor && !g_held.empty(); p = p->pprev) {
            if (!g_release_scanned.insert(p->GetBlockHash()).second) continue;
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

        // End set-asides that are due: nothing left to protect, a certified
        // block at its height or above on the active chain displaced it, or a
        // certified block was built on it (the rest of the committee certified
        // the respend; follow it).
        const bool scan_descendants = now - g_last_descendant_scan_ms >= 1000;
        if (scan_descendants) g_last_descendant_scan_ms = now;
        for (const auto& [hash, a] : g_aside) {
            if (std::find(to_restore.begin(), to_restore.end(), hash) != to_restore.end()) continue;
            bool restore = g_held.empty() || (cert_any && cert_any->nHeight >= a.pindex->nHeight);
            if (!restore && !IndexConflictLocked(a.pindex)) restore = true;
            if (!restore && scan_descendants) {
                for (const auto& [h, p] : chainman.m_blockman.m_block_index) {
                    if (p->nHeight > a.pindex->nHeight && CarriesQuorum(p) && p->GetAncestor(a.pindex->nHeight) == a.pindex) {
                        LogPrintf("Held transactions: certified block %s builds on set-aside block %s; following it\n",
                                  h.ToString(), hash.ToString());
                        restore = true;
                        break;
                    }
                }
            }
            if (restore) to_restore.push_back(hash);
        }

        // Arm two: the lowest sub-quorum block above cert_top on the active
        // chain that carries a conflict is set aside, once.
        if (park && to_restore.empty() && !g_held.empty()) {
            const int stop = cert_top ? cert_top->nHeight : floor;
            CBlockIndex* lowest{nullptr};
            for (CBlockIndex* p = chainman.ActiveChain().Tip(); p && p->nHeight > stop; p = p->pprev) {
                if (IndexConflictLocked(p)) lowest = p;
            }
            if (lowest && !g_given_up.count(lowest->GetBlockHash()) && !g_aside.count(lowest->GetBlockHash())) {
                to_park = lowest;
            }
        }

        // Held transactions go back into the mempool, and are checked against
        // this node's own mining policy, whenever the tip moved or the set changed.
        if (g_dirty_mempool || g_mempool_enforced_tip != tip->GetBlockHash()) {
            g_dirty_mempool = false;
            g_mempool_enforced_tip = tip->GetBlockHash();
            std::vector<const HeldEntry*> order;
            for (const auto& [txid, e] : g_held) order.push_back(&e);
            std::sort(order.begin(), order.end(), [](const HeldEntry* a, const HeldEntry* b) { return a->seq < b->seq; });
            for (const HeldEntry* e : order) to_check.push_back(e->tx);
        }
    }

    bool chain_changed = false;
    if (!to_restore.empty()) {
        LOCK(cs_main);
        LOCK(g_held_mutex);
        for (const uint256& hash : to_restore) {
            if (g_aside.erase(hash)) LogPrintf("Held transactions: set-aside block %s returns to fork choice\n", hash.ToString());
        }
        chainman.ActiveChainstate().ReaddBlockIndexCandidates();
        chain_changed = true;
    }
    if (to_park) {
        LogPrintf("Held transactions: setting aside sub-quorum block %s (height %d), which respends a held transaction\n",
                  to_park->GetBlockHash().ToString(), to_park->nHeight);
        {
            LOCK(g_held_mutex);
            g_aside[to_park->GetBlockHash()] = AsideBlock{to_park, SteadyMs()};
            g_given_up.insert(to_park->GetBlockHash());
            g_hold_progress = g_cert_progress;
        }
        BlockValidationState state;
        if (!chainman.ActiveChainstate().SetAsideBlock(state, to_park)) {
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

    if (!to_check.empty()) {
        // A transaction is held only while this node would mine it now: it is
        // in the mempool with a fee its block assembler takes, or the mempool
        // takes it. A refusal for a lock time or a missing input (a parent not
        // back yet, a conflict on a block that may still be set aside) is
        // temporary; any other refusal, and a fee below the block minimum,
        // ends the hold.
        const CFeeRate min_rate = BlockMinFeeRate();
        std::vector<std::pair<uint256, std::string>> release;
        size_t added = 0;
        {
            LOCK(cs_main);
            LOCK(mempool.cs);
            for (const CTransactionRef& tx : to_check) {
                const uint256 txid = tx->GetHash();
                if (!mempool.exists(GenTxid::Txid(txid))) {
                    for (const CTxIn& in : tx->vin) {
                        if (const CTransaction* c = mempool.GetConflictTx(in.prevout)) {
                            if (c->GetHash() != txid) mempool.removeRecursive(*c, MemPoolRemovalReason::REPLACED);
                        }
                    }
                    const MempoolAcceptResult r = AcceptToMemoryPool(chainman.ActiveChainstate(), tx, GetTime(),
                                                                     /*bypass_limits=*/false, /*test_accept=*/false);
                    if (r.m_result_type != MempoolAcceptResult::ResultType::VALID) {
                        const TxValidationResult kind = r.m_state.GetResult();
                        const std::string reason = r.m_state.GetRejectReason();
                        // Already mined on the active chain (in a block not yet
                        // certified, say) or already pending is not a refusal.
                        const bool known = reason == "txn-already-known" || reason == "txn-already-in-mempool" ||
                                           reason == "txn-same-nonwitness-data-in-mempool";
                        if (!known && kind != TxValidationResult::TX_MISSING_INPUTS && kind != TxValidationResult::TX_PREMATURE_SPEND) {
                            release.emplace_back(txid, "this node would not mine it: " + r.m_state.ToString());
                        } else {
                            LogPrint(BCLog::MEMPOOL, "Held transactions: %s not readmitted yet: %s\n", txid.ToString(), r.m_state.ToString());
                        }
                        continue;
                    }
                    ++added;
                }
                const auto it = mempool.GetIter(txid);
                if (!it) continue;
                const CAmount package_fee = (*it)->GetModFeesWithAncestors().GetValue();
                const CAmount floor_fee = min_rate.GetFee((*it)->GetSizeWithAncestors());
                if (package_fee < floor_fee) {
                    release.emplace_back(txid, strprintf("this node would not mine it: fee value %d below the block minimum %d",
                                                         package_fee, floor_fee));
                }
            }
        }
        if (added) LogPrintf("Held transactions: readmitted %u held transaction(s) to the mempool\n", added);
        if (!release.empty()) {
            LOCK(g_held_mutex);
            for (const auto& [txid, why] : release) {
                LogPrintf("Held transactions: %s released: %s\n", txid.ToString(), why);
                EraseEntry(txid);
            }
            Changed();
        }
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
    for (const auto& [hash, a] : g_aside) info.parked.push_back(hash);
    for (const uint256& hash : g_given_up) info.given_up.push_back(hash);
    if (!g_held.empty() || !g_aside.empty()) info.fork_height = MinForkHeight();
    info.hold_ms = g_hold_since_ms ? SteadyMs() - g_hold_since_ms : -1;
    return info;
}
