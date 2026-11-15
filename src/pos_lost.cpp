// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_lost.h>

#include <anchor.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <logging.h>
#include <mainchainrpc.h>
#include <node/blockstorage.h>
#include <pos.h>
#include <rpc/protocol.h>
#include <streams.h>
#include <sync.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>

using node::ReadBlockFromDisk;

namespace {
//! The Bitcoin daemon's block count.
bool GetMainchainBlockCount(int& count)
{
    try {
        const UniValue reply = CallMainChainRPC("getblockcount", UniValue(UniValue::VARR));
        if (!find_value(reply, "error").isNull()) return false;
        const UniValue result = find_value(reply, "result");
        if (!result.isNum()) return false;
        count = result.get_int();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace

namespace {

enum class AnchorState { UNKNOWN, OK, STALE };

struct LostBlk {
    std::shared_ptr<const CBlock> block;
    uint256 hash;
    uint256 prev;
    int height{0};
    uint32_t anchor_h{0};
    uint256 anchor;
    //! Carries its parent's full quorum: judged when this node connected it,
    //! or verified when the evidence arrived.
    bool certified{false};
    //! This node connected it (and judged its certificate then).
    bool own{false};
    std::vector<std::vector<unsigned char>> btc_headers;
    // Computed by each maintenance pass.
    AnchorState anchor_state{AnchorState::UNKNOWN};
    bool prot{false};
    uint256 fork_hash;
    int fork_height{-1};
};

struct LostSpend {
    uint256 txid;
    uint256 block;
    uint32_t anchor_h;
    uint256 fork_hash;
    int fork_height;
};

Mutex g_lost_mutex;
std::map<uint256, LostBlk> g_blocks GUARDED_BY(g_lost_mutex);
std::map<COutPoint, std::vector<LostSpend>> g_spends GUARDED_BY(g_lost_mutex);
uint64_t g_gen GUARDED_BY(g_lost_mutex){0};
bool g_dirty_file GUARDED_BY(g_lost_mutex){false};
std::deque<std::pair<int64_t, LostEvidenceMsg>> g_inbox GUARDED_BY(g_lost_mutex);
std::map<int64_t, std::set<uint256>> g_sent GUARDED_BY(g_lost_mutex);
//! Blocks refused under the rule (BLOCK_FAILED_LOST), judged again on change.
std::set<uint256> g_refused GUARDED_BY(g_lost_mutex);

std::vector<std::pair<const CBlockIndex*, std::shared_ptr<const CBlock>>> g_pending GUARDED_BY(::cs_main);

bool g_ignore{false};
uint32_t g_window{DEFAULT_POS_LOST_WINDOW};
uint64_t g_last_scan_gen{0};
bool g_refused_scanned{false};
int64_t g_last_replay{0};

fs::path LostPath() { return gArgs.GetDataDirNet() / "lostblocks.dat"; }

LostBlk MakeBlk(const std::shared_ptr<const CBlock>& block, int height)
{
    LostBlk b;
    b.block = block;
    b.hash = block->GetHash();
    b.prev = block->hashPrevBlock;
    b.height = height;
    b.anchor_h = block->m_anchor_height;
    b.anchor = block->m_anchor_hash;
    return b;
}

//! Ask the parent daemon directly (no cache) where `hash` stands.
AnchorState DirectVerdict(uint32_t height, const uint256& hash)
{
    try {
        UniValue params(UniValue::VARR);
        params.push_back(hash.GetHex());
        const UniValue reply = CallMainChainRPC("getblockheader", params);
        if (!find_value(reply, "error").isNull()) return AnchorState::UNKNOWN;
        const UniValue result = find_value(reply, "result");
        if (!result.isObject()) return AnchorState::UNKNOWN;
        const UniValue h = find_value(result.get_obj(), "height");
        if (!h.isNum() || h.get_int64() != (int64_t)height) return AnchorState::UNKNOWN;
        const UniValue conf = find_value(result.get_obj(), "confirmations");
        if (!conf.isNum()) return AnchorState::UNKNOWN;
        return conf.get_int64() < 1 ? AnchorState::STALE : AnchorState::OK;
    } catch (const std::exception&) {
        return AnchorState::UNKNOWN;
    }
}

AnchorState CachedVerdict(uint32_t height, const uint256& hash)
{
    switch (CheckMainchainAnchor(height, hash)) {
    case AnchorCheckResult::OK: return AnchorState::OK;
    case AnchorCheckResult::STALE: return AnchorState::STALE;
    default: return AnchorState::UNKNOWN;
    }
}

//! The Bitcoin headers from the first one off the best chain up to `anchor`,
//! parents first (at most 2 windows plus one).
std::vector<std::vector<unsigned char>> CollectStaleHeaders(const uint256& anchor)
{
    std::vector<std::vector<unsigned char>> out;
    uint256 h = anchor;
    try {
        for (uint32_t i = 0; i < 2 * g_window + 1; ++i) {
            UniValue p(UniValue::VARR);
            p.push_back(h.GetHex());
            const UniValue v = find_value(CallMainChainRPC("getblockheader", p), "result");
            if (!v.isObject()) break;
            if (find_value(v.get_obj(), "confirmations").get_int64() >= 1) break;
            UniValue p2(UniValue::VARR);
            p2.push_back(h.GetHex());
            p2.push_back(false);
            const UniValue hex = find_value(CallMainChainRPC("getblockheader", p2), "result");
            if (!hex.isStr()) break;
            out.push_back(ParseHex(hex.get_str()));
            const UniValue prev = find_value(v.get_obj(), "previousblockhash");
            if (!prev.isStr()) break;
            h = uint256S(prev.get_str());
        }
    } catch (const std::exception&) {
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void SubmitHeaders(const std::vector<std::vector<unsigned char>>& headers)
{
    for (const auto& hdr : headers) {
        try {
            UniValue p(UniValue::VARR);
            p.push_back(HexStr(hdr));
            CallMainChainRPC("submitheader", p);
        } catch (const std::exception&) {
        }
    }
}

void WriteFile() EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    const fs::path tmp = gArgs.GetDataDirNet() / "lostblocks.dat.new";
    FILE* f = fsbridge::fopen(tmp, "wb");
    if (!f) return;
    CAutoFile file(f, SER_DISK, PROTOCOL_VERSION);
    try {
        file << (uint32_t)1 << (uint64_t)g_blocks.size();
        for (const auto& [h, b] : g_blocks) {
            file << *b.block << b.height << b.certified << b.own << b.btc_headers;
        }
        if (!FileCommit(file.Get())) return;
        file.fclose();
        RenameOver(tmp, LostPath());
        g_dirty_file = false;
    } catch (const std::exception& e) {
        LogPrintf("Lost blocks: cannot write %s: %s\n", fs::PathToString(LostPath()), e.what());
    }
}

//! Whether `s` binds a block anchored at `anchor_h` built on `prev`.
bool Binds(const LostSpend& s, uint32_t anchor_h, const CBlockIndex* prev)
{
    if (anchor_h > s.anchor_h + g_window) return false;
    const CBlockIndex* f = prev ? prev->GetAncestor(s.fork_height) : nullptr;
    return f && f->GetBlockHash() == s.fork_hash;
}

std::optional<std::string> ViolationLocked(const CBlock& block, const CBlockIndex* prev) EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    if (g_spends.empty()) return std::nullopt;
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const CTransaction& tx = *block.vtx[i];
        const uint256 txid = tx.GetHash();
        for (const CTxIn& in : tx.vin) {
            if (in.m_is_pegin) continue;
            auto it = g_spends.find(in.prevout);
            if (it == g_spends.end()) continue;
            for (const LostSpend& s : it->second) {
                if (s.txid == txid) continue;
                if (!Binds(s, block.m_anchor_height, prev)) continue;
                return strprintf("tx %s spends %s:%u, which tx %s of lost block %s (anchor %u, fork %d) spends",
                                 txid.ToString(), in.prevout.hash.ToString(), in.prevout.n, s.txid.ToString(),
                                 s.block.ToString(), s.anchor_h, s.fork_height);
            }
        }
    }
    return std::nullopt;
}

} // namespace

bool LostActive()
{
    return g_con_pos && g_con_bitcoin_anchor && g_validate_anchor && !g_ignore;
}

uint32_t LostWindow() { return g_window; }

void LostInit()
{
    g_ignore = gArgs.GetBoolArg("-posdebugignorelost", false);
    g_window = (uint32_t)std::max<int64_t>(1, gArgs.GetIntArg("-poslostwindow", DEFAULT_POS_LOST_WINDOW));
    LOCK(g_lost_mutex);
    FILE* f = fsbridge::fopen(LostPath(), "rb");
    if (!f) return;
    CAutoFile file(f, SER_DISK, PROTOCOL_VERSION);
    try {
        uint32_t version;
        uint64_t n;
        file >> version >> n;
        for (uint64_t i = 0; i < n && i < POS_LOST_MAX_BLOCKS; ++i) {
            auto block = std::make_shared<CBlock>();
            int height;
            bool certified, own;
            std::vector<std::vector<unsigned char>> headers;
            file >> *block >> height >> certified >> own >> headers;
            LostBlk b = MakeBlk(block, height);
            b.certified = certified;
            b.own = own;
            b.btc_headers = std::move(headers);
            g_blocks.emplace(b.hash, std::move(b));
        }
        LogPrintf("Lost blocks: read %u from %s\n", (unsigned)g_blocks.size(), fs::PathToString(LostPath()));
    } catch (const std::exception& e) {
        LogPrintf("Lost blocks: cannot read %s: %s\n", fs::PathToString(LostPath()), e.what());
    }
}

void LostShutdown()
{
    LOCK(g_lost_mutex);
    WriteFile();
}

void LostNoteDisconnected(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block)
{
    AssertLockHeld(::cs_main);
    if (!g_con_pos || !g_con_bitcoin_anchor || !g_validate_anchor || !pindex || !block) return;
    // Held durably before cs_main is released, so before this node signs
    // anything after the disconnect: an unclean stop cannot lose it.
    LOCK(g_lost_mutex);
    const uint256 h = pindex->GetBlockHash();
    if (g_blocks.count(h)) return;
    LostBlk b = MakeBlk(block, pindex->nHeight);
    b.certified = pindex->PosCarriesQuorum();
    b.own = true;
    g_blocks.emplace(h, std::move(b));
    WriteFile();
}

bool LostCheckBlock(const CBlock& block, const CBlockIndex* pindexPrev, bool ibd,
                    const std::function<bool()>& certified_descendant, BlockValidationState& state)
{
    if (!LostActive() || !pindexPrev || ibd) return true;
    LOCK(g_lost_mutex);
    if (auto why = ViolationLocked(block, pindexPrev)) {
        if (certified_descendant()) {
            LogPrintf("Lost blocks: block %s at height %d breaks the rule but has a certified descendant; the evidence does not bear on it: %s\n",
                      block.GetHash().ToString(), pindexPrev->nHeight + 1, *why);
            return true;
        }
        LogPrintf("Lost blocks: refusing block %s at height %d: %s\n", block.GetHash().ToString(),
                  pindexPrev->nHeight + 1, *why);
        return state.Invalid(BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "bad-txns-lost-respend", *why);
    }
    return true;
}

std::optional<uint256> LostConflict(const CTransaction& tx, uint32_t tip_anchor_height)
{
    if (!LostActive()) return std::nullopt;
    LOCK(g_lost_mutex);
    const uint256 txid = tx.GetHash();
    for (const CTxIn& in : tx.vin) {
        auto it = g_spends.find(in.prevout);
        if (it == g_spends.end()) continue;
        for (const LostSpend& s : it->second) {
            if (s.txid != txid && tip_anchor_height <= s.anchor_h + g_window) return s.txid;
        }
    }
    return std::nullopt;
}

void LostNoteRefused(const CBlockIndex* pindex)
{
    LOCK(g_lost_mutex);
    g_refused.insert(pindex->GetBlockHash());
}

void LostQueueEvidence(int64_t peer, LostEvidenceMsg msg)
{
    LOCK(g_lost_mutex);
    g_sent[peer].insert(msg.block.GetHash()); // never echo it back
    if (g_blocks.count(msg.block.GetHash())) return;
    if (g_inbox.size() >= 1000) return;
    g_inbox.emplace_back(peer, std::move(msg));
}

std::string LostAcceptEvidence(ChainstateManager& chainman, const LostEvidenceMsg& msg)
{
    const CBlock& block = msg.block;
    const uint256 hash = block.GetHash();
    {
        LOCK(g_lost_mutex);
        if (g_blocks.count(hash)) return "duplicate";
        if (g_blocks.size() >= POS_LOST_MAX_BLOCKS) return "store-full";
    }
    if (msg.btc_headers.size() > 2 * g_window + 1) return "too-many-headers";
    const Consensus::Params& consensus = Params().GetConsensus();
    BlockValidationState state;
    if (!CheckBlock(block, state, consensus, /*fCheckPOW=*/true, /*fCheckMerkleRoot=*/true)) {
        return "bad-block: " + state.ToString();
    }
    if (block.m_anchor_hash.IsNull()) return "no-anchor";
    // The parent: on the active chain, or a block of the store.
    int height = -1;
    uint32_t parent_anchor_h = 0;
    uint256 parent_anchor;
    bool parent_in_store = false;
    {
        LOCK(cs_main);
        const CBlockIndex* p = chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock);
        if (p && chainman.ActiveChain().Contains(p)) {
            height = p->nHeight + 1;
            parent_anchor_h = p->m_anchor_height;
            parent_anchor = p->m_anchor_hash;
        }
        const CBlockIndex* self = chainman.m_blockman.LookupBlockIndex(hash);
        if (self && chainman.ActiveChain().Contains(self)) return "on-active-chain";
    }
    if (height < 0) {
        LOCK(g_lost_mutex);
        auto it = g_blocks.find(block.hashPrevBlock);
        if (it == g_blocks.end()) return "unknown-parent";
        height = it->second.height + 1;
        parent_anchor_h = it->second.anchor_h;
        parent_anchor = it->second.anchor;
        parent_in_store = true;
    }
    if (g_con_blockheightinheader && (uint32_t)height != block.block_height) return "bad-height";
    if (block.m_anchor_height < parent_anchor_h) return "bad-anchor-not-monotonic";
    int btc_h = 0;
    if (GetMainchainBlockCount(btc_h) && (uint32_t)btc_h > block.m_anchor_height + g_window) return "expired";
    // Where its anchor stands: a block of a branch Bitcoin left, or the child
    // of one. A node whose daemon never saw the stale Bitcoin block learns it
    // from the headers carried with the evidence; they cost real work.
    AnchorState verdict = DirectVerdict(block.m_anchor_height, block.m_anchor_hash);
    if (verdict == AnchorState::UNKNOWN && !msg.btc_headers.empty()) {
        SubmitHeaders(msg.btc_headers);
        verdict = DirectVerdict(block.m_anchor_height, block.m_anchor_hash);
    }
    if (verdict == AnchorState::UNKNOWN) return "anchor-unknown";
    if (verdict == AnchorState::OK && !parent_in_store) return "anchor-on-best-chain";
    // The certificate, against the registry at the parent.
    // PROTOTYPE LIMIT: the registry used is the active tip's. That is the
    // parent's only when no block since the fork point changed the stake
    // (true in the tests here); the specification needs the registry
    // reconstructed at the parent (doc 04, "Lost blocks").
    CBlockIndex parent_idx;
    parent_idx.nHeight = height - 1;
    parent_idx.m_anchor_height = parent_anchor_h;
    parent_idx.m_anchor_hash = parent_anchor;
    const StakeRegistry& reg = StakeRegistry::GetInstance();
    std::string reason;
    const int signers = PosVerifyBitfieldCertificate(block, &parent_idx, reg, reason);
    if (signers < 0) return "bad-certificate: " + reason;
    const bool certified = signers >= PosSlotQuorumAt(reg, height);
    if (!certified && !parent_in_store && verdict == AnchorState::OK) return "not-certified";
    LostBlk b = MakeBlk(std::make_shared<const CBlock>(block), height);
    b.certified = certified;
    b.btc_headers = msg.btc_headers;
    b.anchor_state = verdict;
    LOCK(g_lost_mutex);
    LogPrintf("Lost blocks: took evidence %s at height %d (anchor %u %s, %d signer(s), %s)\n", hash.ToString(), height,
              b.anchor_h, verdict == AnchorState::STALE ? "off Bitcoin's best chain" : "on Bitcoin's best chain",
              signers, certified ? "certified" : "below the quorum");
    g_blocks.emplace(hash, std::move(b));
    g_dirty_file = true;
    return "";
}

std::vector<LostEvidenceMsg> LostToSend(int64_t peer)
{
    std::vector<LostEvidenceMsg> out;
    if (!LostActive()) return out;
    LOCK(g_lost_mutex);
    std::vector<const LostBlk*> todo;
    std::set<uint256>& sent = g_sent[peer];
    for (const auto& [h, b] : g_blocks) {
        if (b.prot && !sent.count(h)) todo.push_back(&b);
    }
    std::sort(todo.begin(), todo.end(), [](const LostBlk* a, const LostBlk* b) { return a->height < b->height; });
    for (const LostBlk* b : todo) {
        if (out.size() >= 8) break;
        sent.insert(b->hash);
        out.push_back(LostEvidenceMsg{*b->block, b->btc_headers});
    }
    return out;
}

void LostPeerGone(int64_t peer)
{
    LOCK(g_lost_mutex);
    g_sent.erase(peer);
}

void LostMaintain(ChainstateManager& chainman, CTxMemPool* mempool)
{
    if (!g_con_pos || !g_con_bitcoin_anchor || !g_validate_anchor) return;
    // 1) What DisconnectTip left: a block this node connected and judged.
    {
        LOCK2(cs_main, g_lost_mutex);
        for (const auto& [pindex, block] : g_pending) {
            const uint256 h = pindex->GetBlockHash();
            if (g_blocks.count(h)) continue;
            LostBlk b = MakeBlk(block, pindex->nHeight);
            b.certified = pindex->PosCarriesQuorum();
            b.own = true;
            g_blocks.emplace(h, std::move(b));
        }
        g_pending.clear();
    }
    // 2) Evidence from peers.
    for (int i = 0; i < 64; ++i) {
        std::pair<int64_t, LostEvidenceMsg> item;
        {
            LOCK(g_lost_mutex);
            if (g_inbox.empty()) break;
            item = std::move(g_inbox.front());
            g_inbox.pop_front();
        }
        const std::string why = LostAcceptEvidence(chainman, item.second);
        if (!why.empty() && why != "duplicate") {
            LogPrint(BCLog::VALIDATION, "Lost blocks: evidence %s from peer %d not taken: %s\n",
                     item.second.block.GetHash().ToString(), item.first, why);
        }
    }
    // 3) Where every anchor stands (the watcher's caches make this cheap).
    std::vector<std::pair<uint256, std::pair<uint32_t, uint256>>> anchors;
    {
        LOCK(g_lost_mutex);
        for (const auto& [h, b] : g_blocks) anchors.push_back({h, {b.anchor_h, b.anchor}});
    }
    std::map<uint256, AnchorState> verdicts;
    for (const auto& [h, a] : anchors) verdicts[h] = CachedVerdict(a.first, a.second);
    // The lapse is read from Bitcoin's height, never from the tip's anchor: a
    // tip that cannot advance would otherwise hold a refusal for ever.
    int btc_h = -1;
    if (!GetMainchainBlockCount(btc_h)) btc_h = -1;
    // 4) Which blocks are lost, and the outpoints they bind.
    bool changed = false;
    std::vector<uint256> need_headers;
    {
        LOCK2(cs_main, g_lost_mutex);
        for (auto it = g_blocks.begin(); it != g_blocks.end();) {
            LostBlk& b = it->second;
            auto v = verdicts.find(it->first);
            if (v != verdicts.end() && v->second != AnchorState::UNKNOWN) b.anchor_state = v->second;
            const CBlockIndex* self = chainman.m_blockman.LookupBlockIndex(b.hash);
            const bool active = self && chainman.ActiveChain().Contains(self);
            const bool expired = btc_h >= 0 && (uint32_t)btc_h > b.anchor_h + g_window;
            if (active || expired) {
                LogPrint(BCLog::VALIDATION, "Lost blocks: dropping %s (%s)\n", b.hash.ToString(), active ? "on the active chain" : "past the window");
                it = g_blocks.erase(it);
                g_dirty_file = true;
                continue;
            }
            ++it;
        }
        // Fork points, and whether a branch went stale at or below each block.
        std::map<uint256, bool> branch_stale;
        for (auto& [h, b] : g_blocks) {
            b.fork_height = -1;
            b.fork_hash.SetNull();
            bool stale = false;
            const LostBlk* walk = &b;
            for (int guard = 0; walk && guard < 10000; ++guard) {
                if (walk->anchor_state == AnchorState::STALE) stale = true;
                auto p = g_blocks.find(walk->prev);
                if (p != g_blocks.end()) {
                    walk = &p->second;
                    continue;
                }
                const CBlockIndex* f = chainman.m_blockman.LookupBlockIndex(walk->prev);
                if (f && chainman.ActiveChain().Contains(f)) {
                    b.fork_height = f->nHeight;
                    b.fork_hash = f->GetBlockHash();
                }
                break;
            }
            branch_stale[h] = stale && b.fork_height >= 0;
        }
        // Protected: a certified block of a stale branch, and its ancestors in the store.
        std::set<uint256> prot;
        for (const auto& [h, b] : g_blocks) {
            if (!b.certified || !branch_stale[h]) continue;
            const LostBlk* walk = &b;
            while (walk && prot.insert(walk->hash).second) {
                auto p = g_blocks.find(walk->prev);
                walk = p == g_blocks.end() ? nullptr : &p->second;
            }
        }
        std::map<COutPoint, std::vector<LostSpend>> spends;
        for (auto& [h, b] : g_blocks) {
            const bool was = b.prot;
            b.prot = prot.count(h) > 0;
            if (b.prot && !was) {
                LogPrintf("Lost blocks: %s at height %d (anchor %u, fork %d) is lost; its spends are bound until anchor %u\n",
                          h.ToString(), b.height, b.anchor_h, b.fork_height, b.anchor_h + g_window);
                if (b.own && b.btc_headers.empty()) need_headers.push_back(h);
            }
            if (!b.prot) continue;
            for (size_t i = 1; i < b.block->vtx.size(); ++i) {
                const CTransaction& tx = *b.block->vtx[i];
                for (const CTxIn& in : tx.vin) {
                    if (in.m_is_pegin) continue;
                    spends[in.prevout].push_back(LostSpend{tx.GetHash(), h, b.anchor_h, b.fork_hash, b.fork_height});
                }
            }
        }
        auto key = [](const std::map<COutPoint, std::vector<LostSpend>>& m) {
            std::vector<std::tuple<COutPoint, uint256, uint256, int>> k;
            for (const auto& [o, v] : m) for (const auto& s : v) k.emplace_back(o, s.txid, s.fork_hash, s.fork_height);
            return k;
        };
        if (key(spends) != key(g_spends)) {
            g_spends = std::move(spends);
            ++g_gen;
            changed = true;
            g_dirty_file = true;
        }
    }
    for (const uint256& h : need_headers) {
        uint256 anchor;
        {
            LOCK(g_lost_mutex);
            auto it = g_blocks.find(h);
            if (it == g_blocks.end()) continue;
            anchor = it->second.anchor;
        }
        auto headers = CollectStaleHeaders(anchor);
        LOCK(g_lost_mutex);
        auto it = g_blocks.find(h);
        if (it != g_blocks.end()) it->second.btc_headers = std::move(headers);
    }
    {
        LOCK(g_lost_mutex);
        if (g_dirty_file) WriteFile();
    }
    if (!LostActive()) return;
    const int64_t now = GetTime();
    uint64_t gen;
    bool any;
    {
        LOCK(g_lost_mutex);
        gen = g_gen;
        any = !g_spends.empty() || !g_refused.empty() || !g_refused_scanned;
    }
    if (!any) return;
    // 5) The mempool: drop what conflicts, put lost transactions back, first in
    //    line for the next block whatever their fee.
    if (mempool && (changed || now - g_last_replay >= 2)) {
        g_last_replay = now;
        std::vector<CTransactionRef> lost_txs;
        {
            LOCK(g_lost_mutex);
            std::vector<const LostBlk*> order;
            for (const auto& [h, b] : g_blocks) if (b.prot) order.push_back(&b);
            std::sort(order.begin(), order.end(), [](const LostBlk* a, const LostBlk* b) { return a->height < b->height; });
            for (const LostBlk* b : order) for (size_t i = 1; i < b->block->vtx.size(); ++i) lost_txs.push_back(b->block->vtx[i]);
        }
        LOCK2(cs_main, mempool->cs);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        const uint32_t tip_anchor_h = tip ? tip->m_anchor_height : 0;
        std::vector<CTransactionRef> conflicting;
        for (const auto& tx : lost_txs) {
            for (const CTxIn& in : tx->vin) {
                const CTransaction* c = mempool->GetConflictTx(in.prevout);
                if (c && c->GetHash() != tx->GetHash() && LostConflict(*c, tip_anchor_h)) {
                    conflicting.push_back(mempool->get(c->GetHash()));
                }
            }
        }
        for (const auto& c : conflicting) {
            if (!c || !mempool->exists(GenTxid::Txid(c->GetHash()))) continue;
            LogPrintf("Lost blocks: removing %s from the mempool: it respends a lost transaction\n", c->GetHash().ToString());
            mempool->removeRecursive(*c, MemPoolRemovalReason::CONFLICT);
        }
        for (const auto& tx : lost_txs) {
            if (mempool->exists(GenTxid::Txid(tx->GetHash()))) continue;
            const MempoolAcceptResult r = AcceptToMemoryPool(chainman.ActiveChainstate(), tx, GetTime(), /*bypass_limits=*/true, /*test_accept=*/false);
            if (r.m_result_type == MempoolAcceptResult::ResultType::VALID) {
                mempool->PrioritiseTransaction(tx->GetHash(), 100 * COIN);
                LogPrintf("Lost blocks: lost transaction %s is back in the mempool, first in line\n", tx->GetHash().ToString());
            }
        }
    }
    if (gen == g_last_scan_gen && !changed && g_refused_scanned) return;
    g_last_scan_gen = gen;
    // 6) Judge again every block refused before. Nothing on the active chain
    //    is judged again: evidence can stop a block from being accepted, and
    //    never undoes one (forward-only).
    std::vector<CBlockIndex*> revive;
    {
        LOCK(cs_main);
        if (!g_refused_scanned) {
            // Refusals written to the block index before a restart.
            g_refused_scanned = true;
            for (const auto& [h, p] : chainman.m_blockman.m_block_index) {
                if (p->nStatus & BLOCK_FAILED_LOST) {
                    LOCK(g_lost_mutex);
                    g_refused.insert(h);
                }
            }
        }
        std::set<uint256> refused;
        {
            LOCK(g_lost_mutex);
            refused = g_refused;
        }
        for (const uint256& h : refused) {
            CBlockIndex* p = chainman.m_blockman.LookupBlockIndex(h);
            if (!p || !(p->nStatus & BLOCK_FAILED_LOST)) {
                LOCK(g_lost_mutex);
                g_refused.erase(h);
                continue;
            }
            CBlock blk;
            if (!ReadBlockFromDisk(blk, p, Params().GetConsensus())) continue;
            LOCK(g_lost_mutex);
            if (!ViolationLocked(blk, p->pprev)) revive.push_back(p);
        }
    }
    if (!revive.empty()) {
        {
            LOCK(cs_main);
            for (CBlockIndex* p : revive) {
                LogPrintf("Lost blocks: block %s at height %d no longer breaks the rule; judging it again\n", p->GetBlockHash().ToString(), p->nHeight);
                chainman.ActiveChainstate().ResetBlockFailureFlags(p);
                p->nStatus &= ~BLOCK_FAILED_LOST;
                LOCK(g_lost_mutex);
                g_refused.erase(p->GetBlockHash());
            }
        }
        BlockValidationState state;
        chainman.ActiveChainstate().ActivateBestChain(state);
    }
}

UniValue LostGetInfo()
{
    UniValue out(UniValue::VOBJ);
    LOCK(g_lost_mutex);
    out.pushKV("active", LostActive());
    out.pushKV("window", (int)g_window);
    UniValue blocks(UniValue::VARR);
    for (const auto& [h, b] : g_blocks) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("hash", h.GetHex());
        o.pushKV("height", b.height);
        o.pushKV("anchorheight", (int)b.anchor_h);
        o.pushKV("anchor", b.anchor_state == AnchorState::STALE ? "stale" : b.anchor_state == AnchorState::OK ? "best" : "unknown");
        o.pushKV("certified", b.certified);
        o.pushKV("own", b.own);
        o.pushKV("lost", b.prot);
        o.pushKV("forkheight", b.fork_height);
        o.pushKV("bitcoinheaders", (int)b.btc_headers.size());
        UniValue txids(UniValue::VARR);
        for (size_t i = 1; i < b.block->vtx.size(); ++i) txids.push_back(b.block->vtx[i]->GetHash().GetHex());
        o.pushKV("txids", txids);
        o.pushKV("size", (int)GetSerializeSize(*b.block, PROTOCOL_VERSION));
        blocks.push_back(o);
    }
    out.pushKV("blocks", blocks);
    out.pushKV("boundoutpoints", (int)g_spends.size());
    UniValue refused(UniValue::VARR);
    for (const uint256& h : g_refused) refused.push_back(h.GetHex());
    out.pushKV("refused", refused);
    return out;
}

std::optional<LostEvidenceMsg> LostGetEvidence(const uint256& hash)
{
    LOCK(g_lost_mutex);
    auto it = g_blocks.find(hash);
    if (it == g_blocks.end()) return std::nullopt;
    return LostEvidenceMsg{*it->second.block, it->second.btc_headers};
}
