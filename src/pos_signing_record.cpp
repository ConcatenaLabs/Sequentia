// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_signing_record.h>

#include <clientversion.h>
#include <fs.h>
#include <hash.h>
#include <logging.h>
#include <primitives/block.h>
#include <shutdown.h>
#include <streams.h>
#include <util/system.h>
#include <util/time.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

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

Mutex g_mutex;
std::vector<Stored> g_entries GUARDED_BY(g_mutex);
std::map<int, PosRestoredLock> g_restored GUARDED_BY(g_mutex);
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
    uint64_t bytes = 0;
    bool ok = true;
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
    DirectoryCommit(gArgs.GetDataDirNet());
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

} // namespace

bool PosSigningEntry::Locks() const
{
    return kind == (uint8_t)PosSignKind::SHARE || kind == (uint8_t)PosSignKind::CERTIFIED ||
           kind == (uint8_t)PosSignKind::LEADER;
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

bool PosSigningRecordLoad(std::string& error)
{
    LOCK(g_mutex);
    g_entries.clear();
    g_restored.clear();
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
    size_t pos = 0;
    bool torn = false;
    while (pos < data.size()) {
        const size_t left = data.size() - pos;
        uint32_t magic = 0, len = 0, sum = 0;
        if (left >= 8) {
            std::memcpy(&magic, &data[pos], 4);
            std::memcpy(&len, &data[pos + 4], 4);
        }
        const bool whole = left >= 8 && magic == FRAME_MAGIC && len <= MAX_PAYLOAD && left >= 12 + (size_t)len;
        bool good = false;
        PosSigningEntry e;
        if (whole) {
            std::memcpy(&sum, &data[pos + 8 + len], 4);
            const Span<const std::byte> payload(&data[pos + 8], len);
            if (Checksum(payload) == sum) {
                try {
                    CDataStream s(payload, SER_DISK, CLIENT_VERSION);
                    s >> e;
                    good = s.empty();
                } catch (const std::exception&) {
                    good = false;
                }
            }
        }
        if (!good) {
            // A write that never completed leaves a torn last entry: its
            // signature never left the process. Anything after a bad entry
            // means the file is damaged, and starting without it could sign
            // a rival of a block this member signed.
            if (!whole || pos + 12 + (size_t)len == data.size()) {
                torn = true;
                LogPrintf("Signing record: dropping a torn last entry (%u bytes): a write that never completed\n",
                          (unsigned)left);
                break;
            }
            error = strprintf("the signing record %s is corrupt at byte %u; move it aside to start without it",
                              fs::PathToString(path), (unsigned)pos);
            return false;
        }
        Stored s;
        s.entry = std::move(e);
        s.bytes = 12 + len;
        g_file_bytes += s.bytes;
        g_entries.push_back(std::move(s));
        pos += 12 + (size_t)len;
    }
    // Rebuild the share-lock: per height, the latest entry that locks it.
    for (const Stored& s : g_entries) {
        if (!s.entry.Locks() || s.entry.height < 0) continue;
        PosRestoredLock l;
        l.block_hash = s.entry.block_hash;
        l.height = s.entry.height;
        l.anchor_height = s.entry.anchor_height;
        l.anchor_hash = s.entry.anchor_hash;
        g_restored[s.entry.height] = l;
    }
    LogPrintf("Signing record: %u entries loaded, %u height(s) signed\n", (unsigned)g_entries.size(), (unsigned)g_restored.size());
    if (torn) {
        // Rewrite now, so that no later entry is appended after the torn one.
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
        if (!fs::exists(path)) {
            // A new file (or one removed under the node): write every entry
            // kept in memory with this one, so that the file has them all.
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

std::optional<PosRestoredLock> PosSigningRecordRestoredLock(int height)
{
    LOCK(g_mutex);
    auto it = g_restored.find(height);
    if (it == g_restored.end()) return std::nullopt;
    return it->second;
}

void PosSigningRecordClearRestoredLock(int height)
{
    LOCK(g_mutex);
    g_restored.erase(height);
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
            auto it = g_restored.find(e.height);
            if (it != g_restored.end() && it->second.block_hash == e.block_hash) g_restored.erase(it);
        }
        if (dropped > 0) {
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
    g_restored.clear();
    g_file_bytes = 0;
    g_dropped_bytes = 0;
    g_failed = false;
    g_stats = PosSigningRecordStats{};
}
