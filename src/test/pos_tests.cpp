// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos.h>

#include <key.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pos_tests, BasicTestingSetup)

static CPubKey MakeKey()
{
    CKey key;
    key.MakeNewKey(true);
    return key.GetPubKey();
}

// The election seed is a deterministic function of its inputs.
BOOST_AUTO_TEST_CASE(pos_seed_deterministic)
{
    uint256 a = uint256S("0x01");
    uint256 b = uint256S("0x02");
    BOOST_CHECK(ComputePosSeed(a, b, 5) == ComputePosSeed(a, b, 5));
    BOOST_CHECK(ComputePosSeed(a, b, 5) != ComputePosSeed(a, b, 6));
    BOOST_CHECK(ComputePosSeed(a, b, 5) != ComputePosSeed(b, a, 5));
    // The Bitcoin anchor is part of the seed (ties PoS to challenge 2).
    BOOST_CHECK(ComputePosSeed(a, b, 5) != ComputePosSeed(a, uint256S("0x03"), 5));
}

// The "<pubkey> OP_CHECKSIG" challenge round-trips, and malformed challenges
// are rejected.
BOOST_AUTO_TEST_CASE(pos_challenge_roundtrip)
{
    CPubKey pubkey = MakeKey();
    CScript challenge = BuildPosChallenge(pubkey);
    auto extracted = PosChallengeToPubKey(challenge);
    BOOST_REQUIRE(extracted.has_value());
    BOOST_CHECK(*extracted == pubkey);

    // Not a single-key checksig.
    BOOST_CHECK(!PosChallengeToPubKey(CScript() << OP_TRUE).has_value());
    BOOST_CHECK(!PosChallengeToPubKey(CScript() << ToByteVector(pubkey)).has_value());
    BOOST_CHECK(!PosChallengeToPubKey(CScript() << ToByteVector(pubkey) << OP_CHECKSIG << OP_TRUE).has_value());
}

// Every registered staker appears exactly once in the schedule, the ordering is
// deterministic, and a non-registered key has no rank.
BOOST_AUTO_TEST_CASE(pos_schedule_well_formed)
{
    StakeRegistry reg;
    std::vector<CPubKey> keys;
    for (int i = 0; i < 8; ++i) {
        CPubKey k = MakeKey();
        keys.push_back(k);
        reg.SetStake(k, 1);
    }
    uint256 seed = ComputePosSeed(uint256S("0xab"), uint256(), 1);
    std::vector<CPubKey> schedule = PosSchedule(reg, seed);
    BOOST_CHECK_EQUAL(schedule.size(), keys.size());

    // It is a permutation of the registered keys.
    std::set<CPubKey> as_set(schedule.begin(), schedule.end());
    BOOST_CHECK_EQUAL(as_set.size(), keys.size());
    for (const CPubKey& k : keys) BOOST_CHECK(as_set.count(k) == 1);

    // Deterministic and consistent with PosRank.
    BOOST_CHECK(PosSchedule(reg, seed) == schedule);
    for (size_t i = 0; i < schedule.size(); ++i) {
        auto rank = PosRank(reg, seed, schedule[i]);
        BOOST_REQUIRE(rank.has_value());
        BOOST_CHECK_EQUAL(*rank, i);
    }

    // A key that is not registered has no rank.
    BOOST_CHECK(!PosRank(reg, seed, MakeKey()).has_value());
}

// Higher stake weight wins rank 0 far more often across many seeds.
BOOST_AUTO_TEST_CASE(pos_weighting_is_proportional)
{
    StakeRegistry reg;
    CPubKey big = MakeKey();
    CPubKey small = MakeKey();
    reg.SetStake(big, 9);
    reg.SetStake(small, 1);

    int big_wins = 0;
    const int trials = 2000;
    for (int i = 0; i < trials; ++i) {
        uint256 seed = ComputePosSeed(uint256S("0xcd"), uint256(), i);
        if (PosSchedule(reg, seed).front() == big) big_wins++;
    }
    // Expect ~90% for the 9:1 weighting; allow generous slack for randomness.
    BOOST_CHECK(big_wins > trials * 0.80);
    BOOST_CHECK(big_wins < trials * 0.98);
}

// The schedule changes with the seed (so leaders reshuffle each block).
BOOST_AUTO_TEST_CASE(pos_schedule_reshuffles)
{
    StakeRegistry reg;
    for (int i = 0; i < 10; ++i) reg.SetStake(MakeKey(), 1);

    int differing_leaders = 0;
    CPubKey prev_leader;
    for (int i = 0; i < 20; ++i) {
        uint256 seed = ComputePosSeed(uint256S("0xef"), uint256(), i);
        CPubKey leader = PosSchedule(reg, seed).front();
        if (i > 0 && leader != prev_leader) differing_leaders++;
        prev_leader = leader;
    }
    // With 10 equal stakers the rank-0 leader should change most of the time.
    BOOST_CHECK(differing_leaders >= 10);
}

BOOST_AUTO_TEST_SUITE_END()
