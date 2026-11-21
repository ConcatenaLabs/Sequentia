// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_lost.h>

#include <anchor.h>
#include <arith_uint256.h>
#include <btc_work.h>
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
#include <script/btcpayment.h>
#include <script/interpreter.h>
#include <streams.h>
#include <sync.h>
#include <txmempool.h>
#include <undo.h>
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
    //! The Bitcoin headers from the best chain to this block's stale anchor,
    //! parents first: what a record or a peer needs to check its work.
    std::vector<std::vector<unsigned char>> btc;
    //! Its anchor may seed its child's committee: on the chain, or a header
    //! meeting Bitcoin's real difficulty there.
    bool seed_ok{true};
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
    int height; //!< the lost block's height
    uint256 leaf{}; //!< LostProofLeaf of the input, null when it never yields
    bool own{false}; //!< of a block this node had on its own chain
};

Mutex g_lost_mutex;
std::map<uint256, LostBlk> g_blocks GUARDED_BY(g_lost_mutex);
std::map<COutPoint, std::vector<LostSpend>> g_spends GUARDED_BY(g_lost_mutex);
uint64_t g_gen GUARDED_BY(g_lost_mutex){0};
bool g_dirty_file GUARDED_BY(g_lost_mutex){false};
std::deque<std::pair<int64_t, LostEvidenceMsg>> g_inbox GUARDED_BY(g_lost_mutex);
//! F8: the inbox's bytes per peer and in all, and the cap on blocks taken as
//! evidence whose certificate is not (yet) verified in order from the fork.
std::map<int64_t, size_t> g_inbox_bytes GUARDED_BY(g_lost_mutex);
size_t g_inbox_total GUARDED_BY(g_lost_mutex){0};
constexpr size_t LOST_INBOX_PEER_BYTES = 8 * 1000 * 1000;
constexpr size_t LOST_INBOX_TOTAL_BYTES = 64 * 1000 * 1000;
constexpr size_t LOST_MAX_UNCERTIFIED = 256;
std::map<int64_t, std::set<uint256>> g_sent GUARDED_BY(g_lost_mutex);
//! Blocks refused under the rule (BLOCK_FAILED_LOST), judged again on change.
std::set<uint256> g_refused GUARDED_BY(g_lost_mutex);
//! Headers above a refused block, by hash, and how many are kept per parent.
//! Unverified until the maintenance pass, which drops any whose certificate
//! fails, so junk cannot hold a parent's slots for long.
constexpr size_t LOST_WAIVER_MAX_HEADERS = 4096;
constexpr size_t LOST_WAIVER_MAX_CHILDREN = 16;
constexpr size_t LOST_WAIVER_MAX_BYTES = 8 * 1000 * 1000;
std::map<uint256, CBlockHeader> g_waiver_headers GUARDED_BY(g_lost_mutex);
std::map<uint256, size_t> g_waiver_children GUARDED_BY(g_lost_mutex);
size_t g_waiver_bytes GUARDED_BY(g_lost_mutex){0};
bool g_waiver_dirty GUARDED_BY(g_lost_mutex){false};
//! Parents with more certified evidence-only children than LostSiblingCap.
std::set<uint256> g_sibling_capped GUARDED_BY(g_lost_mutex);
//! Refused blocks with enough certified blocks above them: this holder follows.
std::set<uint256> g_waived GUARDED_BY(g_lost_mutex);
int g_waiver_depth{DEFAULT_LOST_WAIVER_DEPTH};

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
//! The height of the active tip's child. What a node does for the rule outside
//! block validation (relaying evidence, putting lost transactions back in the
//! mempool) waits for the rule's height, like the validation itself.
std::atomic<int> g_next_height{-1};
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

//! Whether this node can hand the block on whole: an anchor on Bitcoin's best
//! chain needs no proof, and any other needs the headers that link it there. A
//! receiver answers `anchor-unproven` to a stale-anchored block without them.
bool EvidenceComplete(const LostBlk& b)
{
    return b.anchor_state == AnchorState::OK || !b.btc.empty();
}

//! Take one header out of the waiver's side store, freeing its parent's slot.
void DropWaiverHeader(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    auto it = g_waiver_headers.find(hash);
    if (it == g_waiver_headers.end()) return;
    auto kept = g_waiver_children.find(it->second.hashPrevBlock);
    if (kept != g_waiver_children.end() && --kept->second == 0) g_waiver_children.erase(kept);
    g_waiver_bytes -= std::min(g_waiver_bytes, GetSerializeSize(it->second, PROTOCOL_VERSION));
    g_waiver_headers.erase(it);
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

//! Where a Bitcoin block hash stands on the daemon's best chain, judged by
//! height: OK when the best chain has it there, STALE when the best chain
//! reaches that height with another block, UNKNOWN otherwise. The daemon need
//! not know the stale block itself. This is the holders' view (the duty, soft);
//! validity reads the judged block's own anchor ancestry instead.
AnchorState BestChainVerdict(uint32_t height, const uint256& hash)
{
    try {
        int count = -1;
        if (!GetMainchainBlockCount(count) || count < 0 || (uint32_t)count < height) return AnchorState::UNKNOWN;
        UniValue p(UniValue::VARR);
        p.push_back((int)height);
        const UniValue reply = CallMainChainRPC("getblockhash", p);
        if (!find_value(reply, "error").isNull()) return AnchorState::UNKNOWN;
        const UniValue r = find_value(reply, "result");
        if (!r.isStr()) return AnchorState::UNKNOWN;
        return uint256S(r.get_str()) == hash ? AnchorState::OK : AnchorState::STALE;
    } catch (const std::exception&) {
        return AnchorState::UNKNOWN;
    }
}

fs::path JournalPath() { return gArgs.GetDataDirNet() / "lostblocks.journal"; }

void WriteFile() EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    const fs::path tmp = gArgs.GetDataDirNet() / "lostblocks.dat.new";
    FILE* f = fsbridge::fopen(tmp, "wb");
    if (!f) return;
    CAutoFile file(f, SER_DISK, PROTOCOL_VERSION);
    try {
        file << (uint32_t)6 << (uint64_t)g_blocks.size() << g_epoch;
        for (const auto& [h, b] : g_blocks) {
            file << *b.block << b.height << b.certified << b.own << b.epoch
                 << b.prot << b.fork_hash << b.fork_height << (uint8_t)b.anchor_state << b.btc << b.seed_ok;
        }
        file << g_sibling_capped << g_waiver_headers;
        if (!FileCommit(file.Get())) return;
        file.fclose();
        if (!RenameOver(tmp, LostPath())) return;
        // The journal's blocks are in the file now.
        fs::remove(JournalPath());
        g_dirty_file = false;
    } catch (const std::exception& e) {
        LogPrintf("Lost blocks: cannot write %s: %s\n", fs::PathToString(LostPath()), e.what());
    }
}

//! F9: one disconnected block appended to the journal and synced, in time
//! proportional to the block (the whole store is rewritten on the
//! maintenance pass, outside the chain lock).
bool AppendJournal(const LostBlk& b) EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    FILE* f = fsbridge::fopen(JournalPath(), "ab");
    if (!f) return false;
    CAutoFile file(f, SER_DISK, PROTOCOL_VERSION);
    try {
        file << (uint32_t)5 << *b.block << b.height << b.certified << b.own << b.epoch;
        return FileCommit(file.Get());
    } catch (const std::exception& e) {
        LogPrintf("Lost blocks: cannot append to %s: %s\n", fs::PathToString(JournalPath()), e.what());
        return false;
    }
}

} // namespace

//! P4 (doc 03, "The duty of a holder"): a transaction of a lost block at
//! `lost_height`, anchored at `lost_anchor_h`, still binds a block at `height`
//! anchored at `anchor_h` and built on `prev`, until that block's chain has
//! rebuilt `lost_height` (block R) and W Bitcoin blocks have passed above both
//! the lost anchor and R's. A function of the judged chain alone.
bool LostStillBinds(int lost_height, uint32_t lost_anchor_h, int height, uint32_t anchor_h, const CBlockIndex* prev)
{
    if (height < lost_height) return true;
    uint32_t r_anchor = anchor_h;
    if (height > lost_height) {
        const CBlockIndex* r = prev ? prev->GetAncestor(lost_height) : nullptr;
        if (!r) return true;
        r_anchor = r->m_anchor_height;
    }
    return (int64_t)anchor_h <= (int64_t)std::max(lost_anchor_h, r_anchor) + (int64_t)g_window;
}

namespace {

//! Whether `s` binds a block anchored at `anchor_h` built on `prev`.
bool Binds(const LostSpend& s, uint32_t anchor_h, const CBlockIndex* prev)
{
    if (!prev) return false;
    const CBlockIndex* f = prev->GetAncestor(s.fork_height);
    if (!f || f->GetBlockHash() != s.fork_hash) return false;
    return LostStillBinds(s.height, s.anchor_h, prev->nHeight + 1, anchor_h, prev);
}

std::optional<std::string> ViolationLocked(const CBlock& block, const CBlockIndex* prev, const CCoinsViewCache& view) EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
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
        for (size_t n = 0; n < tx.vin.size(); ++n) {
            const CTxIn& in = tx.vin[n];
            if (in.m_is_pegin) continue;
            auto it = g_spends.find(in.prevout);
            if (it == g_spends.end()) continue;
            for (const LostSpend& s : it->second) {
                if (s.txid == txid) continue;
                if (!Binds(s, block.m_anchor_height, prev)) continue;
                if (LostBindingYields(s.leaf, tx, n, view, prev)) continue;
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

uint256 LostProofLeaf(const CTransaction& tx, size_t n)
{
    if (n >= tx.vin.size() || tx.vin[n].m_is_pegin || tx.witness.vtxinwit.size() <= n) return uint256();
    const CTxInWitness& wit = tx.witness.vtxinwit[n];
    // A payment proof and no floor: records are 0x01 then 0x02, each at most once.
    const auto& slot = wit.m_pegin_witness.stack;
    if (slot.size() != 1 || slot[0].empty() || slot[0][0] != BtcPayment::RECORD_PAYMENT) return uint256();
    const auto& st = wit.scriptWitness.stack;
    size_t size = st.size();
    if (size >= 2 && !st.back().empty() && st.back()[0] == ANNEX_TAG) --size;
    if (size < 2) return uint256();
    const std::vector<unsigned char>& control = st[size - 1];
    const std::vector<unsigned char>& script = st[size - 2];
    if (control.size() < TAPROOT_CONTROL_BASE_SIZE || control.size() > TAPROOT_CONTROL_MAX_SIZE ||
        (control.size() - TAPROOT_CONTROL_BASE_SIZE) % TAPROOT_CONTROL_NODE_SIZE != 0 ||
        (control[0] & TAPROOT_LEAF_MASK) != TAPROOT_LEAF_TAPSCRIPT) {
        return uint256();
    }
    // Consensus allows a proof no opcode reads; such a leaf never yields.
    const CScript leaf(script.begin(), script.end());
    CScript::const_iterator pc = leaf.begin();
    opcodetype op;
    bool reads = false;
    while (pc < leaf.end()) {
        if (!leaf.GetOp(pc, op)) return uint256();
        if (op == OP_INSPECTBTCPAYMENT) reads = true;
    }
    if (!reads) return uint256();
    CHashWriter h(SER_GETHASH, 0);
    h << script << control;
    return h.GetHash();
}

bool LostBindingYields(const uint256& bound_leaf, const CTransaction& tx, size_t n, const CCoinsViewCache& view,
                       const CBlockIndex* prev)
{
    AssertLockHeld(::cs_main);
    if (bound_leaf.IsNull() || n >= tx.vin.size()) return false;
    const Coin& coin = view.AccessCoin(tx.vin[n].prevout);
    int version;
    std::vector<unsigned char> program;
    if (coin.IsSpent() || !coin.out.scriptPubKey.IsWitnessProgram(version, program) || version != 1 || program.size() != 32) return false;
    if (LostProofLeaf(tx, n) != bound_leaf) return false;
    // Last, as it runs the script: the proof is read by an executed opcode, so
    // a branch of the leaf that ignores it never passes a binding.
    return LostInputReadsRecords(tx, n, view, prev);
}

bool LostActive()
{
    return g_con_pos && g_con_bitcoin_anchor && g_validate_anchor && !g_ignore;
}

bool LostActiveAt(int height)
{
    return LostActive() && Params().GetConsensus().LostBlocksActiveAt(height);
}

uint32_t LostWindow() { return g_window; }
int LostWaiverDepth() { return g_waiver_depth; }

void LostInit()
{
    g_ignore = gArgs.GetBoolArg("-posdebugignorelost", false);
    // W is a consensus value: the setting is honoured only on custom chains
    // (the functional tests' elementsregtest), never on the testnet or mainnet.
    const std::string chain = gArgs.GetChainName();
    g_window = DEFAULT_POS_LOST_WINDOW;
    if (chain != CBaseChainParams::TESTNET && chain != "sequentia") {
        g_window = (uint32_t)std::max<int64_t>(1, gArgs.GetIntArg("-poslostwindow", DEFAULT_POS_LOST_WINDOW));
    }
    g_waiver_depth = DEFAULT_LOST_WAIVER_DEPTH;
    if (chain != CBaseChainParams::TESTNET && chain != "sequentia") {
        g_waiver_depth = (int)std::max<int64_t>(1, gArgs.GetIntArg("-poslostwaiverdepth", DEFAULT_LOST_WAIVER_DEPTH));
    }
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
    if (FILE* f = fsbridge::fopen(LostPath(), "rb")) {
    CAutoFile file(f, SER_DISK, PROTOCOL_VERSION);
    try {
        uint32_t version;
        uint64_t n;
        file >> version >> n;
        if (version != 5 && version != 6) throw std::runtime_error("unknown version");
        file >> g_epoch;
        for (uint64_t i = 0; i < n && i < POS_LOST_MAX_BLOCKS; ++i) {
            auto block = std::make_shared<CBlock>();
            int height;
            bool certified, own;
            uint64_t epoch;
            bool prot;
            uint256 fork_hash;
            int fork_height;
            uint8_t anchor_state;
            std::vector<std::vector<unsigned char>> btc;
            bool seed_ok;
            file >> *block >> height >> certified >> own >> epoch >> prot >> fork_hash >> fork_height >> anchor_state >> btc >> seed_ok;
            LostBlk b = MakeBlk(block, height);
            b.btc = std::move(btc);
            b.seed_ok = seed_ok;
            b.prot = prot;
            b.fork_hash = fork_hash;
            b.fork_height = fork_height;
            b.anchor_state = (AnchorState)std::min<uint8_t>(anchor_state, 2);
            b.certified = certified;
            b.own = own;
            b.epoch = epoch;
            g_blocks.emplace(b.hash, std::move(b));
        }
        file >> g_sibling_capped;
        if (version >= 6) {
            file >> g_waiver_headers;
            for (const auto& [h, hdr] : g_waiver_headers) {
                ++g_waiver_children[hdr.hashPrevBlock];
                g_waiver_bytes += GetSerializeSize(hdr, PROTOCOL_VERSION);
            }
            g_waiver_dirty = !g_waiver_headers.empty();
        }
        // The duty holds from the first block this node judges after a
        // restart, before any maintenance pass: the bound spends as written.
        // Evidence binds only what the next maintenance pass finds valid here.
        for (const auto& [h, b] : g_blocks) {
            if (!b.prot || !b.own) continue;
            for (size_t i = 1; i < b.block->vtx.size(); ++i) {
                const CTransaction& tx = *b.block->vtx[i];
                for (size_t n = 0; n < tx.vin.size(); ++n) {
                    if (tx.vin[n].m_is_pegin) continue;
                    g_spends[tx.vin[n].prevout].push_back(LostSpend{tx.GetHash(), h, b.anchor_h, b.fork_hash, b.fork_height, b.height, LostProofLeaf(tx, n), /*own=*/true});
                }
            }
        }
        LogPrintf("Lost blocks: read %u from %s (%u bound outpoints)\n", (unsigned)g_blocks.size(), fs::PathToString(LostPath()), (unsigned)g_spends.size());
    } catch (const std::exception& e) {
        LogPrintf("Lost blocks: cannot read %s: %s\n", fs::PathToString(LostPath()), e.what());
    }
    }
    // Blocks disconnected after the last full write (F9's journal). Their
    // protection is computed by the first maintenance pass; until then the
    // block-level check holds them through LostNoteDisconnected's state.
    if (FILE* jf = fsbridge::fopen(JournalPath(), "rb")) {
        CAutoFile file(jf, SER_DISK, PROTOCOL_VERSION);
        size_t n = 0;
        try {
            while (g_blocks.size() < POS_LOST_MAX_BLOCKS) {
                uint32_t version;
                file >> version;
                if (version != 5) break;
                auto block = std::make_shared<CBlock>();
                int height;
                bool certified, own;
                uint64_t epoch;
                file >> *block >> height >> certified >> own >> epoch;
                LostBlk b = MakeBlk(block, height);
                b.certified = certified;
                b.own = own;
                b.epoch = epoch;
                g_epoch = std::max(g_epoch, epoch);
                // A block held as evidence that this node then had on its chain.
                if (auto known = g_blocks.find(b.hash); known != g_blocks.end()) {
                    if (b.own && !known->second.own) {
                        known->second.own = true;
                        known->second.epoch = b.epoch;
                        ++n;
                    }
                } else {
                    g_blocks.emplace(b.hash, std::move(b));
                    ++n;
                }
            }
        } catch (const std::exception&) {
            // The end of the journal, or a torn last entry from an unclean stop.
        }
        if (n) {
            g_dirty_file = true;
            LogPrintf("Lost blocks: read %u from %s\n", (unsigned)n, fs::PathToString(JournalPath()));
        }
    }
}

void LostShutdown()
{
    LOCK(g_lost_mutex);
    WriteFile();
}

void LostNoteSigned(const CBlock& block, int height)
{
    if (!g_bind_signed || !LostActiveAt(height)) return;
    LOCK(g_lost_mutex);
    const uint256 h = block.GetHash();
    if (g_signed.count(h)) return;
    g_signed.emplace(h, SignedBlk{std::make_shared<const CBlock>(block), height});
    // On disk before the signature leaves this node.
    WriteSignedFile();
}

void LostNoteDisconnected(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block, bool by_watcher)
{
    AssertLockHeld(::cs_main);
    if (!g_con_pos || !g_con_bitcoin_anchor || !g_validate_anchor || !pindex || !block) return;
    g_next_height = pindex->nHeight;
    // Its own chain means on this node's chain when Bitcoin took it. The watcher
    // disconnects only such blocks. Any other disconnect (fork choice, an
    // operator) counts only if Bitcoin had already left the anchor; a daemon
    // that does not answer counts as having left it, so that a block this node
    // did have on its chain is never demoted to evidence by an outage.
    const bool taken = by_watcher || CheckMainchainAnchor(block->m_anchor_height, block->m_anchor_hash) != AnchorCheckResult::OK;
    // Held durably before cs_main is released, so before this node signs
    // anything after the disconnect: an unclean stop cannot lose it.
    LOCK(g_lost_mutex);
    const uint256 h = pindex->GetBlockHash();
    auto known = g_blocks.find(h);
    if (known != g_blocks.end() && (known->second.own || !taken)) return;
    if (known != g_blocks.end()) {
        LogPrintf("Lost blocks: %s, held as evidence, was on this chain when Bitcoin took it: now this node's own\n", h.ToString());
    }
    LostBlk b = known != g_blocks.end() ? known->second : MakeBlk(block, pindex->nHeight);
    b.certified = b.certified || pindex->PosCarriesQuorum();
    b.own = taken;
    // DisconnectTip goes down from the tip: a block whose child was the last
    // one noted belongs to the same loss.
    if (g_last_noted_prev != h) ++g_epoch;
    g_last_noted_prev = b.prev;
    b.epoch = g_epoch;
    // F9: appended and synced, O(block); before cs_main is released.
    if (!AppendJournal(b)) WriteFile();
    g_blocks[h] = std::move(b);
    ++g_gen;
    g_dirty_file = true;
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

std::vector<unsigned char> SerTx(const CTransaction& tx)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << tx;
    const auto sp = MakeUCharSpan(ss);
    return {sp.begin(), sp.end()};
}

std::optional<CTransactionRef> DeserTx(const std::vector<unsigned char>& b)
{
    try {
        CDataStream ss(b, SER_NETWORK, PROTOCOL_VERSION);
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

//! One input a record binds: while LostStillBinds(height, anchor_h, ...) for
//! the judged block, only `txid` may spend `o`.
struct Binding {
    COutPoint o;
    uint256 txid;
    int height{0};          //!< the lost block's height
    uint32_t anchor_h{0};   //!< the lost block's anchor height
    uint256 leaf;           //!< LostProofLeaf of the recorded input
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
        auto tx = DeserTx(e.tx);
        if (!h || !tx) continue;
        for (size_t n = 0; n < (*tx)->vin.size(); ++n) {
            const CTxIn& in = (*tx)->vin[n];
            if (in.m_is_pegin) continue;
            out.push_back(Binding{in.prevout, (*tx)->GetHash(), (int)h->block_height, h->m_anchor_height, LostProofLeaf(**tx, n)});
        }
    }
    return out;
}

//! Whether a record in `p` (an ancestor of `prev`, or `prev`) can still bind a
//! block at `height` anchored at `anchor_h`: a record binds only while its
//! lost block (at most LOST_MAX_BRANCH above p, anchored at most W above p's
//! anchor) is not yet rebuilt plus W. Monotone going back: the walk stops at
//! the first `p` for which it is false.
bool RecordReach(const CBlockIndex* p, const CBlockIndex* prev, int height, uint32_t anchor_h)
{
    const int top = p->nHeight + LOST_MAX_BRANCH;
    if (height <= top) return true;
    const CBlockIndex* q = prev->GetAncestor(top);
    if (!q) return true;
    return (int64_t)anchor_h <= (int64_t)q->m_anchor_height + 2 * (int64_t)g_window;
}

//! F10: the bindings of the records of `tip`'s chain, rebuilt after a restart
//! from the blocks the index marks as carrying one (no count limit).
void LoadRecorded(const CBlockIndex* tip) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    if (g_recorded_loaded || !tip) return;
    g_recorded_loaded = true;
    for (const CBlockIndex* p = tip; p && RecordReach(p, tip, tip->nHeight + 1, tip->m_anchor_height); p = p->pprev) {
        if (!(p->nStatus & BLOCK_HAS_LOST_RECORD)) continue;
        if (!Params().GetConsensus().LostBlocksActiveAt(p->nHeight)) continue;
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

//! Every binding of `prev`'s chain still in force for its child anchored at `anchor_h`.
std::map<COutPoint, LostBoundSpend> ChainBindings(const CBlockIndex* prev, uint32_t anchor_h) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    std::map<COutPoint, LostBoundSpend> out;
    if (g_recorded.empty() || !prev) return out;
    const int height = prev->nHeight + 1;
    for (const CBlockIndex* p = prev; p && RecordReach(p, prev, height, anchor_h); p = p->pprev) {
        if (!Params().GetConsensus().LostBlocksActiveAt(p->nHeight)) continue;
        auto it = g_recorded.find(p->GetBlockHash());
        if (it == g_recorded.end()) continue;
        for (const Binding& b : it->second) {
            // Newest first, so the oldest binding in force is the one kept.
            if (LostStillBinds(b.height, b.anchor_h, height, anchor_h, prev)) out[b.o] = LostBoundSpend{b.txid, b.leaf};
        }
    }
    return out;
}

//! What the daemon says of one Bitcoin header. A header's fields and height
//! are facts of its hash, so a reading is kept and reused, bounded.
std::optional<BtcWork::Ancestor> BtcHeaderInfo(const uint256& hash)
{
    static Mutex memo_mutex;
    static std::map<uint256, BtcWork::Ancestor> memo GUARDED_BY(memo_mutex);
    {
        LOCK(memo_mutex);
        auto it = memo.find(hash);
        if (it != memo.end()) return it->second;
    }
    BtcWork::Ancestor a;
    a.hash = hash;
    try {
        UniValue p(UniValue::VARR);
        p.push_back(hash.GetHex());
        const UniValue reply = CallMainChainRPC("getblockheader", p);
        if (!find_value(reply, "error").isNull()) return std::nullopt;
        const UniValue r = find_value(reply, "result");
        if (!r.isObject()) return std::nullopt;
        const UniValue hv = find_value(r.get_obj(), "height");
        const UniValue tv = find_value(r.get_obj(), "time");
        const UniValue bv = find_value(r.get_obj(), "bits");
        if (!hv.isNum() || !tv.isNum() || !bv.isStr()) return std::nullopt;
        a.height = (uint32_t)hv.get_int64();
        a.time = (uint32_t)tv.get_int64();
        a.bits = (uint32_t)std::stoul(bv.get_str(), nullptr, 16);
        const UniValue prev = find_value(r.get_obj(), "previousblockhash");
        if (prev.isStr()) a.prev = uint256S(prev.get_str());
    } catch (const std::exception&) {
        return std::nullopt;
    }
    LOCK(memo_mutex);
    if (memo.size() > 6000) memo.clear();
    memo[hash] = a;
    return a;
}

//! The parent network whose difficulty rule this chain's anchors follow.
BtcWork::Net ParentNet()
{
    const std::string chain = gArgs.GetChainName();
    if (chain == "sequentia") return BtcWork::Net::MAIN;
    if (chain == CBaseChainParams::TESTNET) return BtcWork::Net::TESTNET4;
    return BtcWork::Net::REGTEST;
}

//! P1: the hashes of `anchor`'s ancestors (and itself) at heights from
//! `from_h` to `anchor_h`, read by walking parents from `anchor`. A fact of
//! Bitcoin's header tree: no reorganization changes it. nullopt when the
//! daemon does not know `anchor` or does not answer (soft, never a verdict).
std::optional<std::map<uint32_t, uint256>> AnchorAncestry(const uint256& anchor, uint32_t anchor_h, uint32_t from_h)
{
    if (from_h > anchor_h) return std::map<uint32_t, uint256>{};
    std::map<uint32_t, uint256> out;
    uint256 h = anchor;
    for (uint32_t height = anchor_h;; --height) {
        const std::optional<BtcWork::Ancestor> a = BtcHeaderInfo(h);
        // A header at another height than its anchor claims is not this
        // anchor's chain: R3 refuses it; nothing to say here.
        if (!a || a->height != height) return std::nullopt;
        out[height] = h;
        if (height == from_h) break;
        h = a->prev;
    }
    return out;
}

//! Rule 2 of a record (doc 03, "Records and bindings"): every anchor of the
//! lost headers up to `top_k` is on `ancestry`, or is a carried Bitcoin header
//! that links to it and meets Bitcoin's target. `seed_ok[k]` says whether
//! header k's anchor may seed header k+1's committee. Empty when proven; `soft`
//! when the daemon could not read an ancestor the rule needs.
std::string ProveLostAnchors(const std::vector<std::vector<unsigned char>>& carried, const std::vector<CBlockHeader>& lost,
                             size_t top_k, const std::map<uint32_t, uint256>& ancestry, std::vector<char>& seed_ok, bool& soft)
{
    if (carried.size() > 2 * (size_t)g_window) return "btc-too-many-headers";
    std::map<uint256, uint32_t> on_chain;
    for (const auto& [h, x] : ancestry) on_chain[x] = h;
    std::map<uint256, BtcWork::Ancestor> shown;
    std::map<uint256, bool> real;
    const BtcWork::Lookup lookup = [&](const uint256& hash) -> std::optional<BtcWork::Ancestor> {
        auto it = shown.find(hash);
        if (it != shown.end()) return it->second;
        return BtcHeaderInfo(hash);
    };
    for (const auto& bytes : carried) {
        if (bytes.size() != 80) return "btc-header-size";
        Sidechain::Bitcoin::CBlockHeader hdr;
        try {
            CDataStream hs(bytes, SER_NETWORK, PROTOCOL_VERSION);
            hs >> hdr;
        } catch (const std::exception&) {
            return "btc-header-decode";
        }
        uint32_t parent_h;
        if (auto c = shown.find(hdr.hashPrevBlock); c != shown.end()) {
            parent_h = c->second.height;
        } else if (auto o = on_chain.find(hdr.hashPrevBlock); o != on_chain.end()) {
            parent_h = o->second;
        } else {
            return "btc-header-unlinked";
        }
        const BtcWork::Verdict v = BtcWork::Judge(ParentNet(), hdr, parent_h + 1, lookup);
        if (v.state == BtcWork::State::UNANSWERED) { soft = true; return "bitcoin-unanswered"; }
        if (v.state != BtcWork::State::OK) return v.why;
        const uint256 hash = hdr.GetHash();
        shown[hash] = BtcWork::Ancestor{hash, hdr.hashPrevBlock, parent_h + 1, hdr.nTime, hdr.nBits};
        real[hash] = v.real;
    }
    seed_ok.assign(lost.size(), 0);
    for (size_t i = 0; i <= top_k && i < lost.size(); ++i) {
        const uint256& a = lost[i].m_anchor_hash;
        if (auto o = on_chain.find(a); o != on_chain.end() && o->second == lost[i].m_anchor_height) {
            seed_ok[i] = 1;
        } else if (auto c = shown.find(a); c != shown.end() && c->second.height == lost[i].m_anchor_height) {
            seed_ok[i] = real[a] ? 1 : 0;
        } else {
            return "lost-anchor-unproven";
        }
    }
    return "";
}

//! A certificate's seats under the registry at `fork_hash`, for a parent with
//! this anchor: verified once. The key is the header's whole bytes, the parent's
//! height and anchor, and the fork point that fixes the registry, so a verdict
//! is never reused for another header, registry or parent.
int CertSeatsCached(const CBlockHeader& h, const CBlockIndex* parent, const uint256& fork_hash, const StakeRegistry& reg,
                    std::string& reason)
{
    static Mutex cert_mutex;
    static std::map<uint256, std::pair<int, std::string>> certs GUARDED_BY(cert_mutex);
    CHashWriter key(SER_GETHASH, 0);
    key << SerHeader(h) << parent->nHeight << parent->m_anchor_height << parent->m_anchor_hash << fork_hash;
    const uint256 id = key.GetHash();
    {
        LOCK(cert_mutex);
        auto it = certs.find(id);
        if (it != certs.end()) {
            reason = it->second.second;
            return it->second.first;
        }
    }
    const int seats = PosVerifyBitfieldCertificate(h, parent, reg, reason);
    LOCK(cert_mutex);
    if (certs.size() >= 4096) certs.clear();
    certs[id] = {seats, reason};
    return seats;
}

//! The stake registry as it stood at `fork`, the registry the committee of
//! `fork`'s child was drawn from: the live registry, which mirrors `top`,
//! with the stake changes of every block above `fork` up to `top` reverted
//! from their undo data. A committee seeded by a value that was public at
//! `fork` is thereby drawn from stake that existed before that value was
//! known, so no key registered after it can sit on it (doc 03, "Records and
//! bindings"). nullptr when `fork` is not below `top` or a block or its undo
//! data cannot be read (soft: this node's own fault, never a verdict).
std::shared_ptr<const StakeRegistry> RegistryAt(const CBlockIndex* fork, const CBlockIndex* top) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    static Mutex cache_mutex;
    static std::map<std::pair<uint256, uint256>, std::shared_ptr<const StakeRegistry>> cache GUARDED_BY(cache_mutex);
    if (!fork || !top || top->GetAncestor(fork->nHeight) != fork) return nullptr;
    const std::pair<uint256, uint256> key{fork->GetBlockHash(), top->GetBlockHash()};
    {
        LOCK(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    auto reg = std::make_shared<StakeRegistry>(StakeRegistry::GetInstance());
    for (const CBlockIndex* p = top; p != fork; p = p->pprev) {
        CBlock blk;
        CBlockUndo undo;
        if (!ReadBlockFromDisk(blk, p, Params().GetConsensus()) || !node::UndoReadFromDisk(undo, p)) return nullptr;
        PosRevertBlockStake(blk, undo, p->nHeight, *reg);
    }
    LOCK(cache_mutex);
    if (cache.size() > 16) cache.clear();
    cache[key] = reg;
    return reg;
}

//! 2b-i: the certificates of a lost branch, verified in order from its fork
//! point under the seat quorum, with the registry at the fork point (the
//! registry the lost branch's first committee was really drawn from; a stake
//! change inside the lost branch is not in it, so a header after one does not
//! verify and its entries fall back to the holders). Header 0's committee is
//! seeded from the fork point's anchor (on the chain), each later one's from
//! its verified predecessor's anchor, and only when that anchor may seed
//! (`seed_ok`: on the judged block's ancestry, or a carried header meeting the
//! real difficulty), so no committee is seeded by a value an author chooses
//! without a Bitcoin block's work, and none counts a key registered after its
//! seed was public. `top` is the block the live registry mirrors. Returns how
//! many headers, from the first, carry such a certificate; `why` says why the
//! next does not, and `soft` is set when the registry could not be rebuilt.
size_t CertifiedPrefix(const std::vector<CBlockHeader>& lost, const std::vector<char>& seed_ok, const CBlockIndex* fork,
                       const CBlockIndex* top, std::string& why, bool& soft) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const std::shared_ptr<const StakeRegistry> at_fork = RegistryAt(fork, top);
    if (!at_fork) {
        soft = true;
        why = "the stake registry at the fork point cannot be rebuilt";
        return 0;
    }
    const StakeRegistry& reg = *at_fork;
    for (size_t k = 0; k < lost.size(); ++k) {
        CBlockIndex parent_view;
        const CBlockIndex* parent = fork;
        if (k > 0) {
            if (!seed_ok[k - 1]) {
                why = strprintf("lost header %u is seeded by an anchor below Bitcoin's real difficulty", (unsigned)k);
                return k;
            }
            parent_view.nHeight = fork->nHeight + (int)k;
            parent_view.m_anchor_height = lost[k - 1].m_anchor_height;
            parent_view.m_anchor_hash = lost[k - 1].m_anchor_hash;
            parent = &parent_view;
        }
        std::string reason;
        const int seats = CertSeatsCached(lost[k], parent, fork->GetBlockHash(), reg, reason);
        if (seats < 0 || seats < PosSlotQuorumAt(reg, fork->nHeight + 1 + (int)k)) {
            why = strprintf("certificate of lost header %u %s", (unsigned)k, reason.empty() ? "below the quorum" : reason);
            return k;
        }
    }
    return lost.size();
}

//! Judge one record for a block at `prev`'s child anchored at `anchor_h` /
//! `anchor`. Empty when valid; `soft` when the Bitcoin daemon could not say
//! what `anchor`'s ancestry is (the block is left aside and retried). `view`
//! holds the coins at `prev` and whatever earlier records of the block spent;
//! `bound` is the chain's bindings in force for this block.
std::string JudgeRecord(const LostRecord& r, uint32_t anchor_h, const uint256& anchor, const CBlockIndex* prev, bool& soft,
                        std::vector<Binding>& bindings, CCoinsViewCache& view,
                        const std::map<COutPoint, LostBoundSpend>& bound) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    soft = false;
    if (r.version != 3) return "version";
    if (r.lost.empty() || r.entries.empty()) return "empty";
    if (r.lost.size() > (size_t)LOST_MAX_BRANCH) return "branch-too-long";
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
        // A lost header anchored more than W above the fork point's anchor
        // left the window before it was lost.
        if (lost[i].m_anchor_height > fork->m_anchor_height + g_window) return "fork-too-deep";
        last_anchor_h = lost[i].m_anchor_height;
        last_anchor = lost[i].m_anchor_hash;
    }
    // Entries: structure, merkle, window (P4), all before Bitcoin is asked.
    uint32_t top_k = 0;
    std::vector<CTransactionRef> txs;
    for (const LostRecordEntry& e : r.entries) {
        if (e.k >= lost.size()) return "entry-block";
        if (e.pos == 0) return "entry-coinbase";
        // F7: the position names a leaf of a tree of exactly this depth.
        if (e.branch.size() > 31 || (uint64_t)e.pos >= ((uint64_t)1 << e.branch.size())) return "entry-position";
        auto tx = DeserTx(e.tx);
        if (!tx) return "entry-tx-decode";
        if (MerkleFromBranch((*tx)->GetHash(), e.branch, e.pos) != lost[e.k].hashMerkleRoot) return "entry-merkle";
        if (!LostStillBinds((int)lost[e.k].block_height, lost[e.k].m_anchor_height, prev->nHeight + 1, anchor_h, prev)) return "past-window";
        top_k = std::max(top_k, e.k);
        txs.push_back(*tx);
    }
    // Rule 2: every lost anchor up to the highest entry is on B's anchor
    // ancestry, or is a carried Bitcoin header with the work Bitcoin requires.
    const uint32_t from_h = fork->m_anchor_height;
    const std::optional<std::map<uint32_t, uint256>> ancestry = AnchorAncestry(anchor, anchor_h, from_h);
    if (!ancestry) { soft = true; return "bitcoin-unanswered"; }
    std::vector<char> seed_ok;
    const std::string unproven = ProveLostAnchors(r.btc, lost, top_k, *ancestry, seed_ok, soft);
    if (!unproven.empty()) return unproven;
    // Certificates (2b-i): every header up to the highest entry, in order.
    std::string why;
    if (CertifiedPrefix(std::vector<CBlockHeader>(lost.begin(), lost.begin() + top_k + 1), seed_ok, fork, prev, why, soft) <= top_k) return why;
    // P1: a header at or below each entry whose anchor is at most B's anchor
    // height and is not B's anchor's ancestor there.
    std::vector<int> first_off(top_k + 1, -1);
    int seen = -1;
    for (size_t i = 0; i <= top_k; ++i) {
        if (seen < 0 && lost[i].m_anchor_height <= anchor_h) {
            auto it = ancestry->find(lost[i].m_anchor_height);
            if (it == ancestry->end()) { soft = true; return "bitcoin-unanswered"; }
            if (it->second != lost[i].m_anchor_hash) seen = (int)i;
        }
        first_off[i] = seen;
    }
    for (const LostRecordEntry& e : r.entries) {
        if (first_off[e.k] < 0) return "not-lost";
    }
    // The first binding stands: a record cannot move a bound coin to another
    // transaction, not even one the binding would yield to.
    for (const CTransactionRef& t : txs) {
        for (const CTxIn& in : t->vin) {
            auto it = bound.find(in.prevout);
            if (it != bound.end() && it->second.txid != t->GetHash()) return "entry-conflicts-with-a-binding";
        }
    }
    // Rule 1: each entry is valid in this block, in order, by the block's own
    // transaction checks, with one exception: its payment proof may link to a
    // proven anchor of this branch. `view` then holds what it spent and made,
    // for the record after it.
    std::set<std::pair<uint32_t, uint256>> lost_anchors;
    for (size_t i = 0; i <= top_k; ++i) lost_anchors.insert({lost[i].m_anchor_height, lost[i].m_anchor_hash});
    const std::vector<std::string> verdicts = LostJudgeTxsInBlock(txs, view, prev, anchor_h, anchor, lost_anchors, bound);
    for (size_t j = 0; j < txs.size(); ++j) {
        if (!verdicts[j].empty()) return "entry-not-valid-here: " + verdicts[j];
    }
    for (size_t j = 0; j < r.entries.size(); ++j) {
        const LostRecordEntry& e = r.entries[j];
        for (size_t n = 0; n < txs[j]->vin.size(); ++n) {
            const CTxIn& in = txs[j]->vin[n];
            if (in.m_is_pegin) continue;
            bindings.push_back(Binding{in.prevout, txs[j]->GetHash(), (int)lost[e.k].block_height, lost[e.k].m_anchor_height, LostProofLeaf(*txs[j], n)});
        }
    }
    return "";
}

//! The holders' side of a record: it counts as a spend. The first binding of a
//! coin stands, so a record naming another transaction would take the coin from
//! a transaction this node had on its own chain as surely as mining the respend
//! would, even before the duty names that transaction.
std::optional<std::string> RecordViolationLocked(const CBlock& block, const CBlockIndex* prev) EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    for (const auto& r : LostParseRecords(block)) {
        if (!r) continue;
        for (const LostRecordEntry& e : r->entries) {
            const auto entry = DeserTx(e.tx);
            if (!entry) continue;
            const uint256 txid = (*entry)->GetHash();
            for (const CTxIn& in : (*entry)->vin) {
                if (in.m_is_pegin) continue;
                auto it = g_spends.find(in.prevout);
                if (it == g_spends.end()) continue;
                for (const LostSpend& s : it->second) {
                    if (!s.own || s.txid == txid || !Binds(s, block.m_anchor_height, prev)) continue;
                    return strprintf("a record binds %s:%u to tx %s, which tx %s of lost block %s (anchor %u, fork %d) spends",
                                     in.prevout.hash.ToString(), in.prevout.n, txid.ToString(), s.txid.ToString(),
                                     s.block.ToString(), s.anchor_h, s.fork_height);
                }
            }
        }
    }
    return std::nullopt;
}

//! What a holder's duty stands on, of the blocks in `prot`. Every block it had
//! on its own chain stands. An evidence-only sibling of one, and that sibling's
//! descendants, do not. An evidence-only transaction that conflicts with one of
//! the holder's own does not; two evidence-only transactions that conflict with
//! each other, where the holder had neither, both stand aside. A certificate
//! shows that a quorum signed a block, never which of two came first, and what
//! this node itself saw on its chain is the one order a late sibling cannot
//! rewrite.
struct Standing {
    std::set<uint256> blocks_out;
    std::set<uint256> txs_out;
};
Standing StandingOf(const std::set<uint256>& prot) EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    Standing st;
    std::set<std::pair<uint256, int>> own_at;
    std::vector<const LostBlk*> order;
    for (const uint256& h : prot) {
        auto it = g_blocks.find(h);
        if (it == g_blocks.end() || it->second.fork_height < 0) continue;
        order.push_back(&it->second);
        if (it->second.own) own_at.insert({it->second.fork_hash, it->second.height});
    }
    std::sort(order.begin(), order.end(), [](const LostBlk* a, const LostBlk* b) { return a->height < b->height; });
    for (const LostBlk* b : order) {
        if (b->own) continue;
        if (own_at.count({b->fork_hash, b->height}) || st.blocks_out.count(b->prev) || g_sibling_capped.count(b->prev)) {
            st.blocks_out.insert(b->hash);
        }
    }
    std::map<COutPoint, std::set<uint256>> own_spend, evidence_spend;
    for (const LostBlk* b : order) {
        if (st.blocks_out.count(b->hash)) continue;
        for (size_t i = 1; i < b->block->vtx.size(); ++i) {
            const CTransaction& tx = *b->block->vtx[i];
            for (const CTxIn& in : tx.vin) {
                if (!in.m_is_pegin) (b->own ? own_spend : evidence_spend)[in.prevout].insert(tx.GetHash());
            }
        }
    }
    for (const auto& [o, ids] : evidence_spend) {
        auto mine = own_spend.find(o);
        if (mine != own_spend.end()) {
            for (const uint256& id : ids) if (!mine->second.count(id)) st.txs_out.insert(id);
        } else if (ids.size() > 1) {
            st.txs_out.insert(ids.begin(), ids.end());
        }
    }
    return st;
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

//! What the classification of a list of lost transactions found. It reads the
//! parent, the transactions with their witnesses and the chain's bindings, and
//! nothing about the candidate's anchor, so proposals on one parent that carry
//! different anchors share it.
struct ClassMemo {
    uint256 key;
    std::vector<std::pair<LostTxCat, std::string>> classes;
    std::vector<int64_t> sigops;
};
constexpr size_t LOST_CLASS_MEMOS = 4;
std::deque<std::shared_ptr<const ClassMemo>> g_class_memos GUARDED_BY(g_lost_mutex);

//! The txids of the blocks above `from_height` on `prev`'s chain, read once per
//! block. Only the current walk is kept, so its size follows the distance to
//! the fork point. A block that cannot be read gives an empty answer, which
//! only costs the classification its shortcut.
std::map<uint256, std::shared_ptr<const std::vector<uint256>>> g_block_txids GUARDED_BY(::cs_main);

std::set<uint256> MinedSince(const CBlockIndex* prev, int from_height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    std::set<uint256> out;
    std::map<uint256, std::shared_ptr<const std::vector<uint256>>> walked;
    for (const CBlockIndex* p = prev; p && p->nHeight > from_height; p = p->pprev) {
        auto it = g_block_txids.find(p->GetBlockHash());
        std::shared_ptr<const std::vector<uint256>> ids;
        if (it != g_block_txids.end()) {
            ids = it->second;
        } else {
            CBlock blk;
            if (!ReadBlockFromDisk(blk, p, Params().GetConsensus())) return {};
            auto fresh = std::make_shared<std::vector<uint256>>();
            fresh->reserve(blk.vtx.size());
            for (const auto& tx : blk.vtx) fresh->push_back(tx->GetHash());
            ids = std::move(fresh);
        }
        out.insert(ids->begin(), ids->end());
        walked.emplace(p->GetBlockHash(), std::move(ids));
    }
    g_block_txids = std::move(walked);
    return out;
}

std::shared_ptr<const ClassMemo> ClassifyMemoized(const std::vector<CTransactionRef>& txs, CCoinsViewCache& view, const CBlockIndex* prev,
                                                  const Consensus::Params& consensus, const std::map<COutPoint, LostBoundSpend>& bound,
                                                  int lowest_fork)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, g_lost_mutex)
{
    CHashWriter hw(SER_GETHASH, 0);
    hw << prev->GetBlockHash();
    for (const CTransactionRef& tx : txs) hw << tx->GetWitnessHash();
    for (const auto& [o, t] : bound) hw << o << t.txid << t.leaf;
    const uint256 key = hw.GetHash();
    for (const auto& m : g_class_memos) {
        if (m->key == key) return m;
    }
    auto memo = std::make_shared<ClassMemo>();
    memo->key = key;
    const std::set<uint256> replayed = MinedSince(prev, lowest_fork);
    memo->classes = LostClassifyTxs(txs, view, prev, consensus, &memo->sigops, &bound, &replayed);
    g_class_memos.push_front(memo);
    if (g_class_memos.size() > LOST_CLASS_MEMOS) g_class_memos.pop_back();
    return memo;
}

Plan PlanCompute(const CBlockIndex* prev, uint32_t anchor_h, const uint256& anchor, CCoinsViewCache& view, bool build)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, g_lost_mutex)
{
    Plan plan;
    // The pending lost transactions of every lost branch of this chain that
    // this node had on its own chain, by loss, then height, then position.
    // Evidence-only blocks bind what stands of them and owe nothing.
    std::vector<const LostBlk*> blocks;
    for (const auto& [h, b] : g_blocks) {
        if (!b.prot || !b.own || b.fork_height < 0) continue;
        const CBlockIndex* f = prev->GetAncestor(b.fork_height);
        if (!f || f->GetBlockHash() != b.fork_hash) continue;
        if (!LostStillBinds(b.height, b.anchor_h, prev->nHeight + 1, anchor_h, prev)) continue;
        blocks.push_back(&b);
    }
    if (blocks.empty()) return plan;
    std::sort(blocks.begin(), blocks.end(), [](const LostBlk* a, const LostBlk* b) {
        return std::tie(a->epoch, a->height, a->hash) < std::tie(b->epoch, b->height, b->hash);
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
    // P5: the chain's bindings go into the classification, so a lost
    // transaction that a record of this chain forbids is never due.
    const auto bound = ChainBindings(prev, anchor_h);
    int lowest_fork = prev->nHeight;
    for (const LostBlk* b : blocks) lowest_fork = std::min(lowest_fork, b->fork_height);
    const std::shared_ptr<const ClassMemo> classified = ClassifyMemoized(txs, view, prev, consensus, bound, lowest_fork);
    const auto& classes = classified->classes;
    const auto& sigops = classified->sigops;
    std::set<uint256> bound_txids;
    for (const auto& [o, t] : bound) bound_txids.insert(t.txid);
    // The branch of a store block from its fork point's child, or empty.
    const auto branch_of = [&](const LostBlk* top) {
        std::vector<const LostBlk*> path;
        for (const LostBlk* w = top; w;) {
            path.push_back(w);
            if (w->height <= top->fork_height + 1) break;
            auto p = g_blocks.find(w->prev);
            w = p != g_blocks.end() ? &p->second : nullptr;
        }
        std::reverse(path.begin(), path.end());
        if (path.empty() || path.front()->prev != top->fork_hash) path.clear();
        return path;
    };
    // An entry's bytes, made once per transaction.
    std::map<std::pair<const LostBlk*, uint32_t>, LostRecordEntry> made;
    const auto entry_of = [&](const LostBlk* b, uint32_t pos) -> const LostRecordEntry& {
        auto it = made.find({b, pos});
        if (it != made.end()) return it->second;
        std::vector<uint256> leaves;
        for (const auto& tx : b->block->vtx) leaves.push_back(tx->GetHash());
        LostRecordEntry e;
        e.pos = pos;
        e.branch = MerkleBranch(leaves, pos);
        e.tx = SerTx(*b->block->vtx[pos]);
        return made.emplace(std::make_pair(b, pos), std::move(e)).first->second;
    };
    // The records, one per lost branch, of the given entries.
    const auto build_records = [&](const std::vector<std::pair<const LostBlk*, uint32_t>>& entries) {
        std::map<uint256, std::vector<std::pair<const LostBlk*, uint32_t>>> by_fork;
        for (const auto& e : entries) by_fork[e.first->fork_hash].push_back(e);
        std::vector<LostRecord> out;
        for (const auto& [fork_hash, items] : by_fork) {
            LostRecord rec;
            const LostBlk* t = items.front().first;
            for (const auto& [b, pos] : items) if (b->height > t->height) t = b;
            std::map<const LostBlk*, uint32_t> index;
            // The stale Bitcoin headers of the branch, parents first.
            std::map<std::pair<uint32_t, uint256>, std::vector<unsigned char>> stale;
            for (const LostBlk* w : branch_of(t)) {
                index[w] = rec.lost.size();
                rec.lost.push_back(SerHeader(w->block->GetBlockHeader()));
                for (size_t i = 0; i < w->btc.size(); ++i) {
                    const uint32_t h = w->anchor_h - (uint32_t)(w->btc.size() - 1 - i);
                    stale.emplace(std::make_pair(h, Hash(w->btc[i])), w->btc[i]);
                }
            }
            for (auto& [key, bytes] : stale) rec.btc.push_back(bytes);
            for (const auto& [b, pos] : items) {
                LostRecordEntry e = entry_of(b, pos);
                e.k = index[b];
                rec.entries.push_back(std::move(e));
            }
            out.push_back(std::move(rec));
        }
        return out;
    };
    const auto bytes_of = [&](const std::vector<std::pair<const LostBlk*, uint32_t>>& entries) {
        size_t bytes = 0;
        for (const LostRecord& r : build_records(entries)) bytes += GetSerializeSize(r, PROTOCOL_VERSION);
        return bytes;
    };
    // What to record: a transaction that fails as it is, not yet recorded on
    // this chain, that a record in this block would make valid: judged by the
    // record check's own function (P5), so the duty never asks for a record
    // that validity refuses.
    std::vector<std::pair<const LostBlk*, uint32_t>> recordable;
    int n_due = 0, n_later = 0, n_fails = 0, n_gone = 0, n_never = 0;
    std::string unrecordable;
    for (size_t i = 0; i < txs.size(); ++i) {
        switch (classes[i].first) {
        case LostTxCat::DUE: ++n_due; break;
        case LostTxCat::LATER: ++n_later; break;
        case LostTxCat::GONE: ++n_gone; break;
        case LostTxCat::FAILS: {
            ++n_fails;
            if (bound_txids.count(txs[i]->GetHash())) break;
            const std::vector<std::pair<const LostBlk*, uint32_t>> one{where[i]};
            std::vector<LostRecord> recs = build_records(one);
            if (recs.size() != 1 || recs[0].lost.empty()) break;
            bool soft = false;
            std::vector<Binding> ignored;
            CCoinsViewCache scratch(&view);
            const std::string why = JudgeRecord(recs[0], anchor_h, anchor, prev, soft, ignored, scratch, bound);
            if (!why.empty()) {
                LogPrint(BCLog::VALIDATION, "Lost blocks: %s cannot be recorded in a block anchored at %u on %s: %s\n",
                         txs[i]->GetHash().ToString(), anchor_h, prev->GetBlockHash().ToString(), why);
                break;
            }
            // An entry whose record cannot fit even alone is never recordable
            // (doc 03, "Records and bindings"): stated, logged, and skipped, so
            // it never empties the records owed after it.
            if (GetSerializeSize(recs[0], PROTOCOL_VERSION) > LOST_RECORD_MAX_BYTES) {
                ++n_never;
                unrecordable = txs[i]->GetHash().ToString();
                LogPrintf("Lost blocks: %s can never be recorded: its record alone is %u bytes, over %u; the holders' refusal is its only protection\n",
                          unrecordable, (unsigned)GetSerializeSize(recs[0], PROTOCOL_VERSION), (unsigned)LOST_RECORD_MAX_BYTES);
                break;
            }
            recordable.push_back(where[i]);
            break;
        }
        }
    }
    // The record owed by this block: the longest prefix, in lost order, of
    // the recordable transactions whose records fit the block's record bytes.
    // The rest is owed by the next block.
    std::vector<std::pair<const LostBlk*, uint32_t>> chosen;
    {
        // Two entries that spend one coin cannot both be valid in one block:
        // the first in lost order stands.
        std::set<COutPoint> taken;
        for (const auto& e : recordable) {
            const CTransaction& t = *e.first->block->vtx[e.second];
            bool clash = false;
            for (const CTxIn& in : t.vin) clash = clash || taken.count(in.prevout);
            if (clash) continue;
            for (const CTxIn& in : t.vin) taken.insert(in.prevout);
            chosen.push_back(e);
        }
        // The size grows with the run, so the longest that fits is a search.
        size_t lo = 0, hi = chosen.size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo + 1) / 2;
            const std::vector<std::pair<const LostBlk*, uint32_t>> run(chosen.begin(), chosen.begin() + mid);
            if (bytes_of(run) <= LOST_RECORD_MAX_BYTES) lo = mid; else hi = mid - 1;
        }
        chosen.resize(lo);
    }
    // The whole set, judged as the block will be: if it would not be valid,
    // owe nothing rather than demand an invalid block.
    if (!chosen.empty()) {
        std::vector<Binding> ignored;
        CCoinsViewCache scratch(&view);
        for (const LostRecord& r : build_records(chosen)) {
            bool soft = false;
            const std::string why = JudgeRecord(r, anchor_h, anchor, prev, soft, ignored, scratch, bound);
            if (!why.empty()) {
                LogPrintf("Lost blocks: the records owed on %s would not be valid (%s); none is owed\n", prev->GetBlockHash().ToString(), why);
                chosen.clear();
                break;
            }
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
    plan.summary = strprintf("%d due (%d in the prefix%s), %d later, %d failing (%d to record of %d recordable, %d never recordable), %d gone; classified in %d us",
                             n_due, (int)plan.prefix.size(), plan.cut ? ", cut" : "", n_later, n_fails,
                             (int)plan.to_record.size(), (int)recordable.size(), n_never, n_gone, (int)plan.micros);
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
    uint256 anchor;
    uint256 store;
    Plan plan;
};
std::optional<PlanMemo> g_plan_memo GUARDED_BY(g_lost_mutex);

uint256 StoreFingerprint() EXCLUSIVE_LOCKS_REQUIRED(g_lost_mutex)
{
    CHashWriter hw(SER_GETHASH, 0);
    hw << g_gen;
    // The plan carries only blocks this node had on its own chain, so evidence
    // that binds nothing new does not make it compute again.
    for (const auto& [h, b] : g_blocks) {
        if (!b.own) continue;
        hw << h << b.prot << b.certified << (uint8_t)b.anchor_state << b.fork_height << b.epoch;
    }
    return hw.GetHash();
}

Plan PlanLocked(const CBlockIndex* prev, uint32_t anchor_h, const uint256& anchor, CCoinsViewCache& view, bool /*build*/)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, g_lost_mutex)
{
    const uint256 store = StoreFingerprint();
    if (g_plan_memo && g_plan_memo->prev == prev->GetBlockHash() && g_plan_memo->anchor_h == anchor_h && g_plan_memo->anchor == anchor && g_plan_memo->store == store) {
        Plan hit = g_plan_memo->plan;
        hit.summary += "; reused";
        return hit;
    }
    Plan plan = PlanCompute(prev, anchor_h, anchor, view, /*build=*/true);
    if (!plan.summary.empty()) LogPrint(BCLog::VALIDATION, "Lost blocks: plan computed on %s at height %d: %s\n", prev->GetBlockHash().ToString(), prev->nHeight + 1, plan.summary);
    g_plan_memo = PlanMemo{prev->GetBlockHash(), anchor_h, anchor, store, plan};
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

bool LostCheckRecords(const CBlock& block, const CBlockIndex* pindexPrev, bool just_check, BlockValidationState& state,
                      CCoinsViewCache& view, bool* has_record)
{
    AssertLockHeld(::cs_main);
    if (has_record) *has_record = false;
    if (!g_con_pos || !g_con_bitcoin_anchor || !g_validate_anchor || !pindexPrev || g_ignore) return true;
    // Below the rule's height a record is an ordinary OP_RETURN output: judged by
    // nothing and binding nothing.
    if (!Params().GetConsensus().LostBlocksActiveAt(pindexPrev->nHeight + 1)) return true;
    LoadRecorded(pindexPrev);
    std::vector<Binding> mine;
    size_t bytes = 0;
    // The bindings already in force, and the coins as this block's records
    // leave them, so each record's entries are judged after the ones before.
    std::map<COutPoint, LostBoundSpend> bound = ChainBindings(pindexPrev, block.m_anchor_height);
    CCoinsViewCache records_view(&view);
    for (const auto& r : LostParseRecords(block)) {
        if (!r) return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-lost-record", "malformed");
        bytes += GetSerializeSize(*r, PROTOCOL_VERSION);
        if (bytes > LOST_RECORD_MAX_BYTES) return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-lost-record", "too large");
        bool soft = false;
        const std::string why = JudgeRecord(*r, block.m_anchor_height, block.m_anchor_hash, pindexPrev, soft, mine, records_view, bound);
        if (!why.empty()) {
            LogPrintf("Lost blocks: block %s carries a record that is not valid: %s\n", block.GetHash().ToString(), why);
            if (soft) return state.Invalid(BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "lost-record-unverifiable", why);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-lost-record", why);
        }
    }
    for (const Binding& b : mine) bound.emplace(b.o, LostBoundSpend{b.txid, b.leaf});
    if (!bound.empty()) {
        // Coins this block's own transactions created cannot be bound yet, so
        // every bound input it spends is in the parent's view.
        for (size_t i = 1; i < block.vtx.size(); ++i) {
            const CTransaction& tx = *block.vtx[i];
            for (size_t n = 0; n < tx.vin.size(); ++n) {
                const CTxIn& in = tx.vin[n];
                auto it = bound.find(in.prevout);
                if (it == bound.end() || it->second.txid == tx.GetHash()) continue;
                if (LostBindingYields(it->second.leaf, tx, n, view, pindexPrev)) continue;
                const std::string why = strprintf("tx %s spends %s:%u, which recorded tx %s spends", tx.GetHash().ToString(),
                                                  in.prevout.hash.ToString(), in.prevout.n, it->second.txid.ToString());
                LogPrintf("Lost blocks: block %s breaks a record: %s\n", block.GetHash().ToString(), why);
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-recorded-respend", why);
            }
        }
    }
    if (has_record) *has_record = !mine.empty();
    if (!just_check && !mine.empty()) {
        g_recorded[block.GetHash()] = std::move(mine);
        LogPrintf("Lost blocks: block %s at height %d records %d bound input(s)\n", block.GetHash().ToString(),
                  pindexPrev->nHeight + 1, (int)g_recorded[block.GetHash()].size());
    }
    return true;
}

LostTemplateParts LostForTemplate(const CBlockIndex* pindexPrev, uint32_t anchor_height, const uint256& anchor_hash, CCoinsViewCache& view)
{
    AssertLockHeld(::cs_main);
    LostTemplateParts out;
    if (!pindexPrev || !LostActiveAt(pindexPrev->nHeight + 1)) return out;
    LoadRecorded(pindexPrev);
    LOCK(g_lost_mutex);
    Plan plan = PlanLocked(pindexPrev, anchor_height, anchor_hash, view, /*build=*/true);
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
                    BlockValidationState& state, bool just_check)
{
    AssertLockHeld(::cs_main);
    if (!pindexPrev || ibd || !LostActiveAt(pindexPrev->nHeight + 1)) return true;
    LOCK(g_lost_mutex);
    if (!g_waived.empty()) {
        // A quorum has certified past this holder: it takes that block, and the
        // duty is off above it, or it would be refused again one block later. It
        // still does not sign the waived block itself.
        bool waived = !just_check && g_waived.count(block.GetHash()) > 0;
        const CBlockIndex* w = pindexPrev;
        for (int i = 0; !waived && w && i <= LOST_MAX_BRANCH; ++i, w = w->pprev) waived = g_waived.count(w->GetBlockHash()) > 0;
        if (waived) {
            LogPrintf("Lost blocks: block %s at height %d is at or above a block a quorum certified past this holder; the duty does not bear on it\n",
                      block.GetHash().ToString(), pindexPrev->nHeight + 1);
            return true;
        }
    }
    std::optional<std::string> why;
    std::string reason;
    if ((why = ViolationLocked(block, pindexPrev, view)) || (why = RecordViolationLocked(block, pindexPrev))) {
        reason = "bad-txns-lost-respend";
    } else if ((why = SignedViolationLocked(block, pindexPrev))) {
        reason = "bad-txns-signed-respend";
    } else if (!g_spends.empty()) {
        // The duty: the due lost transactions first, and the records owed.
        Plan plan = PlanLocked(pindexPrev, block.m_anchor_height, block.m_anchor_hash, view, /*build=*/false);
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
            // A block that mines the transaction itself owes no record of it.
            std::set<uint256> carried;
            for (const auto& t : block.vtx) carried.insert(t->GetHash());
            for (const auto& r : LostParseRecords(block)) {
                if (!r) continue;
                for (const auto& e : r->entries) {
                    if (auto tx = DeserTx(e.tx)) carried.insert((*tx)->GetHash());
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

std::optional<uint256> LostRecordedConflict(const CTransaction& tx, const CBlockIndex* tip, uint32_t anchor_h,
                                            const CCoinsViewCache& view)
{
    AssertLockHeld(::cs_main);
    if (!tip || !LostActiveAt(tip->nHeight + 1)) return std::nullopt;
    LoadRecorded(tip);
    if (g_recorded.empty()) return std::nullopt;
    // One walk of the chain per tip and anchor, however many transactions ask.
    static uint256 memo_tip;
    static uint32_t memo_anchor{0};
    static size_t memo_records{0};
    static std::map<COutPoint, LostBoundSpend> memo;
    if (memo_tip != tip->GetBlockHash() || memo_anchor != anchor_h || memo_records != g_recorded.size()) {
        memo = ChainBindings(tip, anchor_h);
        memo_tip = tip->GetBlockHash();
        memo_anchor = anchor_h;
        memo_records = g_recorded.size();
    }
    const uint256 txid = tx.GetHash();
    for (size_t n = 0; n < tx.vin.size(); ++n) {
        auto it = memo.find(tx.vin[n].prevout);
        if (it == memo.end() || it->second.txid == txid) continue;
        if (LostBindingYields(it->second.leaf, tx, n, view, tip)) continue;
        return it->second.txid;
    }
    return std::nullopt;
}

std::optional<uint256> LostConflict(const CTransaction& tx, const CBlockIndex* tip, const CCoinsViewCache& view)
{
    if (!tip || !LostActiveAt(tip->nHeight + 1)) return std::nullopt;
    LOCK(g_lost_mutex);
    const uint256 txid = tx.GetHash();
    for (size_t n = 0; n < tx.vin.size(); ++n) {
        const CTxIn& in = tx.vin[n];
        auto it = g_spends.find(in.prevout);
        if (it == g_spends.end()) continue;
        for (const LostSpend& s : it->second) {
            if (s.txid == txid || !Binds(s, tip->m_anchor_height, tip)) continue;
            if (!LostBindingYields(s.leaf, tx, n, view, tip)) return s.txid;
        }
        if (!g_bind_signed) continue;
        auto st = g_signed_spends.find(in.prevout);
        if (st == g_signed_spends.end()) continue;
        for (const LostSpend& s : st->second) {
            if (s.txid != txid && Binds(s, tip->m_anchor_height, tip)) return s.txid;
        }
    }
    return std::nullopt;
}

void LostNoteRefused(const CBlockIndex* pindex)
{
    LOCK(g_lost_mutex);
    g_refused.insert(pindex->GetBlockHash());
    g_waiver_dirty = true;
}

void LostNoteRefusedChild(const CBlockHeader& header, bool only_if_parent_kept)
{
    LOCK(g_lost_mutex);
    if (only_if_parent_kept && !g_waiver_headers.count(header.hashPrevBlock)) return;
    const uint256 hash = header.GetHash();
    if (g_waiver_headers.count(hash) || g_waiver_headers.size() >= LOST_WAIVER_MAX_HEADERS) return;
    // Only the run a holder counts is kept: the waiver depth above the refused block.
    int above = 1;
    for (auto p = g_waiver_headers.find(header.hashPrevBlock); p != g_waiver_headers.end(); p = g_waiver_headers.find(p->second.hashPrevBlock)) {
        if (++above > g_waiver_depth) return;
    }
    const size_t bytes = GetSerializeSize(header, PROTOCOL_VERSION);
    if (g_waiver_bytes + bytes > LOST_WAIVER_MAX_BYTES) return;
    size_t& kept = g_waiver_children[header.hashPrevBlock];
    if (kept >= LOST_WAIVER_MAX_CHILDREN) return;
    ++kept;
    g_waiver_bytes += bytes;
    g_waiver_headers.emplace(hash, header);
    g_waiver_dirty = true;
    g_dirty_file = true;
}

bool LostWaiverKept(const uint256& hash)
{
    LOCK(g_lost_mutex);
    return g_waiver_headers.count(hash) > 0;
}

void LostQueueEvidence(int64_t peer, LostEvidenceMsg msg)
{
    LOCK(g_lost_mutex);
    g_sent[peer].insert(msg.block.GetHash()); // never echo it back
    if (g_blocks.count(msg.block.GetHash())) return;
    // F8: the inbox is bounded in bytes per peer and in all, not only in messages.
    const size_t bytes = GetSerializeSize(msg.block, PROTOCOL_VERSION);
    size_t& mine = g_inbox_bytes[peer];
    if (g_inbox.size() >= 1000 || mine + bytes > LOST_INBOX_PEER_BYTES || g_inbox_total + bytes > LOST_INBOX_TOTAL_BYTES) return;
    mine += bytes;
    g_inbox_total += bytes;
    g_inbox.emplace_back(peer, std::move(msg));
}

//! Evidence's stale anchor: the carried headers lead from a block of Bitcoin's
//! best chain to `block`'s anchor, each with the work Bitcoin requires. The
//! receiver's daemon need not have seen any of them. Empty when proven, else
//! why not; `seed_ok` says whether the anchor meets the real difficulty.
static std::string ProveEvidenceAnchor(const CBlock& block, const std::vector<std::vector<unsigned char>>& carried, bool& seed_ok)
{
    if (carried.empty()) return "anchor-unproven";
    if (carried.size() > 2 * (size_t)g_window) return "anchor-proof-too-long";
    std::map<uint256, BtcWork::Ancestor> shown;
    const BtcWork::Lookup lookup = [&](const uint256& hash) -> std::optional<BtcWork::Ancestor> {
        auto it = shown.find(hash);
        if (it != shown.end()) return it->second;
        return BtcHeaderInfo(hash);
    };
    uint256 prev;
    uint32_t height = block.m_anchor_height - (uint32_t)(carried.size() - 1);
    bool real = false;
    for (size_t i = 0; i < carried.size(); ++i, ++height) {
        if (carried[i].size() != 80) return "anchor-proof-size";
        Sidechain::Bitcoin::CBlockHeader hdr;
        try {
            CDataStream hs(carried[i], SER_NETWORK, PROTOCOL_VERSION);
            hs >> hdr;
        } catch (const std::exception&) {
            return "anchor-proof-decode";
        }
        if (i == 0) {
            if (BestChainVerdict(height - 1, hdr.hashPrevBlock) != AnchorState::OK) return "anchor-proof-unlinked";
        } else if (hdr.hashPrevBlock != prev) {
            return "anchor-proof-unlinked";
        }
        const BtcWork::Verdict v = BtcWork::Judge(ParentNet(), hdr, height, lookup);
        if (v.state == BtcWork::State::UNANSWERED) return "anchor-unknown";
        if (v.state != BtcWork::State::OK) return "anchor-proof: " + v.why;
        prev = hdr.GetHash();
        real = v.real;
        shown[prev] = BtcWork::Ancestor{prev, hdr.hashPrevBlock, height, hdr.nTime, hdr.nBits};
    }
    if (prev != block.m_anchor_hash) return "anchor-unproven";
    seed_ok = real;
    return "";
}

//! The headers from Bitcoin's best chain to `anchor`, parents first, as this
//! node's daemon has them. Empty when it does not.
static std::vector<std::vector<unsigned char>> ReadStaleHeaders(const uint256& anchor, uint32_t anchor_h)
{
    std::vector<std::vector<unsigned char>> out;
    uint256 at = anchor;
    uint32_t h = anchor_h;
    try {
        for (uint32_t steps = 0; steps <= 2 * g_window; ++steps, --h) {
            if (BestChainVerdict(h, at) == AnchorState::OK) {
                std::reverse(out.begin(), out.end());
                return out;
            }
            UniValue p(UniValue::VARR);
            p.push_back(at.GetHex());
            p.push_back(false);
            const UniValue reply = CallMainChainRPC("getblockheader", p);
            const UniValue res = find_value(reply, "result");
            if (!find_value(reply, "error").isNull() || !res.isStr()) return {};
            std::vector<unsigned char> raw = ParseHex(res.get_str());
            if (raw.size() != 80) return {};
            CDataStream hs(raw, SER_NETWORK, PROTOCOL_VERSION);
            Sidechain::Bitcoin::CBlockHeader hdr;
            hs >> hdr;
            out.push_back(std::move(raw));
            at = hdr.hashPrevBlock;
        }
    } catch (const std::exception&) {
    }
    return {};
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
    // 2b-i: a certificate is trusted only when every header below it, back to
    // the fork point, is itself certified: its committee is then seeded from
    // an anchor the chain or a verified certificate fixed, never one chosen by
    // whoever shows the block.
    bool rooted = false;
    bool expired = false;
    uint256 fork_hash;
    {
        LOCK(cs_main);
        const CBlockIndex* p = chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        if (p && chainman.ActiveChain().Contains(p)) {
            fork_hash = p->GetBlockHash();
            height = p->nHeight + 1;
            parent_anchor_h = p->m_anchor_height;
            parent_anchor = p->m_anchor_hash;
            rooted = true;
            // P4: already past its lapse on the active chain.
            expired = tip && !LostStillBinds(height, block.m_anchor_height, tip->nHeight + 1, tip->m_anchor_height, tip);
        }
        const CBlockIndex* self = chainman.m_blockman.LookupBlockIndex(hash);
        if (self && chainman.ActiveChain().Contains(self)) return "on-active-chain";
    }
    if (height < 0) {
        LOCK(g_lost_mutex);
        auto it = g_blocks.find(block.hashPrevBlock);
        if (it == g_blocks.end()) return "unknown-parent";
        // The branch's fork point: its first parent outside the store.
        const LostBlk* walk = &it->second;
        for (int guard = 0; guard < LOST_MAX_BRANCH + 1; ++guard) {
            auto up = g_blocks.find(walk->prev);
            if (up == g_blocks.end()) break;
            walk = &up->second;
        }
        fork_hash = walk->prev;
        height = it->second.height + 1;
        parent_anchor_h = it->second.anchor_h;
        parent_anchor = it->second.anchor;
        parent_in_store = true;
        // Its anchor seeds this block's committee: a header below Bitcoin's
        // real difficulty is a value its author could grind.
        rooted = it->second.certified && it->second.seed_ok;
    }
    if (g_con_blockheightinheader && (uint32_t)height != block.block_height) return "bad-height";
    if (block.m_anchor_height < parent_anchor_h) return "bad-anchor-not-monotonic";
    // A fork point more than W below the block is not carried: whatever
    // forked that long ago left the window before it was lost (doc 03).
    if (!parent_in_store && block.m_anchor_height > parent_anchor_h + g_window) return "fork-too-deep";
    if (expired) return "expired";
    // Where its anchor stands on this node's view of Bitcoin's best chain (the
    // duty's view, soft): a block of a branch Bitcoin left, or the child of
    // one. The daemon need not have seen the stale block: the best chain
    // having another block at that height is enough.
    AnchorState verdict = BestChainVerdict(block.m_anchor_height, block.m_anchor_hash);
    if (verdict == AnchorState::UNKNOWN) return "anchor-unknown";
    if (verdict == AnchorState::OK && !parent_in_store) return "anchor-on-best-chain";
    // A stale anchor is proven by the headers the message carries, whatever
    // this node's own daemon saw: an invented block binds nothing.
    bool seed_ok = true;
    if (verdict == AnchorState::STALE) {
        const std::string unproven = ProveEvidenceAnchor(block, msg.btc, seed_ok);
        if (!unproven.empty()) return unproven;
    }
    // The certificate, with the registry at the branch's fork point, as the
    // record check reads it (doc 03, "Records and bindings"): no key
    // registered after the seed of the branch's first committee was public
    // counts.
    CBlockIndex parent_idx;
    parent_idx.nHeight = height - 1;
    parent_idx.m_anchor_height = parent_anchor_h;
    parent_idx.m_anchor_hash = parent_anchor;
    std::shared_ptr<const StakeRegistry> at_fork;
    {
        LOCK(cs_main);
        const CBlockIndex* fork = chainman.m_blockman.LookupBlockIndex(fork_hash);
        if (fork && chainman.ActiveChain().Contains(fork)) at_fork = RegistryAt(fork, chainman.ActiveChain().Tip());
    }
    if (!at_fork) return "registry-unavailable";
    const StakeRegistry& reg = *at_fork;
    std::string reason;
    const int signers = CertSeatsCached(block.GetBlockHeader(), &parent_idx, fork_hash, reg, reason);
    if (signers < 0) return "bad-certificate: " + reason;
    const bool certified = rooted && signers >= PosSlotQuorumAt(reg, height);
    if (!certified && !parent_in_store && verdict == AnchorState::OK) return "not-certified";
    LostBlk b = MakeBlk(std::make_shared<const CBlock>(block), height);
    b.certified = certified;
    b.anchor_state = verdict;
    b.btc = msg.btc;
    b.seed_ok = seed_ok;
    LOCK(g_lost_mutex);
    {
        uint32_t certified_children = 0;
        int below_quorum = 0;
        for (const auto& [h, x] : g_blocks) {
            if (x.own || x.prev != block.hashPrevBlock) continue;
            if (x.certified) ++certified_children; else ++below_quorum;
        }
        if (certified && certified_children >= LostSiblingCap(g_window)) {
            if (g_sibling_capped.insert(block.hashPrevBlock).second) {
                ++g_gen;
                g_dirty_file = true;
            }
            return "sibling-cap";
        }
        if (!certified && below_quorum >= LOST_MAX_BELOW_QUORUM_CHILDREN) return "sibling-below-quorum";
    }
    if (!certified) {
        // F8: blocks that are not (yet) certified in order are few and bounded.
        size_t n = 0;
        for (const auto& [h, x] : g_blocks) n += !x.certified;
        if (n >= LOST_MAX_UNCERTIFIED) return "store-full-uncertified";
    }
    {
        auto parent = g_blocks.find(block.hashPrevBlock);
        b.epoch = parent != g_blocks.end() ? parent->second.epoch : ++g_epoch;
    }
    LogPrintf("Lost blocks: took evidence %s at height %d (anchor %u %s, %d signer(s), %s)\n", hash.ToString(), height,
              b.anchor_h, verdict == AnchorState::STALE ? "off Bitcoin's best chain" : "on Bitcoin's best chain",
              signers, certified ? "certified" : rooted ? "below the quorum" : "not rooted in a certified branch");
    g_blocks.emplace(hash, std::move(b));
    g_dirty_file = true;
    return "";
}

std::vector<LostEvidenceMsg> LostToSend(int64_t peer)
{
    std::vector<LostEvidenceMsg> out;
    if (!LostActiveAt(g_next_height.load())) return out;
    LOCK(g_lost_mutex);
    std::vector<const LostBlk*> todo;
    std::set<uint256>& sent = g_sent[peer];
    for (const auto& [h, b] : g_blocks) {
        if (b.prot && !sent.count(h)) todo.push_back(&b);
    }
    std::sort(todo.begin(), todo.end(), [](const LostBlk* a, const LostBlk* b) { return a->height < b->height; });
    // A block is marked sent only when it went out whole. One whose headers are
    // not read yet waits, and so do its children: a receiver turns a child down
    // while it lacks the parent.
    std::set<uint256> waiting;
    for (const LostBlk* b : todo) {
        if (out.size() >= 8) break;
        if (!EvidenceComplete(*b) || waiting.count(b->prev)) {
            waiting.insert(b->hash);
            continue;
        }
        sent.insert(b->hash);
        out.push_back(LostEvidenceMsg{*b->block, b->btc});
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
            const size_t bytes = GetSerializeSize(item.second.block, PROTOCOL_VERSION);
            size_t& mine = g_inbox_bytes[item.first];
            mine -= std::min(mine, bytes);
            g_inbox_total -= std::min(g_inbox_total, bytes);
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
    // One question per distinct anchor, however many blocks name it.
    std::map<std::pair<uint32_t, uint256>, AnchorState> by_anchor;
    std::map<uint256, AnchorState> verdicts;
    for (const auto& [h, a] : anchors) {
        auto it = by_anchor.find(a);
        if (it == by_anchor.end()) it = by_anchor.emplace(a, BestChainVerdict(a.first, a.second)).first;
        verdicts[h] = it->second;
    }
    // The stale headers of blocks this node connected, read once while its
    // daemon has them: a record or a peer needs them to check the work.
    std::map<uint256, std::vector<std::vector<unsigned char>>> stale_headers;
    {
        std::vector<std::pair<uint256, std::pair<uint32_t, uint256>>> need;
        {
            LOCK(g_lost_mutex);
            for (const auto& [h, b] : g_blocks) {
                auto v = verdicts.find(h);
                if (b.btc.empty() && v != verdicts.end() && v->second == AnchorState::STALE) need.push_back({h, {b.anchor_h, b.anchor}});
            }
        }
        for (const auto& [h, a] : need) {
            auto got = ReadStaleHeaders(a.second, a.first);
            if (!got.empty()) stale_headers[h] = std::move(got);
        }
    }
    // The lapse is the active chain's (P4). Bitcoin's height only bounds how
    // long a block whose fork point left the active chain is kept (a resource
    // bound, not a rule).
    int btc_h = -1;
    if (!GetMainchainBlockCount(btc_h)) btc_h = -1;
    if (btc_h >= 0) g_btc_h = btc_h;
    // 4) Which blocks are lost, and the outpoints they bind.
    bool changed = false;
    {
        LOCK2(cs_main, g_lost_mutex);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        for (auto it = g_blocks.begin(); it != g_blocks.end();) {
            LostBlk& b = it->second;
            auto v = verdicts.find(it->first);
            if (v != verdicts.end() && v->second != AnchorState::UNKNOWN) b.anchor_state = v->second;
            if (auto sh = stale_headers.find(it->first); sh != stale_headers.end() && b.btc.empty()) {
                b.btc = sh->second;
                g_dirty_file = true;
                // Sent before Bitcoin took its anchor, it went without them.
                for (auto& [peer, sent] : g_sent) sent.erase(it->first);
            }
            const CBlockIndex* self = chainman.m_blockman.LookupBlockIndex(b.hash);
            const bool active = self && chainman.ActiveChain().Contains(self);
            const CBlockIndex* f = (tip && b.fork_height >= 0) ? tip->GetAncestor(b.fork_height) : nullptr;
            const bool on_fork = f && f->GetBlockHash() == b.fork_hash;
            const bool expired = on_fork ? !LostStillBinds(b.height, b.anchor_h, tip->nHeight + 1, tip->m_anchor_height, tip)
                                         : (btc_h >= 0 && (uint32_t)btc_h > b.anchor_h + 2 * g_window);
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
        std::set<uint256> evidence_valid;
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
        // Evidence binds only what stands and is valid here but for a payment
        // proof that links to a lost anchor: an invented or not-yet-valid
        // transaction binds nothing. Judged once per tip and store, never at
        // every pass.
        const Standing standing = StandingOf(prot);
        {
            static uint256 memo_key;
            static std::set<uint256> memo_valid;
            CHashWriter key(SER_GETHASH, 0);
            key << (tip ? tip->GetBlockHash() : uint256());
            std::vector<const LostBlk*> order;
            bool any_evidence = false;
            for (const auto& [h, b] : g_blocks) {
                if (!prot.count(h)) continue;
                key << h;
                any_evidence = any_evidence || !b.own;
                if (!standing.blocks_out.count(h)) order.push_back(&b);
            }
            const uint256 id = key.GetHash();
            if (!any_evidence || !tip) {
                memo_valid.clear();
                memo_key = id;
            } else if (id != memo_key) {
                std::sort(order.begin(), order.end(), [](const LostBlk* a, const LostBlk* b) {
                    if (a->own != b->own) return a->own;
                    return std::tie(a->epoch, a->height, a->hash) < std::tie(b->epoch, b->height, b->hash);
                });
                std::vector<CTransactionRef> all;
                std::set<std::pair<uint32_t, uint256>> lost_anchors;
                for (const LostBlk* b : order) {
                    lost_anchors.insert({b->anchor_h, b->anchor});
                    for (size_t i = 1; i < b->block->vtx.size(); ++i) {
                        if (!standing.txs_out.count(b->block->vtx[i]->GetHash())) all.push_back(b->block->vtx[i]);
                    }
                }
                LoadRecorded(tip);
                const auto bound_now = ChainBindings(tip, tip->m_anchor_height);
                CCoinsViewCache here(&chainman.ActiveChainstate().CoinsTip());
                const std::vector<std::string> verdicts = LostJudgeTxsInBlock(all, here, tip, tip->m_anchor_height, tip->m_anchor_hash, lost_anchors, bound_now);
                memo_valid.clear();
                for (size_t i = 0; i < all.size(); ++i) {
                    if (verdicts[i].empty()) memo_valid.insert(all[i]->GetHash());
                }
                memo_key = id;
            }
            evidence_valid = memo_valid;
        }
        std::map<COutPoint, std::vector<LostSpend>> spends;
        for (auto& [h, b] : g_blocks) {
            const bool was = b.prot;
            b.prot = prot.count(h) > 0;
            if (b.prot && !was) {
                LogPrintf("Lost blocks: %s at height %d (anchor %u, fork %d) is lost; its spends are bound until anchor %u\n",
                          h.ToString(), b.height, b.anchor_h, b.fork_height, b.anchor_h + g_window);
            }
            if (!b.prot || standing.blocks_out.count(h)) continue;
            for (size_t i = 1; i < b.block->vtx.size(); ++i) {
                const CTransaction& tx = *b.block->vtx[i];
                if (!b.own && (standing.txs_out.count(tx.GetHash()) || !evidence_valid.count(tx.GetHash()))) continue;
                for (size_t n = 0; n < tx.vin.size(); ++n) {
                    if (tx.vin[n].m_is_pegin) continue;
                    spends[tx.vin[n].prevout].push_back(LostSpend{tx.GetHash(), h, b.anchor_h, b.fork_hash, b.fork_height, b.height, LostProofLeaf(tx, n), b.own});
                }
            }
        }
        auto key = [](const std::map<COutPoint, std::vector<LostSpend>>& m) {
            std::vector<std::tuple<COutPoint, uint256, uint256, int, bool>> k;
            for (const auto& [o, v] : m) for (const auto& s : v) k.emplace_back(o, s.txid, s.fork_hash, s.fork_height, s.own);
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
        for (const auto& [h, a] : sanchors) sv[h] = BestChainVerdict(a.first, a.second);
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
                        spends[in.prevout].push_back(LostSpend{blk.vtx[i]->GetHash(), it->first, blk.m_anchor_height, blk.hashPrevBlock, it->second.height - 1, it->second.height});
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
    {
        LOCK(g_lost_mutex);
        if (g_dirty_file) WriteFile();
    }
    // Below the rule's height the store is kept, so that a loss just before it
    // is known when it starts, and nothing else happens: the mempool, the
    // templates and the relay are those of a node without the rule. The pass
    // that judges refused blocks again runs at the first height it applies,
    // because its generation has then not been seen.
    {
        LOCK(cs_main);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        g_next_height = tip ? tip->nHeight + 1 : 0;
    }
    if (!LostActiveAt(g_next_height.load())) return;
    const int64_t now = GetTime();
    uint64_t gen;
    bool any;
    {
        LOCK(g_lost_mutex);
        gen = g_gen;
        any = !g_spends.empty() || !g_signed_spends.empty() || !g_refused.empty() || !g_refused_scanned;
    }
    bool any_recorded;
    {
        LOCK(cs_main);
        any_recorded = !g_recorded.empty();
    }
    any = any || any_recorded;
    if (!any) return;
    // 5) The mempool: drop what conflicts, put lost transactions back, first in
    //    line for the next block whatever their fee.
    if (mempool && (changed || now - g_last_replay >= 2)) {
        g_last_replay = now;
        std::vector<CTransactionRef> lost_txs, evidence_txs;
        {
            LOCK(g_lost_mutex);
            std::vector<const LostBlk*> order;
            // Only what this node carries goes back first in line.
            for (const auto& [h, b] : g_blocks) if (b.prot && b.own) order.push_back(&b);
            std::sort(order.begin(), order.end(), [](const LostBlk* a, const LostBlk* b) { return a->height < b->height; });
            for (const LostBlk* b : order) for (size_t i = 1; i < b->block->vtx.size(); ++i) lost_txs.push_back(b->block->vtx[i]);
            // What stands of evidence-only blocks: the transactions that bind.
            std::set<uint256> standing_ids;
            for (const auto& [o, v] : g_spends) for (const LostSpend& sp : v) if (!sp.own) standing_ids.insert(sp.txid);
            for (const auto& [h, b] : g_blocks) {
                if (!b.prot || b.own) continue;
                for (size_t i = 1; i < b.block->vtx.size(); ++i) {
                    if (standing_ids.count(b.block->vtx[i]->GetHash())) evidence_txs.push_back(b.block->vtx[i]);
                }
            }
        }
        LOCK2(cs_main, mempool->cs);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        std::vector<CTransactionRef> conflicting;
        for (const auto& tx : lost_txs) {
            for (const CTxIn& in : tx->vin) {
                const CTransaction* c = mempool->GetConflictTx(in.prevout);
                if (c && c->GetHash() != tx->GetHash() && LostConflict(*c, tip, chainman.ActiveChainstate().CoinsTip())) {
                    conflicting.push_back(mempool->get(c->GetHash()));
                }
            }
        }
        // What entered before a record of this chain bound its input.
        if (tip) {
            for (const auto& [o, spend] : ChainBindings(tip, tip->m_anchor_height)) {
                const CTransaction* c = mempool->GetConflictTx(o);
                if (c && LostRecordedConflict(*c, tip, tip->m_anchor_height, chainman.ActiveChainstate().CoinsTip())) {
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
        // Evidence-only transactions go back under ordinary fee policy: no
        // priority and no duty. Each is offered once per tip.
        static uint256 offered_tip;
        static std::set<uint256> offered;
        if (tip && offered_tip != tip->GetBlockHash()) {
            offered_tip = tip->GetBlockHash();
            offered.clear();
        }
        for (const auto& tx : evidence_txs) {
            if (mempool->exists(GenTxid::Txid(tx->GetHash())) || !offered.insert(tx->GetHash()).second) continue;
            const MempoolAcceptResult r = AcceptToMemoryPool(chainman.ActiveChainstate(), tx, GetTime(), /*bypass_limits=*/false, /*test_accept=*/false);
            if (r.m_result_type == MempoolAcceptResult::ResultType::VALID) {
                LogPrintf("Lost blocks: evidence transaction %s is in the mempool under ordinary fee policy\n", tx->GetHash().ToString());
            }
        }
    }
    // The waiver: a function of the headers this node has seen, in any order.
    {
        LOCK2(cs_main, g_lost_mutex);
        // Before the first scan, g_refused does not yet hold the refusals of the
        // last run, and the headers kept above them must survive until it does.
        if (g_refused_scanned && g_blocks.empty() && g_refused.empty()) {
            if (!g_waiver_headers.empty()) g_dirty_file = true;
            g_waived.clear();
            g_waiver_headers.clear();
            g_waiver_children.clear();
            g_waiver_bytes = 0;
            g_sibling_capped.clear();
        } else if (g_waiver_dirty && !g_refused.empty()) {
            g_waiver_dirty = false;
            // Children by parent: those kept aside, and those the index took
            // before their ancestor was refused.
            std::map<uint256, std::vector<CBlockHeader>> kids;
            for (const auto& [h, hdr] : g_waiver_headers) kids[hdr.hashPrevBlock].push_back(hdr);
            int lowest = std::numeric_limits<int>::max();
            for (const uint256& root : g_refused) {
                if (const CBlockIndex* x = chainman.m_blockman.LookupBlockIndex(root)) lowest = std::min(lowest, x->nHeight);
            }
            for (const auto& [h, p] : chainman.m_blockman.m_block_index) {
                if (!p->pprev || p->nHeight <= lowest || !(p->nStatus & BLOCK_FAILED_MASK) || p->trimmed()) continue;
                if (!g_waiver_headers.count(h)) kids[p->pprev->GetBlockHash()].push_back(p->GetBlockHeader());
            }
            bool dropped = false;
            // The longest run of certified headers above `parent`, up to `need`,
            // under the registry at the branch's fork point.
            std::function<int(const uint256&, const CBlockIndex&, const uint256&, const StakeRegistry&, int)> run =
                [&](const uint256& parent, const CBlockIndex& view, const uint256& fork, const StakeRegistry& reg, int need) -> int {
                if (need <= 0) return 0;
                auto it = kids.find(parent);
                if (it == kids.end()) return 0;
                int best = 0;
                for (const CBlockHeader& hdr : it->second) {
                    const uint256 hash = hdr.GetHash();
                    std::string reason;
                    const int seats = CertSeatsCached(hdr, &view, fork, reg, reason);
                    if (seats < 0 || seats < PosSlotQuorumAt(reg, view.nHeight + 1)) {
                        // Junk does not keep a parent's slot.
                        if (g_waiver_headers.count(hash)) {
                            DropWaiverHeader(hash);
                            dropped = true;
                            g_dirty_file = true;
                        }
                        continue;
                    }
                    CBlockIndex next;
                    next.nHeight = view.nHeight + 1;
                    next.m_anchor_height = hdr.m_anchor_height;
                    next.m_anchor_hash = hdr.m_anchor_hash;
                    best = std::max(best, 1 + run(hash, next, fork, reg, need - 1));
                    if (best >= need) break;
                }
                return best;
            };
            for (const uint256& root : g_refused) {
                if (g_waived.count(root)) continue;
                const CBlockIndex* x = chainman.m_blockman.LookupBlockIndex(root);
                if (!x) continue;
                CBlockIndex view;
                view.nHeight = x->nHeight;
                view.m_anchor_height = x->m_anchor_height;
                view.m_anchor_hash = x->m_anchor_hash;
                // The registry the branch's first committee was drawn from, as
                // a record reads it: a key registered on the refused branch
                // counts for nothing here.
                const CBlockIndex* fork = chainman.ActiveChain().FindFork(x);
                const std::shared_ptr<const StakeRegistry> at_fork = fork ? RegistryAt(fork, chainman.ActiveChain().Tip()) : nullptr;
                if (!at_fork) {
                    g_waiver_dirty = true; // this node's own read failed: next pass
                    continue;
                }
                const int depth = run(root, view, fork->GetBlockHash(), *at_fork, g_waiver_depth);
                if (depth >= g_waiver_depth) {
                    LogPrintf("Lost blocks: refused block %s has %d certified block(s) above it; following the quorum\n", root.ToString(), depth);
                    g_waived.insert(root);
                    changed = true;
                }
            }
            // A header no refused block roots any more holds nothing. Not before
            // the first scan: the refusals of the last run are not all back yet.
            std::set<uint256> rooted;
            std::vector<uint256> walk(g_refused.begin(), g_refused.end());
            while (!walk.empty()) {
                const uint256 at = walk.back();
                walk.pop_back();
                auto it = kids.find(at);
                if (it == kids.end()) continue;
                for (const CBlockHeader& hdr : it->second) {
                    const uint256 hash = hdr.GetHash();
                    if (rooted.insert(hash).second) walk.push_back(hash);
                }
            }
            for (auto it = g_waiver_headers.begin(); g_refused_scanned && it != g_waiver_headers.end();) {
                const uint256 hash = it->first;
                ++it;
                if (!rooted.count(hash)) {
                    DropWaiverHeader(hash);
                    g_dirty_file = true;
                }
            }
            // A slot freed: ask again, in case a real header was turned away.
            if (dropped) ++g_anchor_refetch_epoch;
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

void LostNoteTipConnected(const CBlockIndex* tip)
{
    g_next_height = tip->nHeight + 1;
    if (!LostActiveAt(tip->nHeight + 1)) return;
    bool pending;
    {
        LOCK(g_lost_mutex);
        pending = !g_spends.empty();
    }
    if (pending) g_anchor_watch_wake = true;
}

UniValue LostGetInfo(int next_height)
{
    UniValue out(UniValue::VOBJ);
    LOCK(g_lost_mutex);
    out.pushKV("active", LostActiveAt(next_height));
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
    out.pushKV("waiverheaders", (int)g_waiver_headers.size());
    return out;
}

std::optional<LostEvidenceMsg> LostGetEvidence(const uint256& hash, bool& incomplete)
{
    incomplete = false;
    LOCK(g_lost_mutex);
    auto it = g_blocks.find(hash);
    if (it == g_blocks.end()) return std::nullopt;
    if (!EvidenceComplete(it->second)) {
        incomplete = true;
        return std::nullopt;
    }
    return LostEvidenceMsg{*it->second.block, it->second.btc};
}
