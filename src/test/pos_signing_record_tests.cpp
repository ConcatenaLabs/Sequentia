// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos_signing_record.h>

#include <fs.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <cstdio>

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

void AppendBytes(const std::vector<unsigned char>& bytes)
{
    FILE* f = fsbridge::fopen(RecordFile(), "ab");
    BOOST_REQUIRE(f);
    BOOST_REQUIRE_EQUAL(fwrite(bytes.data(), 1, bytes.size(), f), bytes.size());
    fclose(f);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(pos_signing_record_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(load_rebuilds_the_share_lock)
{
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::PROPOSAL, 5, 0x10, 2)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 5, 0x20, 2)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::PROPOSAL, 6, 0x30, 0)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::CERTIFIED, 7, 0x40, 1)));
    PosSigningRecordResetForTesting();
    std::string error;
    BOOST_REQUIRE(PosSigningRecordLoad(error));
    BOOST_CHECK_EQUAL(PosSigningRecordEntries().size(), 4U);
    // A share or a certified block holds its height; a proposal does not.
    const auto at5 = PosSigningRecordRestoredLock(5);
    BOOST_REQUIRE(at5);
    BOOST_CHECK(at5->block_hash == uint256(std::vector<unsigned char>(32, 0x20)));
    BOOST_CHECK_EQUAL(at5->anchor_height, 105U);
    BOOST_CHECK(!PosSigningRecordRestoredLock(6));
    BOOST_CHECK(PosSigningRecordRestoredLock(7));
    PosSigningRecordClearRestoredLock(7);
    BOOST_CHECK(!PosSigningRecordRestoredLock(7));
    // The spends survive the round trip.
    BOOST_CHECK_EQUAL(PosSigningRecordEntries()[1].spends.size(), 2U);
}

BOOST_AUTO_TEST_CASE(torn_last_entry_is_dropped)
{
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 9, 0x50, 3)));
    const uint64_t whole = fs::file_size(RecordFile());
    // A write killed part-way: a frame header and a few payload bytes.
    AppendBytes({0x53, 0x47, 0x52, 0x31, 0xff, 0x00, 0x00, 0x00, 0x01, 0x02});
    PosSigningRecordResetForTesting();
    std::string error;
    BOOST_REQUIRE(PosSigningRecordLoad(error));
    BOOST_CHECK_EQUAL(PosSigningRecordEntries().size(), 1U);
    // Rewritten without the torn tail, so the next entry follows a whole one.
    BOOST_CHECK_EQUAL(fs::file_size(RecordFile()), whole);
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 10, 0x60, 0)));
    PosSigningRecordResetForTesting();
    BOOST_REQUIRE(PosSigningRecordLoad(error));
    BOOST_CHECK_EQUAL(PosSigningRecordEntries().size(), 2U);
}

BOOST_AUTO_TEST_CASE(damage_before_the_last_entry_stops_the_start)
{
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 11, 0x70, 1)));
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 12, 0x80, 1)));
    // Flip one byte inside the first entry's payload.
    FILE* f = fsbridge::fopen(RecordFile(), "r+b");
    BOOST_REQUIRE(f);
    BOOST_REQUIRE_EQUAL(fseek(f, 20, SEEK_SET), 0);
    const int c = fgetc(f);
    BOOST_REQUIRE_EQUAL(fseek(f, 20, SEEK_SET), 0);
    fputc(c ^ 0xff, f);
    fclose(f);
    PosSigningRecordResetForTesting();
    std::string error;
    BOOST_CHECK(!PosSigningRecordLoad(error));
    BOOST_CHECK(error.find("corrupt") != std::string::npos);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_CASE(a_removed_file_is_written_whole_again)
{
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 13, 0x90, 0)));
    fs::remove(RecordFile());
    BOOST_CHECK(PosSigningRecordAppend(Entry(PosSignKind::SHARE, 14, 0xa0, 0)));
    PosSigningRecordResetForTesting();
    std::string error;
    BOOST_REQUIRE(PosSigningRecordLoad(error));
    BOOST_CHECK_EQUAL(PosSigningRecordEntries().size(), 2U);
    fs::remove(RecordFile());
    PosSigningRecordResetForTesting();
}

BOOST_AUTO_TEST_SUITE_END()
