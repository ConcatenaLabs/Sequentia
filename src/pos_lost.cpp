// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_lost.h>

#include <anchor.h>
#include <chain.h>
#include <chainparams.h>
#include <confidential_validation.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <hash.h>
#include <policy/policy.h>
#include <script/script.h>
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
    //! Which loss this block belongs to: blocks disconnected together share it,
    //! and an earlier loss is carried before a later one.
    uint64_t epoch{0};
    std::vector<std::vector<unsigned char>> btc_headers;
    // Computed by each maintenance pass.
    AnchorState anchor_state{AnchorState::UNKNOWN};
    bool prot{false};
    uint256 fork_hash;
    int fork_height{-1};
    int64_t fork_anchor_h{-1};
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
uint64_t g_epoch GUARDED_BY(g_lost_mutex){0};
uint256 g_last_noted_prev GUARDED_BY(g_lost_mutex);
//! C2 (-poslostbindsigned, off by default): blocks this member signed, held
//! until their anchor is past the window. One whose anchor went stale and that
//! never certified binds, on this member only, the outpoints its transactions
//! spend: conflict-only, no replay duty, no record, never relayed.
struct SignedBlk {
    std::shared_ptr<const CBlock> block;
    int height{0};
};
bool g_bind_signed{false};
std::map<uint256, SignedBlk> g_signed GUARDED_BY(g_lost_mutex);
std::map<COutPoint, std::vector<LostSpend>> g_signed_spends GUARDED_BY(g_lost_mutex);
//! Bitcoin's best height, as the last maintenance pass read it.
std::atomic<int> g_btc_h{-1};
int64_t g_last_replay{0};

fs::path LostPath() { return gArgs.GetDataDirNet() / "lostblocks.dat"; }
fs::path SignedPath() { return gArgs.GetDataDirNet() / "lostsigned.dat"; }

void WriteSignedFile() EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    const fs::path tmp = gArgs.GetDataDirNet() / "lostsigned.dat.new";
    FILE* f = fsbridge::fopen(tmp, "wb");
    if (!f) return;
    CAutoFile file(f, SER_DISK, PROTOCOL_VERSION);
    try {
        file << (uint32_t)1 << (uint64_t)g_signed.size();
        for (const auto& [h, b] : g_signed) file << *b.block << b.height;
        if (!FileCommit(file.Get())) return;
        file.fclose();
        (void)RenameOver(tmp, SignedPath());
    } catch (const std::exception& e) {
        LogPrintf("Lost blocks: cannot write %s: %s\n", fs::PathToString(SignedPath()), e.what());
    }
}

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
        file << (uint32_t)3 << (uint64_t)g_blocks.size() << g_epoch;
        for (const auto& [h, b] : g_blocks) {
            file << *b.block << b.height << b.certified << b.own << b.epoch << b.btc_headers
                 << b.prot << b.fork_hash << b.fork_height << (uint8_t)b.anchor_state;
        }
        if (!FileCommit(file.Get())) return;
        file.fclose();
        (void)RenameOver(tmp, LostPath());
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
    std::set<uint256> lost_txids;
    for (const auto& [o, v] : g_spends) for (const auto& sp : v) lost_txids.insert(sp.txid);
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const CTransaction& tx = *block.vtx[i];
        const uint256 txid = tx.GetHash();
        // A lost transaction is carried, not judged as a respend: which of two
        // conflicting lost transactions stands is the replay's order.
        if (lost_txids.count(txid)) continue;
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

std::optional<std::string> SignedViolationLocked(const CBlock& block, const CBlockIndex* prev) EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    if (!g_bind_signed || g_signed_spends.empty()) return std::nullopt;
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const CTransaction& tx = *block.vtx[i];
        const uint256 txid = tx.GetHash();
        for (const CTxIn& in : tx.vin) {
            if (in.m_is_pegin) continue;
            auto it = g_signed_spends.find(in.prevout);
            if (it == g_signed_spends.end()) continue;
            for (const LostSpend& sp : it->second) {
                if (sp.txid == txid || !Binds(sp, block.m_anchor_height, prev)) continue;
                return strprintf("tx %s spends %s:%u, which tx %s of block %s (signed here, anchor %u, never certified) spends",
                                 txid.ToString(), in.prevout.hash.ToString(), in.prevout.n, sp.txid.ToString(), sp.block.ToString(), sp.anchor_h);
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
    g_bind_signed = gArgs.GetBoolArg("-poslostbindsigned", false);
    LOCK(g_lost_mutex);
    if (g_bind_signed) {
        if (FILE* sf = fsbridge::fopen(SignedPath(), "rb")) {
            CAutoFile file(sf, SER_DISK, PROTOCOL_VERSION);
            try {
                uint32_t version;
                uint64_t n;
                file >> version >> n;
                for (uint64_t i = 0; i < n && i < POS_LOST_MAX_BLOCKS; ++i) {
                    auto block = std::make_shared<CBlock>();
                    int height;
                    file >> *block >> height;
                    g_signed.emplace(block->GetHash(), SignedBlk{block, height});
                }
            } catch (const std::exception& e) {
                LogPrintf("Lost blocks: cannot read %s: %s\n", fs::PathToString(SignedPath()), e.what());
            }
        }
    }
    FILE* f = fsbridge::fopen(LostPath(), "rb");
    if (!f) return;
    CAutoFile file(f, SER_DISK, PROTOCOL_VERSION);
    try {
        uint32_t version;
        uint64_t n;
        file >> version >> n;
        if (version != 3) throw std::runtime_error("unknown version");
        file >> g_epoch;
        for (uint64_t i = 0; i < n && i < POS_LOST_MAX_BLOCKS; ++i) {
            auto block = std::make_shared<CBlock>();
            int height;
            bool certified, own;
            uint64_t epoch;
            std::vector<std::vector<unsigned char>> headers;
            bool prot;
            uint256 fork_hash;
            int fork_height;
            uint8_t anchor_state;
            file >> *block >> height >> certified >> own >> epoch >> headers >> prot >> fork_hash >> fork_height >> anchor_state;
            LostBlk b = MakeBlk(block, height);
            b.prot = prot;
            b.fork_hash = fork_hash;
            b.fork_height = fork_height;
            b.anchor_state = (AnchorState)std::min<uint8_t>(anchor_state, 2);
            b.certified = certified;
            b.own = own;
            b.epoch = epoch;
            b.btc_headers = std::move(headers);
            g_blocks.emplace(b.hash, std::move(b));
        }
        // The duty holds from the first block this node judges after a
        // restart, before any maintenance pass: the bound spends as written.
        for (const auto& [h, b] : g_blocks) {
            if (!b.prot) continue;
            for (size_t i = 1; i < b.block->vtx.size(); ++i) {
                const CTransaction& tx = *b.block->vtx[i];
                for (const CTxIn& in : tx.vin) {
                    if (in.m_is_pegin) continue;
                    g_spends[in.prevout].push_back(LostSpend{tx.GetHash(), h, b.anchor_h, b.fork_hash, b.fork_height});
                }
            }
        }
        LogPrintf("Lost blocks: read %u from %s (%u bound outpoints)\n", (unsigned)g_blocks.size(), fs::PathToString(LostPath()), (unsigned)g_spends.size());
    } catch (const std::exception& e) {
        LogPrintf("Lost blocks: cannot read %s: %s\n", fs::PathToString(LostPath()), e.what());
    }
}

void LostShutdown()
{
    LOCK(g_lost_mutex);
    WriteFile();
}

void LostNoteSigned(const CBlock& block, int height)
{
    if (!g_bind_signed || !LostActive()) return;
    LOCK(g_lost_mutex);
    const uint256 h = block.GetHash();
    if (g_signed.count(h)) return;
    g_signed.emplace(h, SignedBlk{std::make_shared<const CBlock>(block), height});
    // On disk before the signature leaves this node.
    WriteSignedFile();
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
    // DisconnectTip goes down from the tip: a block whose child was the last
    // one noted belongs to the same loss.
    if (g_last_noted_prev != h) ++g_epoch;
    g_last_noted_prev = b.prev;
    b.epoch = g_epoch;
    g_blocks.emplace(h, std::move(b));
    WriteFile();
}

namespace {

const std::vector<unsigned char> LOST_RECORD_TAG{'S', 'Q', 'L', 'R'};

std::vector<uint256> MerkleBranch(std::vector<uint256> level, uint32_t pos)
{
    std::vector<uint256> branch;
    while (level.size() > 1) {
        if (level.size() & 1) level.push_back(level.back());
        branch.push_back(level[pos ^ 1]);
        std::vector<uint256> up;
        for (size_t i = 0; i < level.size(); i += 2) up.push_back(Hash(level[i], level[i + 1]));
        level = std::move(up);
        pos >>= 1;
    }
    return branch;
}

uint256 MerkleFromBranch(uint256 leaf, const std::vector<uint256>& branch, uint32_t pos)
{
    for (const uint256& b : branch) {
        leaf = (pos & 1) ? Hash(b, leaf) : Hash(leaf, b);
        pos >>= 1;
    }
    return leaf;
}

std::vector<unsigned char> SerHeader(const CBlockHeader& h)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << h;
    const auto sp = MakeUCharSpan(ss);
    return {sp.begin(), sp.end()};
}

std::optional<CBlockHeader> DeserHeader(const std::vector<unsigned char>& b)
{
    try {
        CDataStream ss(b, SER_NETWORK, PROTOCOL_VERSION);
        CBlockHeader h;
        ss >> h;
        if (!ss.empty()) return std::nullopt;
        return h;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<unsigned char> SerTxNoWitness(const CTransaction& tx)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
    ss << tx;
    const auto sp = MakeUCharSpan(ss);
    return {sp.begin(), sp.end()};
}

std::optional<CTransactionRef> DeserTxNoWitness(const std::vector<unsigned char>& b)
{
    try {
        CDataStream ss(b, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
        CMutableTransaction mtx;
        ss >> mtx;
        if (!ss.empty()) return std::nullopt;
        return MakeTransactionRef(std::move(mtx));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

CTxOut RecordOutput(const LostRecord& r)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << r;
    const auto sp = MakeUCharSpan(ss);
    std::vector<unsigned char> bytes(sp.begin(), sp.end());
    return CTxOut(::policyAsset, 0, CScript() << OP_RETURN << LOST_RECORD_TAG << bytes);
}

//! One input a record binds: until a block anchored above `until`, only `txid`
//! may spend `o`.
struct Binding {
    COutPoint o;
    uint256 txid;
    uint32_t until{0};
};

//! Bindings of the records in blocks of the active chain, by block hash.
std::map<uint256, std::vector<Binding>> g_recorded GUARDED_BY(::cs_main);
bool g_recorded_loaded GUARDED_BY(::cs_main){false};

//! The bindings a parsed record makes, without judging it (blocks already accepted).
std::vector<Binding> BindingsOf(const LostRecord& r)
{
    std::vector<Binding> out;
    for (const LostRecordEntry& e : r.entries) {
        if (e.k >= r.lost.size()) continue;
        auto h = DeserHeader(r.lost[e.k]);
        auto tx = DeserTxNoWitness(e.tx);
        if (!h || !tx) continue;
        for (const CTxIn& in : (*tx)->vin) {
            if (in.m_is_pegin) continue;
            out.push_back(Binding{in.prevout, (*tx)->GetHash(), h->m_anchor_height + g_window});
        }
    }
    return out;
}

void LoadRecorded(const CBlockIndex* tip) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    if (g_recorded_loaded) return;
    g_recorded_loaded = true;
    int n = 0;
    for (const CBlockIndex* p = tip; p && p->m_anchor_height + 3 * g_window >= tip->m_anchor_height && n < 5000; p = p->pprev, ++n) {
        CBlock blk;
        if (!ReadBlockFromDisk(blk, p, Params().GetConsensus())) continue;
        std::vector<Binding> b;
        for (const auto& r : LostParseRecords(blk)) {
            if (!r) continue;
            auto add = BindingsOf(*r);
            b.insert(b.end(), add.begin(), add.end());
        }
        if (!b.empty()) g_recorded[p->GetBlockHash()] = std::move(b);
    }
}

//! Every binding of `prev`'s chain still in force for a block anchored at `anchor_h`.
std::map<COutPoint, uint256> ChainBindings(const CBlockIndex* prev, uint32_t anchor_h) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    std::map<COutPoint, uint256> out;
    if (g_recorded.empty()) return out;
    int n = 0;
    for (const CBlockIndex* p = prev; p && p->m_anchor_height + 3 * g_window >= anchor_h && n < 5000; p = p->pprev, ++n) {
        auto it = g_recorded.find(p->GetBlockHash());
        if (it == g_recorded.end()) continue;
        for (const Binding& b : it->second) {
            if (anchor_h <= b.until) out.emplace(b.o, b.txid);
        }
    }
    return out;
}

bool DaemonHeaderKnown(const uint256& hash, uint32_t height, bool& answered)
{
    answered = false;
    try {
        UniValue p(UniValue::VARR);
        p.push_back(hash.GetHex());
        const UniValue reply = CallMainChainRPC("getblockheader", p);
        answered = true;
        if (!find_value(reply, "error").isNull()) return false;
        const UniValue r = find_value(reply, "result");
        if (!r.isObject()) return false;
        const UniValue h = find_value(r.get_obj(), "height");
        return h.isNum() && h.get_int64() == (int64_t)height;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<uint256> DaemonBestHashAt(uint32_t height)
{
    try {
        UniValue p(UniValue::VARR);
        p.push_back((int)height);
        const UniValue reply = CallMainChainRPC("getblockhash", p);
        if (!find_value(reply, "error").isNull()) return std::nullopt;
        const UniValue r = find_value(reply, "result");
        if (!r.isStr()) return std::nullopt;
        return uint256S(r.get_str());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

//! A lost header's parent as the certificate check reads it (height and anchor).
struct ParentView {
    CBlockIndex idx;
};

//! Judge one record for a block anchored at `anchor_h` on `prev`. Empty when
//! valid; `soft` when the Bitcoin daemon could not answer.
std::string JudgeRecord(const LostRecord& r, uint32_t anchor_h, const CBlockIndex* prev, bool& soft,
                        std::vector<Binding>& bindings) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    soft = false;
    if (r.version != 1) return "version";
    if (r.lost.empty() || r.entries.empty()) return "empty";
    if (r.btc_headers.size() > 2 * g_window + 1) return "too-many-bitcoin-headers";
    std::vector<CBlockHeader> lost;
    for (const auto& b : r.lost) {
        auto h = DeserHeader(b);
        if (!h) return "lost-header-decode";
        lost.push_back(*h);
    }
    // The branch leaves this chain at F and links header to header.
    const int f_height = (int)lost[0].block_height - 1;
    if (f_height < 0 || f_height > prev->nHeight) return "fork-height";
    const CBlockIndex* fork = prev->GetAncestor(f_height);
    if (!fork || fork->GetBlockHash() != lost[0].hashPrevBlock) return "fork-not-in-chain";
    uint32_t last_anchor_h = fork->m_anchor_height;
    uint256 last_anchor = fork->m_anchor_hash;
    for (size_t i = 0; i < lost.size(); ++i) {
        if (i > 0 && lost[i].hashPrevBlock != lost[i - 1].GetHash()) return "lost-not-linked";
        if ((int)lost[i].block_height != f_height + 1 + (int)i) return "lost-height";
        if (lost[i].m_anchor_height < last_anchor_h) return "lost-anchor-not-monotonic";
        if (lost[i].m_anchor_height == last_anchor_h && lost[i].m_anchor_hash != last_anchor) return "lost-anchor-swapped";
        last_anchor_h = lost[i].m_anchor_height;
        last_anchor = lost[i].m_anchor_hash;
    }
    // Bitcoin: the carried headers become known to the daemon (they cost real
    // work); a lost header's anchor is off this block's anchor chain when the
    // daemon knows it and the best chain has another block at its height.
    SubmitHeaders(r.btc_headers);
    std::vector<int> off(lost.size(), -1); // -1 unknown, 0 on the chain, 1 off it
    const auto judge_off = [&](size_t i) -> int {
        if (off[i] >= 0) return off[i];
        const uint32_t ah = lost[i].m_anchor_height;
        if (ah > anchor_h) return off[i] = 0; // above this block's anchor: not shown off its chain
        bool answered = false;
        if (!DaemonHeaderKnown(lost[i].m_anchor_hash, ah, answered)) {
            if (!answered) soft = true;
            return off[i] = 0;
        }
        auto best = DaemonBestHashAt(ah);
        if (!best) { soft = true; return off[i] = 0; }
        return off[i] = (*best != lost[i].m_anchor_hash) ? 1 : 0;
    };
    // The certificate of each block an entry names, under the registry at
    // this block's parent (exact when nothing changed the stake since the
    // lost block's parent; doc 03 states the limit).
    const StakeRegistry& reg = StakeRegistry::GetInstance();
    std::map<uint32_t, bool> certified;
    const auto judge_cert = [&](uint32_t k) -> std::string {
        auto it = certified.find(k);
        if (it != certified.end()) return it->second ? "" : "certificate";
        CBlockIndex parent_view;
        const CBlockIndex* parent = fork;
        if (k > 0) {
            parent_view.nHeight = f_height + (int)k;
            parent_view.m_anchor_height = lost[k - 1].m_anchor_height;
            parent_view.m_anchor_hash = lost[k - 1].m_anchor_hash;
            parent = &parent_view;
        }
        std::string reason;
        const int signers = PosVerifyBitfieldCertificate(lost[k], parent, reg, reason);
        const bool ok = signers >= 0 && signers >= PosSlotQuorumAt(reg, f_height + 1 + (int)k);
        certified[k] = ok;
        return ok ? "" : "certificate" + (reason.empty() ? std::string() : " " + reason);
    };
    for (const LostRecordEntry& e : r.entries) {
        if (e.k >= lost.size()) return "entry-block";
        if (e.pos == 0) return "entry-coinbase";
        auto tx = DeserTxNoWitness(e.tx);
        if (!tx) return "entry-tx-decode";
        const uint256 txid = (*tx)->GetHash();
        if (MerkleFromBranch(txid, e.branch, e.pos) != lost[e.k].hashMerkleRoot) return "entry-merkle";
        if (anchor_h > lost[e.k].m_anchor_height + g_window) return "past-window";
        if (lost[e.k].m_anchor_height > fork->m_anchor_height + g_window) return "fork-too-deep";
        bool any_off = false;
        for (size_t i = 0; i <= e.k && !any_off; ++i) any_off = judge_off(i) == 1;
        if (!any_off) return soft ? "bitcoin-unanswered" : "not-lost";
        const std::string c = judge_cert(e.k);
        if (!c.empty()) return c;
        for (const CTxIn& in : (*tx)->vin) {
            if (in.m_is_pegin) continue;
            bindings.push_back(Binding{in.prevout, txid, lost[e.k].m_anchor_height + g_window});
        }
    }
    soft = false;
    return "";
}

//! The holders' plan for a block on `prev` anchored at `anchor_h`.
struct Plan {
    std::vector<CTransactionRef> prefix;
    bool cut{false};
    std::set<uint256> to_record;
    std::vector<LostRecord> records;
    std::string summary;
    int64_t micros{0};
};

Plan PlanCompute(const CBlockIndex* prev, uint32_t anchor_h, CCoinsViewCache& view, bool build)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, g_lost_mutex)
{
    Plan plan;
    // The pending lost transactions of every lost branch of this chain, by
    // loss, then height, then position.
    std::vector<const LostBlk*> blocks;
    for (const auto& [h, b] : g_blocks) {
        if (!b.prot || b.fork_height < 0) continue;
        const CBlockIndex* f = prev->GetAncestor(b.fork_height);
        if (!f || f->GetBlockHash() != b.fork_hash) continue;
        if (anchor_h > b.anchor_h + g_window) continue;
        blocks.push_back(&b);
    }
    if (blocks.empty()) return plan;
    std::sort(blocks.begin(), blocks.end(), [](const LostBlk* a, const LostBlk* b) {
        return std::tie(a->epoch, a->height) < std::tie(b->epoch, b->height);
    });
    std::vector<CTransactionRef> txs;
    std::vector<std::pair<const LostBlk*, uint32_t>> where;
    for (const LostBlk* b : blocks) {
        for (size_t i = 1; i < b->block->vtx.size(); ++i) {
            txs.push_back(b->block->vtx[i]);
            where.emplace_back(b, (uint32_t)i);
        }
    }
    const Consensus::Params& consensus = Params().GetConsensus();
    const int64_t t0 = GetTimeMicros();
    std::vector<int64_t> sigops;
    const auto classes = LostClassifyTxs(txs, view, prev, consensus, &sigops);
    const auto bound = ChainBindings(prev, anchor_h);
    std::set<uint256> bound_txids;
    for (const auto& [o, t] : bound) bound_txids.insert(t);
    // What to record: a transaction that fails as it is, not yet recorded on
    // this chain, whose block this record can show lost and certified.
    std::vector<std::pair<const LostBlk*, uint32_t>> recordable;
    int n_due = 0, n_later = 0, n_fails = 0, n_gone = 0;
    for (size_t i = 0; i < txs.size(); ++i) {
        switch (classes[i].first) {
        case LostTxCat::DUE: ++n_due; break;
        case LostTxCat::LATER: ++n_later; break;
        case LostTxCat::GONE: ++n_gone; break;
        case LostTxCat::FAILS: {
            ++n_fails;
            const uint256 txid = txs[i]->GetHash();
            if (bound_txids.count(txid)) break;
            // The branch from the fork point to this block, and a block of it
            // whose anchor is stale and at or below this block's anchor.
            const LostBlk* b = where[i].first;
            std::vector<const LostBlk*> path;
            for (const LostBlk* w = b; w;) {
                path.push_back(w);
                auto p = g_blocks.find(w->prev);
                w = (p != g_blocks.end() && w->height > b->fork_height + 1) ? &p->second : nullptr;
            }
            std::reverse(path.begin(), path.end());
            if (path.empty() || path.front()->prev != b->fork_hash) break;
            bool shown = false;
            for (const LostBlk* w : path) shown |= (w->anchor_state == AnchorState::STALE && w->anchor_h <= anchor_h && !w->btc_headers.empty());
            if (!shown) break;
            // Certified under the registry this block is judged with.
            CBlockIndex parent_view;
            const CBlockIndex* parent = prev->GetAncestor(b->height - 1);
            if (b->height - 1 > b->fork_height) {
                const LostBlk* pb = path.size() >= 2 ? path[path.size() - 2] : nullptr;
                if (!pb) break;
                parent_view.nHeight = b->height - 1;
                parent_view.m_anchor_height = pb->anchor_h;
                parent_view.m_anchor_hash = pb->anchor;
                parent = &parent_view;
            }
            std::string reason;
            const StakeRegistry& reg = StakeRegistry::GetInstance();
            const int signers = parent ? PosVerifyBitfieldCertificate(*b->block, parent, reg, reason) : -1;
            if (signers < 0 || signers < PosSlotQuorumAt(reg, b->height)) {
                LogPrint(BCLog::VALIDATION, "Lost blocks: %s cannot be recorded: the certificate of %s does not hold under this registry (%s)\n",
                         txid.ToString(), b->hash.ToString(), reason);
                break;
            }
            recordable.push_back(where[i]);
            break;
        }
        }
    }
    // The records, one per lost branch, of the given entries.
    const auto build_records = [&](const std::vector<std::pair<const LostBlk*, uint32_t>>& entries) {
        std::map<uint256, std::vector<std::pair<const LostBlk*, uint32_t>>> by_fork;
        for (const auto& e : entries) by_fork[e.first->fork_hash].push_back(e);
        std::vector<LostRecord> out;
        for (const auto& [fork_hash, items] : by_fork) {
            LostRecord rec;
            int top = 0;
            for (const auto& [b, pos] : items) top = std::max(top, b->height);
            const LostBlk* t = nullptr;
            for (const auto& [b, pos] : items) if (b->height == top) t = b;
            std::vector<const LostBlk*> path;
            for (const LostBlk* w = t; w;) {
                path.push_back(w);
                auto p = g_blocks.find(w->prev);
                w = (p != g_blocks.end() && w->height > t->fork_height + 1) ? &p->second : nullptr;
            }
            std::reverse(path.begin(), path.end());
            std::map<const LostBlk*, uint32_t> index;
            for (const LostBlk* w : path) {
                index[w] = rec.lost.size();
                rec.lost.push_back(SerHeader(w->block->GetBlockHeader()));
                if (rec.btc_headers.empty() && w->anchor_state == AnchorState::STALE) rec.btc_headers = w->btc_headers;
            }
            for (const auto& [b, pos] : items) {
                std::vector<uint256> leaves;
                for (const auto& tx : b->block->vtx) leaves.push_back(tx->GetHash());
                LostRecordEntry e;
                e.k = index[b];
                e.pos = pos;
                e.branch = MerkleBranch(leaves, pos);
                e.tx = SerTxNoWitness(*b->block->vtx[pos]);
                rec.entries.push_back(std::move(e));
            }
            out.push_back(std::move(rec));
        }
        return out;
    };
    // The record owed by this block: the longest prefix, in lost order, of
    // the recordable transactions whose records fit the block's record bytes.
    // The rest is owed by the next block (each block of the window records
    // what is left), so a large failing set never makes every block invalid.
    std::vector<std::pair<const LostBlk*, uint32_t>> chosen;
    for (const auto& e : recordable) {
        chosen.push_back(e);
        size_t bytes = 0;
        for (const LostRecord& r : build_records(chosen)) bytes += GetSerializeSize(r, PROTOCOL_VERSION);
        if (bytes > LOST_RECORD_MAX_BYTES) {
            chosen.pop_back();
            break;
        }
    }
    for (const auto& [b, pos] : chosen) plan.to_record.insert(b->block->vtx[pos]->GetHash());
    // The prefix: the due transactions in lost order, the longest run of them
    // that fits the consensus limits. The weight left for them is the block's
    // less a fixed reserve for the header, the coinbase and the certificate,
    // less the record bytes when a record is owed, and less one coinbase fee
    // output (at its largest payee script) per fee asset the prefix pays in;
    // the signature operations are counted as ConnectBlock counts them. Fixed
    // reserves keep the floor a function of the chain and the lost blocks
    // alone, so a leader cannot shorten it by padding its own coinbase.
    const uint32_t max_weight = consensus.nMaxBlockWeight ? consensus.nMaxBlockWeight : MAX_BLOCK_WEIGHT;
    int64_t budget = (int64_t)max_weight - LOST_RESERVED_WEIGHT - (int64_t)consensus.max_block_signature_size -
                     (plan.to_record.empty() ? 0 : (int64_t)(WITNESS_SCALE_FACTOR * LOST_RECORD_MAX_BYTES));
    int64_t sig_budget = MAX_BLOCK_SIGOPS_COST - LOST_RESERVED_SIGOPS;
    std::set<CAsset> fee_assets;
    for (size_t i = 0; i < txs.size(); ++i) {
        if (classes[i].first != LostTxCat::DUE) continue;
        int64_t w = GetTransactionWeight(*txs[i]);
        for (const auto& [asset, amount] : GetFeeMap(*txs[i])) {
            if (amount > 0 && !fee_assets.count(asset)) w += LOST_FEE_OUTPUT_WEIGHT;
        }
        const int64_t sig = sigops[i];
        if (w > budget || sig > sig_budget) { plan.cut = true; break; }
        budget -= w;
        sig_budget -= sig;
        for (const auto& [asset, amount] : GetFeeMap(*txs[i])) if (amount > 0) fee_assets.insert(asset);
        plan.prefix.push_back(txs[i]);
    }
    if (build) plan.records = build_records(chosen);
    plan.micros = GetTimeMicros() - t0;
    plan.summary = strprintf("%d due (%d in the prefix%s), %d later, %d failing (%d to record of %d recordable), %d gone; classified in %d us",
                             n_due, (int)plan.prefix.size(), plan.cut ? ", cut" : "", n_later, n_fails,
                             (int)plan.to_record.size(), (int)recordable.size(), n_gone, (int)plan.micros);
    return plan;
}

} // namespace

//! The plan is a function of the parent (its chain, its coins, the records
//! on it), the candidate's anchor height and the store. It is computed once
//! for each and shared by the template, the block's test, every proposal on
//! the same parent and the block's connection.
struct PlanMemo {
    uint256 prev;
    uint32_t anchor_h{0};
    uint256 store;
    Plan plan;
};
std::optional<PlanMemo> g_plan_memo GUARDED_BY(g_lost_mutex);

uint256 StoreFingerprint() EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    CHashWriter hw(SER_GETHASH, 0);
    hw << g_gen;
    for (const auto& [h, b] : g_blocks) {
        hw << h << b.prot << (uint8_t)b.anchor_state << (uint64_t)b.btc_headers.size() << b.fork_height << b.epoch;
    }
    return hw.GetHash();
}

Plan PlanLocked(const CBlockIndex* prev, uint32_t anchor_h, CCoinsViewCache& view, bool /*build*/)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, g_lost_mutex)
{
    const uint256 store = StoreFingerprint();
    if (g_plan_memo && g_plan_memo->prev == prev->GetBlockHash() && g_plan_memo->anchor_h == anchor_h && g_plan_memo->store == store) {
        Plan hit = g_plan_memo->plan;
        hit.summary += "; reused";
        return hit;
    }
    Plan plan = PlanCompute(prev, anchor_h, view, /*build=*/true);
    if (!plan.summary.empty()) LogPrint(BCLog::VALIDATION, "Lost blocks: plan computed on %s at height %d: %s\n", prev->GetBlockHash().ToString(), prev->nHeight + 1, plan.summary);
    g_plan_memo = PlanMemo{prev->GetBlockHash(), anchor_h, store, plan};
    return plan;
}

std::vector<std::optional<LostRecord>> LostParseRecords(const CBlock& block)
{
    std::vector<std::optional<LostRecord>> out;
    if (block.vtx.empty()) return out;
    for (const CTxOut& o : block.vtx[0]->vout) {
        const CScript& s = o.scriptPubKey;
        CScript::const_iterator pc = s.begin();
        opcodetype op;
        std::vector<unsigned char> data;
        if (!s.GetOp(pc, op) || op != OP_RETURN) continue;
        if (!s.GetOp(pc, op, data) || data != LOST_RECORD_TAG) continue;
        if (!s.GetOp(pc, op, data) || pc != s.end()) { out.emplace_back(std::nullopt); continue; }
        try {
            CDataStream ss(data, SER_NETWORK, PROTOCOL_VERSION);
            LostRecord r;
            ss >> r;
            if (!ss.empty()) { out.emplace_back(std::nullopt); continue; }
            out.emplace_back(std::move(r));
        } catch (const std::exception&) {
            out.emplace_back(std::nullopt);
        }
    }
    return out;
}

bool LostIsRefusal(const std::string& reason)
{
    return reason == "bad-txns-lost-respend" || reason == "bad-lost-replay" || reason == "bad-lost-unrecorded" ||
           reason == "bad-txns-signed-respend";
}

bool LostCheckRecords(const CBlock& block, const CBlockIndex* pindexPrev, bool just_check, BlockValidationState& state)
{
    AssertLockHeld(::cs_main);
    if (!g_con_pos || !g_con_bitcoin_anchor || !g_validate_anchor || !pindexPrev || g_ignore) return true;
    LoadRecorded(pindexPrev);
    std::vector<Binding> mine;
    size_t bytes = 0;
    for (const auto& r : LostParseRecords(block)) {
        if (!r) return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-lost-record", "malformed");
        bytes += GetSerializeSize(*r, PROTOCOL_VERSION);
        if (bytes > LOST_RECORD_MAX_BYTES) return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-lost-record", "too large");
        bool soft = false;
        const std::string why = JudgeRecord(*r, block.m_anchor_height, pindexPrev, soft, mine);
        if (!why.empty()) {
            LogPrintf("Lost blocks: block %s carries a record that is not valid: %s\n", block.GetHash().ToString(), why);
            if (soft) return state.Invalid(BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "lost-record-unverifiable", why);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-lost-record", why);
        }
    }
    std::map<COutPoint, uint256> bound = ChainBindings(pindexPrev, block.m_anchor_height);
    for (const Binding& b : mine) bound.emplace(b.o, b.txid);
    if (!bound.empty()) {
        for (size_t i = 1; i < block.vtx.size(); ++i) {
            const CTransaction& tx = *block.vtx[i];
            for (const CTxIn& in : tx.vin) {
                auto it = bound.find(in.prevout);
                if (it == bound.end() || it->second == tx.GetHash()) continue;
                const std::string why = strprintf("tx %s spends %s:%u, which recorded tx %s spends", tx.GetHash().ToString(),
                                                  in.prevout.hash.ToString(), in.prevout.n, it->second.ToString());
                LogPrintf("Lost blocks: block %s breaks a record: %s\n", block.GetHash().ToString(), why);
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-recorded-respend", why);
            }
        }
    }
    if (!just_check && !mine.empty()) {
        g_recorded[block.GetHash()] = std::move(mine);
        LogPrintf("Lost blocks: block %s at height %d records %d bound input(s)\n", block.GetHash().ToString(),
                  pindexPrev->nHeight + 1, (int)g_recorded[block.GetHash()].size());
    }
    return true;
}

LostTemplateParts LostForTemplate(const CBlockIndex* pindexPrev, uint32_t anchor_height, CCoinsViewCache& view)
{
    AssertLockHeld(::cs_main);
    LostTemplateParts out;
    if (!LostActive() || !pindexPrev) return out;
    LoadRecorded(pindexPrev);
    LOCK(g_lost_mutex);
    Plan plan = PlanLocked(pindexPrev, anchor_height, view, /*build=*/true);
    out.prefix = std::move(plan.prefix);
    out.cut = plan.cut;
    for (const LostRecord& r : plan.records) out.records.push_back(RecordOutput(r));
    if (!out.prefix.empty() || !out.records.empty()) {
        LogPrintf("Lost blocks: template on %s: %s\n", pindexPrev->GetBlockHash().ToString(), plan.summary);
    }
    return out;
}

bool LostCheckBlock(const CBlock& block, const CBlockIndex* pindexPrev, bool ibd,
                    const std::function<bool()>& certified_descendant, CCoinsViewCache& view,
                    BlockValidationState& state)
{
    AssertLockHeld(::cs_main);
    if (!LostActive() || !pindexPrev || ibd) return true;
    LOCK(g_lost_mutex);
    std::optional<std::string> why;
    std::string reason;
    if ((why = ViolationLocked(block, pindexPrev))) {
        reason = "bad-txns-lost-respend";
    } else if ((why = SignedViolationLocked(block, pindexPrev))) {
        reason = "bad-txns-signed-respend";
    } else if (!g_spends.empty()) {
        // The duty: the due lost transactions first, and the records owed.
        Plan plan = PlanLocked(pindexPrev, block.m_anchor_height, view, /*build=*/false);
        size_t i = 0;
        for (; i < plan.prefix.size(); ++i) {
            if (i + 1 >= block.vtx.size() || block.vtx[i + 1]->GetHash() != plan.prefix[i]->GetHash()) break;
        }
        if (i < plan.prefix.size()) {
            why = strprintf("does not begin with the due lost transactions (%d of %d in place; %s)", (int)i, (int)plan.prefix.size(), plan.summary);
            reason = "bad-lost-replay";
        } else if (plan.cut && block.vtx.size() != 1 + plan.prefix.size()) {
            why = strprintf("carries other transactions while due lost transactions did not fit (%s)", plan.summary);
            reason = "bad-lost-replay";
        } else if (!plan.to_record.empty()) {
            std::set<uint256> carried;
            for (const auto& r : LostParseRecords(block)) {
                if (!r) continue;
                for (const auto& e : r->entries) {
                    if (auto tx = DeserTxNoWitness(e.tx)) carried.insert((*tx)->GetHash());
                }
            }
            for (const uint256& t : plan.to_record) {
                if (!carried.count(t)) {
                    why = strprintf("does not record lost tx %s (%s)", t.ToString(), plan.summary);
                    reason = "bad-lost-unrecorded";
                    break;
                }
            }
        }
    }
    if (!why) return true;
    if (certified_descendant()) {
        LogPrintf("Lost blocks: block %s at height %d breaks the duty but has a certified descendant; the evidence does not bear on it: %s\n",
                  block.GetHash().ToString(), pindexPrev->nHeight + 1, *why);
        return true;
    }
    LogPrintf("Lost blocks: refusing block %s at height %d: %s: %s\n", block.GetHash().ToString(),
              pindexPrev->nHeight + 1, reason, *why);
    return state.Invalid(BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, reason, *why);
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
        if (!g_bind_signed) continue;
        auto st = g_signed_spends.find(in.prevout);
        if (st == g_signed_spends.end()) continue;
        for (const LostSpend& s : st->second) {
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
    // A fork point more than W below the block is not carried: whatever
    // forked that long ago left the window before it was lost (doc 03).
    if (!parent_in_store && block.m_anchor_height > parent_anchor_h + g_window) return "fork-too-deep";
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
    // reconstructed at the parent (doc 03 §3, "Transactions of a lost block").
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
    {
        auto parent = g_blocks.find(block.hashPrevBlock);
        b.epoch = parent != g_blocks.end() ? parent->second.epoch : ++g_epoch;
    }
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
    if (btc_h >= 0) g_btc_h = btc_h;
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
            b.fork_anchor_h = -1;
            bool stale = false;
            const LostBlk* walk = &b;
            for (int guard = 0; walk && guard < 10000; ++guard) {
                if (walk->anchor_state == AnchorState::STALE) stale = true;
                auto p = g_blocks.find(walk->prev);
                if (p != g_blocks.end()) {
                    walk = &p->second;
                    continue;
                }
                // The fork point is the branch's first parent outside the
                // store, whether or not this node's tip has reached it yet (a
                // node restarting behind it must still refuse what the lost
                // block binds once it gets there; each candidate checks that
                // the fork point is its ancestor).
                b.fork_height = walk->height - 1;
                b.fork_hash = walk->prev;
                if (const CBlockIndex* f = chainman.m_blockman.LookupBlockIndex(walk->prev)) b.fork_anchor_h = (int64_t)f->m_anchor_height;
                break;
            }
            branch_stale[h] = stale && b.fork_height >= 0;
        }
        // Protected: a certified block of a stale branch, and its ancestors in the store.
        std::set<uint256> prot;
        for (const auto& [h, b] : g_blocks) {
            if (!b.certified || !branch_stale[h]) continue;
            // Within the window of its fork point (an unknown fork point is
            // judged when it is known).
            if (b.fork_anchor_h >= 0 && (int64_t)b.anchor_h > b.fork_anchor_h + (int64_t)g_window) continue;
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
    if (g_bind_signed) {
        std::vector<std::pair<uint256, std::pair<uint32_t, uint256>>> sanchors;
        {
            LOCK(g_lost_mutex);
            for (const auto& [h, b] : g_signed) sanchors.push_back({h, {b.block->m_anchor_height, b.block->m_anchor_hash}});
        }
        std::map<uint256, AnchorState> sv;
        for (const auto& [h, a] : sanchors) sv[h] = CachedVerdict(a.first, a.second);
        LOCK2(cs_main, g_lost_mutex);
        std::map<COutPoint, std::vector<LostSpend>> spends;
        bool dirty = false;
        for (auto it = g_signed.begin(); it != g_signed.end();) {
            const CBlock& blk = *it->second.block;
            const CBlockIndex* self = chainman.m_blockman.LookupBlockIndex(it->first);
            const bool active = self && chainman.ActiveChain().Contains(self);
            const bool expired = btc_h >= 0 && (uint32_t)btc_h > blk.m_anchor_height + g_window;
            if (active || expired) { it = g_signed.erase(it); dirty = true; continue; }
            const bool certified = self && self->PosCarriesQuorum();
            if (!certified && sv[it->first] == AnchorState::STALE) {
                for (size_t i = 1; i < blk.vtx.size(); ++i) {
                    for (const CTxIn& in : blk.vtx[i]->vin) {
                        if (in.m_is_pegin) continue;
                        spends[in.prevout].push_back(LostSpend{blk.vtx[i]->GetHash(), it->first, blk.m_anchor_height, blk.hashPrevBlock, it->second.height - 1});
                    }
                }
            }
            ++it;
        }
        if (dirty) WriteSignedFile();
        std::set<std::pair<COutPoint, uint256>> a, b;
        for (const auto& [o, v] : spends) for (const auto& x : v) a.insert({o, x.txid});
        for (const auto& [o, v] : g_signed_spends) for (const auto& x : v) b.insert({o, x.txid});
        if (a != b) {
            LogPrintf("Lost blocks: %u outpoint(s) bound by blocks this member signed that never certified and whose anchor went stale\n", (unsigned)spends.size());
            g_signed_spends = std::move(spends);
            ++g_gen;
            changed = true;
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
        any = !g_spends.empty() || !g_signed_spends.empty() || !g_refused.empty() || !g_refused_scanned;
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
            // Judged again on its next activation, by the whole duty.
            revive.push_back(p);
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
        // The children of a refused block were refused at their headers
        // without being kept (bad-prevblk-lost-respend): ask every peer for
        // headers again, so they are taken now and not at the next block.
        ++g_anchor_refetch_epoch;
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
    out.pushKV("bindsigned", g_bind_signed);
    out.pushKV("signed", (int)g_signed.size());
    out.pushKV("signedboundoutpoints", (int)g_signed_spends.size());
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
