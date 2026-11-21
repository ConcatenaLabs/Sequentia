// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <btc_work.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>

using namespace BtcWork;

namespace {

constexpr uint32_t LIMIT = 0x1d00ffff;

uint256 Id(uint32_t height) { return ArithToUint256(arith_uint256(height) + 1); }

//! A chain from `first_height` to `last_height`: timestamps and bits set by the
//! caller for the first and last, the rest spaced evenly with the first's bits.
struct Chain {
    std::map<uint256, Ancestor> by_hash;
    Lookup lookup() const
    {
        return [this](const uint256& h) -> std::optional<Ancestor> {
            auto it = by_hash.find(h);
            if (it == by_hash.end()) return std::nullopt;
            return it->second;
        };
    }
    Ancestor& at(uint32_t height) { return by_hash[Id(height)]; }
};

Chain MakePeriod(uint32_t last_height, uint32_t first_time, uint32_t last_time, uint32_t first_bits, uint32_t last_bits)
{
    Chain c;
    const uint32_t first_height = last_height - 2015;
    for (uint32_t h = first_height; h <= last_height; ++h) {
        Ancestor a;
        a.hash = Id(h);
        a.prev = Id(h - 1);
        a.height = h;
        a.time = first_time + (uint64_t)(last_time - first_time) * (h - first_height) / 2015;
        a.bits = h == last_height ? last_bits : first_bits;
        c.by_hash[a.hash] = a;
    }
    return c;
}

Sidechain::Bitcoin::CBlockHeader MainnetBlock1()
{
    Sidechain::Bitcoin::CBlockHeader h;
    h.nVersion = 1;
    h.hashPrevBlock = uint256S("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    h.hashMerkleRoot = uint256S("0e3e2357e806b6cdb1f70b54c3a3a17b6714ee1f0e68bebb44a74b1efd512098");
    h.nTime = 1231469665;
    h.nBits = LIMIT;
    h.nNonce = 2573394689;
    return h;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(btc_work_tests, BasicTestingSetup)

// Bitcoin Core's retarget vectors (pow_tests.cpp), read through the period's
// real first and last blocks.
BOOST_AUTO_TEST_CASE(mainnet_retarget_vectors)
{
    struct V { uint32_t last_height, first_time, last_time, bits, want; };
    for (const V& v : {V{32255, 1261130161, 1262152739, 0x1d00ffff, 0x1d00d86a},
                       V{2015, 1231006505, 1233061996, 0x1d00ffff, 0x1d00ffff},
                       V{68543, 1279008237, 1279297671, 0x1c05a3f4, 0x1c0168fd},
                       V{46367, 1263163443, 1269211443, 0x1c387f6f, 0x1d00e1fd}}) {
        Chain c = MakePeriod(v.last_height, v.first_time, v.last_time, v.bits, v.bits);
        const auto got = RequiredBits(Net::MAIN, c.at(v.last_height), v.last_time + 600, false, c.lookup());
        BOOST_REQUIRE(got);
        BOOST_CHECK_EQUAL(*got, v.want);
    }
}

BOOST_AUTO_TEST_CASE(mainnet_inside_a_period_keeps_the_parents_bits)
{
    Chain c = MakePeriod(32255, 1261130161, 1262152739, 0x1c05a3f4, 0x1c05a3f4);
    const auto got = RequiredBits(Net::MAIN, c.at(32254), 1262152739 + 100000, false, c.lookup());
    BOOST_REQUIRE(got);
    BOOST_CHECK_EQUAL(*got, 0x1c05a3f4u);
}

BOOST_AUTO_TEST_CASE(testnet4_retarget_scales_the_first_blocks_target)
{
    // BIP94: a period ending on a minimum-difficulty block does not reset the
    // difficulty; on mainnet's formula the same period would.
    const uint32_t t0 = 1714777860;
    Chain c = MakePeriod(4031, t0, t0 + 1209600, 0x1c00ffff, LIMIT);
    const auto t4 = RequiredBits(Net::TESTNET4, c.at(4031), t0 + 1209600 + 600, false, c.lookup());
    const auto main = RequiredBits(Net::MAIN, c.at(4031), t0 + 1209600 + 600, false, c.lookup());
    BOOST_REQUIRE(t4 && main);
    BOOST_CHECK_EQUAL(*t4, 0x1c00ffffu);
    BOOST_CHECK_EQUAL(*main, LIMIT);
}

BOOST_AUTO_TEST_CASE(testnet4_minimum_difficulty_rule)
{
    const uint32_t t0 = 1714777860;
    Chain c = MakePeriod(4031, t0, t0 + 1000000, 0x1c00ffff, 0x1c00ffff);
    // The last three blocks used the exception.
    for (uint32_t h = 4027; h <= 4029; ++h) c.at(h).bits = LIMIT;
    Ancestor parent = c.at(4029);
    // Over 20 minutes after its parent: the exception applies.
    auto bits = RequiredBits(Net::TESTNET4, parent, parent.time + 1201, false, c.lookup());
    BOOST_REQUIRE(bits);
    BOOST_CHECK_EQUAL(*bits, LIMIT);
    // Exactly 20 minutes: it does not, and the rule walks back past the
    // minimum-difficulty blocks to the real difficulty.
    bits = RequiredBits(Net::TESTNET4, parent, parent.time + 1200, false, c.lookup());
    BOOST_REQUIRE(bits);
    BOOST_CHECK_EQUAL(*bits, 0x1c00ffffu);
    // The real difficulty ignores the exception whatever the timestamp.
    bits = RequiredBits(Net::TESTNET4, parent, parent.time + 100000, true, c.lookup());
    BOOST_REQUIRE(bits);
    BOOST_CHECK_EQUAL(*bits, 0x1c00ffffu);
    // The walk stops at the period's first block even when it is at the limit.
    Chain d = MakePeriod(4031, t0, t0 + 1000000, LIMIT, LIMIT);
    bits = RequiredBits(Net::TESTNET4, d.at(4029), d.at(4029).time + 100000, true, d.lookup());
    BOOST_REQUIRE(bits);
    BOOST_CHECK_EQUAL(*bits, LIMIT);
    // An ancestor that cannot be read is no verdict.
    Chain gap = MakePeriod(4031, t0, t0 + 1000000, LIMIT, LIMIT);
    gap.by_hash.erase(Id(4020));
    BOOST_CHECK(!RequiredBits(Net::TESTNET4, gap.at(4029), gap.at(4029).time + 100, false, gap.lookup()));
}

BOOST_AUTO_TEST_CASE(judge_a_real_header)
{
    const auto header = MainnetBlock1();
    BOOST_CHECK_EQUAL(header.GetHash().GetHex(), "00000000839a8e6886ab5951d76f411475428afc90947ee320161bbf18eb6048");
    Chain c;
    Ancestor parent;
    parent.hash = header.hashPrevBlock;
    parent.prev = Id(1000);
    parent.height = 1001;
    parent.time = header.nTime - 1201;
    parent.bits = 0x1c00ffff;
    c.by_hash[parent.hash] = parent;

    // Testnet4: valid by the 20-minute exception, and below the real difficulty.
    Verdict v = Judge(Net::TESTNET4, header, 1002, c.lookup());
    BOOST_CHECK(v.state == State::OK);
    BOOST_CHECK(!v.real);

    // The same header 20 minutes after its parent claims the wrong bits.
    c.by_hash[parent.hash].time = header.nTime - 1200;
    v = Judge(Net::TESTNET4, header, 1002, c.lookup());
    BOOST_CHECK(v.state == State::BAD);
    BOOST_CHECK_EQUAL(v.why, "btc-header-bits");

    // Mainnet at the parent's own difficulty: meets it, and it is the real one.
    c.by_hash[parent.hash].bits = LIMIT;
    v = Judge(Net::MAIN, header, 1002, c.lookup());
    BOOST_CHECK(v.state == State::OK);
    BOOST_CHECK(v.real);

    // The right bits and too little work.
    auto weak = header;
    weak.nNonce += 1;
    v = Judge(Net::MAIN, weak, 1002, c.lookup());
    BOOST_CHECK(v.state == State::BAD);
    BOOST_CHECK_EQUAL(v.why, "btc-header-work");

    // A height that is not its parent's plus one; an unknown parent.
    v = Judge(Net::MAIN, header, 1003, c.lookup());
    BOOST_CHECK_EQUAL(v.why, "btc-header-height");
    auto orphan = header;
    orphan.hashPrevBlock = Id(5);
    v = Judge(Net::MAIN, orphan, 1002, c.lookup());
    BOOST_CHECK(v.state == State::UNANSWERED);
}

BOOST_AUTO_TEST_CASE(regtest_keeps_the_parents_bits)
{
    Chain c = MakePeriod(4031, 1000000, 1001000, 0x207fffff, 0x207fffff);
    const auto bits = RequiredBits(Net::REGTEST, c.at(4031), 1001600, false, c.lookup());
    BOOST_REQUIRE(bits);
    BOOST_CHECK_EQUAL(*bits, 0x207fffffu);
}

BOOST_AUTO_TEST_SUITE_END()
