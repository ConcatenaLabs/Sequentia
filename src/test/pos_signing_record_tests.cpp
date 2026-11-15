// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_signing_record.h>

#include <pos.h>

#include <fs.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

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

struct AnchoredChain {
    const bool saved{g_con_bitcoin_anchor};
    AnchoredChain()
    {
        g_con_bitcoin_anchor = true;
        g_verdicts.clear();
        PosSigningSetAnchorJudge([](uint32_t, const uint256& anchor) -> std::optional<PosAnchorStanding> {
            auto it = g_verdicts.find(anchor);
            return it == g_verdicts.end() ? PosAnchorStanding::STANDS : it->second;
        });
    }
    ~AnchoredChain()
    {
        PosSigningSetAnchorJudge(nullptr);
        g_con_bitcoin_anchor = saved;
    }
};

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
    // A certificate at the held height ends it, and the file forgets it.
    PosSigningNoteCertificate(Hash32(0xc3), 21);
    BOOST_CHECK(PosSigningCheckNow(Entry(PosSignKind::SHARE, 21, 0xc1, 0), 1000).ok);
    BOOST_CHECK(!PosSigningGetDamagedHold().active);
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
    PosSigningNoteCertificate(Hash32(0x01), 40);
    c = PosSigningCheckNow(z, 40000 + 10 * round);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("that block is certified") != std::string::npos);
    BOOST_CHECK(!PosSigningCheckNow(z, 40000 + 20 * round).ok);
    // The operator invalidates X on this node: a recovery step, so the member
    // is released after one round, as for an uncertified block.
    PosSigningNoteInvalidated({Hash32(0x01)}, true);
    c = PosSigningCheckNow(z, 50000);
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("signs no other block there") != std::string::npos);
    BOOST_CHECK(PosSigningCheckNow(z, 50000 + round).ok);
    // Revived (reconsiderblock): the certificate holds the height again.
    PosSigningNoteInvalidated({Hash32(0x01)}, false);
    BOOST_CHECK(!PosSigningCheckNow(z, 60000).ok);
    // Another held block's invalidation changes nothing for X.
    PosSigningNoteInvalidated({Hash32(0x09)}, true);
    BOOST_CHECK(!PosSigningCheckNow(z, 70000).ok);
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

BOOST_AUTO_TEST_CASE(a_bare_hash_is_refused_while_a_signed_block_binds)
{
    AnchoredChain anchored;
    Fresh();
    // The tip is 59; this member signed X at 60.
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::PARTIAL, 60, 0x07, 0xa7, 700)));
    PosSigningEntry bare = Entry(PosSignKind::PARTIAL, -1, 0x08, 0);
    const PosSignCheck c = PosSigningCheckNow(bare, 1000, 59, Hash32(0x59));
    BOOST_CHECK(!c.ok);
    BOOST_CHECK(c.why.find("give the block") != std::string::npos);
    // Once X is the tip, a bare hash is free.
    BOOST_CHECK(PosSigningCheckNow(bare, 1000, 60, Hash32(0x07)).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_node_that_does_not_watch_bitcoin_holds_only_this_runs_blocks)
{
    AnchoredChain anchored;
    PosSigningSetAnchorJudge([](uint32_t, const uint256&) -> std::optional<PosAnchorStanding> { return std::nullopt; });
    Fresh();
    BOOST_CHECK(PosSigningRecordAppend(Anchored(PosSignKind::SHARE, 70, 0x09, 0xa9, 800)));
    BOOST_CHECK(!PosSigningCheckNow(Anchored(PosSignKind::SHARE, 70, 0x0a, 0xaa, 800), 1000).ok);
    std::string error;
    BOOST_REQUIRE(Reload(error));
    BOOST_CHECK(PosSigningCheckNow(Anchored(PosSignKind::SHARE, 70, 0x0a, 0xaa, 800), 1000).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_SUITE_END()
