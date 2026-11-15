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
void Certify(unsigned char tag, int height, unsigned char anchor_tag, uint32_t anchor_height, PosCertRoute route)
{
    PosSigningNoteCertificate(PosCertifiedBlock{Hash32(tag), height, anchor_height, Hash32(anchor_tag)}, route);
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
    Certify(0x20, 80, 0xb0, 900, PosCertRoute::INDEX_AT_START_ACTIVE);
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
    PosSigningSetAnchorJudge([](uint32_t, const uint256&) -> std::optional<PosAnchorStanding> { return std::nullopt; });
    Fresh();
    const PosSigningEntry rival = Anchored(PosSignKind::SHARE, 120, 0x51, 0xe1, 1000);
    // A certificate learnt in this run binds before its block connects ...
    Certify(0x50, 120, 0xe0, 1000, PosCertRoute::CERTIFICATE);
    BOOST_CHECK(!PosSigningCheckNow(rival, 1000).ok);
    // ... and while it is on the active chain ...
    Certify(0x50, 120, 0xe0, 1000, PosCertRoute::CONNECTED);
    BOOST_CHECK(!PosSigningCheckNow(rival, 1000 + 50 * PosSigningRoundMs()).ok);
    // ... and not once the node has left it (how it follows a Bitcoin reorganization).
    PosSigningNoteDisconnected(Hash32(0x50), 120);
    BOOST_CHECK(PosSigningCheckNow(rival, 2000).ok);
    // After a restart: a block the index holds on the active chain binds; one
    // off the active chain binds nothing (this node cannot judge its anchor).
    std::string error;
    BOOST_REQUIRE(Reload(error));
    Certify(0x50, 120, 0xe0, 1000, PosCertRoute::INDEX_AT_START_ACTIVE);
    BOOST_CHECK(!PosSigningCheckNow(rival, 3000).ok);
    BOOST_REQUIRE(Reload(error));
    Certify(0x50, 120, 0xe0, 1000, PosCertRoute::INDEX_AT_START);
    BOOST_CHECK(PosSigningCheckNow(rival, 3000).ok);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(an_invalidated_certified_block_releases_after_one_round_across_a_restart)
{
    AnchoredChain anchored;
    Fresh();
    const int64_t round = PosSigningRoundMs();
    Certify(0x60, 130, 0xf0, 1100, PosCertRoute::INDEX_AT_START_INVALID);
    const PosSigningEntry rival = Anchored(PosSignKind::SHARE, 130, 0x61, 0xf1, 1100);
    BOOST_CHECK(!PosSigningCheckNow(rival, 1000).ok);
    BOOST_CHECK(PosSigningCheckNow(rival, 1000 + round).ok);
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
    const auto t0 = std::chrono::steady_clock::now();
    PosSigningNoteIndexAtStart(chainman);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    BOOST_TEST_MESSAGE("start scan over " << chainman.m_blockman.m_block_index.size() << " index entries: " << ms << " ms");
    size_t held = 0, side = 0;
    for (const PosSigningHeld& h : PosSigningHeldBlocks()) {
        ++held;
        BOOST_CHECK(h.certified && !h.signed_here);
        side += h.hash == side_in->GetBlockHash();
        BOOST_CHECK(h.hash != side_out->GetBlockHash());
        BOOST_CHECK(tip_anchor <= h.anchor_height + POS_SIGNING_RECORD_WINDOW);
    }
    // The window: anchors from tip_anchor - 144, ten blocks each, plus the tip's own partial anchor; and the side block in it.
    BOOST_CHECK_EQUAL(held, (size_t)(POS_SIGNING_RECORD_WINDOW * 10 + (N - 1) % 10 + 1 + 1));
    BOOST_CHECK_EQUAL(side, 1U);
    BOOST_CHECK(ms < 1000.0);
    // What a node that never signs pays: one note per connected block, and at
    // each block index write a pass over the blocks the window keeps.
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        PosSigningNoteCertificate(PosCertifiedBlock{GetRandHash(), N + i, tip_anchor, GetRandHash()}, PosCertRoute::CONNECTED);
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

BOOST_AUTO_TEST_SUITE_END()
