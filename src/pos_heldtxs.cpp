// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_heldtxs.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <fs.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <policy/policy.h>
#include <pos.h>
#include <pos_producer.h>
#include <primitives/block.h>
#include <serialize.h>
#include <shutdown.h>
#include <streams.h>
#include <sync.h>
#include <txmempool.h>
#include <util/system.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <utility>

using node::ReadBlockFromDisk;

namespace {

struct HeldEntry {
    CTransactionRef tx;
    int fork_height{0};         //!< height of the highest block the disconnect kept
    uint32_t release_anchor{0}; //!< released once the tip anchors at or above this
    uint64_t seq{0};            //!< block order across disconnects: parents before children
    bool verified{false};       //!< passed every check against the active chain once (amounts, scripts)
};

struct AsideBlock {
    const CBlockIndex* pindex{nullptr};
    int64_t since_ms{0};
};

std::atomic<bool> g_held_active{false};
//! The blocks DisconnectTip took off the active chain that the record has
//! not taken yet, with the block it read to disconnect them. Emptied, under
//! the same cs_main hold, by HeldTxsRecordDeparted.
struct Departed {
    const CBlockIndex* pindex;
    std::shared_ptr<const CBlock> block;
};
std::vector<Departed> g_pending GUARDED_BY(::cs_main);
//! Certified blocks that left and are still off the active chain: a block
//! below one of them on its old branch is held too, whichever activation
//! took it off.
std::set<const CBlockIndex*> g_departed_cert GUARDED_BY(::cs_main);
//! How many blocks are set aside, read without the lock by the certification hook.
std::atomic<size_t> g_aside_count{0};

Mutex g_held_mutex;
std::map<uint256, HeldEntry> g_held GUARDED_BY(g_held_mutex);
std::map<COutPoint, uint256> g_spent_by GUARDED_BY(g_held_mutex);
uint64_t g_seq GUARDED_BY(g_held_mutex){0};
//! Arm two: blocks kept out of fork choice. Memory only.
std::map<uint256, AsideBlock> g_aside GUARDED_BY(g_held_mutex);
//! Blocks set aside once already; never set aside again. Memory only.
std::set<uint256> g_given_up GUARDED_BY(g_held_mutex);
//! Set-aside blocks a certified block is known to build on (HeldTxsNoteCertified).
std::set<uint256> g_follow GUARDED_BY(g_held_mutex);
//! Per block hash: the conflict it carries, if any. Cleared whenever the held set changes.
std::map<uint256, std::optional<std::string>> g_block_conflict GUARDED_BY(g_held_mutex);
//! Blocks of the active chain already examined for releases; cleared by every disconnect.
std::set<uint256> g_release_scanned GUARDED_BY(g_held_mutex);
uint256 g_mempool_enforced_tip GUARDED_BY(g_held_mutex);
bool g_dirty_mempool GUARDED_BY(g_held_mutex){false};
//! The liveness bound: when the current hold began (0: none).
int64_t g_hold_since_ms GUARDED_BY(g_held_mutex){0};
//! The member declined to sign or to propose for a held transaction since the
//! last certified block; the bound's clock starts once it has an honest
//! proposal to sign or to make at the held height.
bool g_declined GUARDED_BY(g_held_mutex){false};
//! The highest block of the active chain that carries its parent's quorum, as
//! the last maintenance pass or disconnect saw it (nullptr: none in reach).
//! Block index entries live as long as the node, so the pointers stay valid.
const CBlockIndex* g_cert_block GUARDED_BY(g_held_mutex){nullptr};
//! The order in which blocks were first judged to carry their parent's quorum
//! (HeldTxsNoteCertified), for the blocks judged while this member held
//! anything. A block is judged once: connecting it again, after Bitcoin
//! returned to its branch, gives it no new place. A block that is absent was
//! judged before everything held now. Memory only.
uint64_t g_cert_seq GUARDED_BY(g_held_mutex){0};
std::map<const CBlockIndex*, uint64_t> g_cert_order GUARDED_BY(g_held_mutex);
//! g_cert_seq when the hold began (its first decline or set-aside), or at the
//! last disconnect since.
//! The hold ends once the active chain's highest certified block was judged
//! after that: by when it was certified, never by its height or its branch,
//! so that a Bitcoin reorganization that lowers the chain does not hide the
//! certified blocks of the new branch, and one that returns to a branch
//! already seen does not pass its old certified blocks off as new.
uint64_t g_hold_mark GUARDED_BY(g_held_mutex){0};
//! The backstop's clock: when the current hold began (its first decline or
//! set-aside), 0 when none. Ends with the hold, at a new certified block.
int64_t g_hold_first_ms GUARDED_BY(g_held_mutex){0};
//! The liveness bound, fixed at startup from the gossip timings
//! (HeldTxsBoundMs), so that every member applies it, producer or not.
int64_t g_bound_ms GUARDED_BY(g_held_mutex){0};

//! The maintenance thread (HeldTxsStart, HeldTxsStop).
std::thread g_held_thread;
std::atomic<bool> g_held_stop{false};

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

void AddEntry(const CTransactionRef& tx, int fork_height, uint32_t release_anchor, std::optional<uint64_t> order = std::nullopt)
    EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    const uint256 txid = tx->GetHash();
    auto it = g_held.find(txid);
    if (it != g_held.end()) {
        it->second.fork_height = std::min(it->second.fork_height, fork_height);
        it->second.release_anchor = std::max(it->second.release_anchor, release_anchor);
        return;
    }
    g_held[txid] = HeldEntry{tx, fork_height, release_anchor, order ? *order : g_seq++};
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

//! The record as it is to be written, waiting for FlushRecord, and the mutex
//! that orders the writes. The file is written, and synced, with g_held_mutex
//! released: validation takes g_held_mutex for every newly certified block
//! (HeldTxsNoteCertified), and must never wait on a disk sync.
std::optional<std::vector<unsigned char>> g_pending_save GUARDED_BY(g_held_mutex);
//! The record changed since it was last serialized: FlushRecord serializes it.
bool g_save_due GUARDED_BY(g_held_mutex){false};
//! When the record last took a departure (steady ms), for -posdebugheldfillms.
int64_t g_departure_ms GUARDED_BY(g_held_mutex){0};
Mutex g_held_file_mutex;

void SaveLocked() EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    // Only the held transactions are written: a restarted member must still
    // refuse to sign their respends. Set-asides are never written, so no
    // record, lost or stale, can keep a block out of a node's fork choice.
    // Serialized here, in memory; FlushRecord writes the latest form.
    std::vector<const HeldEntry*> order;
    for (const auto& [txid, e] : g_held) order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const HeldEntry* a, const HeldEntry* b) { return a->seq < b->seq; });
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << HELD_FILE_VERSION;
    ss << (uint64_t)order.size();
    for (const HeldEntry* e : order) ss << e->tx << e->fork_height << e->release_anchor;
    g_pending_save.emplace(UCharCast(ss.data()), UCharCast(ss.data()) + ss.size());
}

//! Write the record's latest form, if one waits. Called with no lock held,
//! at the end of every public function that can change the record, and by
//! the maintenance thread. A later form always replaces an earlier one that
//! has not been written yet, and writes happen one at a time in that order.
void FlushRecord(bool force = false) LOCKS_EXCLUDED(g_held_mutex, g_held_file_mutex)
{
    LOCK(g_held_file_mutex);
    std::optional<std::vector<unsigned char>> data;
    {
        LOCK(g_held_mutex);
        // Test only (-posdebugheldfillms): a departure waits in memory for a
        // while, unless a flush of the chainstate needs it on disk first.
        if (!force && g_departure_ms != 0) {
            const int64_t fill_ms = gArgs.GetIntArg("-posdebugheldfillms", 0);
            if (fill_ms > 0 && SteadyMs() - g_departure_ms < fill_ms) return;
        }
        if (std::exchange(g_save_due, false)) SaveLocked();
        data.swap(g_pending_save);
    }
    if (!data) return;
    const fs::path tmp = gArgs.GetDataDirNet() / "heldtxs.dat.new";
    FILE* f = fsbridge::fopen(tmp, "wb");
    if (!f) {
        LogPrintf("Held transactions: cannot write %s\n", fs::PathToString(tmp));
        return;
    }
    CAutoFile file(f, SER_DISK, CLIENT_VERSION);
    try {
        file.write(MakeByteSpan(*data));
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
    // Serialized by FlushRecord, off every caller's path.
    g_save_due = true;
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

//! The highest block of `tip`'s chain that carries its parent's quorum,
//! looking back at most 2000 blocks.
const CBlockIndex* HighestCertified(const CBlockIndex* tip) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    int scanned = 0;
    for (const CBlockIndex* p = tip; p && scanned < 2000; p = p->pprev, ++scanned) {
        if (CarriesQuorum(p)) return p;
    }
    return nullptr;
}

//! Whether a block has been certified since the hold's clocks started: the
//! active chain's highest certified block was first judged certified after
//! the mark. The blocks of a branch Bitcoin returns to were judged before it,
//! so flipping between branches already seen restarts no clock.
bool CertifiedSinceMarkLocked() EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    const auto it = g_cert_order.find(g_cert_block);
    return it != g_cert_order.end() && it->second > g_hold_mark;
}

//! A hold begins (its first decline or set-aside): start the backstop's clock.
void BeginHoldLocked() EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    if (g_hold_first_ms == 0) {
        g_hold_first_ms = SteadyMs();
        g_hold_mark = g_cert_seq;
    }
}

void ReleaseAllLocked(const std::string& why) EXCLUSIVE_LOCKS_REQUIRED(g_held_mutex)
{
    LogPrintf("Held transactions: %s; released %u held transaction(s) and %u set-aside block(s)\n",
              why, g_held.size(), g_aside.size());
    g_held.clear();
    g_spent_by.clear();
    g_given_up.clear();
    g_follow.clear();
    g_hold_since_ms = 0;
    g_hold_first_ms = 0;
    g_declined = false;
    Changed();
}

} // namespace

bool HeldTxsActive() { return g_held_active; }
void HeldTxsNoteDisconnected(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block)
{
    AssertLockHeld(cs_main);
    if (!g_held_active || !pindex || !block) return;
    g_pending.push_back(Departed{pindex, block});
}

void HeldTxsRecordDeparted(const CBlockIndex* tip)
{
    const auto on_chain = [tip](const CBlockIndex* p) { return tip && p->nHeight <= tip->nHeight && tip->GetAncestor(p->nHeight) == p; };
    AssertLockHeld(cs_main);
    if (g_pending.empty()) return;
    std::vector<Departed> pending;
    pending.swap(g_pending);
    if (!g_held_active) return;
    const int64_t start = GetTimeMicros();
    // What left and has not come back, lowest first, once each: a block that
    // left and returned within the same activation is no departure.
    std::map<const CBlockIndex*, std::shared_ptr<const CBlock>> by_block;
    for (const Departed& d : pending) {
        if (!on_chain(d.pindex)) by_block[d.pindex] = d.block;
    }
    std::vector<std::pair<const CBlockIndex*, std::shared_ptr<const CBlock>>> off(by_block.begin(), by_block.end());
    std::sort(off.begin(), off.end(), [](const auto& x, const auto& y) {
        return x.first->nHeight != y.first->nHeight ? x.first->nHeight < y.first->nHeight : x.first < y.first;
    });
    LOCK(g_held_mutex);
    if (g_held.empty()) g_departed_cert.clear();
    for (auto it = g_departed_cert.begin(); it != g_departed_cert.end();) {
        it = on_chain(*it) ? g_departed_cert.erase(it) : std::next(it);
    }
    // A block is as final as the highest certified block above it on its
    // branch: hold every block at or below one that carried its parent's
    // quorum, whether that one left in this activation or an earlier one.
    for (const auto& [p, _] : off) {
        if (CarriesQuorum(p)) g_departed_cert.insert(p);
    }
    uint32_t release_anchor = 0;
    for (const auto& [p, _] : off) release_anchor = std::max(release_anchor, p->m_anchor_height);
    release_anchor += POS_HELD_WINDOW;
    // A transaction that spends the coinbase of a block that left can never be
    // mined on the new chain (that coinbase is not mined again), nor can one
    // that spends its outputs: holding them would only freeze their other
    // inputs. Such a block can leave after the block that spends it, so what
    // is already held is pruned too.
    std::set<uint256> dead;
    for (const auto& [p, block] : off) {
        if (!block->vtx.empty() && block->vtx[0]->IsCoinBase()) dead.insert(block->vtx[0]->GetHash());
    }
    int blocks = 0;
    //! A block left that the record does not refuse: neither one the rule set
    //! aside, nor one that respends a held transaction, nor one built on such
    //! a block (an operator's invalidateblock of a respend does on an RPC-only
    //! member what a set-aside does on a producer).
    bool departed = false;
    std::set<const CBlockIndex*> refused;
    int fork_height = std::numeric_limits<int>::max();
    size_t n = 0;
    for (const auto& [p, block] : off) {
        // A block the rule set aside itself is the maintenance pass's, not a departure.
        bool aside = false;
        for (const auto& [hash, a] : g_aside) {
            if (p->nHeight >= a.pindex->nHeight && p->GetAncestor(a.pindex->nHeight) == a.pindex) aside = true;
        }
        if (aside) continue;
        if ((p->pprev && refused.count(p->pprev)) || TxsConflictLocked(*block)) {
            refused.insert(p);
        } else {
            departed = true;
        }
        bool held = false;
        for (const CBlockIndex* c : g_departed_cert) {
            if (c->nHeight >= p->nHeight && c->GetAncestor(p->nHeight) == p) {
                held = true;
                break;
            }
        }
        if (!held) continue;
        const CBlockIndex* fork = tip ? LastCommonAncestor(tip, p) : nullptr;
        const int fh = fork ? fork->nHeight : -1;
        fork_height = std::min(fork_height, fh);
        ++blocks;
        for (size_t i = 0; i < block->vtx.size(); ++i) {
            const CTransactionRef& tx = block->vtx[i];
            if (tx->IsCoinBase()) continue;
            bool is_dead = false;
            for (const CTxIn& in : tx->vin) {
                if (!in.m_is_pegin && dead.count(in.prevout.hash)) is_dead = true;
            }
            if (is_dead) {
                dead.insert(tx->GetHash());
                continue;
            }
            // Parents before children, whatever order the blocks leave in.
            AddEntry(tx, fh, release_anchor, ((uint64_t)p->nHeight << 24) | (uint64_t)i);
            ++n;
        }
    }
    for (bool more = true; more;) {
        more = false;
        std::vector<uint256> drop;
        for (const auto& [txid, e] : g_held) {
            for (const CTxIn& in : e.tx->vin) {
                if (!in.m_is_pegin && dead.count(in.prevout.hash)) {
                    drop.push_back(txid);
                    break;
                }
            }
        }
        for (const uint256& txid : drop) {
            dead.insert(txid);
            EraseEntry(txid);
            more = true;
        }
    }
    if (!departed) return;
    // The departure lowered the chain, whether or not anything certified
    // left: progress is read again from the new tip, and counted from here
    // on: only a block certified after this departure ends the hold. The
    // bound's clock starts again from the member's next honest proposal or
    // share after a decline, since what it holds has had no round on this
    // chain yet. The backstop's clock runs on.
    g_release_scanned.clear();
    g_cert_block = HighestCertified(tip);
    g_hold_since_ms = 0;
    g_declined = false;
    if (g_hold_first_ms != 0) g_hold_mark = g_cert_seq;
    if (blocks == 0) {
        LogPrint(BCLog::VALIDATION, "Held transactions: none of the %u block(s) that left was certified; nothing held\n", off.size());
        return;
    }
    // Only the lowest point any held block left from bounds the check of a
    // proposal's ancestors (MinForkHeight), whichever entry carries it.
    for (auto& [_, e] : g_held) e.fork_height = std::min(e.fork_height, fork_height);
    g_departure_ms = SteadyMs();
    Changed();
    LogPrintf("Held transactions: holding %u transaction(s) of %d disconnected block(s) above height %d "
              "(release by anchor %u), %u held in all, filled in %.2fms\n",
              n, blocks, fork_height, release_anchor, g_held.size(), (GetTimeMicros() - start) * 0.001);
}

void HeldTxsCheckNoneCarried(const CBlockIndex* tip)
{
    AssertLockHeld(cs_main);
    if (g_pending.empty()) return;
    LogPrintf("Held transactions: ERROR: %u departure(s) carried into another activation; recording them now\n", g_pending.size());
    HeldTxsRecordDeparted(tip);
}

void HeldTxsFlush(bool force) { FlushRecord(force); }

int64_t HeldTxsBoundMs()
{
    return POS_HELD_BOUND_ROUNDS * PosGossipRoundMs() + PosGossipWindowMs();
}

void HeldTxsInit()
{
    g_held_active = g_con_pos && g_con_bitcoin_anchor && !gArgs.GetBoolArg("-posdebugignoreheld", false);
    WITH_LOCK(g_held_mutex, g_bound_ms = HeldTxsBoundMs());
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
    if (g_held.empty()) return;
    g_declined = true;
    BeginHoldLocked();
}

void HeldTxsNoteHold()
{
    if (!g_held_active) return;
    LOCK(g_held_mutex);
    // The bound's clock starts here; the mark stays where the hold began
    // (BeginHoldLocked), so a block first certified between the first decline
    // and this share still ends the hold.
    if (g_hold_since_ms == 0 && !g_held.empty() && (g_declined || !g_aside.empty())) {
        g_hold_since_ms = SteadyMs();
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

void HeldTxsNoteCertified(const CBlockIndex* pindex, bool newly)
{
    AssertLockHeld(cs_main);
    if (!g_held_active || !pindex) return;
    if (!newly && g_aside_count.load() == 0) return;
    LOCK(g_held_mutex);
    // Its place in the order of certification, given once (emplace keeps a
    // place already given), and only while something is held: a hold compares
    // nothing older.
    if (newly && (!g_held.empty() || !g_aside.empty())) g_cert_order.emplace(pindex, ++g_cert_seq);
    if (g_aside.empty() || !CarriesQuorum(pindex)) return;
    for (const auto& [hash, a] : g_aside) {
        if (pindex->nHeight > a.pindex->nHeight && pindex->GetAncestor(a.pindex->nHeight) == a.pindex && g_follow.insert(hash).second) {
            LogPrintf("Held transactions: certified block %s builds on set-aside block %s; following it\n",
                      pindex->GetBlockHash().ToString(), hash.ToString());
        }
    }
}

std::vector<CTransactionRef> HeldTxsForMining()
{
    std::vector<CTransactionRef> out;
    if (!g_held_active) return out;
    LOCK(g_held_mutex);
    std::vector<const HeldEntry*> order;
    for (const auto& [txid, e] : g_held) order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const HeldEntry* a, const HeldEntry* b) { return a->seq < b->seq; });
    for (const HeldEntry* e : order) out.push_back(e->tx);
    return out;
}

static void Release(const uint256& txid, const std::string& why)
{
    if (!g_held_active) return;
    LOCK(g_held_mutex);
    if (!g_held.count(txid)) return;
    LogPrintf("Held transactions: %s released: %s\n", txid.ToString(), why);
    EraseEntry(txid);
    Changed();
}

static bool Maintain(ChainstateManager& chainman, CTxMemPool& mempool, bool park)
{
    if (!g_held_active) return false;
    {
        LOCK(g_held_mutex);
        if (g_held.empty() && g_aside.empty()) {
            g_block_conflict.clear();
            g_given_up.clear();
            g_follow.clear();
            g_cert_order.clear();
            g_hold_since_ms = 0;
            g_hold_first_ms = 0;
            g_declined = false;
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
        const CBlockIndex* cert_any = HighestCertified(tip);
        g_cert_block = cert_any;

        const int64_t bound = g_bound_ms;
        // A certified block since the hold began ends the hold (the bound's
        // clock and the backstop's).
        if ((g_hold_since_ms != 0 || g_hold_first_ms != 0 || g_declined) && CertifiedSinceMarkLocked()) {
            g_hold_since_ms = 0;
            g_hold_first_ms = 0;
            g_declined = false;
        }
        if (g_hold_first_ms == 0 && !g_aside.empty()) BeginHoldLocked();
        if (bound > 0 && g_hold_since_ms != 0 && now - g_hold_since_ms >= bound) {
            // The liveness bound: counted from the first honest proposal or share.
            for (const auto& [hash, a] : g_aside) to_restore.push_back(hash);
            ReleaseAllLocked(strprintf("no certified block for %ds while holding (the liveness bound)",
                                       (int)((now - g_hold_since_ms) / 1000)));
        } else if (bound > 0 && g_hold_first_ms != 0 && now - g_hold_first_ms >= POS_HELD_BACKSTOP_BOUNDS * bound) {
            // The backstop: counted from the hold's first decline or set-aside,
            // whatever this member has done or seen since.
            for (const auto& [hash, a] : g_aside) to_restore.push_back(hash);
            ReleaseAllLocked(strprintf("held for %.1fs since the first decline or set-aside (the backstop, %.1fs)",
                                       (now - g_hold_first_ms) * 0.001, POS_HELD_BACKSTOP_BOUNDS * bound * 0.001));
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
        // the respend; follow it). The last is learnt from the certification
        // hook (HeldTxsNoteCertified), and from one scan of the block index
        // when the block is set aside, never by scanning it again.
        for (const auto& [hash, a] : g_aside) {
            if (std::find(to_restore.begin(), to_restore.end(), hash) != to_restore.end()) continue;
            bool restore = g_held.empty() || (cert_any && cert_any->nHeight >= a.pindex->nHeight) || g_follow.count(hash);
            if (!restore && !IndexConflictLocked(a.pindex)) restore = true;
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
                // A certified block may already build on it: one scan, now.
                for (const auto& [h, p] : chainman.m_blockman.m_block_index) {
                    if (p->nHeight > lowest->nHeight && CarriesQuorum(p) && p->GetAncestor(lowest->nHeight) == lowest) {
                        LogPrintf("Held transactions: certified block %s builds on %s; not setting it aside\n",
                                  h.ToString(), lowest->GetBlockHash().ToString());
                        to_park = nullptr;
                        g_given_up.insert(lowest->GetBlockHash());
                        break;
                    }
                }
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
            g_follow.erase(hash);
        }
        g_aside_count = g_aside.size();
        chainman.ActiveChainstate().ReaddBlockIndexCandidates();
        chain_changed = true;
    }
    if (to_park) {
        LogPrintf("Held transactions: setting aside sub-quorum block %s (height %d), which respends a held transaction\n",
                  to_park->GetBlockHash().ToString(), to_park->nHeight);
        {
            LOCK(g_held_mutex);
            g_aside[to_park->GetBlockHash()] = AsideBlock{to_park, SteadyMs()};
            g_aside_count = g_aside.size();
            g_given_up.insert(to_park->GetBlockHash());
            BeginHoldLocked();
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
        // A transaction stays held whatever its fee, and whatever this node's
        // mempool would make of it: a producer that holds mines it from the
        // record (HeldTxsForMining). It is released here only when it is
        // invalid for good on the active chain, by what its transaction id
        // commits to: the transaction itself, or its inputs and amounts against
        // the coins it spends. A script failure is a verdict on one witness,
        // which the id does not commit to (a payment claim's proof record, say,
        // is replaced under the same id), so it keeps the hold. So do a missing
        // input (a parent held or not valid yet, a conflict in a block not
        // certified) and a lock that time lifts (a lock time, a sequence lock,
        // an immature coinbase). A transaction that spends one released as
        // invalid is released with it.
        std::vector<std::pair<uint256, std::string>> release;
        {
            LOCK(cs_main);
            CBlockIndex* tip = chainman.ActiveChain().Tip();
            CCoinsViewCache view(&chainman.ActiveChainstate().CoinsTip());
            std::set<uint256> verified_now;
            std::set<uint256> dead;
            std::set<uint256> already;
            {
                LOCK(g_held_mutex);
                for (const auto& [txid, e] : g_held) {
                    if (e.verified) already.insert(txid);
                }
            }
            for (const CTransactionRef& tx : to_check) {
                if (!tip) break;
                const uint256 txid = tx->GetHash();
                bool parent_dead = false;
                for (const CTxIn& in : tx->vin) {
                    if (!in.m_is_pegin && dead.count(in.prevout.hash)) parent_dead = true;
                }
                if (parent_dead) {
                    release.emplace_back(txid, "it spends a held transaction released as invalid");
                    dead.insert(txid);
                    continue;
                }
                if (already.count(txid)) {
                    // Checked once already; only its outputs matter now, for its children.
                    if (view.HaveInputs(*tx)) {
                        for (const CTxIn& in : tx->vin) {
                            if (!in.m_is_pegin) view.SpendCoin(in.prevout);
                        }
                        AddCoins(view, *tx, tip->nHeight + 1);
                    }
                    continue;
                }
                TxValidationState st;
                CAmountMap fee_map;
                int64_t sigops = 0;
                bool script_failure = false;
                if (CheckTxForNextBlock(*tx, view, tip, Params().GetConsensus(), st, fee_map, sigops, /*cache=*/false, &script_failure)) {
                    verified_now.insert(txid);
                    for (const CTxIn& in : tx->vin) {
                        if (!in.m_is_pegin) view.SpendCoin(in.prevout);
                    }
                    AddCoins(view, *tx, tip->nHeight + 1);
                    continue;
                }
                const TxValidationResult kind = st.GetResult();
                if (kind == TxValidationResult::TX_CONSENSUS && !script_failure) {
                    release.emplace_back(txid, "invalid on the active chain: " + st.ToString());
                    dead.insert(txid);
                } else {
                    LogPrint(BCLog::MEMPOOL, "Held transactions: %s not minable yet: %s\n", txid.ToString(), st.ToString());
                }
            }
            LOCK(g_held_mutex);
            for (const uint256& txid : verified_now) {
                auto it = g_held.find(txid);
                if (it != g_held.end()) it->second.verified = true;
            }
            if (!release.empty()) {
                for (const auto& [txid, why] : release) {
                    LogPrintf("Held transactions: %s released: %s\n", txid.ToString(), why);
                    EraseEntry(txid);
                }
                Changed();
            }
        }
        // The rest go back into the mempool where its policy takes them, so
        // that nodes without the rule relay and mine them too. A refusal here
        // releases nothing.
        size_t added = 0;
        {
            LOCK(cs_main);
            LOCK(mempool.cs);
            for (const CTransactionRef& tx : to_check) {
                const uint256 txid = tx->GetHash();
                if (mempool.exists(GenTxid::Txid(txid))) continue;
                if (std::any_of(release.begin(), release.end(), [&](const auto& r) { return r.first == txid; })) continue;
                for (const CTxIn& in : tx->vin) {
                    if (const CTransaction* c = mempool.GetConflictTx(in.prevout)) {
                        if (c->GetHash() != txid) mempool.removeRecursive(*c, MemPoolRemovalReason::REPLACED);
                    }
                }
                const MempoolAcceptResult r = AcceptToMemoryPool(chainman.ActiveChainstate(), tx, GetTime(),
                                                                 /*bypass_limits=*/false, /*test_accept=*/false);
                if (r.m_result_type == MempoolAcceptResult::ResultType::VALID) {
                    ++added;
                } else {
                    LogPrint(BCLog::MEMPOOL, "Held transactions: %s not readmitted to the mempool: %s\n", txid.ToString(), r.m_state.ToString());
                }
            }
        }
        if (added) LogPrintf("Held transactions: readmitted %u held transaction(s) to the mempool\n", added);
    }
    return false;
}

void HeldTxsRelease(const uint256& txid, const std::string& why)
{
    Release(txid, why);
    FlushRecord();
}

bool HeldTxsMaintain(ChainstateManager& chainman, CTxMemPool& mempool, bool park)
{
    const bool changed_chain = Maintain(chainman, mempool, park);
    FlushRecord();
    return changed_chain;
}

void HeldTxsStart(ChainstateManager& chainman, CTxMemPool& mempool)
{
    if (!g_held_active || g_held_thread.joinable()) return;
    g_held_stop = false;
    g_held_thread = std::thread(&util::TraceThread, "heldtxs", [&chainman, &mempool] {
        while (!g_held_stop && !ShutdownRequested()) {
            try {
                HeldTxsMaintain(chainman, mempool, /*park=*/false);
            } catch (const std::exception& e) {
                LogPrintf("WARNING: held-transactions maintenance failed, retrying at the next pass: %s\n", e.what());
            }
            for (int64_t i = 0; i < POS_HELD_MAINTAIN_MS / 100 && !g_held_stop && !ShutdownRequested(); ++i) {
                UninterruptibleSleep(std::chrono::milliseconds{100});
            }
        }
    });
}

void HeldTxsStop()
{
    g_held_stop = true;
    if (g_held_thread.joinable()) g_held_thread.join();
    FlushRecord();
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
    info.held_for_ms = g_hold_first_ms ? SteadyMs() - g_hold_first_ms : -1;
    return info;
}
