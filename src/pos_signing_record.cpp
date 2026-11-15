// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_signing_record.h>

#include <clientversion.h>
#include <fs.h>
#include <hash.h>
#include <logging.h>
#include <pos.h>
#include <primitives/block.h>
#include <shutdown.h>
#include <streams.h>
#include <util/system.h>
#include <util/time.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#ifndef WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

const char* const POS_SIGNING_RECORD_WRITE_ERROR = "cannot write the signing record";

namespace {

//! Each entry on disk: magic, payload length, payload, the payload's checksum.
constexpr uint32_t FRAME_MAGIC = 0x31524753; // "SGR1"
constexpr uint32_t MAX_PAYLOAD = 32 * 1024 * 1024;

struct Stored {
    PosSigningEntry entry;
    uint64_t bytes{0};
    bool dropped{false};
};

//! A block the signing check holds: one per block hash, for every live entry
//! that locks its height (PosSigningEntry::Locks).
struct Held {
    int height{0};
    uint32_t anchor_height{0};
    uint256 anchor_hash;
    bool from_disk{false};       //!< loaded at the start, not signed in this run
    int64_t grace_start_ms{0};   //!< when its escape's round began (0: not begun)
    bool queried{false};         //!< its certificate was asked for in this grace
    bool escaped{false};         //!< the round passed with no certificate while its anchor stood
    bool certified{false};       //!< a certificate for it reached this node
    bool invalidated{false};     //!< the operator invalidated it (or an ancestor) on this node
    std::string state{"not judged"};
};

Mutex g_mutex;
std::vector<Stored> g_entries GUARDED_BY(g_mutex);
std::map<int, std::map<uint256, Held>> g_held GUARDED_BY(g_mutex);
uint256 g_genesis GUARDED_BY(g_mutex);
PosSigningDamagedHold g_hold GUARDED_BY(g_mutex);
std::set<int> g_certified_heights GUARDED_BY(g_mutex);
PosAnchorJudge g_judge GUARDED_BY(g_mutex);
std::function<void(const uint256&)> g_query GUARDED_BY(g_mutex);
std::function<int()> g_peers GUARDED_BY(g_mutex);
uint64_t g_file_bytes GUARDED_BY(g_mutex){0};
uint64_t g_dropped_bytes GUARDED_BY(g_mutex){0};
bool g_failed GUARDED_BY(g_mutex){false};
PosSigningRecordStats g_stats GUARDED_BY(g_mutex);

fs::path RecordPath() { return gArgs.GetDataDirNet() / "possigned.dat"; }
fs::path RecordTmpPath() { return gArgs.GetDataDirNet() / "possigned.dat.new"; }

uint32_t Checksum(Span<const std::byte> payload)
{
    const uint256 h = Hash(payload);
    uint32_t c;
    std::memcpy(&c, h.begin(), 4);
    return c;
}

std::vector<std::byte> Frame(const PosSigningEntry& entry)
{
    CDataStream payload(SER_DISK, CLIENT_VERSION);
    payload << entry;
    CDataStream out(SER_DISK, CLIENT_VERSION);
    out << FRAME_MAGIC << (uint32_t)payload.size();
    out.write(MakeByteSpan(payload));
    out << Checksum(MakeByteSpan(payload));
    return std::vector<std::byte>(MakeByteSpan(out).begin(), MakeByteSpan(out).end());
}

bool WriteAll(FILE* f, const std::vector<std::byte>& data)
{
    return data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
}

PosSigningEntry ChainEntry(const uint256& genesis)
{
    PosSigningEntry e;
    e.kind = (uint8_t)PosSignKind::CHAIN;
    e.block_hash = genesis;
    e.time = GetTime();
    return e;
}

//! Sync a directory, so that a file created or renamed in it survives a power
//! loss. Every step is checked: a directory that cannot be synced means the
//! record may not be durable, and no signature may leave on its strength.
bool SyncDirectory(const fs::path& dir, std::string& why)
{
#ifndef WIN32
    const int fd = open(fs::PathToString(dir).c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        why = strprintf("cannot open the directory %s (%s)", fs::PathToString(dir), std::strerror(errno));
        return false;
    }
    // EINVAL: the file system cannot sync a directory at all (as FileCommit).
    if (fsync(fd) != 0 && errno != EINVAL) {
        why = strprintf("cannot sync the directory %s (%s)", fs::PathToString(dir), std::strerror(errno));
        close(fd);
        return false;
    }
    if (close(fd) != 0) {
        why = strprintf("cannot close the directory %s (%s)", fs::PathToString(dir), std::strerror(errno));
        return false;
    }
#endif
    return true;
}

//! Write every live entry (and `extra`, if any) to a new file and rename it
//! over the record. g_mutex held.
bool RewriteLocked(const std::vector<std::byte>* extra, std::string& why) EXCLUSIVE_LOCKS_REQUIRED(g_mutex)
{
    const fs::path tmp = RecordTmpPath();
    FILE* f = fsbridge::fopen(tmp, "wb");
    if (!f) {
        why = "cannot open " + fs::PathToString(tmp);
        return false;
    }
    const std::vector<std::byte> head = Frame(ChainEntry(g_genesis));
    uint64_t bytes = head.size();
    bool ok = WriteAll(f, head);
    for (const Stored& s : g_entries) {
        if (s.dropped) continue;
        const std::vector<std::byte> fr = Frame(s.entry);
        ok = ok && WriteAll(f, fr);
        bytes += fr.size();
    }
    if (extra) {
        ok = ok && WriteAll(f, *extra);
        bytes += extra->size();
    }
    if (!ok) {
        fclose(f);
        why = "cannot write " + fs::PathToString(tmp);
        return false;
    }
    if (!FileCommit(f)) {
        fclose(f);
        why = "cannot sync " + fs::PathToString(tmp);
        return false;
    }
    if (fclose(f) != 0) {
        why = "cannot close " + fs::PathToString(tmp);
        return false;
    }
    if (!RenameOver(tmp, RecordPath())) {
        why = "cannot rename " + fs::PathToString(tmp);
        return false;
    }
    if (!SyncDirectory(gArgs.GetDataDirNet(), why)) return false;
    std::vector<Stored> live;
    for (Stored& s : g_entries) {
        if (!s.dropped) live.push_back(std::move(s));
    }
    g_entries = std::move(live);
    g_file_bytes = bytes - (extra ? extra->size() : 0);
    g_dropped_bytes = 0;
    ++g_stats.rewrites;
    return true;
}

void Fail(const std::string& why) EXCLUSIVE_LOCKS_REQUIRED(g_mutex)
{
    g_failed = true;
    LogPrintf("Signing record: %s; this member signs nothing more\n", why);
}

int64_t SteadyUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

//! Hold `e`'s block in the signing check (it locks its height).
void HoldEntry(const PosSigningEntry& e, bool from_disk) EXCLUSIVE_LOCKS_REQUIRED(g_mutex)
{
    if (!e.Locks()) return;
    auto& at = g_held[e.height];
    auto it = at.find(e.block_hash);
    if (it == at.end()) {
        Held h;
        h.height = e.height;
        h.anchor_height = e.anchor_height;
        h.anchor_hash = e.anchor_hash;
        h.from_disk = from_disk;
        at.emplace(e.block_hash, h);
    } else if (!from_disk) {
        it->second.from_disk = false; // signed again in this run
    }
}

//! The blocks the record's live entries still hold; forget the others.
void PruneHeld() EXCLUSIVE_LOCKS_REQUIRED(g_mutex)
{
    std::set<uint256> live;
    for (const Stored& s : g_entries) {
        if (!s.dropped && s.entry.Locks()) live.insert(s.entry.block_hash);
    }
    for (auto h = g_held.begin(); h != g_held.end();) {
        for (auto b = h->second.begin(); b != h->second.end();) {
            b = live.count(b->first) ? std::next(b) : h->second.erase(b);
        }
        h = h->second.empty() ? g_held.erase(h) : std::next(h);
    }
}

//! Judge an anchor; g_mutex NOT held (the judge may ask the parent daemon).
PosAnchorStanding Judge(const PosAnchorJudge& judge, const Held& h, bool& ignored)
{
    ignored = false;
    if (!g_con_bitcoin_anchor || h.anchor_hash.IsNull()) return PosAnchorStanding::STANDS;
    const std::optional<PosAnchorStanding> v = judge ? judge(h.anchor_height, h.anchor_hash) : std::nullopt;
    if (!v) {
        // No watcher: a block signed in this run stands, as the share-lock
        // always held it; one loaded from the disk binds nothing, since this
        // node cannot tell whether Bitcoin has since replaced its anchor.
        ignored = h.from_disk;
        return PosAnchorStanding::STANDS;
    }
    return *v;
}

//! End the damaged-entry hold: drop its stand-in entries and rewrite the file
//! without them, so that a later start does not hold again.
void EndDamagedHold(const char* how) EXCLUSIVE_LOCKS_REQUIRED(g_mutex)
{
    if (!g_hold.active) return;
    LogPrintf("Signing record: the hold for an unreadable entry ends at height %d: %s\n", g_hold.height, how);
    g_hold.active = false;
    for (Stored& s : g_entries) {
        if (!s.dropped && s.entry.kind == (uint8_t)PosSignKind::UNREADABLE) {
            s.dropped = true;
            g_dropped_bytes += s.bytes;
        }
    }
    std::string why;
    if (!RewriteLocked(nullptr, why)) Fail(why);
}

//! The damaged-entry hold's part of the check, for a binding signature at
//! `height` (-1: unknown). Returns a refusal, or nothing.
std::optional<std::string> DamagedHoldRefuses(int height, int64_t now_ms) EXCLUSIVE_LOCKS_REQUIRED(g_mutex)
{
    if (!g_hold.active) return std::nullopt;
    if (height < 0) {
        return std::string("this member's signing record ended with an unreadable entry, whose height is unknown, so it "
                           "cannot tell where a block given by hash alone would stand: give the block");
    }
    if (height < g_hold.floor) return std::nullopt;
    if (g_hold.height < 0) {
        g_hold.height = height;
        LogPrintf("Signing record: holding height %d for an unreadable entry: signing nothing there until it is "
                  "certified, passed, or %d ms with a peer pass\n", height, (int)g_hold.bound_ms);
    }
    if (height > g_hold.height) {
        EndDamagedHold("a signature is asked at a greater height");
        return std::nullopt;
    }
    if (height < g_hold.height) return std::nullopt;
    if (g_certified_heights.count(height)) {
        EndDamagedHold("a certificate at that height reached this node");
        return std::nullopt;
    }
    if (g_hold.bound_start_ms == 0 && g_peers && g_peers() > 0) g_hold.bound_start_ms = now_ms;
    if (g_hold.bound_start_ms != 0 && now_ms - g_hold.bound_start_ms >= g_hold.bound_ms) {
        EndDamagedHold("one window and two rounds passed with a peer and no certificate");
        return std::nullopt;
    }
    return strprintf("this member's signing record ended with an unreadable entry that may name a block it signed at "
                     "height %d; it signs nothing there until that height is certified or passed, or for %d ms with a peer",
                     height, (int)g_hold.bound_ms);
}

PosSignCheck RunCheck(const PosSigningEntry& entry, int64_t now_ms, int tip_height, const uint256& tip_hash, std::vector<uint256>& queries)
{
    PosSignCheck out;
    out.height = entry.height;
    // A proposal binds nothing (a proposal certifies only with the shares of
    // others, who each run this check), and nothing else is checked for it.
    if (entry.kind == (uint8_t)PosSignKind::PROPOSAL) return out;

    PosAnchorJudge judge;
    std::vector<std::pair<uint256, Held>> held;
    {
        LOCK(g_mutex);
        if (g_failed) {
            out.ok = false;
            out.why = POS_SIGNING_RECORD_WRITE_ERROR;
            return out;
        }
        if (auto refusal = DamagedHoldRefuses(entry.height, now_ms)) {
            out.ok = false;
            out.why = *refusal;
            return out;
        }
        judge = g_judge;
        if (entry.height >= 0) {
            auto at = g_held.find(entry.height);
            if (at != g_held.end()) {
                for (const auto& [hash, h] : at->second) {
                    if (hash != entry.block_hash) held.emplace_back(hash, h);
                }
            }
        } else {
            // A bare hash: the block's height is unknown, so it could be a
            // rival of any block this member signed that is not settled.
            for (const auto& [height, at] : g_held) {
                if (height < tip_height) continue;
                for (const auto& [hash, h] : at) {
                    if (hash != entry.block_hash && !(height == tip_height && hash == tip_hash)) held.emplace_back(hash, h);
                }
            }
        }
    }
    std::vector<std::pair<PosAnchorStanding, bool>> verdicts;
    for (const auto& [hash, h] : held) {
        bool ignored;
        const PosAnchorStanding v = Judge(judge, h, ignored);
        verdicts.emplace_back(v, ignored);
    }
    const int64_t round_ms = PosSigningRoundMs();
    LOCK(g_mutex);
    for (size_t i = 0; i < held.size(); ++i) {
        auto at = g_held.find(held[i].second.height);
        if (at == g_held.end()) continue;
        auto it = at->second.find(held[i].first);
        if (it == at->second.end()) continue;
        Held& h = it->second;
        const auto [v, ignored] = verdicts[i];
        if (ignored) {
            h.state = "not judged";
            continue;
        }
        if (v == PosAnchorStanding::GONE) {
            if (h.state != "anchor gone") {
                LogPrintf("Signing record: signed %s at height %d; Bitcoin replaced its anchor %s at %u, so it binds nothing\n",
                          held[i].first.GetHex(), h.height, h.anchor_hash.GetHex(), h.anchor_height);
            }
            h.state = "anchor gone";
            h.escaped = false;
            h.grace_start_ms = 0;
            h.queried = false;
            continue;
        }
        // Every block that binds is judged and has its grace started, so that
        // they all escape together; the first refusal is the one returned.
        const auto refuse = [&](std::string why) {
            if (!out.ok) return;
            out.ok = false;
            out.bound = held[i].first;
            out.height = h.height;
            out.why = std::move(why);
        };
        if (entry.height < 0) {
            if (v == PosAnchorStanding::STANDS && h.escaped && !h.certified) continue;
            refuse(strprintf("msg is a block this node does not know, and this member signed %s at height %d, which "
                             "may still be certified: give the block (the block argument) so that the signing "
                             "record can check its height", held[i].first.GetHex(), h.height));
            continue;
        }
        if (v == PosAnchorStanding::UNDECIDED) {
            if (h.state != "undecided") {
                LogPrintf("Signing record: holding height %d: signed %s there; waiting for the parent chain to judge its anchor %s at %u\n",
                          h.height, held[i].first.GetHex(), h.anchor_hash.GetHex(), h.anchor_height);
            }
            h.state = "undecided";
            refuse(strprintf("this member signed %s at height %d, and the parent chain's daemon has not judged its "
                             "Bitcoin anchor %s at %u: it signs nothing else at that height until it does",
                             held[i].first.GetHex(), h.height, h.anchor_hash.GetHex(), h.anchor_height));
            continue;
        }
        if (h.certified && !h.invalidated) {
            h.state = "certified";
            refuse(strprintf("this member signed %s at height %d, and that block is certified", held[i].first.GetHex(), h.height));
            continue;
        }
        if (h.escaped) {
            h.state = "escaped";
            continue;
        }
        h.state = "binds";
        if (h.grace_start_ms == 0) {
            h.grace_start_ms = now_ms;
            h.queried = false;
        }
        if (!h.queried) {
            h.queried = true;
            queries.push_back(held[i].first);
        }
        if (now_ms - h.grace_start_ms >= round_ms) {
            LogPrintf("Signing record: no certificate for %s at height %d within a round; it no longer holds the height\n",
                      held[i].first.GetHex(), h.height);
            if (h.certified) {
                LogPrintf("Signing record: this member is about to sign a rival of %s at height %d, a block it holds a "
                          "full certificate for, because that block was invalidated on this node; invalidating a "
                          "certified block is the operator's responsibility\n",
                          held[i].first.GetHex(), h.height);
            }
            h.escaped = true;
            h.state = "escaped";
            continue;
        }
        refuse(strprintf("this member signed %s at height %d, and its Bitcoin anchor stands: it asked its peers for "
                         "that block's certificate and signs no other block there for %d ms more",
                         held[i].first.GetHex(), h.height, (int)(h.grace_start_ms + round_ms - now_ms)));
    }
    if (out.ok) {
        out.bound.SetNull();
        out.height = entry.height;
    }
    return out;
}

void SendQueries(const std::vector<uint256>& hashes)
{
    if (hashes.empty()) return;
    std::function<void(const uint256&)> query;
    {
        LOCK(g_mutex);
        query = g_query;
    }
    for (const uint256& h : hashes) {
        LogPrintf("PoS gossip: share-lock at %s — querying peers for a certificate\n", h.GetHex());
        if (query) query(h);
    }
}

std::mutex g_gate_mutex;

} // namespace

bool PosSigningEntry::Locks() const
{
    if (height < 0) return false;
    return kind == (uint8_t)PosSignKind::SHARE || kind == (uint8_t)PosSignKind::CERTIFIED ||
           kind == (uint8_t)PosSignKind::LEADER || kind == (uint8_t)PosSignKind::PARTIAL ||
           kind == (uint8_t)PosSignKind::RECERTIFY;
}

PosSigningEntry PosSigningEntryFor(PosSignKind kind, const CBlock& block, int height, std::vector<CPubKey> signers)
{
    PosSigningEntry e;
    e.kind = (uint8_t)kind;
    e.block_hash = block.GetHash();
    e.height = height;
    e.parent_hash = block.hashPrevBlock;
    e.anchor_height = block.m_anchor_height;
    e.anchor_hash = block.m_anchor_hash;
    e.time = GetTime();
    e.signers = std::move(signers);
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const uint256 txid = block.vtx[i]->GetHash();
        for (const CTxIn& in : block.vtx[i]->vin) e.spends.emplace_back(in.prevout, txid);
    }
    return e;
}

int64_t PosSigningRoundMs()
{
    const int64_t o = std::clamp<int64_t>(gArgs.GetIntArg("-posroundms", 0), 0, 60000);
    return o > 0 ? o : 700 + 35 * (int64_t)g_pos_committee_size;
}

int64_t PosSigningWindowMs()
{
    const int64_t o = std::clamp<int64_t>(gArgs.GetIntArg("-poswindowms", 0), 0, 60000);
    return o > 0 ? o : 500 + 25 * (int64_t)g_pos_committee_size;
}

bool PosSigningRecordLoad(const uint256& genesis, std::string& error)
{
    LOCK(g_mutex);
    g_entries.clear();
    g_held.clear();
    g_certified_heights.clear();
    g_hold = PosSigningDamagedHold{};
    g_genesis = genesis;
    g_file_bytes = 0;
    g_dropped_bytes = 0;
    const fs::path path = RecordPath();
    FILE* f = fsbridge::fopen(path, "rb");
    if (!f) {
        if (fs::exists(path)) {
            error = "cannot open the signing record " + fs::PathToString(path);
            return false;
        }
        return true;
    }
    std::vector<std::byte> data;
    {
        std::byte buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
        const bool bad = ferror(f);
        fclose(f);
        if (bad) {
            error = "cannot read the signing record " + fs::PathToString(path);
            return false;
        }
    }
    const std::string where = fs::PathToString(path);
    const auto magic_at = [&](size_t at) {
        uint32_t m;
        std::memcpy(&m, &data[at], 4);
        return m;
    };
    // A frame mark of a later format: "SGR" followed by a digit above 1.
    const auto newer = [](uint32_t m) {
        const uint8_t v = m >> 24;
        return (m & 0x00FFFFFF) == (FRAME_MAGIC & 0x00FFFFFF) && v > '1' && v <= '9';
    };
    size_t pos = 0;
    bool torn = false;
    bool damaged = false;
    uint64_t damaged_bytes = 0;
    bool chain_seen = false;
    while (pos < data.size()) {
        const size_t left = data.size() - pos;
        uint32_t magic = 0, len = 0, sum = 0;
        if (left >= 4) magic = magic_at(pos);
        if (left >= 4 && newer(magic)) {
            error = strprintf("the signing record %s was written by a newer version of this software (entry format %c). "
                              "Start that version again; or, once every height this member signed is certified and "
                              "buried, move the file aside to start without it", where, (char)(magic >> 24));
            return false;
        }
        if (left >= 8) std::memcpy(&len, &data[pos + 4], 4);
        // Cut short: fewer bytes than any whole entry, or a frame whose length
        // runs past the end of the file. A write that never completed: its
        // signature never left the process (a signature leaves only after its
        // entry is synced), and nothing can follow it.
        if (left < 8 || (magic == FRAME_MAGIC && len <= MAX_PAYLOAD && left < 12 + (size_t)len)) {
            torn = true;
            LogPrintf("Signing record: dropping a last entry cut short (%u bytes): a write that never completed\n",
                      (unsigned)left);
            break;
        }
        bool whole = magic == FRAME_MAGIC && len <= MAX_PAYLOAD;
        bool good = false;
        PosSigningEntry e;
        if (whole) {
            std::memcpy(&sum, &data[pos + 8 + len], 4);
            const Span<const std::byte> payload(&data[pos + 8], len);
            if (Checksum(payload) == sum) {
                try {
                    CDataStream st(payload, SER_DISK, CLIENT_VERSION);
                    st >> e;
                    good = st.empty();
                } catch (const std::exception&) {
                    good = false;
                }
            }
        }
        if (!good) {
            // Whole in length but unreadable. As the last entry it is either
            // an append whose size reached the disk before its contents (its
            // signature never left) or a synced entry that rotted (it may
            // have): the node starts and holds (PosSigningDamagedHold).
            // Anything readable after it means the file is damaged inside.
            bool last;
            if (whole) {
                last = pos + 12 + (size_t)len == data.size();
            } else {
                last = true;
                for (size_t at = pos + 1; at + 4 <= data.size(); ++at) {
                    if (magic_at(at) == FRAME_MAGIC) {
                        last = false;
                        break;
                    }
                }
            }
            if (!last) {
                error = strprintf("the signing record %s is damaged at byte %u. Once every height this member signed is "
                                  "certified and buried, move the file aside to start without it", where, (unsigned)pos);
                return false;
            }
            damaged = true;
            damaged_bytes = left;
            break;
        }
        if (!chain_seen) {
            if (e.kind != (uint8_t)PosSignKind::CHAIN) {
                error = strprintf("the signing record %s names no chain. Move it aside if it is not this chain's; if it "
                                  "is, start without it only once every height this member signed is certified and buried", where);
                return false;
            }
            if (e.block_hash != genesis) {
                error = strprintf("the signing record %s belongs to another chain (genesis %s; this node's is %s). It "
                                  "holds nothing here: move it aside, or start this node with that chain's data directory",
                                  where, e.block_hash.GetHex(), genesis.GetHex());
                return false;
            }
            chain_seen = true;
            g_file_bytes += 12 + len;
            pos += 12 + (size_t)len;
            continue;
        }
        if (e.kind == (uint8_t)PosSignKind::CHAIN) {
            error = strprintf("the signing record %s is damaged at byte %u (a second chain entry). Once every height this "
                              "member signed is certified and buried, move the file aside to start without it", where, (unsigned)pos);
            return false;
        }
        Stored st;
        st.entry = std::move(e);
        st.bytes = 12 + len;
        g_file_bytes += st.bytes;
        g_entries.push_back(std::move(st));
        pos += 12 + (size_t)len;
    }
    if (!chain_seen && !g_entries.empty()) {
        error = "the signing record " + where + " names no chain";
        return false;
    }
    if (damaged) {
        // Keep the file as it was beside the record, for the operator.
        std::string copy = where + ".unreadable-" + strprintf("%d", GetTime());
        try {
            fs::copy_file(path, fs::PathFromString(copy), fs::copy_options::overwrite_existing);
        } catch (const std::exception& ex) {
            error = strprintf("cannot keep a copy of the signing record %s before rewriting it: %s", where, ex.what());
            return false;
        }
        int floor = 0;
        for (const Stored& st : g_entries) floor = std::max(floor, (int)st.entry.height);
        PosSigningEntry stand_in;
        stand_in.kind = (uint8_t)PosSignKind::UNREADABLE;
        stand_in.height = floor;
        stand_in.time = GetTime();
        Stored st;
        st.entry = stand_in;
        st.bytes = Frame(stand_in).size();
        g_entries.push_back(std::move(st));
        LogPrintf("Signing record: the last entry (%u bytes) is whole but unreadable; a copy of the file is kept as %s. "
                  "This member signs nothing at the first height from %d it is asked to sign at, until that height is "
                  "certified or passed, or for one window and two rounds with a peer\n", (unsigned)damaged_bytes, copy, floor);
        g_hold.copy = copy;
        g_hold.bytes = damaged_bytes;
    }
    // A hold for an unreadable entry, found now or at an earlier start.
    for (const Stored& st : g_entries) {
        if (st.entry.kind != (uint8_t)PosSignKind::UNREADABLE) continue;
        g_hold.active = true;
        g_hold.floor = std::max(g_hold.floor, (int)st.entry.height);
    }
    if (g_hold.active) {
        g_hold.bound_ms = PosSigningWindowMs() + 2 * PosSigningRoundMs();
        if (!damaged) {
            LogPrintf("Signing record: an unreadable entry found at an earlier start still holds: this member signs "
                      "nothing at the first height from %d it is asked to sign at, until that height is certified or "
                      "passed, or for %d ms with a peer\n", g_hold.floor, (int)g_hold.bound_ms);
        }
    }
    for (const Stored& st : g_entries) HoldEntry(st.entry, /*from_disk=*/true);
    LogPrintf("Signing record: %u entries loaded, %u height(s) signed\n", (unsigned)g_entries.size(), (unsigned)g_held.size());
    if (torn || damaged || (!chain_seen && !data.empty())) {
        // Rewrite now, so that no later entry is appended after the bad one.
        std::string why;
        if (!RewriteLocked(nullptr, why)) {
            error = "cannot rewrite the signing record: " + why;
            return false;
        }
    }
    return true;
}

bool PosSigningRecordAppend(const PosSigningEntry& entry)
{
    bool abort = false;
    {
        LOCK(g_mutex);
        if (g_failed) return false;
        const int64_t t0 = SteadyUs();
        const std::vector<std::byte> fr = Frame(entry);
        const fs::path path = RecordPath();
        std::string why;
        bool ok;
        if (!fs::exists(path) || g_file_bytes == 0) {
            // A new file (or one removed under the node): write the chain
            // entry and every entry kept in memory with this one.
            ok = RewriteLocked(&fr, why);
        } else {
            FILE* f = fsbridge::fopen(path, "ab");
            ok = f != nullptr;
            if (!ok) why = "cannot open " + fs::PathToString(path);
            if (ok && !WriteAll(f, fr)) {
                ok = false;
                why = "cannot write " + fs::PathToString(path);
            }
            if (ok && !FileCommit(f)) {
                ok = false;
                why = "cannot sync " + fs::PathToString(path);
            }
            if (f && fclose(f) != 0 && ok) {
                ok = false;
                why = "cannot close " + fs::PathToString(path);
            }
        }
        if (!ok) {
            Fail(why);
            abort = true;
        } else {
            Stored s;
            s.entry = entry;
            s.bytes = fr.size();
            g_file_bytes += s.bytes;
            g_entries.push_back(std::move(s));
            HoldEntry(entry, /*from_disk=*/false);
            const int64_t us = SteadyUs() - t0;
            ++g_stats.appends;
            g_stats.last_append_us = us;
            g_stats.max_append_us = std::max(g_stats.max_append_us, us);
            g_stats.total_append_us += us;
            LogPrint(BCLog::VALIDATION, "Signing record: kind %d, block %s at height %d, %u bytes, synced in %d us\n",
                     (int)entry.kind, entry.block_hash.GetHex(), entry.height, (unsigned)fr.size(), (int)us);
        }
    }
    if (abort) {
        AbortNode("Failed to write the signing record");
        return false;
    }
    return true;
}

void PosSigningSetAnchorJudge(PosAnchorJudge judge)
{
    LOCK(g_mutex);
    g_judge = std::move(judge);
}

void PosSigningSetNetwork(std::function<void(const uint256&)> query_certificate, std::function<int()> peer_count)
{
    LOCK(g_mutex);
    g_query = std::move(query_certificate);
    g_peers = std::move(peer_count);
}

void PosSigningParentMoved(int unchanged_height)
{
    LOCK(g_mutex);
    for (auto& [height, at] : g_held) {
        for (auto& [hash, h] : at) {
            if (unchanged_height >= 0 && h.anchor_height <= (uint32_t)unchanged_height) continue;
            if (h.escaped || h.grace_start_ms != 0) {
                LogPrintf("Signing record: Bitcoin reorganized below the anchor of %s at height %d; any escape it had is void\n",
                          hash.GetHex(), height);
            }
            h.escaped = false;
            h.grace_start_ms = 0;
            h.queried = false;
            h.state = "not judged";
        }
    }
}

void PosSigningNoteCertificate(const uint256& block_hash, int height)
{
    LOCK(g_mutex);
    g_certified_heights.insert(height);
    while (g_certified_heights.size() > 1000) g_certified_heights.erase(g_certified_heights.begin());
    auto at = g_held.find(height);
    if (at == g_held.end()) return;
    auto it = at->second.find(block_hash);
    if (it != at->second.end()) it->second.certified = true;
}

void PosSigningNoteInvalidated(const std::vector<uint256>& hashes, bool invalidated)
{
    LOCK(g_mutex);
    for (auto& [height, at] : g_held) {
        for (auto& [hash, h] : at) {
            if (std::find(hashes.begin(), hashes.end(), hash) == hashes.end()) continue;
            h.invalidated = invalidated;
            // The release counts from the invalidation: one new query and round.
            h.escaped = false;
            h.grace_start_ms = 0;
            h.queried = false;
        }
    }
}

PosSignCheck PosSigningCheckNow(const PosSigningEntry& entry, int64_t now_ms, int tip_height, const uint256& tip_hash)
{
    std::vector<uint256> queries;
    PosSignCheck out = RunCheck(entry, now_ms, tip_height, tip_hash, queries);
    SendQueries(queries);
    return out;
}

PosSigningGate::PosSigningGate(PosSigningEntry entry, int64_t now_ms, int tip_height, const uint256& tip_hash)
    : m_entry(std::move(entry))
{
    g_gate_mutex.lock();
    m_locked = true;
    m_check = PosSigningCheckNow(m_entry, now_ms, tip_height, tip_hash);
    if (!m_check.ok) {
        LogPrint(BCLog::VALIDATION, "Signing record: not signing %s at height %d: %s\n", m_entry.block_hash.GetHex(),
                 m_entry.height, m_check.why);
    }
}

PosSigningGate::~PosSigningGate()
{
    if (m_locked) g_gate_mutex.unlock();
}

bool PosSigningGate::Record()
{
    const bool ok = m_check.ok && PosSigningRecordAppend(m_entry);
    if (m_locked) {
        m_locked = false;
        g_gate_mutex.unlock();
    }
    return ok;
}

std::optional<PosBoundBlock> PosSigningBoundAt(int height, const uint256& except, int64_t now_ms)
{
    PosAnchorJudge judge;
    std::vector<std::pair<uint256, Held>> held;
    {
        LOCK(g_mutex);
        judge = g_judge;
        auto at = g_held.find(height);
        if (at == g_held.end()) return std::nullopt;
        for (const auto& [hash, h] : at->second) {
            if (hash != except) held.emplace_back(hash, h);
        }
    }
    std::vector<std::pair<PosAnchorStanding, bool>> verdicts;
    for (const auto& [hash, h] : held) {
        bool ignored;
        const PosAnchorStanding v = Judge(judge, h, ignored);
        verdicts.emplace_back(v, ignored);
    }
    std::optional<PosBoundBlock> out;
    std::vector<uint256> queries;
    {
        LOCK(g_mutex);
        for (size_t i = 0; i < held.size() && !out; ++i) {
            auto at = g_held.find(height);
            if (at == g_held.end()) break;
            auto it = at->second.find(held[i].first);
            if (it == at->second.end()) continue;
            Held& h = it->second;
            const auto [v, ignored] = verdicts[i];
            if (ignored) continue;
            if (v == PosAnchorStanding::GONE) {
                h.state = "anchor gone";
                h.escaped = false;
                h.grace_start_ms = 0;
                h.queried = false;
                continue;
            }
            if (h.escaped && v == PosAnchorStanding::STANDS) continue;
            PosBoundBlock b;
            b.hash = held[i].first;
            b.undecided = v == PosAnchorStanding::UNDECIDED;
            b.certified = h.certified;
            if (h.grace_start_ms == 0) {
                h.grace_start_ms = now_ms;
                h.queried = false;
            }
            if (!h.queried) {
                h.queried = true;
                queries.push_back(b.hash);
            }
            b.grace_start_ms = h.grace_start_ms;
            out = b;
        }
    }
    SendQueries(queries);
    return out;
}

bool PosSigningSignedAt(int height)
{
    PosAnchorJudge judge;
    std::vector<Held> held;
    {
        LOCK(g_mutex);
        if (g_hold.active && height >= g_hold.floor && (g_hold.height < 0 || g_hold.height == height)) return true;
        judge = g_judge;
        auto at = g_held.find(height);
        if (at == g_held.end()) return false;
        for (const auto& [hash, h] : at->second) held.push_back(h);
    }
    for (const Held& h : held) {
        bool ignored;
        if (Judge(judge, h, ignored) != PosAnchorStanding::GONE && !ignored) return true;
    }
    return false;
}

std::vector<uint256> PosSigningRestartGrace(int height, int64_t now_ms)
{
    std::vector<uint256> out;
    LOCK(g_mutex);
    auto at = g_held.find(height);
    if (at == g_held.end()) return out;
    for (auto& [hash, h] : at->second) {
        if (h.escaped || h.state == "anchor gone") continue;
        h.grace_start_ms = now_ms;
        // Asked about now only if judged to bind; one not judged since Bitcoin
        // last moved is asked about by the next check that finds it standing.
        h.queried = h.state == "binds" || h.state == "undecided";
        if (h.queried) out.push_back(hash);
    }
    return out;
}

void PosSigningSendQueries(const std::vector<uint256>& hashes) { SendQueries(hashes); }

PosSigningDamagedHold PosSigningGetDamagedHold()
{
    LOCK(g_mutex);
    return g_hold;
}

void PosSigningRecordIndexWritten(int tip_height, uint32_t tip_anchor_height, const PosActiveAt& active_at)
{
    bool abort = false;
    {
        LOCK(g_mutex);
        if (g_failed || g_entries.empty()) return;
        const uint64_t ref = g_con_bitcoin_anchor ? tip_anchor_height : (uint64_t)std::max(0, tip_height);
        size_t dropped = 0;
        for (Stored& s : g_entries) {
            if (s.dropped) continue;
            const PosSigningEntry& e = s.entry;
            // The stand-in for an unreadable entry goes only when its hold ends.
            if (e.kind == (uint8_t)PosSignKind::UNREADABLE) continue;
            bool drop = false;
            if (e.height >= 0 && e.height <= tip_height) {
                if (const auto at = active_at(e.height)) {
                    // On the active chain, and in the index just written; or
                    // lost to a quorum-certified sibling the index now carries.
                    drop = at->first == e.block_hash || at->second;
                }
            }
            const uint64_t key = (g_con_bitcoin_anchor || e.height < 0) ? e.anchor_height : (uint64_t)e.height;
            if (!drop && ref > key + POS_SIGNING_RECORD_WINDOW) drop = true;
            if (!drop) continue;
            s.dropped = true;
            g_dropped_bytes += s.bytes;
            ++dropped;
        }
        if (dropped > 0) {
            PruneHeld();
            LogPrint(BCLog::VALIDATION, "Signing record: %u entr%s dropped after the block index write\n",
                     (unsigned)dropped, dropped == 1 ? "y" : "ies");
        }
        if (g_dropped_bytes > 0 && 2 * g_dropped_bytes > g_file_bytes) {
            std::string why;
            if (!RewriteLocked(nullptr, why)) {
                Fail(why);
                abort = true;
            }
        }
    }
    if (abort) AbortNode("Failed to write the signing record");
}

std::vector<PosSigningEntry> PosSigningRecordEntries()
{
    LOCK(g_mutex);
    std::vector<PosSigningEntry> out;
    for (const Stored& s : g_entries) {
        if (!s.dropped) out.push_back(s.entry);
    }
    return out;
}

uint256 PosSigningRecordChain()
{
    LOCK(g_mutex);
    return g_genesis;
}

std::vector<PosSigningHeld> PosSigningHeldBlocks()
{
    LOCK(g_mutex);
    std::vector<PosSigningHeld> out;
    for (const auto& [height, at] : g_held) {
        for (const auto& [hash, h] : at) {
            PosSigningHeld o;
            o.hash = hash;
            o.height = height;
            o.anchor_height = h.anchor_height;
            o.anchor_hash = h.anchor_hash;
            o.from_disk = h.from_disk;
            o.state = h.certified && !h.invalidated ? "certified" : h.state;
            out.push_back(o);
        }
    }
    return out;
}

PosSigningRecordStats PosSigningRecordGetStats()
{
    LOCK(g_mutex);
    PosSigningRecordStats st = g_stats;
    for (const Stored& s : g_entries) {
        if (s.dropped) continue;
        ++st.entries;
        st.live_bytes += s.bytes;
    }
    st.file_bytes = g_file_bytes;
    return st;
}

void PosSigningRecordResetForTesting()
{
    LOCK(g_mutex);
    g_entries.clear();
    g_held.clear();
    g_certified_heights.clear();
    g_hold = PosSigningDamagedHold{};
    g_file_bytes = 0;
    g_dropped_bytes = 0;
    g_failed = false;
    g_stats = PosSigningRecordStats{};
}
