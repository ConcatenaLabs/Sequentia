// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_signing_record.h>

#include <pos.h>

#include <chain.h>
#include <fs.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <set>

namespace {

PosSigningEntry Entry(PosSignKind kind, int height, unsigned char tag, size_t inputs)
{
    PosSigningEntry e;
    e.kind = (uint8_t)kind;
    e.block_hash = uint256(std::vector<unsigned char>(32, tag));
    e.height = height;
    e.anchor_height = 100 + height;
    e.time = 1;
    for (size_t i = 0; i < inputs; ++i) e.spends.emplace_back(COutPoint(uint256(std::vector<unsigned char>(32, tag + 1)), i), uint256());
    return e;
}

fs::path RecordFile() { return gArgs.GetDataDirNet() / "possigned.dat"; }

const uint256 GENESIS{std::vector<unsigned char>(32, 0xee)};

uint256 Hash32(unsigned char tag) { return uint256(std::vector<unsigned char>(32, tag)); }

//! A verified full certificate for block `tag` at `height`, anchored at `anchor_tag`.
void Certify(unsigned char tag, int height, unsigned char anchor_tag, uint32_t anchor_height, PosCertRoute route,
             const uint256& parent = uint256())
{
    PosSigningNoteCertificate(PosCertifiedBlock{Hash32(tag), height, parent, anchor_height, Hash32(anchor_tag)}, route);
}

size_t EntriesFor(const uint256& hash)
{
    size_t n = 0;
    for (const PosSigningEntry& e : PosSigningRecordEntries()) n += e.block_hash == hash;
    return n;
}

void AppendBytes(const std::vector<unsigned char>& bytes)
{
    FILE* f = fsbridge::fopen(RecordFile(), "ab");
    BOOST_REQUIRE(f);
    BOOST_REQUIRE_EQUAL(fwrite(bytes.data(), 1, bytes.size(), f), bytes.size());
    fclose(f);
}

void FlipByteFromEnd(long back)
{
    FILE* f = fsbridge::fopen(RecordFile(), "r+b");
    BOOST_REQUIRE(f);
    BOOST_REQUIRE_EQUAL(fseek(f, -back, SEEK_END), 0);
    const int c = fgetc(f);
    BOOST_REQUIRE_EQUAL(fseek(f, -back, SEEK_END), 0);
    fputc(c ^ 0x01, f);
    fclose(f);
}

//! A fresh, empty record for GENESIS.
void Fresh()
{
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
    std::string error;
    BOOST_REQUIRE(PosSigningRecordLoad(GENESIS, error));
}

//! Restart: forget memory, load the file again.
bool Reload(std::string& error)
{
    PosSigningRecordResetForTesting();
    return PosSigningRecordLoad(GENESIS, error);
}

std::string StateOf(const uint256& hash)
{
    for (const PosSigningHeld& h : PosSigningHeldBlocks()) {
        if (h.hash == hash) return h.state;
    }
    return "absent";
}

//! Anchors judged by the test: the anchor hash's first byte names the verdict.
std::map<uint256, PosAnchorStanding> g_verdicts;
//! The block index as the test sets it: a block not named stands as its
//! parent does, and a block whose parent is not named either is unknown.
std::map<uint256, PosIndexStanding> g_index;
//! The anchor the anchor watcher's mark names, for an ORPHANED block in g_index.
std::map<uint256, std::pair<uint32_t, uint256>> g_marks;
//! ORPHANED blocks with a failed run below them that the watcher did not make.
std::set<uint256> g_also_failed;
//! Failure marks cleared on the node (PosIndexState::revivals).
uint64_t g_revivals{0};

//! How many times the anchor judge was asked, and every anchor it was asked
//! about, in order (one entry per anchor per call).
size_t g_judge_calls{0};
std::vector<std::pair<uint32_t, uint256>> g_asked;

//! An anchor judge as the node's: asked once per check about a list of
//! anchors, it answers one verdict each, here from `f`; nothing when `f`
//! answers nothing (a node that does not watch Bitcoin).
PosAnchorJudge PerAnchor(std::function<std::optional<PosAnchorStanding>(uint32_t, const uint256&)> f)
{
    return [f](const std::vector<std::pair<uint32_t, uint256>>& anchors) -> std::optional<std::vector<PosAnchorStanding>> {
        ++g_judge_calls;
        g_asked.insert(g_asked.end(), anchors.begin(), anchors.end());
        std::vector<PosAnchorStanding> out;
        for (const auto& [height, anchor] : anchors) {
            const std::optional<PosAnchorStanding> v = f(height, anchor);
            if (!v) return std::nullopt;
            out.push_back(*v);
        }
        return out;
    };
}

//! The anchor watcher dropped `tag` (from the disk or in this run alike) for
//! Bitcoin block `anchor_tag` at `anchor_height`, whose verdict now is `now`.
void Orphan(unsigned char tag, unsigned char anchor_tag, uint32_t anchor_height, PosAnchorStanding now);

struct AnchoredChain {
    const bool saved{g_con_bitcoin_anchor};
    AnchoredChain()
    {
        g_con_bitcoin_anchor = true;
        g_verdicts.clear();
        g_index.clear();
        g_marks.clear();
        g_also_failed.clear();
        g_revivals = 0;
        g_judge_calls = 0;
        g_asked.clear();
        PosSigningSetAnchorJudge(PerAnchor([](uint32_t, const uint256& anchor) -> std::optional<PosAnchorStanding> {
            auto it = g_verdicts.find(anchor);
            return it == g_verdicts.end() ? PosAnchorStanding::STANDS : it->second;
        }));
        PosSigningSetIndexJudge([](const uint256& hash, const uint256& parent) {
            const auto with_mark = [](const uint256& key, PosIndexStanding standing) {
                PosIndexState st(standing);
                auto m = g_marks.find(key);
                if (standing == PosIndexStanding::ORPHANED && m != g_marks.end()) st.orphan_anchors.push_back(m->second);
                st.failed_otherwise = standing == PosIndexStanding::ORPHANED && g_also_failed.count(key);
                return st;
            };
            PosIndexState st;
            if (auto it = g_index.find(hash); it != g_index.end()) {
                st = with_mark(hash, it->second);
            } else if (auto it = g_index.find(parent); it != g_index.end()) {
                st = with_mark(parent, it->second == PosIndexStanding::ACTIVE ? PosIndexStanding::OFF_ACTIVE : it->second);
            }
            st.revivals = g_revivals;
            return st;
        });
    }
    ~AnchoredChain()
    {
        PosSigningSetAnchorJudge(nullptr);
        PosSigningSetIndexJudge(nullptr);
        g_con_bitcoin_anchor = saved;
    }
};

void Orphan(unsigned char tag, unsigned char anchor_tag, uint32_t anchor_height, PosAnchorStanding now)
{
    g_index[Hash32(tag)] = PosIndexStanding::ORPHANED;
    g_marks[Hash32(tag)] = {anchor_height, Hash32(anchor_tag)};
    g_verdicts[Hash32(anchor_tag)] = now;
}

PosSigningEntry Anchored(PosSignKind kind, int height, unsigned char tag, unsigned char anchor_tag, uint32_t anchor_height)
{
    PosSigningEntry e = Entry(kind, height, tag, 0);
    e.anchor_height = anchor_height;
    e.anchor_hash = Hash32(anchor_tag);
    return e;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(pos_signing_record_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(load_holds_every_signed_block_per_height)
{
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::PROPOSAL, 5, 0x10, 2)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 5, 0x20, 2)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 5, 0x21, 0)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::PROPOSAL, 6, 0x30, 0)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::CERTIFIED, 7, 0x40, 1)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::PARTIAL, 8, 0x41, 0)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::PARTIAL, -1, 0x42, 0)));
    std::string error;
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK_EQUAL(PosSigningRecordEntries().size(), 7U);
    BOOST_CHECK(PosSigningRecordChain() == GENESIS);
    // Both shares at 5 hold (not only the latest); a certified block and a
    // partial signature whose block is known hold; a proposal and a partial
    // signature over a bare hash do not.
    std::set<uint256> held;
    for (const PosSigningHeld& h : PosSigningHeldBlocks()) {
        held.insert(h.hash);
        BOOST_CHECK(h.from_disk);
    }
    BOOST_CHECK(held == (std::set<uint256>{Hash32(0x20), Hash32(0x21), Hash32(0x40), Hash32(0x41)}));
    // The spends survive the round trip.
    BOOST_CHECK_EQUAL(PosSigningRecordEntries()[1].spends.size(), 2U);
}

BOOST_AUTO_TEST_CASE(a_last_entry_cut_short_is_dropped)
{
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 9, 0x50, 3)));
    const uint64_t whole = fs::file_size(RecordFile());
    // A write killed part-way: a frame header and a few payload bytes.
    AppendBytes({0x53, 0x47, 0x52, 0x31, 0xff, 0x00, 0x00, 0x00, 0x01, 0x02});
    std::string error;
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK_EQUAL(PosSigningRecordEntries().size(), 1U);
    BOOST_CHECK(!PosSigningGetDamagedHold().active);
    // Rewritten without it, so the next entry follows a whole one.
    BOOST_CHECK_EQUAL(fs::file_size(RecordFile()), whole);
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 10, 0x60, 0)));
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK_EQUAL(PosSigningRecordEntries().size(), 2U);
}

BOOST_AUTO_TEST_CASE(damage_before_the_last_entry_stops_the_start)
{
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 11, 0x70, 1)));
    const uint64_t first = fs::file_size(RecordFile());
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 12, 0x80, 1)));
    // Flip one byte inside the first entry's payload.
    FlipByteFromEnd((long)(fs::file_size(RecordFile()) - first) + 20);
    std::string error;
    BOOST_CHECK(!PosSigningRecordLoad(GENESIS, error));
    BOOST_CHECK(error.find("damaged at byte") != std::string::npos);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_removed_file_is_written_whole_again)
{
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 13, 0x90, 0)));
    fs::remove(RecordFile());
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 14, 0xa0, 0)));
    std::string error;
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK_EQUAL(PosSigningRecordEntries().size(), 2U);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_record_of_another_chain_or_a_newer_format_stops_the_start)
{
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 3, 0x11, 0)));
    PosSigningRecordResetForTesting();
    std::string error;
    BOOST_CHECK(!PosSigningRecordLoad(Hash32(0xdd), error));
    BOOST_CHECK(error.find("belongs to another chain") != std::string::npos);
    // A later format's mark on the last entry: refused, not dropped as torn.
    BOOST_REQUIRE(Reload(error));
    const long entry_at = (long)(fs::file_size(RecordFile()) - PosSigningRecordGetStats().live_bytes);
    FILE* f = fsbridge::fopen(RecordFile(), "r+b");
    BOOST_REQUIRE(f);
    BOOST_REQUIRE_EQUAL(fseek(f, entry_at + 3, SEEK_SET), 0);
    BOOST_REQUIRE_EQUAL(fgetc(f), '1');
    BOOST_REQUIRE_EQUAL(fseek(f, entry_at + 3, SEEK_SET), 0);
    fputc('2', f);
    fclose(f);
    BOOST_CHECK(!Reload(error));
    BOOST_CHECK(error.find("newer version") != std::string::npos);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_whole_but_unreadable_last_entry_holds_the_next_height)
{
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 20, 0xb0, 0)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 21, 0xb1, 0)));
    FlipByteFromEnd(1); // the last entry's checksum
    std::string error;
    BOOST_REQUIRE(Reload(error));
    PosSigningDamagedHold hold = PosSigningGetDamagedHold();
    BOOST_REQUIRE(hold.active);
    BOOST_CHECK_EQUAL(hold.floor, 20);
    BOOST_CHECK(fs::exists(fs::PathFromString(hold.copy)));
    // Below the floor: free. At the first height asked from the floor: held.
    BOOST_CHECK(PosSigningCheckNow(Entry(PosSignKind::SHARE, 19, 0xc0, 0), 1000).ok);
    const PosSignCheck held = PosSigningCheckNow(Entry(PosSignKind::SHARE, 21, 0xc1, 0), 1000);
    BOOST_CHECK(!held.ok);
    BOOST_CHECK(held.why.find("unreadable entry") != std::string::npos);
    BOOST_CHECK(PosSigningSignedAt(21));
    // A bare hash cannot be placed: refused.
    BOOST_CHECK(!PosSigningCheckNow(Entry(PosSignKind::PARTIAL, -1, 0xc2, 0), 1000).ok);
    // A restart in the hold holds again.
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK(PosSigningGetDamagedHold().active);
    BOOST_CHECK(!PosSigningCheckNow(Entry(PosSignKind::SHARE, 21, 0xc1, 0), 1000).ok);
    // A certificate at the held height ends it, and the file forgets it. The
    // certified block then holds that height itself: it alone may be signed.
    Certify(0xc3, 21, 0x00, 0, PosCertRoute::CERTIFICATE);
    BOOST_CHECK(PosSigningCheckNow(Entry(PosSignKind::SHARE, 21, 0xc3, 0), 1000).ok);
    BOOST_CHECK(!PosSigningGetDamagedHold().active);
    BOOST_CHECK(!PosSigningCheckNow(Entry(PosSignKind::SHARE, 21, 0xc1, 0), 1000).ok);
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK(!PosSigningGetDamagedHold().active);
    fs::remove(fs::PathFromString(hold.copy));
}

BOOST_AUTO_TEST_CASE(the_unreadable_hold_ends_when_the_height_passes)
{
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 30, 0xd0, 0)));
    FlipByteFromEnd(1);
    std::string error;
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK(!PosSigningCheckNow(Entry(PosSignKind::SHARE, 31, 0xd1, 0), 1000).ok);
    BOOST_CHECK(PosSigningCheckNow(Entry(PosSignKind::SHARE, 32, 0xd2, 0), 1000).ok);
    BOOST_CHECK(!PosSigningGetDamagedHold().active);
    fs::remove(fs::PathFromString(PosSigningGetDamagedHold().copy));
}

BOOST_AUTO_TEST_CASE(the_check_escapes_after_one_round_and_binds_again_after_a_return)
{
    AnchoredChain anchored;
    Fresh();
    const int64_t round = PosSigningRoundMs();
    // X at 40, anchored at Bitcoin block A (height 500).
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, 40, 0x01, 0xa1, 500)));
    const PosSigningEntry y = Anchored(PosSignKind::SHARE, 40, 0x02, 0xa2, 501);
    // The same block again is free; a rival waits one round, then goes.
    BOOST_CHECK(PosSigningCheckNow(Anchored(PosSignKind::SHARE, 40, 0x01, 0xa1, 500), 10000).ok);
    PosSignCheck c = PosSigningCheckNow(y, 10000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.bound == Hash32(0x01));
    BOOST_CHECK_EQUAL(StateOf(Hash32(0x01)), "binds");
    BOOST_CHECK(!PosSigningCheckNow(y, 10000 + round - 1).ok);
    BOOST_CHECK(PosSigningCheckNow(y, 10000 + round).ok);
    BOOST_CHECK_EQUAL(StateOf(Hash32(0x01)), "escaped");
    // Bitcoin replaces A: X binds nothing; Y is signed at once.
    g_verdicts[Hash32(0xa1)] = PosAnchorStanding::GONE;
    PosSigningParentMoved(499);
    BOOST_CHECK(PosSigningCheckNow(y, 20000).ok);
    BOOST_CHECK_EQUAL(StateOf(Hash32(0x01)), "anchor gone");
    BOOST_CHECK(PosSigningRecordAppend(y));
    // Bitcoin returns to A (Y's anchor is gone now): X binds again, so Z waits
    // for a new query and a new round.
    g_verdicts[Hash32(0xa1)] = PosAnchorStanding::STANDS;
    g_verdicts[Hash32(0xa2)] = PosAnchorStanding::GONE;
    PosSigningParentMoved(499);
    const PosSigningEntry z = Anchored(PosSignKind::SHARE, 40, 0x03, 0xa1, 500);
    c = PosSigningCheckNow(z, 30000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.bound == Hash32(0x01));
    BOOST_CHECK(PosSigningCheckNow(z, 30000 + round).ok);
    // X itself again is free throughout.
    BOOST_CHECK(PosSigningCheckNow(Anchored(PosSignKind::SHARE, 40, 0x01, 0xa1, 500), 30000).ok);
    // A restart forgets every escape: X binds again after it too.
    std::string error;
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK(!PosSigningCheckNow(z, 40000).ok);
    // A certificate for X: it holds the height for good.
    Certify(0x01, 40, 0xa1, 500, PosCertRoute::CERTIFICATE);
    c = PosSigningCheckNow(z, 40000 + 10 * round);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("holds a full certificate for it") != std::string::npos);
    BOOST_CHECK(!PosSigningCheckNow(z, 40000 + 20 * round).ok);
    // The operator invalidates X on this node: a recovery step, so the member
    // is released after one round, as for an uncertified block.
    g_index[Hash32(0x01)] = PosIndexStanding::FAILED;
    c = PosSigningCheckNow(z, 50000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("signs no other block there") != std::string::npos);
    BOOST_CHECK(PosSigningCheckNow(z, 50000 + round).ok);
    // Revived (reconsiderblock): the certificate holds the height again.
    g_index[Hash32(0x01)] = PosIndexStanding::ACTIVE;
    BOOST_CHECK(!PosSigningCheckNow(z, 60000).ok);
    // Another held block's invalidation changes nothing for X.
    g_index[Hash32(0x09)] = PosIndexStanding::FAILED;
    BOOST_CHECK(!PosSigningCheckNow(z, 70000).ok);
    // Invalid again: a new round, counted from the first check that finds it so.
    g_index[Hash32(0x01)] = PosIndexStanding::FAILED;
    BOOST_CHECK(!PosSigningCheckNow(z, 80000).ok);
    BOOST_CHECK(!PosSigningCheckNow(z, 80000 + round - 1).ok);
    BOOST_CHECK(PosSigningCheckNow(z, 80000 + round).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(no_verdict_signs_nothing_at_that_height_until_one_comes)
{
    AnchoredChain anchored;
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::CERTIFIED, 50, 0x05, 0xa5, 600)));
    g_verdicts[Hash32(0xa5)] = PosAnchorStanding::UNDECIDED;
    const PosSigningEntry rival = Anchored(PosSignKind::SHARE, 50, 0x06, 0xa6, 601);
    const int64_t round = PosSigningRoundMs();
    PosSignCheck c = PosSigningCheckNow(rival, 1000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("has not judged") != std::string::npos);
    BOOST_CHECK(!PosSigningCheckNow(rival, 1000 + 100 * round).ok); // no time limit while no verdict
    BOOST_CHECK(PosSigningSignedAt(50));
    // The daemon answers: replaced, so free at once.
    g_verdicts[Hash32(0xa5)] = PosAnchorStanding::GONE;
    BOOST_CHECK(PosSigningCheckNow(rival, 1000 + 100 * round).ok);
    BOOST_CHECK(!PosSigningSignedAt(50));
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_bare_hash_is_refused_while_a_block_at_or_above_the_tip_binds)
{
    AnchoredChain anchored;
    Fresh();
    // The tip is 59; this member signed X at 60.
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::PARTIAL, 60, 0x07, 0xa7, 700)));
    PosSigningEntry bare = Entry(PosSignKind::PARTIAL, -1, 0x08, 0);
    PosSignCheck c = PosSigningCheckNow(bare, 1000, 59, Hash32(0x59));
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("give the block") != std::string::npos);
    // X is the tip now, and certified: a hash this node does not know may be a
    // rival at the tip's height, so it is still refused.
    Certify(0x07, 60, 0xa7, 700, PosCertRoute::CONNECTED);
    c = PosSigningCheckNow(bare, 1000 + 100 * PosSigningRoundMs(), 60, Hash32(0x07));
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.bound == Hash32(0x07));
    // The block itself, given whole, places the hash at 61: free.
    BOOST_CHECK(PosSigningCheckNow(Anchored(PosSignKind::PARTIAL, 61, 0x08, 0xa8, 701), 1000, 60, Hash32(0x07)).ok);
    // A certified tip whose anchor Bitcoin replaced binds nothing.
    g_verdicts[Hash32(0xa7)] = PosAnchorStanding::GONE;
    BOOST_CHECK(PosSigningCheckNow(bare, 2000, 60, Hash32(0x07)).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_node_that_does_not_watch_bitcoin_holds_only_this_runs_blocks)
{
    AnchoredChain anchored;
    PosSigningSetAnchorJudge(PerAnchor([](uint32_t, const uint256&) -> std::optional<PosAnchorStanding> { return std::nullopt; }));
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, 70, 0x09, 0xa9, 800)));
    BOOST_CHECK(!PosSigningCheckNow(Anchored(PosSignKind::SHARE, 70, 0x0a, 0xaa, 800), 1000).ok);
    std::string error;
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK(PosSigningCheckNow(Anchored(PosSignKind::SHARE, 70, 0x0a, 0xaa, 800), 1000).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(compaction_never_weakens_the_check)
{
    AnchoredChain anchored;
    Fresh();
    const int64_t round = PosSigningRoundMs();
    const auto certifies = [](std::set<uint256> s) { return [s](const uint256& h) { return s.count(h) > 0; }; };
    // X certified at 80 (this member signed it), U signed at 81 and uncertified.
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::CERTIFIED, 80, 0x20, 0xb0, 900)));
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, 81, 0x21, 0xb1, 900)));
    Certify(0x20, 80, 0xb0, 900, PosCertRoute::CONNECTED);
    const PosSigningEntry rival = Anchored(PosSignKind::SHARE, 80, 0x22, 0xb2, 901);
    // The index is written: it carries X with a verified certificate, so X's
    // entry goes; U is uncertified, so its entry stays, whatever the active chain holds.
    PosSigningRecordIndexWritten(81, 901, certifies({Hash32(0x20)}));
    BOOST_CHECK_EQUAL(EntriesFor(Hash32(0x20)), 0U);
    BOOST_CHECK_EQUAL(EntriesFor(Hash32(0x21)), 1U);
    // X still binds, for good, with no restart: the index answers for it.
    BOOST_CHECK(!PosSigningCheckNow(rival, 1000).ok);
    BOOST_CHECK(!PosSigningCheckNow(rival, 1000 + 50 * round).ok);
    // A restart: the record no longer names X; the block index scan does.
    std::string error;
    BOOST_REQUIRE(Reload(error));
    // Whatever the file still holds of X binds only by the escape (one round),
    // and a rewrite may have removed it: the record alone no longer keeps X ...
    if (EntriesFor(Hash32(0x20)) > 0) BOOST_CHECK(!PosSigningCheckNow(rival, 2000).ok);
    BOOST_CHECK(PosSigningCheckNow(rival, 2000 + round).ok);
    Certify(0x20, 80, 0xb0, 900, PosCertRoute::INDEX_AT_START);
    BOOST_CHECK(!PosSigningCheckNow(rival, 3000 + 50 * round).ok); // ... which is why the scan runs before any signing path
    // U's escape is unchanged by the compaction: one round.
    const PosSigningEntry u_rival = Anchored(PosSignKind::SHARE, 81, 0x23, 0xb1, 900);
    BOOST_CHECK(!PosSigningCheckNow(u_rival, 4000).ok);
    BOOST_CHECK(PosSigningCheckNow(u_rival, 4000 + round).ok);
    // Past the window, both go: X's certificate and U's entry.
    PosSigningRecordIndexWritten(500, 900 + POS_SIGNING_RECORD_WINDOW + 1, certifies({}));
    BOOST_CHECK_EQUAL(StateOf(Hash32(0x20)), "absent");
    BOOST_CHECK_EQUAL(EntriesFor(Hash32(0x21)), 0U);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_signature_lost_to_a_certified_sibling_keeps_its_escape)
{
    AnchoredChain anchored;
    Fresh();
    const int64_t round = PosSigningRoundMs();
    // This member signed Y at 90; the certified sibling X' took the height on
    // the active chain. Writing the index drops nothing for Y.
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, 90, 0x30, 0xc0, 950)));
    Certify(0x31, 90, 0xc1, 951, PosCertRoute::CONNECTED);
    PosSigningRecordIndexWritten(90, 951, [](const uint256& h) { return h == Hash32(0x31); });
    BOOST_CHECK_EQUAL(EntriesFor(Hash32(0x30)), 1U);
    // Bitcoin replaces X''s anchor, not Y's: Y binds by its escape again.
    g_verdicts[Hash32(0xc1)] = PosAnchorStanding::GONE;
    const PosSigningEntry z = Anchored(PosSignKind::SHARE, 90, 0x32, 0xc0, 950);
    PosSignCheck c = PosSigningCheckNow(z, 1000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.bound == Hash32(0x30));
    BOOST_CHECK(PosSigningCheckNow(z, 1000 + round).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_proposal_waits_for_a_certificate_and_the_unreadable_hold_only)
{
    AnchoredChain anchored;
    Fresh();
    // An uncertified block this member signed does not stop its proposal.
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, 100, 0x40, 0xd0, 990)));
    const PosSigningEntry proposal = Anchored(PosSignKind::PROPOSAL, 100, 0x41, 0xd0, 990);
    BOOST_CHECK(PosSigningCheckNow(proposal, 1000).ok);
    // A certified block at that height does, for good, while its anchor stands.
    Certify(0x42, 100, 0xd2, 990, PosCertRoute::CERTIFICATE);
    PosSignCheck c = PosSigningCheckNow(proposal, 1000 + 50 * PosSigningRoundMs());
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.bound == Hash32(0x42));
    g_verdicts[Hash32(0xd2)] = PosAnchorStanding::GONE;
    BOOST_CHECK(PosSigningCheckNow(proposal, 2000).ok);
    // A whole but unreadable last entry holds proposals too.
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, 110, 0x43, 0xd3, 995)));
    FlipByteFromEnd(1);
    std::string error;
    BOOST_REQUIRE(Reload(error));
    c = PosSigningCheckNow(Anchored(PosSignKind::PROPOSAL, 111, 0x44, 0xd4, 996), 3000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("unreadable entry") != std::string::npos);
    fs::remove(fs::PathFromString(PosSigningGetDamagedHold().copy));
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_node_that_does_not_watch_bitcoin_follows_its_active_chain)
{
    AnchoredChain anchored;
    PosSigningSetAnchorJudge(PerAnchor([](uint32_t, const uint256&) -> std::optional<PosAnchorStanding> { return std::nullopt; }));
    Fresh();
    const PosSigningEntry rival = Anchored(PosSignKind::SHARE, 120, 0x51, 0xe1, 1000);
    // A certificate whose body has not connected binds nothing: nothing this
    // node does would ever tell it that Bitcoin replaced that block's anchor.
    g_index[Hash32(0x4f)] = PosIndexStanding::ACTIVE; // the parent, the tip
    Certify(0x50, 120, 0xe0, 1000, PosCertRoute::CERTIFICATE, Hash32(0x4f));
    BOOST_CHECK(PosSigningCheckNow(rival, 1000).ok);
    // Once the block is on the active chain, it binds ...
    g_index[Hash32(0x50)] = PosIndexStanding::ACTIVE;
    BOOST_CHECK(!PosSigningCheckNow(rival, 1000 + 50 * PosSigningRoundMs()).ok);
    // ... and not once the node has left it (how it follows a Bitcoin reorganization).
    g_index[Hash32(0x50)] = PosIndexStanding::OFF_ACTIVE;
    BOOST_CHECK(PosSigningCheckNow(rival, 2000).ok);
    // Back on the active chain, it binds again.
    g_index[Hash32(0x50)] = PosIndexStanding::ACTIVE;
    BOOST_CHECK(!PosSigningCheckNow(rival, 2500).ok);
    // After a restart: a block the index holds on the active chain binds; one
    // off the active chain binds nothing (this node cannot judge its anchor).
    std::string error;
    BOOST_REQUIRE(Reload(error));
    Certify(0x50, 120, 0xe0, 1000, PosCertRoute::INDEX_AT_START);
    BOOST_CHECK(!PosSigningCheckNow(rival, 3000).ok);
    g_index[Hash32(0x50)] = PosIndexStanding::OFF_ACTIVE;
    BOOST_CHECK(PosSigningCheckNow(rival, 3000).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_node_that_does_not_watch_bitcoin_releases_an_invalid_block_after_one_round)
{
    AnchoredChain anchored;
    PosSigningSetAnchorJudge(PerAnchor([](uint32_t, const uint256&) -> std::optional<PosAnchorStanding> { return std::nullopt; }));
    Fresh();
    const int64_t round = PosSigningRoundMs();
    // Invalid here, on a node that does not watch Bitcoin: one query and one
    // round, whatever kind of entry holds the height, with the block and as
    // a bare hash. For each kind, X (with the block) and W (bare hash) are
    // held on the active chain, then the operator invalidates them.
    struct Kind {
        const char* what;
        int height;
        bool signed_here;
        bool certified;
        bool from_disk;
    };
    const Kind kinds[] = {
        {"certified only, this run", 160, false, true, false},
        {"certified only, from the index at the start", 162, false, true, true},
        {"signed here and certified, this run", 164, true, true, false},
        {"signed here and certified, from the disk", 166, true, true, true},
        {"signed here, not certified, this run", 168, true, false, false},
        {"signed here, not certified, from the disk", 170, true, false, true},
    };
    int64_t now = 1000;
    unsigned char tag = 0x10;
    for (const Kind& k : kinds) {
        BOOST_TEST_MESSAGE(k.what);
        const unsigned char x = tag, w = tag + 1;
        tag += 4;
        const int hx = k.height, hw = k.height + 1;
        if (k.signed_here) {
            BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, hx, x, 0xe5, 1500)));
            BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, hw, w, 0xe5, 1500)));
        }
        std::string error;
        if (k.from_disk) BOOST_REQUIRE(Reload(error));
        if (k.certified) {
            const PosCertRoute route = k.from_disk ? PosCertRoute::INDEX_AT_START : PosCertRoute::CONNECTED;
            Certify(x, hx, 0xe5, 1500, route);
            Certify(w, hw, 0xe5, 1500, route);
        }
        g_index[Hash32(x)] = PosIndexStanding::ACTIVE;
        g_index[Hash32(w)] = PosIndexStanding::ACTIVE;
        const PosSigningEntry rival = Anchored(PosSignKind::SHARE, hx, 0xf0, 0xe6, 1500);
        // On the active chain a certified block binds for good.
        if (k.certified) BOOST_CHECK(!PosSigningCheckNow(rival, now + 50 * round).ok);
        now += 100 * round;
        g_index[Hash32(x)] = PosIndexStanding::FAILED;
        g_index[Hash32(w)] = PosIndexStanding::FAILED;
        PosSignCheck c = PosSigningCheckNow(rival, now);
        BOOST_CHECK(!c.ok);
        BOOST_CHECK(c.bound == Hash32(x));
        BOOST_CHECK(c.why.find("invalid on this node") != std::string::npos);
        BOOST_CHECK(c.why.find("anchor stands") == std::string::npos);
        BOOST_CHECK_EQUAL(StateOf(Hash32(x)), "binds");
        BOOST_CHECK(!PosSigningCheckNow(rival, now + round - 1).ok);
        BOOST_CHECK(PosSigningCheckNow(rival, now + round).ok);
        // A bare hash, with W at the tip's height.
        const PosSigningEntry bare = Entry(PosSignKind::PARTIAL, -1, 0xf1, 0);
        c = PosSigningCheckNow(bare, now, hw, Hash32(0xf2));
        BOOST_CHECK(!c.ok);
        BOOST_CHECK(c.bound == Hash32(w));
        BOOST_CHECK(!PosSigningCheckNow(bare, now + round - 1, hw, Hash32(0xf2)).ok);
        BOOST_CHECK(PosSigningCheckNow(bare, now + round, hw, Hash32(0xf2)).ok);
        now += 100 * round;
        if (k.certified) {
            // Revived, it binds for good again; invalid again, a new round.
            g_index[Hash32(x)] = PosIndexStanding::ACTIVE;
            ++g_revivals;
            BOOST_CHECK(!PosSigningCheckNow(rival, now).ok);
            now += 100 * round;
            g_index[Hash32(x)] = PosIndexStanding::FAILED;
            BOOST_CHECK(!PosSigningCheckNow(rival, now).ok);
            BOOST_CHECK(PosSigningCheckNow(rival, now + round).ok);
            now += 100 * round;
            // Revived and invalidated again with no check between: a new
            // round all the same, with the block and as a bare hash.
            ++g_revivals;
            c = PosSigningCheckNow(rival, now);
            BOOST_CHECK(!c.ok);
            BOOST_CHECK(c.bound == Hash32(x));
            BOOST_CHECK(PosSigningCheckNow(rival, now + round).ok);
            ++g_revivals;
            BOOST_CHECK(!PosSigningCheckNow(bare, now, hw, Hash32(0xf2)).ok);
            BOOST_CHECK(PosSigningCheckNow(bare, now + round, hw, Hash32(0xf2)).ok);
            now += 100 * round;
        } else {
            // A block this member signed and never saw certified escapes for
            // as long as its anchor stands, as the share-lock always has.
            ++g_revivals;
            BOOST_CHECK(PosSigningCheckNow(rival, now).ok);
        }
    }
    // A watcher's mark from a run that watched Bitcoin: no verdict can come
    // here, so it is an invalid block like any other.
    Certify(0xda, 180, 0xe5, 1500, PosCertRoute::INDEX_AT_START);
    g_index[Hash32(0xda)] = PosIndexStanding::ORPHANED;
    const PosSigningEntry rival = Anchored(PosSignKind::SHARE, 180, 0xdb, 0xe6, 1500);
    BOOST_CHECK(!PosSigningCheckNow(rival, now).ok);
    BOOST_CHECK(PosSigningCheckNow(rival, now + round).ok);
    // A valid block off the active chain binds nothing here (unchanged).
    Certify(0xdc, 181, 0xe5, 1500, PosCertRoute::INDEX_AT_START);
    g_index[Hash32(0xdc)] = PosIndexStanding::OFF_ACTIVE;
    BOOST_CHECK(PosSigningCheckNow(Anchored(PosSignKind::SHARE, 181, 0xdd, 0xe6, 1500), now).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(an_invalidated_certified_block_releases_after_one_round_across_a_restart)
{
    AnchoredChain anchored;
    Fresh();
    const int64_t round = PosSigningRoundMs();
    // The start scan loads a certified block that failed after it connected.
    Certify(0x60, 130, 0xf0, 1100, PosCertRoute::INDEX_AT_START);
    g_index[Hash32(0x60)] = PosIndexStanding::FAILED;
    const PosSigningEntry rival = Anchored(PosSignKind::SHARE, 130, 0x61, 0xf1, 1100);
    BOOST_CHECK(!PosSigningCheckNow(rival, 1000).ok);
    BOOST_CHECK(PosSigningCheckNow(rival, 1000 + round).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_certified_block_the_anchor_watcher_dropped_binds_nothing)
{
    // X's own anchor stands, but Bitcoin replaced its parent's, and the anchor
    // watcher invalidated the parent and X with it.
    AnchoredChain anchored;
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::CERTIFIED, 140, 0x70, 0x90, 1200)));
    Certify(0x70, 140, 0x90, 1200, PosCertRoute::CONNECTED, Hash32(0x6f));
    g_index[Hash32(0x70)] = PosIndexStanding::ACTIVE;
    const PosSigningEntry y = Anchored(PosSignKind::SHARE, 140, 0x71, 0x91, 1201);
    BOOST_CHECK(!PosSigningCheckNow(y, 1000).ok);
    Orphan(0x70, 0x8f, 1199, PosAnchorStanding::GONE);
    // Signed at once: no escape round is owed to a block Bitcoin dropped.
    BOOST_CHECK(PosSigningCheckNow(y, 1000).ok);
    BOOST_CHECK(PosSigningCheckNow(Anchored(PosSignKind::PROPOSAL, 140, 0x72, 0x91, 1201), 1000).ok);
    BOOST_CHECK(!PosSigningSignedAt(140));
    BOOST_CHECK_EQUAL(StateOf(Hash32(0x70)), "anchor gone");
    // Bitcoin returns and the watcher revives them: X binds for good again.
    g_index[Hash32(0x70)] = PosIndexStanding::ACTIVE;
    const PosSignCheck c = PosSigningCheckNow(y, 1000 + 50 * PosSigningRoundMs());
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.bound == Hash32(0x70));
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(an_anchor_watchers_mark_releases_at_once_only_on_a_current_verdict)
{
    // X certified at 145, dropped by the anchor watcher for its parent's
    // anchor B (a mark that persists on disk). Released at once only while
    // the parent chain says now that B is gone.
    AnchoredChain anchored;
    Fresh();
    const int64_t round = PosSigningRoundMs();
    const PosSigningEntry y = Anchored(PosSignKind::SHARE, 145, 0xa1, 0xb1, 1251);
    const auto check_x = [&](int64_t now) { return PosSigningCheckNow(y, now); };
    for (const bool restarted : {false, true}) {
        BOOST_TEST_MESSAGE((restarted ? "the mark and the certificate read from the disk" : "in this run"));
        std::string error;
        if (restarted) {
            BOOST_REQUIRE(Reload(error));
            Certify(0xa0, 145, 0xb0, 1250, PosCertRoute::INDEX_AT_START, Hash32(0x9f));
        } else {
            BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::CERTIFIED, 145, 0xa0, 0xb0, 1250)));
            Certify(0xa0, 145, 0xb0, 1250, PosCertRoute::CONNECTED, Hash32(0x9f));
        }
        const int64_t t = restarted ? 100000 : 1000;
        // The daemon does not answer: no current verdict, so X binds as a
        // block whose anchor is not judged yet does, round after round.
        Orphan(0xa0, 0xbf, 1249, PosAnchorStanding::UNDECIDED);
        PosSignCheck c = check_x(t);
        BOOST_CHECK(!c.ok);
        BOOST_CHECK(c.bound == Hash32(0xa0));
        BOOST_CHECK(c.why.find("has not judged the Bitcoin anchor that the anchor watcher dropped it for") != std::string::npos);
        BOOST_CHECK(c.why.find(Hash32(0xbf).GetHex()) != std::string::npos);
        BOOST_CHECK_EQUAL(StateOf(Hash32(0xa0)), "undecided");
        BOOST_CHECK(!check_x(t + 50 * round).ok);
        BOOST_CHECK(!PosSigningCheckNow(Entry(PosSignKind::PARTIAL, -1, 0xa2, 0), t + 50 * round, 144, Hash32(0x9f)).ok);
        BOOST_CHECK(!PosSigningCheckNow(Anchored(PosSignKind::PROPOSAL, 145, 0xa3, 0xb1, 1251), t + 50 * round).ok);
        // Bitcoin stands on B: the mark is stale, and X binds for good.
        g_verdicts[Hash32(0xbf)] = PosAnchorStanding::STANDS;
        c = check_x(t + 60 * round);
        BOOST_CHECK(!c.ok);
        BOOST_CHECK(c.why.find("holds a full certificate") != std::string::npos);
        BOOST_CHECK_EQUAL(StateOf(Hash32(0xa0)), "certified");
        BOOST_CHECK(!check_x(t + 120 * round).ok);
        // ... unless a block below it failed for another reason: then one round.
        g_also_failed.insert(Hash32(0xa0));
        BOOST_CHECK(!check_x(t + 200 * round).ok);
        BOOST_CHECK(check_x(t + 201 * round).ok);
        g_also_failed.clear();
        // Bitcoin replaced B, as the daemon says now: free at once.
        Orphan(0xa0, 0xbf, 1249, PosAnchorStanding::GONE);
        BOOST_CHECK(check_x(t + 300 * round).ok);
        BOOST_CHECK_EQUAL(StateOf(Hash32(0xa0)), "anchor gone");
        // A current verdict that X's own anchor is gone frees it too, while B
        // is undecided.
        g_verdicts[Hash32(0xbf)] = PosAnchorStanding::UNDECIDED;
        g_verdicts[Hash32(0xb0)] = PosAnchorStanding::GONE;
        BOOST_CHECK(check_x(t + 400 * round).ok);
        g_verdicts.erase(Hash32(0xb0));
    }
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(each_anchor_is_asked_once_per_check)
{
    // X certified at 146, dropped by the anchor watcher for its own anchor B,
    // and Z, signed here at the same height on the same anchor: one check
    // asks the parent chain once, about B once.
    AnchoredChain anchored;
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::CERTIFIED, 146, 0xc0, 0xc8, 1260)));
    Certify(0xc0, 146, 0xc8, 1260, PosCertRoute::CONNECTED, Hash32(0xbf));
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, 146, 0xc1, 0xc8, 1260)));
    Orphan(0xc0, 0xc8, 1260, PosAnchorStanding::UNDECIDED);
    g_judge_calls = 0;
    g_asked.clear();
    const PosSignCheck c = PosSigningCheckNow(Anchored(PosSignKind::SHARE, 146, 0xc2, 0xc9, 1261), 1000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK_EQUAL(g_judge_calls, 1U);
    BOOST_REQUIRE_EQUAL(g_asked.size(), 1U);
    BOOST_CHECK(g_asked[0] == std::make_pair(uint32_t{1260}, Hash32(0xc8)));
    // getsigningrecord judges every held block with one question too.
    g_judge_calls = 0;
    g_asked.clear();
    BOOST_CHECK_EQUAL(PosSigningHeldBlocks().size(), 2U);
    BOOST_CHECK_EQUAL(g_judge_calls, 1U);
    BOOST_CHECK_EQUAL(g_asked.size(), 1U);
    // An answer of the wrong length is no verdict: undecided, X binds.
    PosSigningSetAnchorJudge([](const std::vector<std::pair<uint32_t, uint256>>&) -> std::optional<std::vector<PosAnchorStanding>> {
        return std::vector<PosAnchorStanding>{};
    });
    BOOST_CHECK(!PosSigningCheckNow(Anchored(PosSignKind::SHARE, 146, 0xc2, 0xc9, 1261), 1000).ok);
    BOOST_CHECK_EQUAL(StateOf(Hash32(0xc0)), "undecided");
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_certificate_after_an_invalidation_releases_after_one_round)
{
    AnchoredChain anchored;
    Fresh();
    const int64_t round = PosSigningRoundMs();
    const PosSigningEntry y = Anchored(PosSignKind::SHARE, 150, 0x81, 0x95, 1300);
    // The operator invalidated X (its header only was here); then X's
    // verified certificate arrives.
    g_index[Hash32(0x80)] = PosIndexStanding::FAILED;
    Certify(0x80, 150, 0x95, 1300, PosCertRoute::CERTIFICATE, Hash32(0x7f));
    PosSignCheck c = PosSigningCheckNow(y, 1000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("invalid on this node") != std::string::npos);
    BOOST_CHECK(PosSigningCheckNow(y, 1000 + round).ok);
    // A certificate for a block the index does not hold, above a parent the
    // operator invalidated: the same.
    g_index[Hash32(0x83)] = PosIndexStanding::FAILED;
    Certify(0x84, 151, 0x96, 1301, PosCertRoute::CERTIFICATE, Hash32(0x83));
    const PosSigningEntry z = Anchored(PosSignKind::SHARE, 151, 0x85, 0x96, 1301);
    BOOST_CHECK(!PosSigningCheckNow(z, 2000).ok);
    BOOST_CHECK(PosSigningCheckNow(z, 2000 + round).ok);
    // ... and above a parent the anchor watcher dropped: free at once.
    Orphan(0x86, 0x9f, 1301, PosAnchorStanding::GONE);
    Certify(0x87, 152, 0x97, 1302, PosCertRoute::CERTIFICATE, Hash32(0x86));
    BOOST_CHECK(PosSigningCheckNow(Anchored(PosSignKind::SHARE, 152, 0x88, 0x97, 1302), 3000).ok);
    // Above a valid parent it binds for good.
    g_index[Hash32(0x89)] = PosIndexStanding::ACTIVE;
    Certify(0x8a, 153, 0x98, 1303, PosCertRoute::CERTIFICATE, Hash32(0x89));
    BOOST_CHECK(!PosSigningCheckNow(Anchored(PosSignKind::SHARE, 153, 0x8b, 0x98, 1303), 4000 + 50 * round).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_bare_hash_escapes_an_invalidated_certified_block_after_one_round)
{
    AnchoredChain anchored;
    Fresh();
    const int64_t round = PosSigningRoundMs();
    // X at 1, certified, invalidated: the tip is the genesis block.
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::PARTIAL, 1, 0xc0, 0x99, 1400)));
    Certify(0xc0, 1, 0x99, 1400, PosCertRoute::CONNECTED, Hash32(0xbf));
    g_index[Hash32(0xc0)] = PosIndexStanding::ACTIVE;
    const PosSigningEntry bare = Entry(PosSignKind::PARTIAL, -1, 0xc1, 0);
    // Standing, it refuses a bare hash for good.
    BOOST_CHECK(!PosSigningCheckNow(bare, 1000, 1, Hash32(0xc0)).ok);
    BOOST_CHECK(!PosSigningCheckNow(bare, 1000 + 50 * round, 1, Hash32(0xc0)).ok);
    g_index[Hash32(0xc0)] = PosIndexStanding::FAILED;
    PosSignCheck c = PosSigningCheckNow(bare, 10000, 0, Hash32(0xbf));
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.bound == Hash32(0xc0));
    BOOST_CHECK(c.why.find("give the block") != std::string::npos);
    BOOST_CHECK(!PosSigningCheckNow(bare, 10000 + round - 1, 0, Hash32(0xbf)).ok);
    BOOST_CHECK(PosSigningCheckNow(bare, 10000 + round, 0, Hash32(0xbf)).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_FIXTURE_TEST_CASE(the_start_scan_reads_only_the_window_of_a_testnet_sized_index, TestingSetup)
{
    // An index of the testnet's size (about 214,000 blocks), one Bitcoin
    // block per ten (60 s blocks), every block certified; and two certified
    // blocks off the active chain, one in the window and one below it.
    constexpr int N = 214000;
    const bool saved_pos = g_con_pos, saved_anchor = g_con_bitcoin_anchor;
    g_con_pos = true;
    g_con_bitcoin_anchor = true;
    PosSigningRecordResetForTesting();
    ChainstateManager& chainman = *m_node.chainman;
    std::vector<std::unique_ptr<CBlockIndex>> made;
    std::vector<uint256> hashes;
    LOCK(cs_main);
    CBlockIndex* const old_tip = chainman.ActiveChain().Tip();
    CBlockIndex* prev = nullptr;
    const auto add = [&](CBlockIndex* parent, int height, uint32_t anchor) {
        made.push_back(std::make_unique<CBlockIndex>());
        CBlockIndex* p = made.back().get();
        p->pprev = parent;
        p->nHeight = height;
        p->m_anchor_height = anchor;
        p->m_anchor_hash = GetRandHash();
        p->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        p->m_pos_countersigs = 3;
        p->m_pos_certified = true;
        p->m_pos_cert_known = true;
        const auto it = chainman.m_blockman.m_block_index.emplace(GetRandHash(), p).first;
        p->phashBlock = &it->first;
        hashes.push_back(it->first);
        return p;
    };
    for (int h = 0; h < N; ++h) prev = add(prev, h, (uint32_t)(h / 10));
    for (CBlockIndex* p = prev; p; p = p->pprev) p->BuildSkip();
    chainman.ActiveChain().SetTip(prev);
    const uint32_t tip_anchor = prev->m_anchor_height;
    CBlockIndex* side_in = add(prev->GetAncestor(N - 100)->pprev, N - 100, tip_anchor - 10);
    CBlockIndex* side_out = add(prev->GetAncestor(N - 5000)->pprev, N - 5000, tip_anchor - 500);
    // A side block in the window that failed after it connected (an
    // operator's invalidateblock), and an unmarked child above it; and a side
    // block the anchor watcher dropped.
    CBlockIndex* side_failed = add(prev->GetAncestor(N - 50)->pprev, N - 50, tip_anchor - 5);
    side_failed->nStatus |= BLOCK_FAILED_VALID;
    CBlockIndex* side_child = add(side_failed, N - 49, tip_anchor - 5);
    CBlockIndex* side_orphaned = add(prev->GetAncestor(N - 40)->pprev, N - 40, tip_anchor - 4);
    side_orphaned->nStatus |= BLOCK_FAILED_VALID | BLOCK_FAILED_ANCHOR;
    const auto t0 = std::chrono::steady_clock::now();
    PosSigningNoteIndexAtStart(chainman);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    BOOST_TEST_MESSAGE("start scan over " << chainman.m_blockman.m_block_index.size() << " index entries: " << ms << " ms");
    size_t held = 0, side = 0;
    for (const PosSigningHeld& h : PosSigningHeldBlocks()) {
        ++held;
        BOOST_CHECK(h.certified && !h.signed_here);
        side += h.hash == side_in->GetBlockHash() || h.hash == side_failed->GetBlockHash() ||
                h.hash == side_child->GetBlockHash() || h.hash == side_orphaned->GetBlockHash();
        BOOST_CHECK(h.hash != side_out->GetBlockHash());
        BOOST_CHECK(tip_anchor <= h.anchor_height + POS_SIGNING_RECORD_WINDOW);
    }
    // The window: anchors from tip_anchor - 144, ten blocks each, plus the
    // tip's own partial anchor; and the four side blocks in it, the failed
    // ones included.
    BOOST_CHECK_EQUAL(held, (size_t)(POS_SIGNING_RECORD_WINDOW * 10 + (N - 1) % 10 + 1 + 4));
    BOOST_CHECK_EQUAL(side, 4U);
    // What the block index says of each, as the check reads it.
    const auto standing = [&](const CBlockIndex* p) { return PosSigningIndexStanding(chainman, p->GetBlockHash(), uint256()).standing; };
    BOOST_CHECK(standing(prev) == PosIndexStanding::ACTIVE);
    BOOST_CHECK(standing(side_in) == PosIndexStanding::OFF_ACTIVE);
    BOOST_CHECK(standing(side_failed) == PosIndexStanding::FAILED);
    BOOST_CHECK(standing(side_child) == PosIndexStanding::FAILED); // through its parent, with no mark of its own
    BOOST_CHECK(standing(side_orphaned) == PosIndexStanding::ORPHANED);
    // The watcher's mark names the anchor it dropped the block for, read at
    // the bottom of the failed run, for a child with no mark of its own too;
    // a run below it that failed for another reason is not hidden by it.
    CBlockIndex* orphan_child = add(side_orphaned, N - 39, tip_anchor - 3);
    orphan_child->nStatus |= BLOCK_FAILED_CHILD;
    for (const CBlockIndex* p : {(const CBlockIndex*)side_orphaned, (const CBlockIndex*)orphan_child}) {
        const PosIndexState st = PosSigningIndexStanding(chainman, p->GetBlockHash(), uint256());
        BOOST_REQUIRE_EQUAL(st.orphan_anchors.size(), 1U);
        BOOST_CHECK_EQUAL(st.orphan_anchors[0].first, side_orphaned->m_anchor_height);
        BOOST_CHECK(st.orphan_anchors[0].second == side_orphaned->m_anchor_hash);
        BOOST_CHECK(!st.failed_otherwise);
    }
    CBlockIndex* over_failed = add(side_child, N - 48, tip_anchor - 4);
    over_failed->nStatus |= BLOCK_FAILED_VALID | BLOCK_FAILED_ANCHOR;
    const PosIndexState mixed = PosSigningIndexStanding(chainman, over_failed->GetBlockHash(), uint256());
    BOOST_CHECK(mixed.standing == PosIndexStanding::ORPHANED);
    BOOST_CHECK_EQUAL(mixed.orphan_anchors.size(), 1U);
    BOOST_CHECK(mixed.failed_otherwise);
    // A block the index does not hold stands as its parent.
    BOOST_CHECK(PosSigningIndexStanding(chainman, GetRandHash(), prev->GetBlockHash()).standing == PosIndexStanding::OFF_ACTIVE);
    BOOST_CHECK(PosSigningIndexStanding(chainman, GetRandHash(), side_child->GetBlockHash()).standing == PosIndexStanding::FAILED);
    BOOST_CHECK(PosSigningIndexStanding(chainman, GetRandHash(), GetRandHash()).standing == PosIndexStanding::UNKNOWN);
    BOOST_CHECK(ms < 1000.0);
    // What a node that never signs pays: one note per connected block, and at
    // each block index write a pass over the blocks the window keeps.
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        PosSigningNoteCertificate(PosCertifiedBlock{GetRandHash(), N + i, uint256(), tip_anchor, GetRandHash()}, PosCertRoute::CONNECTED);
    }
    const auto t2 = std::chrono::steady_clock::now();
    PosSigningRecordIndexWritten(N + 1000, tip_anchor, [](const uint256&) { return true; });
    const auto t3 = std::chrono::steady_clock::now();
    using us = std::chrono::duration<double, std::micro>;
    const double per_block = us(t2 - t1).count() / 1000, per_write = us(t3 - t2).count();
    const size_t kept = PosSigningHeldBlocks().size();
    BOOST_TEST_MESSAGE("per connected block: " << per_block << " us; one block index write with " << kept
                       << " certified blocks kept: " << per_write << " us");
    BOOST_CHECK(PosSigningRecordEntries().empty());
    // Leave the index as it was.
    chainman.ActiveChain().SetTip(old_tip);
    for (const uint256& h : hashes) chainman.m_blockman.m_block_index.erase(h);
    PosSigningRecordResetForTesting();
    g_con_pos = saved_pos;
    g_con_bitcoin_anchor = saved_anchor;
}

BOOST_FIXTURE_TEST_CASE(a_revival_counts_only_for_the_block_it_revives, TestingSetup)
{
    // Off the active chain (genesis): W fails (an operator's invalidateblock)
    // with X above it, failed through W, and Y above X; A fails on a branch of
    // its own; V is a valid side block. X's revival number moves only when
    // the marks of X or of an ancestor of X are cleared.
    // The entries go into this test's block index, which owns them and frees
    // them with the fixture.
    ChainstateManager& chainman = *m_node.chainman;
    LOCK(cs_main);
    CChainState& cs = chainman.ActiveChainstate();
    CBlockIndex* const genesis = chainman.ActiveChain().Tip();
    const auto add = [&](CBlockIndex* parent, uint32_t status) {
        CBlockIndex* p = new CBlockIndex();
        p->pprev = parent;
        p->nHeight = parent->nHeight + 1;
        p->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA | status;
        const auto it = chainman.m_blockman.m_block_index.emplace(GetRandHash(), p).first;
        p->phashBlock = &it->first;
        p->BuildSkip();
        return p;
    };
    CBlockIndex* w = add(genesis, BLOCK_FAILED_VALID);
    CBlockIndex* x = add(w, BLOCK_FAILED_CHILD);
    CBlockIndex* y = add(x, BLOCK_FAILED_CHILD);
    CBlockIndex* a = add(genesis, BLOCK_FAILED_VALID);
    CBlockIndex* v = add(genesis, 0);
    const auto revivals = [&](const CBlockIndex* p) {
        return PosSigningIndexStanding(chainman, p->GetBlockHash(), uint256()).revivals;
    };
    const auto fail_again = [&] {
        w->nStatus |= BLOCK_FAILED_VALID;
        x->nStatus |= BLOCK_FAILED_CHILD;
        y->nStatus |= BLOCK_FAILED_CHILD;
    };
    const uint64_t r0 = revivals(x);
    BOOST_CHECK_EQUAL(r0, 0U);
    // A no-op (a valid block) and another branch: unchanged.
    cs.ResetBlockFailureFlags(v);
    BOOST_CHECK_EQUAL(revivals(x), r0);
    cs.ResetBlockFailureFlags(a);
    BOOST_CHECK(!(a->nStatus & BLOCK_FAILED_MASK));
    BOOST_CHECK_EQUAL(revivals(x), r0);
    BOOST_CHECK(revivals(a) > r0);
    // The failed ancestry that made X fail: moved.
    cs.ResetBlockFailureFlags(w);
    const uint64_t r1 = revivals(x);
    BOOST_CHECK(r1 > r0);
    // X itself, and a descendant (which clears its failed ancestors): moved.
    fail_again();
    cs.ResetBlockFailureFlags(x);
    const uint64_t r2 = revivals(x);
    BOOST_CHECK(r2 > r1);
    fail_again();
    cs.ResetBlockFailureFlags(y);
    const uint64_t r3 = revivals(x);
    BOOST_CHECK(r3 > r2);
    // A certificate only, over X: revived as X is.
    BOOST_CHECK_EQUAL(PosSigningIndexStanding(chainman, GetRandHash(), x->GetBlockHash()).revivals, r3);
    // The anchor watcher's revival that marks the block failed again in the
    // same hold (its anchor is still off Bitcoin's best chain): nobody saw it
    // valid, so unchanged.
    fail_again();
    cs.ReconsiderAnchoredBlock(x, [&](const CBlockIndex* p) { return p != w; });
    BOOST_CHECK(w->nStatus & BLOCK_FAILED_MASK);
    BOOST_CHECK(x->nStatus & BLOCK_FAILED_MASK);
    BOOST_CHECK_EQUAL(revivals(x), r3);
    // ... and one that leaves it valid: moved.
    cs.ReconsiderAnchoredBlock(x, [](const CBlockIndex*) { return true; });
    BOOST_CHECK(!(x->nStatus & BLOCK_FAILED_MASK));
    const uint64_t r4 = revivals(x);
    BOOST_CHECK(r4 > r3);
    // X fails again; a later revival that clears nothing (of a valid block,
    // here V, by the watcher's path) does not take back the revival of X that
    // happened.
    fail_again();
    cs.ReconsiderAnchoredBlock(v, [](const CBlockIndex*) { return true; });
    BOOST_CHECK_EQUAL(revivals(x), r4);
    // A block that arrives above a revived one was revived with it.
    CBlockHeader above_y;
    above_y.hashPrevBlock = y->GetBlockHash();
    above_y.nTime = y->nTime + 1;
    const CBlockIndex* z = chainman.m_blockman.AddToBlockIndex(above_y);
    BOOST_CHECK(z->pprev == y);
    BOOST_CHECK_EQUAL(revivals(z), r4);
}

BOOST_FIXTURE_TEST_CASE(no_number_of_revivals_elsewhere_moves_a_block, TestingSetup)
{
    // X is failed through W (an operator's invalidateblock), off the active
    // chain; A is a block on another branch that fails and is revived again
    // and again, far more often than any bound on a history of revivals
    // (4,096 on the head this replaced). X's number never moves, and the
    // revival of X's own ancestry still moves it afterwards.
    ChainstateManager& chainman = *m_node.chainman;
    LOCK(cs_main);
    CChainState& cs = chainman.ActiveChainstate();
    CBlockIndex* const genesis = chainman.ActiveChain().Tip();
    const auto add = [&](CBlockIndex* parent, uint32_t status) {
        CBlockIndex* p = new CBlockIndex();
        p->pprev = parent;
        p->nHeight = parent->nHeight + 1;
        p->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA | status;
        const auto it = chainman.m_blockman.m_block_index.emplace(GetRandHash(), p).first;
        p->phashBlock = &it->first;
        p->BuildSkip();
        return p;
    };
    const auto revivals = [&](const CBlockIndex* p) {
        return PosSigningIndexStanding(chainman, p->GetBlockHash(), uint256()).revivals;
    };
    CBlockIndex* w = add(genesis, 0);
    CBlockIndex* x = add(w, 0);
    CBlockIndex* a = add(genesis, 0);
    // X revived once, then failed again: its number is that revival's.
    w->nStatus |= BLOCK_FAILED_VALID;
    x->nStatus |= BLOCK_FAILED_CHILD;
    cs.ResetBlockFailureFlags(w);
    const uint64_t rx = revivals(x);
    BOOST_CHECK(rx > 0);
    w->nStatus |= BLOCK_FAILED_VALID;
    x->nStatus |= BLOCK_FAILED_CHILD;
    for (int i = 1; i <= 5000; ++i) {
        a->nStatus |= BLOCK_FAILED_VALID;
        cs.ResetBlockFailureFlags(a);
        if (revivals(x) != rx) {
            BOOST_ERROR("revival " << i << " of another branch moved X's number from " << rx << " to " << revivals(x));
            break;
        }
    }
    BOOST_CHECK(revivals(a) > rx);
    // A block never revived keeps 0 through all of it.
    BOOST_CHECK_EQUAL(revivals(add(genesis, 0)), 0U);
    cs.ResetBlockFailureFlags(w);
    BOOST_CHECK(revivals(x) > revivals(a));
}

BOOST_AUTO_TEST_SUITE_END()
