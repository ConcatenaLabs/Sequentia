// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos.h>
#include <anchor.h>

#include <coins.h>
#include <key.h>
#include <musig.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <undo.h>
#include <vrf.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>

#include <boost/test/unit_test.hpp>

#include <cmath>
#include <limits>
#include <vector>

// These tests register small (weight 1..) stakes and assume permissive PoS
// globals. BasicTestingSetup selects the MAIN chain, which now sets the real
// Sequentia PoS parameters (g_pos_min_stake = 40,000 SEQ, committee 100, 30s
// slots) — under which weight-1 stakes are ineligible and the default committee
// is huge. Reset the PoS globals to test-friendly values per test so the suite
// is robust regardless of chain defaults and test ordering. Individual tests
// still override g_pos_committee_size / g_pos_slot_interval / g_pos_min_stake
// where they need specific values.
struct PosTestingSetup : public BasicTestingSetup {
    PosTestingSetup() {
        g_pos_min_stake = 0;
        g_pos_committee_size = DEFAULT_POS_COMMITTEE_SIZE;
        g_pos_slot_interval = DEFAULT_POS_SLOT_INTERVAL;
        g_pos_unbonding_period = DEFAULT_POS_UNBONDING_PERIOD;
        g_pos_public_committee = false;
    }
};

BOOST_FIXTURE_TEST_SUITE(pos_tests, PosTestingSetup)

static CPubKey MakeKey()
{
    CKey key;
    key.MakeNewKey(true);
    return key.GetPubKey();
}

// The election seed is a deterministic function of its inputs.
BOOST_AUTO_TEST_CASE(pos_seed_deterministic)
{
    uint256 b = uint256S("0x02");  // parent Bitcoin anchor hash
    BOOST_CHECK(ComputePosSeed(b, 5) == ComputePosSeed(b, 5));
    // The height is part of the seed.
    BOOST_CHECK(ComputePosSeed(b, 5) != ComputePosSeed(b, 6));
    // The Bitcoin anchor is part of the seed (ties PoS to challenge 2).
    BOOST_CHECK(ComputePosSeed(b, 5) != ComputePosSeed(uint256S("0x03"), 5));
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
    uint256 seed = ComputePosSeed(uint256(), 1);
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
        uint256 seed = ComputePosSeed(uint256(), i);
        if (PosSchedule(reg, seed).front() == big) big_wins++;
    }
    // Expect ~90% for the 9:1 weighting; allow generous slack for randomness.
    BOOST_CHECK(big_wins > trials * 0.80);
    BOOST_CHECK(big_wins < trials * 0.98);
}

// Escaping stall (whitepaper §3.8): sub-quorum certification is permitted only
// once the parent-chain anchor has advanced by the gap (the "h+3" rule).
BOOST_AUTO_TEST_CASE(pos_escaping_stall_gap)
{
    BOOST_CHECK_EQUAL(POS_ESCAPING_STALL_ANCHOR_GAP, 3U);
    // Parent block anchored at height 100.
    BOOST_CHECK(!PosEscapingStallAllowed(100, 100)); // no advance
    BOOST_CHECK(!PosEscapingStallAllowed(100, 102)); // only +2
    BOOST_CHECK(PosEscapingStallAllowed(100, 103));  // +3: stalled, allowed
    BOOST_CHECK(PosEscapingStallAllowed(100, 130));  // further forward, allowed
    // Genesis / anchoring-disabled (heights 0) never allows it.
    BOOST_CHECK(!PosEscapingStallAllowed(0, 0));
    BOOST_CHECK(PosEscapingStallAllowed(0, 3));
}

// The minimum-stake floor (whitepaper §3.3) excludes sub-minimum stakers from
// the schedule, the rank lookup, the eligible-total weight, and VRF committee
// membership; with the floor at 0 (default) every registered staker is eligible.
BOOST_AUTO_TEST_CASE(pos_min_stake_eligibility)
{
    StakeRegistry reg;
    CPubKey big = MakeKey();   // above the floor
    CPubKey small = MakeKey(); // below the floor
    reg.SetStake(big, 100);
    reg.SetStake(small, 10);
    uint256 seed = ComputePosSeed(uint256(), 1);

    // Default: no floor — both eligible, both in the schedule and total.
    g_pos_min_stake = 0;
    BOOST_CHECK_EQUAL(PosSchedule(reg, seed).size(), 2U);
    BOOST_CHECK(PosRank(reg, seed, small).has_value());
    BOOST_CHECK_EQUAL(PosTotalWeight(reg), 110U);

    // Floor at 50: only `big` qualifies. `small` drops out everywhere.
    g_pos_min_stake = 50;
    std::vector<CPubKey> sched = PosSchedule(reg, seed);
    BOOST_CHECK_EQUAL(sched.size(), 1U);
    BOOST_CHECK(sched.front() == big);
    BOOST_CHECK(PosRank(reg, seed, big).has_value());
    BOOST_CHECK(!PosRank(reg, seed, small).has_value());
    BOOST_CHECK_EQUAL(PosTotalWeight(reg), 100U); // eligible-only denominator
    BOOST_CHECK(PosIsEligibleStake(100));
    BOOST_CHECK(!PosIsEligibleStake(10));
    BOOST_CHECK(!PosIsEligibleStake(49));
    BOOST_CHECK(PosIsEligibleStake(50));

    // A sub-floor staker is never a VRF committee member, whatever its beta.
    uint256 lucky = uint256(); // beta 0 → slot 0, normally a member
    BOOST_CHECK(!PosVrfIsCommitteeMember(lucky, 10, PosTotalWeight(reg)));
    BOOST_CHECK(PosVrfIsCommitteeMember(lucky, 100, PosTotalWeight(reg)));

    g_pos_min_stake = 0; // restore global for other tests
}

// The committee-certification challenge round-trips, the committee is the
// schedule prefix, and the quorum is a strict majority.
BOOST_AUTO_TEST_CASE(pos_committee_challenge_roundtrip)
{
    BOOST_CHECK_EQUAL(PosQuorum(0), 0);
    BOOST_CHECK_EQUAL(PosQuorum(1), 1);
    BOOST_CHECK_EQUAL(PosQuorum(2), 2);
    BOOST_CHECK_EQUAL(PosQuorum(3), 2);
    BOOST_CHECK_EQUAL(PosQuorum(5), 3);
    BOOST_CHECK_EQUAL(PosQuorum(16), 9);

    CPubKey leader = MakeKey();
    std::vector<CPubKey> committee;
    for (int i = 0; i < 5; ++i) committee.push_back(MakeKey());

    CScript challenge = BuildPosBlockChallenge(leader, committee);
    auto parts = ParsePosBlockChallenge(challenge);
    BOOST_REQUIRE(parts.has_value());
    BOOST_CHECK(parts->leader == leader);
    BOOST_CHECK(parts->committee == committee);
    BOOST_CHECK_EQUAL(parts->quorum, PosQuorum(committee.size()));

    // A single-member (or empty) committee degrades to the leader-only form.
    CScript solo = BuildPosBlockChallenge(leader, {committee[0]});
    auto solo_parts = ParsePosBlockChallenge(solo);
    BOOST_REQUIRE(solo_parts.has_value());
    BOOST_CHECK(solo_parts->leader == leader);
    BOOST_CHECK(solo_parts->committee.empty());

    // Tampered scripts are rejected.
    CScript trailing = challenge;
    trailing << OP_TRUE;
    BOOST_CHECK(!ParsePosBlockChallenge(trailing).has_value());
    CScript wrong_count;
    wrong_count << ToByteVector(leader) << OP_CHECKSIGVERIFY << (int64_t)3
                << ToByteVector(committee[0]) << ToByteVector(committee[1])
                << (int64_t)3 << OP_CHECKMULTISIG; // claims 3 keys, has 2
    BOOST_CHECK(!ParsePosBlockChallenge(wrong_count).has_value());
}

// The aggregate-committee challenge (OP_1 <leader> <agg_key>) round-trips and
// is distinguished from the other challenge forms; malformed variants are
// rejected. See doc/sequentia/04-proof-of-stake.md §6.
BOOST_AUTO_TEST_CASE(pos_agg_challenge_roundtrip)
{
    CPubKey leader = MakeKey();
    std::vector<CPubKey> members;
    for (int i = 0; i < 5; ++i) members.push_back(MakeKey());
    auto agg = MuSigAggregatePubkey(members);
    BOOST_REQUIRE(agg.has_value());
    BOOST_REQUIRE_EQUAL(agg->size(), 32U);

    CScript challenge = BuildPosAggChallenge(leader, *agg);
    auto parts = ParsePosBlockChallenge(challenge);
    BOOST_REQUIRE(parts.has_value());
    BOOST_CHECK(parts->leader == leader);
    BOOST_CHECK(parts->agg_key == *agg);
    BOOST_CHECK(parts->committee.empty());

    // The other forms parse with an empty agg_key.
    auto solo = ParsePosBlockChallenge(BuildPosChallenge(leader));
    BOOST_REQUIRE(solo.has_value());
    BOOST_CHECK(solo->agg_key.empty());
    auto multi = ParsePosBlockChallenge(BuildPosBlockChallenge(leader, members));
    BOOST_REQUIRE(multi.has_value());
    BOOST_CHECK(multi->agg_key.empty());

    // Malformed: trailing data, wrong agg-key size, invalid leader key.
    CScript trailing = challenge;
    trailing << OP_TRUE;
    BOOST_CHECK(!ParsePosBlockChallenge(trailing).has_value());
    std::vector<unsigned char> short_key(31, 0x42);
    BOOST_CHECK(!ParsePosBlockChallenge(CScript() << OP_1 << ToByteVector(leader) << short_key).has_value());
    std::vector<unsigned char> bogus_leader(33, 0x02);
    bogus_leader[0] = 0x05; // invalid pubkey header byte
    BOOST_CHECK(!ParsePosBlockChallenge(CScript() << OP_1 << bogus_leader << *agg).has_value());

    // The aggregate is order-independent, so any committee ordering produces
    // the same challenge.
    std::vector<CPubKey> shuffled(members.rbegin(), members.rend());
    auto agg2 = MuSigAggregatePubkey(shuffled);
    BOOST_REQUIRE(agg2.has_value());
    BOOST_CHECK(*agg2 == *agg);
}

// The committee is the first g_pos_committee_size entries of the schedule.
BOOST_AUTO_TEST_CASE(pos_committee_is_schedule_prefix)
{
    StakeRegistry reg;
    for (int i = 0; i < 8; ++i) reg.SetStake(MakeKey(), 1);
    uint256 seed = ComputePosSeed(uint256(), 3);

    int old_size = g_pos_committee_size;
    g_pos_committee_size = 5;
    std::vector<CPubKey> schedule = PosSchedule(reg, seed);
    std::vector<CPubKey> committee = PosCommittee(reg, seed);
    g_pos_committee_size = old_size;

    BOOST_REQUIRE_EQUAL(committee.size(), 5U);
    for (size_t i = 0; i < committee.size(); ++i) {
        BOOST_CHECK(committee[i] == schedule[i]);
    }
}

// The schedule changes with the seed (so leaders reshuffle each block).
BOOST_AUTO_TEST_CASE(pos_schedule_reshuffles)
{
    StakeRegistry reg;
    for (int i = 0; i < 10; ++i) reg.SetStake(MakeKey(), 1);

    int differing_leaders = 0;
    CPubKey prev_leader;
    for (int i = 0; i < 20; ++i) {
        uint256 seed = ComputePosSeed(uint256(), i);
        CPubKey leader = PosSchedule(reg, seed).front();
        if (i > 0 && leader != prev_leader) differing_leaders++;
        prev_leader = leader;
    }
    // With 10 equal stakers the rank-0 leader should change most of the time.
    BOOST_CHECK(differing_leaders >= 10);
}

// VRF sortition slots: deterministic, bounded, and stake-weighted (more stake
// gives statistically earlier slots).
BOOST_AUTO_TEST_CASE(pos_vrf_slot_math)
{
    uint256 beta = uint256S("0x8000000000000000000000000000000000000000000000000000000000000000");
    // Determinism and degenerate inputs.
    BOOST_CHECK_EQUAL(PosVrfSlot(beta, 1, 10), PosVrfSlot(beta, 1, 10));
    BOOST_CHECK_EQUAL(PosVrfSlot(beta, 0, 10), POS_VRF_MAX_SLOT);
    BOOST_CHECK_EQUAL(PosVrfSlot(beta, 1, 0), POS_VRF_MAX_SLOT);
    // beta = 2^255, w=1, W=10: q>>192 = 2^63, slot = 2^63*10/2^64 = 5.
    BOOST_CHECK_EQUAL(PosVrfSlot(beta, 1, 10), 5U);
    // Ten times the weight divides the slot by ten.
    BOOST_CHECK_EQUAL(PosVrfSlot(beta, 10, 10), 0U);
    // The cap holds for tiny weights and huge totals.
    BOOST_CHECK(PosVrfSlot(beta, 1, std::numeric_limits<uint64_t>::max()) <= POS_VRF_MAX_SLOT);

    // Statistical weighting: across many pseudorandom betas, the 9x staker's
    // average slot is far lower than the 1x staker's.
    uint64_t sum_big = 0, sum_small = 0;
    const int trials = 500;
    for (int i = 0; i < trials; ++i) {
        uint256 b = ComputePosSeed(uint256(), i);
        sum_big += PosVrfSlot(b, 9, 10);
        sum_small += PosVrfSlot(b, 1, 10);
    }
    BOOST_CHECK(sum_big * 3 < sum_small);
}

// Exponential-race sortition (PosVrfScoreExp / PosVrfSlotExp): the election is
// exactly stake-proportional AND split-neutral -- splitting a stake into many
// identities does not change its share of blocks, unlike raw-beta election.
// This exercises the real fixed-point functions over a full-election simulation.
BOOST_AUTO_TEST_CASE(pos_vrf_exprace)
{
    uint256 beta = uint256S("0x8000000000000000000000000000000000000000000000000000000000000000");
    // Degenerate inputs match PosVrfSlot's contract; the function is deterministic.
    BOOST_CHECK_EQUAL(PosVrfSlotExp(beta, 0, 10), POS_VRF_MAX_SLOT);
    BOOST_CHECK_EQUAL(PosVrfSlotExp(beta, 1, 0), POS_VRF_MAX_SLOT);
    BOOST_CHECK_EQUAL(PosVrfSlotExp(beta, 1, 10), PosVrfSlotExp(beta, 1, 10));
    // More weight -> statistically earlier slot.
    uint64_t sum_big = 0, sum_small = 0;
    for (int i = 0; i < 500; ++i) {
        uint256 b = ComputePosSeed(uint256(), i);
        sum_big += PosVrfSlotExp(b, 9, 10);
        sum_small += PosVrfSlotExp(b, 1, 10);
    }
    BOOST_CHECK(sum_big < sum_small);

    // One election round: every staker draws a beta; the time-gate offer is
    // max(slot,1)*interval; the field is the earliest-offering bucket; the
    // winner is the lowest FINE score in that field (as BackedForRound would
    // order it). Returns each staker's block count over `rounds` rounds.
    auto elect = [](const std::vector<uint64_t>& w, int rounds, uint32_t salt) {
        uint64_t total = 0; for (uint64_t x : w) total += x;
        std::vector<long> wins(w.size(), 0);
        uint32_t ctr = salt;
        for (int r = 0; r < rounds; ++r) {
            std::vector<arith_uint256> sc(w.size());
            std::vector<uint64_t> off(w.size());
            uint64_t first = std::numeric_limits<uint64_t>::max();
            for (size_t i = 0; i < w.size(); ++i) {
                uint256 b = ComputePosSeed(uint256(), ctr++);
                sc[i] = PosVrfScoreExp(b, w[i], total);
                arith_uint256 sl = sc[i] >> 32;
                uint64_t slot = (sl > arith_uint256((uint64_t)POS_VRF_MAX_SLOT))
                                    ? POS_VRF_MAX_SLOT : sl.GetLow64();
                off[i] = std::max<uint64_t>(slot, 1) * 30;
                first = std::min(first, off[i]);
            }
            int win = -1; arith_uint256 best;
            for (size_t i = 0; i < w.size(); ++i) {
                if (off[i] <= first + 6 && (win < 0 || sc[i] < best)) { best = sc[i]; win = (int)i; }
            }
            wins[win]++;
        }
        return wins;
    };

    const int rounds = 30000;

    // Proportionality: a whale and four minnows each get ~their stake share.
    {
        std::vector<uint64_t> w{80, 5, 5, 5, 5};
        auto wins = elect(w, rounds, 1000);
        double whale = double(wins[0]) / rounds;
        BOOST_TEST_MESSAGE("exp-race whale 80% -> " << (whale * 100) << "% of blocks");
        BOOST_CHECK(whale > 0.74 && whale < 0.86);          // ~0.80, within 7.5%
        for (int i = 1; i < 5; ++i) {
            double s = double(wins[i]) / rounds;
            BOOST_CHECK(s > 0.035 && s < 0.065);            // ~0.05
        }
    }

    // Split-neutrality: 30% as ONE identity vs 30% split into 15 must win the
    // same share of blocks (raw beta would jump from ~1.4x to ~1.5x here).
    {
        std::vector<uint64_t> whole{70, 30};
        auto w1 = elect(whole, rounds, 2000);
        double s1 = double(w1[1]) / rounds;

        std::vector<uint64_t> split{70};
        for (int i = 0; i < 15; ++i) split.push_back(2);    // 15 x 2% = 30%
        auto w15 = elect(split, rounds, 2000);
        long atk = 0; for (size_t i = 1; i < split.size(); ++i) atk += w15[i];
        double s15 = double(atk) / rounds;

        BOOST_TEST_MESSAGE("exp-race 30% as 1 -> " << (s1 * 100) << "%, as 15 -> " << (s15 * 100) << "%");
        BOOST_CHECK(s1 > 0.27 && s1 < 0.33);                // proportional, not inflated
        BOOST_CHECK(s15 > 0.27 && s15 < 0.33);
        BOOST_CHECK(std::abs(s1 - s15) < 0.03);             // splitting does not pay
    }
}

// The coinbase VRF commitment round-trips and rejects malformed payloads.
BOOST_AUTO_TEST_CASE(pos_vrf_commitment_roundtrip)
{
    std::vector<unsigned char> proof(VRF_PROOF_SIZE, 0xAB);
    CScript commitment = BuildPosVrfCommitment(proof);

    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.emplace_back();
    coinbase.vout.back().scriptPubKey = CScript() << OP_TRUE; // unrelated output
    coinbase.vout.emplace_back();
    coinbase.vout.back().scriptPubKey = commitment;
    block.vtx.push_back(MakeTransactionRef(coinbase));

    auto extracted = ExtractPosVrfProof(block);
    BOOST_REQUIRE(extracted.has_value());
    BOOST_CHECK(*extracted == proof);

    // Wrong-size payload is ignored.
    CBlock bad;
    CMutableTransaction cb2;
    cb2.vin.resize(1);
    cb2.vout.emplace_back();
    std::vector<unsigned char> short_proof(VRF_PROOF_SIZE - 1, 0xAB);
    cb2.vout.back().scriptPubKey = BuildPosVrfCommitment(short_proof);
    bad.vtx.push_back(MakeTransactionRef(cb2));
    BOOST_CHECK(!ExtractPosVrfProof(bad).has_value());

    // Empty block.
    CBlock empty;
    BOOST_CHECK(!ExtractPosVrfProof(empty).has_value());
}

// VRF committee membership: threshold semantics, determinism, and the
// expected committee size is ~g_pos_committee_size, weight-proportionally.
BOOST_AUTO_TEST_CASE(pos_vrf_committee_membership)
{
    int old_size = g_pos_committee_size;

    // Threshold semantics match PosVrfSlot.
    g_pos_committee_size = 4;
    uint256 beta = uint256S("0x8000000000000000000000000000000000000000000000000000000000000000");
    // w=1, W=10 => slot 5 (see pos_vrf_slot_math): not < 4.
    BOOST_CHECK(!PosVrfIsCommitteeMember(beta, 1, 10));
    // w=10, W=10 => slot 0: member.
    BOOST_CHECK(PosVrfIsCommitteeMember(beta, 10, 10));
    // T >= W/w makes membership certain: w=1, W=4, slot in [0,4) < 4.
    for (int i = 0; i < 50; ++i) {
        uint256 b = ComputePosSeed(uint256(), i);
        BOOST_CHECK(PosVrfIsCommitteeMember(b, 1, 4));
    }

    // Statistical expected size: 4 unit-weight stakers, T=2 => each member
    // with probability 1/2; expected #members per slot = 2.
    g_pos_committee_size = 2;
    int members = 0;
    const int slots = 200;
    for (int i = 0; i < slots; ++i) {
        for (int k = 0; k < 4; ++k) {
            uint256 b = ComputePosSeed(uint256S(strprintf("0x%x", k)), i);
            if (PosVrfIsCommitteeMember(b, 1, 4)) members++;
        }
    }
    // E = 400; sd = sqrt(800*0.25) ~ 14. Allow generous bounds.
    BOOST_CHECK(members > 320);
    BOOST_CHECK(members < 480);

    g_pos_committee_size = old_size;
}

// Member eligibility commitments round-trip and reject malformed payloads.
BOOST_AUTO_TEST_CASE(pos_vrf_member_commitment_roundtrip)
{
    CPubKey member = MakeKey();
    std::vector<unsigned char> proof(VRF_PROOF_SIZE, 0xCD);

    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.emplace_back();
    coinbase.vout.back().scriptPubKey = BuildPosVrfCommitment(std::vector<unsigned char>(VRF_PROOF_SIZE, 0xAB)); // leader proof: different tag
    coinbase.vout.emplace_back();
    coinbase.vout.back().scriptPubKey = BuildPosVrfMemberCommitment(member, proof);
    block.vtx.push_back(MakeTransactionRef(coinbase));

    std::vector<PosVrfMember> members = ExtractPosVrfMembers(block);
    BOOST_REQUIRE_EQUAL(members.size(), 1U);
    BOOST_CHECK(members[0].pubkey == member);
    BOOST_CHECK(members[0].proof == proof);
    // The leader commitment is still independently extractable.
    BOOST_CHECK(ExtractPosVrfProof(block).has_value());

    // Truncated member payload is skipped.
    CBlock bad;
    CMutableTransaction cb2;
    cb2.vin.resize(1);
    cb2.vout.emplace_back();
    std::vector<unsigned char> short_proof(VRF_PROOF_SIZE - 1, 0xCD);
    cb2.vout.back().scriptPubKey = BuildPosVrfMemberCommitment(member, short_proof);
    bad.vtx.push_back(MakeTransactionRef(cb2));
    BOOST_CHECK(ExtractPosVrfMembers(bad).empty());
}

// Staking script: build/parse round-trip across CSV encodings, rejection of
// malformed scripts, and StakeFromTxOut qualification rules.
BOOST_AUTO_TEST_CASE(pos_stake_script_roundtrip)
{
    CPubKey staker = MakeKey();

    // Round-trips for smallint (OP_1..16), CScriptNum-push height CSV, and
    // time-based CSV (the BIP68 type flag set, 512-second units).
    const uint32_t time_csv = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | 2363u; // ~2 weeks
    for (uint32_t csv : {1u, 5u, 16u, 17u, 144u, 65535u, time_csv}) {
        CScript script = BuildStakeScript(staker, csv);
        auto parsed = ParseStakeScript(script);
        BOOST_REQUIRE_MESSAGE(parsed.has_value(), strprintf("csv=%u", csv));
        BOOST_CHECK(parsed->first == staker);
        BOOST_CHECK_EQUAL(parsed->second, csv);
    }

    // Rejections: zero csv, a stray reserved bit (0x10000), the disable flag,
    // wrong opcodes, trailing data, not a key.
    BOOST_CHECK(!ParseStakeScript(CScript() << (int64_t)0 << OP_CHECKSEQUENCEVERIFY << OP_DROP << ToByteVector(staker) << OP_CHECKSIG).has_value());
    BOOST_CHECK(!ParseStakeScript(CScript() << (int64_t)0x10000 << OP_CHECKSEQUENCEVERIFY << OP_DROP << ToByteVector(staker) << OP_CHECKSIG).has_value());
    BOOST_CHECK(!ParseStakeScript(CScript() << (int64_t)(CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG | 5u) << OP_CHECKSEQUENCEVERIFY << OP_DROP << ToByteVector(staker) << OP_CHECKSIG).has_value());
    BOOST_CHECK(!ParseStakeScript(CScript() << (int64_t)5 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << ToByteVector(staker) << OP_CHECKSIG).has_value());
    BOOST_CHECK(!ParseStakeScript(CScript() << (int64_t)5 << OP_CHECKSEQUENCEVERIFY << OP_DROP << ToByteVector(staker) << OP_CHECKSIGVERIFY).has_value());
    CScript trailing = BuildStakeScript(staker, 5);
    trailing << OP_TRUE;
    BOOST_CHECK(!ParseStakeScript(trailing).has_value());
    BOOST_CHECK(!ParseStakeScript(BuildPosChallenge(staker)).has_value());

    // StakeFromTxOut qualification.
    uint32_t old_unbonding = g_pos_unbonding_period;
    g_pos_unbonding_period = 10;
    CScript ok_script = BuildStakeScript(staker, 10);
    CTxOut ok_out(CConfidentialAsset(::policyAsset), CConfidentialValue(50000), ok_script);
    auto stake = StakeFromTxOut(ok_out);
    BOOST_REQUIRE(stake.has_value());
    BOOST_CHECK(stake->first == staker);
    BOOST_CHECK_EQUAL(stake->second, 50000U);

    // CSV below the unbonding floor does not count.
    CTxOut low_csv(CConfidentialAsset(::policyAsset), CConfidentialValue(50000), BuildStakeScript(staker, 9));
    BOOST_CHECK(!StakeFromTxOut(low_csv).has_value());
    // Wrong asset does not count.
    CAsset other_asset{uint256S("0xaa")};
    CTxOut wrong_asset(CConfidentialAsset(other_asset), CConfidentialValue(50000), ok_script);
    BOOST_CHECK(!StakeFromTxOut(wrong_asset).has_value());
    // Confidential (committed) value cannot carry verifiable weight.
    CConfidentialValue blinded;
    blinded.vchCommitment.assign(33, 0x08);
    blinded.vchCommitment[0] = 0x08;
    CTxOut blinded_out(CConfidentialAsset(::policyAsset), blinded, ok_script);
    BOOST_CHECK(!StakeFromTxOut(blinded_out).has_value());
    // Zero value does not count.
    CTxOut zero(CConfidentialAsset(::policyAsset), CConfidentialValue(0), ok_script);
    BOOST_CHECK(!StakeFromTxOut(zero).has_value());

    // A minimum unbonding lock beyond the 16-bit height-CSV range (the
    // whitepaper's ~2-week / >2016-BTC-block lock at fast slots) can only be
    // met by a time-based CSV; height-based outputs no longer qualify.
    int64_t old_slot = g_pos_slot_interval;
    g_pos_slot_interval = 10;             // 10s slots
    g_pos_unbonding_period = 120960;      // 2 weeks of 10s blocks (> 65535)
    // Required ~= 1,209,600 s. A max height CSV (65535 blocks = 655,350 s)
    // falls short; a time-based CSV of 2363 * 512 = 1,209,856 s clears it.
    CTxOut height_short(CConfidentialAsset(::policyAsset), CConfidentialValue(50000), BuildStakeScript(staker, 65535));
    BOOST_CHECK(!StakeFromTxOut(height_short).has_value());
    CScript time_script = BuildStakeScript(staker, CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | 2363u);
    CTxOut time_ok(CConfidentialAsset(::policyAsset), CConfidentialValue(50000), time_script);
    auto time_stake = StakeFromTxOut(time_ok);
    BOOST_REQUIRE(time_stake.has_value());
    BOOST_CHECK_EQUAL(time_stake->second, 50000U);
    // Just-too-short time lock (one 512s unit under) is rejected.
    CScript time_short_script = BuildStakeScript(staker, CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | 2361u);
    CTxOut time_short(CConfidentialAsset(::policyAsset), CConfidentialValue(50000), time_short_script);
    BOOST_CHECK(!StakeFromTxOut(time_short).has_value());

    g_pos_slot_interval = old_slot;
    g_pos_unbonding_period = old_unbonding;
}

// Registry layering: config + UTXO weights sum; UTXO add/sub mirror exactly.
BOOST_AUTO_TEST_CASE(pos_stake_registry_layers)
{
    StakeRegistry reg;
    CPubKey config_staker = MakeKey();
    CPubKey utxo_staker = MakeKey();
    CPubKey both = MakeKey();

    reg.SetStake(config_staker, 100);
    reg.SetStake(both, 100);
    reg.AddUtxoStake(utxo_staker, 250);
    reg.AddUtxoStake(both, 50);

    BOOST_CHECK_EQUAL(reg.GetWeight(config_staker), 100U);
    BOOST_CHECK_EQUAL(reg.GetWeight(utxo_staker), 250U);
    BOOST_CHECK_EQUAL(reg.GetWeight(both), 150U);
    BOOST_CHECK_EQUAL(reg.Size(), 3U);
    BOOST_CHECK_EQUAL(PosTotalWeight(reg), 500U);

    // Sub mirrors add; weight returns to the config layer alone.
    reg.SubUtxoStake(both, 50);
    BOOST_CHECK_EQUAL(reg.GetWeight(both), 100U);
    reg.SubUtxoStake(utxo_staker, 250);
    BOOST_CHECK_EQUAL(reg.GetWeight(utxo_staker), 0U);
    BOOST_CHECK_EQUAL(reg.Size(), 2U);
}

namespace {
//! A coinbase-shaped tx with no stake.
CMutableTransaction CoinbaseTx()
{
    CMutableTransaction tx;
    tx.vin.emplace_back();
    tx.vin[0].prevout.SetNull();
    tx.vout.emplace_back(CConfidentialAsset(::policyAsset), CConfidentialValue(0), CScript() << OP_TRUE);
    return tx;
}
CTxOut StakeOut(const CPubKey& p, uint32_t csv, CAmount amount)
{
    return CTxOut(CConfidentialAsset(::policyAsset), CConfidentialValue(amount), BuildStakeScript(p, csv));
}
} // namespace

// PosApplyBlockStake and PosRevertBlockStake must be exact inverses, including
// the corner case of a staking output created AND spent within the same block
// (the pubkey's post-block UTXO weight is zero and its registry entry erased).
// Undoing in the wrong order would leave spurious weight behind — a reorg-only
// consensus split. See src/pos.cpp.
BOOST_AUTO_TEST_CASE(pos_apply_revert_is_exact_inverse)
{
    StakeRegistry& reg = StakeRegistry::GetInstance();

    CPubKey p = MakeKey();   // created-and-spent within the block
    CPubKey q = MakeKey();   // pre-existing stake, untouched by the block
    const CAmount a = 70000;

    // tx1 creates p's staking output; tx2 spends it (same block).
    CMutableTransaction tx1;
    tx1.vin.emplace_back();
    tx1.vout.push_back(StakeOut(p, 100, a));
    CMutableTransaction tx2;
    tx2.vin.emplace_back(COutPoint(tx1.GetHash(), 0));
    tx2.vout.emplace_back(CConfidentialAsset(::policyAsset), CConfidentialValue(a), CScript() << OP_TRUE);

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(CoinbaseTx()));
    block.vtx.push_back(MakeTransactionRef(tx1));
    block.vtx.push_back(MakeTransactionRef(tx2));

    // Undo data covers every tx but the coinbase. tx2 spent p's staking output.
    CBlockUndo undo;
    undo.vtxundo.emplace_back();                 // tx1: its inputs aren't stake
    CTxUndo tx2_undo;
    tx2_undo.vprevout.emplace_back(StakeOut(p, 100, a), /*nHeightIn=*/1, /*fCoinBaseIn=*/false);
    undo.vtxundo.push_back(tx2_undo);

    reg.Clear();
    reg.AddUtxoStake(q, 500); // a baseline staker the block must not disturb

    PosApplyBlockStake(block, undo);
    // p was created and spent in the same block → net zero, entry erased.
    BOOST_CHECK_EQUAL(reg.GetWeight(p), 0U);
    BOOST_CHECK_EQUAL(reg.GetWeight(q), 500U);

    PosRevertBlockStake(block, undo);
    // The exact inverse: p stays absent (the ordering bug would leave it at a),
    // q is untouched.
    BOOST_CHECK_EQUAL(reg.GetWeight(p), 0U);
    BOOST_CHECK_EQUAL(reg.GetWeight(q), 500U);
    BOOST_CHECK_EQUAL(reg.Size(), 1U);

    // Also the ordinary case: a block that only *creates* a staking output, and
    // one that only *spends* a pre-existing one, both invert cleanly.
    CMutableTransaction create_only;
    create_only.vin.emplace_back();
    create_only.vout.push_back(StakeOut(p, 100, a));
    CBlock cblock;
    cblock.vtx.push_back(MakeTransactionRef(CoinbaseTx()));
    cblock.vtx.push_back(MakeTransactionRef(create_only));
    CBlockUndo cundo;
    cundo.vtxundo.emplace_back();

    reg.Clear();
    PosApplyBlockStake(cblock, cundo);
    BOOST_CHECK_EQUAL(reg.GetWeight(p), (uint64_t)a);
    PosRevertBlockStake(cblock, cundo);
    BOOST_CHECK_EQUAL(reg.GetWeight(p), 0U);
    BOOST_CHECK_EQUAL(reg.Size(), 0U);

    reg.Clear();
}

// Checkpoint payloads round-trip and reject malformed input.
BOOST_AUTO_TEST_CASE(pos_checkpoint_payload_roundtrip)
{
    uint256 hash = uint256S("0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    for (uint32_t height : {0u, 1u, 65536u, 0xFFFFFFFFu}) {
        std::vector<unsigned char> payload = BuildCheckpointPayload(hash, height);
        BOOST_CHECK_EQUAL(payload.size(), 7U + 32U + 4U);
        auto parsed = ParseCheckpointPayload(payload);
        BOOST_REQUIRE(parsed.has_value());
        BOOST_CHECK(parsed->first == hash);
        BOOST_CHECK_EQUAL(parsed->second, height);
    }
    // Wrong tag.
    std::vector<unsigned char> bad = BuildCheckpointPayload(hash, 5);
    bad[0] ^= 0x01;
    BOOST_CHECK(!ParseCheckpointPayload(bad).has_value());
    // Wrong sizes.
    std::vector<unsigned char> shorter = BuildCheckpointPayload(hash, 5);
    shorter.pop_back();
    BOOST_CHECK(!ParseCheckpointPayload(shorter).has_value());
    std::vector<unsigned char> longer = BuildCheckpointPayload(hash, 5);
    longer.push_back(0);
    BOOST_CHECK(!ParseCheckpointPayload(longer).has_value());
    BOOST_CHECK(!ParseCheckpointPayload({}).has_value());
}

// Public fixed-size committee (impl spec Option A): the quorum derives from
// the ACTUAL committee size with a +1 at odd sizes, so any two quorums overlap
// in at least 2 members at every size; the committee itself is the schedule
// prefix capped at min(pool, cap).
BOOST_AUTO_TEST_CASE(pos_public_committee_quorum_and_size)
{
    // Quorum table: identical to PosQuorum at even k (51-of-100, 126-of-250),
    // one higher at odd k.
    BOOST_CHECK_EQUAL(PosPublicQuorum(0), 0);
    BOOST_CHECK_EQUAL(PosPublicQuorum(1), 1);
    BOOST_CHECK_EQUAL(PosPublicQuorum(2), 2);
    BOOST_CHECK_EQUAL(PosPublicQuorum(3), 3);
    BOOST_CHECK_EQUAL(PosPublicQuorum(4), 3);
    BOOST_CHECK_EQUAL(PosPublicQuorum(5), 4);
    BOOST_CHECK_EQUAL(PosPublicQuorum(59), 31);
    BOOST_CHECK_EQUAL(PosPublicQuorum(100), 51);
    BOOST_CHECK_EQUAL(PosPublicQuorum(249), 126);
    BOOST_CHECK_EQUAL(PosPublicQuorum(250), 126);
    BOOST_CHECK_EQUAL(PosPublicQuorum(251), 127);
    // The overlap invariant that makes disjoint quorums impossible: two
    // quorums out of k members share at least 2q - k >= 2 signers, at every
    // size from 2 upward, and the quorum never exceeds the committee.
    for (int k = 1; k <= 400; ++k) {
        const int q = PosPublicQuorum(k);
        BOOST_CHECK_LE(q, k);
        if (k >= 2) BOOST_CHECK_GE(2 * q - k, 2);
    }

    // Committee size and membership: min(pool, cap), exactly the schedule
    // prefix, and the slot quorum follows the flag. Under the bitfield
    // certificate a member must have a REGISTERED BLS key, so every staker is
    // registered here (a dummy 48-byte key; the committee filter checks only
    // presence, not the key's validity).
    StakeRegistry& reg = StakeRegistry::GetInstance();
    reg.Clear();
    std::vector<CPubKey> stakers;
    for (int i = 0; i < 5; ++i) {
        CPubKey p = MakeKey();
        stakers.push_back(p);
        reg.SetStake(p, 100);
        reg.SetBls(p, std::vector<unsigned char>(48, (unsigned char)(i + 1)));
    }
    const uint256 seed = ComputePosSeed(uint256S("0x07"), 42);

    g_pos_committee_size = 3;
    BOOST_CHECK_EQUAL(PosPublicCommitteeSize(reg), 3);
    std::set<CPubKey> committee = PosPublicCommitteeSet(reg, seed);
    BOOST_CHECK_EQUAL(committee.size(), 3U);
    {
        std::vector<CPubKey> schedule = PosSchedule(reg, seed);
        for (size_t i = 0; i < schedule.size(); ++i) {
            BOOST_CHECK_EQUAL(committee.count(schedule[i]), i < 3 ? 1U : 0U);
        }
    }

    g_pos_committee_size = 10; // pool below the cap: the committee is everyone
    BOOST_CHECK_EQUAL(PosPublicCommitteeSize(reg), 5);
    BOOST_CHECK_EQUAL(PosPublicCommitteeSet(reg, seed).size(), 5U);

    g_pos_public_committee = true;
    BOOST_CHECK_EQUAL(PosSlotQuorum(reg), PosPublicQuorum(5)); // 4-of-5 (odd bump)
    g_pos_committee_size = 4;
    BOOST_CHECK_EQUAL(PosSlotQuorum(reg), 3); // 3-of-4
    g_pos_public_committee = false;
    BOOST_CHECK_EQUAL(PosSlotQuorum(reg), PosQuorum(4)); // nominal when off

    g_pos_committee_size = DEFAULT_POS_COMMITTEE_SIZE;
    reg.Clear();
}

// Runtime UTXO-layer BLS registration (impl spec Option A phase 2): the BLS
// key rides in the staking output, round-trips through the script, and the
// registry's UTXO-BLS layer follows the staker's weight lifecycle (dropped
// when the last output is spent) so it is reorg-safe.
BOOST_AUTO_TEST_CASE(pos_stake_bls_registration)
{
    const CPubKey staker = MakeKey();
    const std::vector<unsigned char> blspub(48, 0x11), pop(96, 0x22);

    // Round-trip through the staking script (with and without a registration).
    CScript reg_script = BuildStakeScript(staker, 10, blspub, pop);
    auto full = ParseStakeScriptFull(reg_script);
    BOOST_REQUIRE(full.has_value());
    BOOST_CHECK(full->pubkey == staker);
    BOOST_CHECK_EQUAL(full->csv, 10U);
    BOOST_CHECK(full->bls_pubkey == blspub);
    BOOST_CHECK(full->bls_pop == pop);
    // The plain parser still returns pubkey + csv, and the spend template is
    // unchanged (the extra pushes are dropped).
    auto plain = ParseStakeScript(reg_script);
    BOOST_REQUIRE(plain.has_value());
    BOOST_CHECK(plain->first == staker);
    auto bls = ParseStakeBlsRegistration(reg_script);
    BOOST_REQUIRE(bls.has_value());
    BOOST_CHECK(bls->first == blspub);
    // An output with no registration parses fine and carries no BLS key.
    CScript plain_script = BuildStakeScript(staker, 10);
    auto full2 = ParseStakeScriptFull(plain_script);
    BOOST_REQUIRE(full2.has_value());
    BOOST_CHECK(full2->bls_pubkey.empty());
    BOOST_CHECK(!ParseStakeBlsRegistration(plain_script).has_value());
    // A malformed registration (wrong PoP size) is rejected wholesale.
    CScript bad;
    bad << (int64_t)10 << OP_CHECKSEQUENCEVERIFY << OP_DROP
        << blspub << OP_DROP << std::vector<unsigned char>(50, 0) << OP_DROP
        << ToByteVector(staker) << OP_CHECKSIG;
    BOOST_CHECK(!ParseStakeScriptFull(bad).has_value());

    // Registry UTXO-BLS lifecycle: the key is present while the staker has UTXO
    // weight and vanishes when the last output is spent.
    StakeRegistry& reg = StakeRegistry::GetInstance();
    reg.Clear();
    reg.AddUtxoStake(staker, 100, blspub);       // first output
    BOOST_CHECK(reg.HasBls(staker));
    BOOST_CHECK(reg.GetBls(staker) == blspub);
    reg.AddUtxoStake(staker, 50, blspub);        // second output, same key
    reg.SubUtxoStake(staker, 50);                // spend one — key stays
    BOOST_CHECK(reg.HasBls(staker));
    reg.SubUtxoStake(staker, 100);               // spend the last — key gone
    BOOST_CHECK(!reg.HasBls(staker));
    BOOST_CHECK(reg.GetBls(staker).empty());

    // Config layer takes precedence over the UTXO layer.
    const std::vector<unsigned char> cfgkey(48, 0x33);
    reg.Clear();
    reg.SetBls(staker, cfgkey);
    reg.AddUtxoStake(staker, 100, blspub);
    BOOST_CHECK(reg.GetBls(staker) == cfgkey);

    // Wholesale rebuild (startup scan) restores the UTXO-BLS layer.
    reg.Clear();
    std::map<CPubKey, uint64_t> w{{staker, 100}};
    std::map<CPubKey, std::vector<unsigned char>> b{{staker, blspub}};
    reg.SetUtxoStake(std::move(w), std::move(b));
    BOOST_CHECK(reg.GetBls(staker) == blspub);
    reg.Clear();
}

// The bitfield certificate codec (impl spec Option A phase 2): the signer
// bitfield round-trips through a proof solution, indexes members by their
// committee position, and rejects malformed solutions.
BOOST_AUTO_TEST_CASE(pos_bitfield_certificate_codec)
{
    // Bit helpers: LSB-first, growable, correct popcount.
    std::vector<unsigned char> bf;
    BOOST_CHECK(!PosBitfieldTest(bf, 0));
    PosBitfieldSet(bf, 0);
    PosBitfieldSet(bf, 3);
    PosBitfieldSet(bf, 9);   // grows into a second byte
    BOOST_CHECK(PosBitfieldTest(bf, 0));
    BOOST_CHECK(PosBitfieldTest(bf, 3));
    BOOST_CHECK(PosBitfieldTest(bf, 9));
    BOOST_CHECK(!PosBitfieldTest(bf, 1));
    BOOST_CHECK(!PosBitfieldTest(bf, 8));
    BOOST_CHECK(!PosBitfieldTest(bf, 99)); // out of range reads false
    BOOST_CHECK_EQUAL(PosBitfieldPopcount(bf), 3);
    BOOST_CHECK_EQUAL(bf.size(), 2U);

    // Solution round-trip: leader sig + 96-byte aggregate + bitfield.
    std::vector<unsigned char> leader_sig(72, 0xAB);
    std::vector<unsigned char> agg(96, 0xCD);
    CScript sol = BuildPosBlsBitfieldSolution(leader_sig, agg, bf);
    auto cert = ParsePosBlsBitfieldSolution(sol);
    BOOST_REQUIRE(cert.has_value());
    BOOST_CHECK(cert->leader_sig == leader_sig);
    BOOST_CHECK(cert->agg_sig == agg);
    BOOST_CHECK(cert->bitfield == bf);

    // Malformed: wrong aggregate size, empty bitfield, trailing junk.
    BOOST_CHECK(!ParsePosBlsBitfieldSolution(BuildPosBlsBitfieldSolution(leader_sig, std::vector<unsigned char>(64, 0), bf)).has_value());
    BOOST_CHECK(!ParsePosBlsBitfieldSolution(CScript() << leader_sig << agg).has_value()); // no bitfield
}

// SEQUENTIA vesting: the optional absolute (BIP65) lock in the staking script.
//
// The point of the whole construction is the pairing asserted below: a stake
// output carrying a liquid_locktime is UNSPENDABLE until that time -- so it
// cannot be sold or transferred -- yet StakeFromTxOut still credits its full
// weight, because weight is granted for a staking UTXO merely existing. That is
// a "staking-only period": the tokens stake and earn fees while illiquid.
BOOST_AUTO_TEST_CASE(pos_stake_script_vesting_locktime)
{
    CPubKey staker = MakeKey();
    const std::vector<unsigned char> bls_pk(48, 0x11);   // BLS_PK_SIZE
    const std::vector<unsigned char> bls_pop(96, 0x22);  // BLS_SIG_SIZE

    // Round-trip across smallint (OP_1..OP_16), multi-byte, the height/time
    // boundary, and the uint32 ceiling -- with and without BLS registration.
    for (int64_t lt : {int64_t{1}, int64_t{16}, int64_t{17}, int64_t{144},
                       int64_t{499999999}, int64_t{500000000}, int64_t{1893456000},
                       int64_t{0xffffffffLL}}) {
        for (bool with_bls : {false, true}) {
            CScript script = with_bls ? BuildStakeScript(staker, 10, bls_pk, bls_pop, lt)
                                      : BuildStakeScript(staker, 10, {}, {}, lt);
            auto parsed = ParseStakeScriptFull(script);
            BOOST_REQUIRE_MESSAGE(parsed.has_value(), strprintf("locktime=%d bls=%d", lt, with_bls));
            BOOST_CHECK(parsed->pubkey == staker);
            BOOST_CHECK_EQUAL(parsed->csv, 10U);
            BOOST_CHECK_EQUAL(parsed->liquid_locktime, lt);
            BOOST_CHECK(with_bls ? (parsed->bls_pubkey == bls_pk) : parsed->bls_pubkey.empty());
            // The BLS registration must still be recoverable alongside the lock.
            BOOST_CHECK_EQUAL(ParseStakeBlsRegistration(script).has_value(), with_bls);
        }
    }

    // Backward compatibility: a script with no lock parses, reporting 0.
    auto no_lock = ParseStakeScriptFull(BuildStakeScript(staker, 10));
    BOOST_REQUIRE(no_lock.has_value());
    BOOST_CHECK_EQUAL(no_lock->liquid_locktime, 0);
    auto no_lock_bls = ParseStakeScriptFull(BuildStakeScript(staker, 10, bls_pk, bls_pop));
    BOOST_REQUIRE(no_lock_bls.has_value());
    BOOST_CHECK_EQUAL(no_lock_bls->liquid_locktime, 0);

    // THE LOAD-BEARING PROPERTY: a vesting-locked stake still carries weight.
    uint32_t old_unbonding = g_pos_unbonding_period;
    g_pos_unbonding_period = 10;
    CScript vested = BuildStakeScript(staker, 10, {}, {}, /*liquid_locktime=*/1893456000);
    CTxOut vested_out(CConfidentialAsset(::policyAsset), CConfidentialValue(50000), vested);
    auto stake = StakeFromTxOut(vested_out);
    BOOST_REQUIRE(stake.has_value());
    BOOST_CHECK(stake->first == staker);
    BOOST_CHECK_EQUAL(stake->second, 50000U);

    // Rejections. The locktime must be positive, minimally encoded, and within
    // the uint32 range of nLockTime (a larger value could never be satisfied,
    // burning the stake rather than vesting it).
    const CScript prefix = CScript() << (int64_t)10 << OP_CHECKSEQUENCEVERIFY << OP_DROP;
    auto with_lock_bytes = [&](const std::vector<unsigned char>& lt_push) {
        CScript s = prefix;
        s << lt_push << OP_CHECKLOCKTIMEVERIFY << OP_DROP << ToByteVector(staker) << OP_CHECKSIG;
        return s;
    };
    // Zero (OP_0 pushes an empty vector).
    BOOST_CHECK(!ParseStakeScriptFull(CScript() << (int64_t)10 << OP_CHECKSEQUENCEVERIFY << OP_DROP
                                                << (int64_t)0 << OP_CHECKLOCKTIMEVERIFY << OP_DROP
                                                << ToByteVector(staker) << OP_CHECKSIG).has_value());
    // Negative (-5 in sign-magnitude).
    BOOST_CHECK(!ParseStakeScriptFull(with_lock_bytes({0x85})).has_value());
    // Non-minimal encoding of 5.
    BOOST_CHECK(!ParseStakeScriptFull(with_lock_bytes({0x05, 0x00})).has_value());
    // 2^32, one past the nLockTime ceiling.
    BOOST_CHECK(!ParseStakeScriptFull(with_lock_bytes({0x00, 0x00, 0x00, 0x00, 0x01})).has_value());
    // Six-byte push: beyond what OP_CHECKLOCKTIMEVERIFY itself accepts.
    BOOST_CHECK(!ParseStakeScriptFull(with_lock_bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06})).has_value());
    // Missing the OP_DROP after OP_CHECKLOCKTIMEVERIFY.
    BOOST_CHECK(!ParseStakeScriptFull(CScript() << (int64_t)10 << OP_CHECKSEQUENCEVERIFY << OP_DROP
                                                << (int64_t)144 << OP_CHECKLOCKTIMEVERIFY
                                                << ToByteVector(staker) << OP_CHECKSIG).has_value());
    // Trailing junk after a well-formed vesting script is still rejected.
    CScript trailing = BuildStakeScript(staker, 10, {}, {}, 144);
    trailing << OP_TRUE;
    BOOST_CHECK(!ParseStakeScriptFull(trailing).has_value());
    // The lock must sit after the BLS block, not before it.
    BOOST_CHECK(!ParseStakeScriptFull(CScript() << (int64_t)10 << OP_CHECKSEQUENCEVERIFY << OP_DROP
                                                << (int64_t)144 << OP_CHECKLOCKTIMEVERIFY << OP_DROP
                                                << bls_pk << OP_DROP << bls_pop << OP_DROP
                                                << ToByteVector(staker) << OP_CHECKSIG).has_value());

    g_pos_unbonding_period = old_unbonding;
}

// SEQUENTIA delegation: a staker lends its block-signing rights to a signer (a
// pool operator, or its own hot key) via a separate record output, so the stake
// itself -- which may be frozen for years by a vesting lock -- never moves.
BOOST_AUTO_TEST_CASE(pos_delegation_script_roundtrip)
{
    CPubKey controller = MakeKey();
    CPubKey signer = MakeKey();

    CScript script = BuildDelegationScript(controller, signer);
    auto parsed = ParseDelegationScript(script);
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK(parsed->first == controller);
    BOOST_CHECK(parsed->second == signer);

    // A delegation record is not a staking output, and vice versa: the templates
    // cannot be confused (a stake script opens with a CSV number push).
    BOOST_CHECK(!ParseStakeScript(script).has_value());
    BOOST_CHECK(!ParseDelegationScript(BuildStakeScript(controller, 10)).has_value());
    BOOST_CHECK(!ParseDelegationScript(BuildStakeScript(controller, 10, {}, {}, 1893456000)).has_value());

    // The record carries no weight of its own.
    CTxOut rec(CConfidentialAsset(::policyAsset), CConfidentialValue(1000), script);
    BOOST_CHECK(!StakeFromTxOut(rec).has_value());
    BOOST_CHECK(DelegationFromTxOut(rec).has_value());

    // Rejections: bad marker, wrong opcodes, trailing junk, not a key.
    BOOST_CHECK(!ParseDelegationScript(CScript() << std::vector<unsigned char>{'X'} << OP_DROP
                                                 << ToByteVector(signer) << OP_DROP
                                                 << ToByteVector(controller) << OP_CHECKSIG).has_value());
    BOOST_CHECK(!ParseDelegationScript(CScript() << std::vector<unsigned char>{'S','E','Q','D','E','L'} << OP_DROP
                                                 << ToByteVector(signer) << OP_DROP
                                                 << ToByteVector(controller) << OP_CHECKSIGVERIFY).has_value());
    CScript trailing = BuildDelegationScript(controller, signer);
    trailing << OP_TRUE;
    BOOST_CHECK(!ParseDelegationScript(trailing).has_value());
    BOOST_CHECK(!ParseDelegationScript(CScript() << std::vector<unsigned char>{'S','E','Q','D','E','L'} << OP_DROP
                                                 << std::vector<unsigned char>(33, 0x01) << OP_DROP
                                                 << ToByteVector(controller) << OP_CHECKSIG).has_value());
}

// The registry: delegation re-keys weight onto the signer, one hop, and a
// rotation performed inside one block leaves the NEW signer in force.
BOOST_AUTO_TEST_CASE(pos_delegation_registry_weights)
{
    StakeRegistry& reg = StakeRegistry::GetInstance();
    reg.Clear();

    CPubKey alice = MakeKey();   // a staker who will delegate
    CPubKey pool1 = MakeKey();
    CPubKey pool2 = MakeKey();

    reg.AddUtxoStake(alice, 1000);
    BOOST_CHECK_EQUAL(reg.GetWeight(alice), 1000U);
    BOOST_CHECK_EQUAL(reg.GetWeight(pool1), 0U);
    BOOST_CHECK(reg.SignerFor(alice) == alice);

    // Delegating moves the WEIGHT to the pool. The coins have not moved: the
    // raw, controller-keyed weight is untouched.
    reg.AddUtxoDelegation(alice, pool1);
    BOOST_CHECK_EQUAL(reg.GetWeight(pool1), 1000U);
    BOOST_CHECK_EQUAL(reg.GetWeight(alice), 0U);
    BOOST_CHECK_EQUAL(reg.ControllerWeights().at(alice), 1000U);
    BOOST_CHECK(reg.SignerFor(alice) == pool1);
    BOOST_CHECK_EQUAL(reg.Weights().at(pool1), 1000U);

    // A pool's own stake adds to what it has been lent.
    reg.AddUtxoStake(pool1, 500);
    BOOST_CHECK_EQUAL(reg.GetWeight(pool1), 1500U);

    // Rotation within one block: PosApplyBlockStake adds created outputs before
    // subtracting spent ones, so the removal of the old record must NOT erase
    // the new one. This is the ordering hazard SubUtxoDelegation guards against.
    reg.AddUtxoDelegation(alice, pool2);   // new record created
    reg.SubUtxoDelegation(alice, pool1);   // old record spent
    BOOST_CHECK(reg.SignerFor(alice) == pool2);
    BOOST_CHECK_EQUAL(reg.GetWeight(pool2), 1000U);
    BOOST_CHECK_EQUAL(reg.GetWeight(pool1), 500U); // only its own stake now

    // Reclaiming: spending the record with nothing to replace it.
    reg.SubUtxoDelegation(alice, pool2);
    BOOST_CHECK(reg.SignerFor(alice) == alice);
    BOOST_CHECK_EQUAL(reg.GetWeight(alice), 1000U);
    BOOST_CHECK_EQUAL(reg.GetWeight(pool2), 0U);

    // One hop, never chained: alice->pool1, pool1->pool2 sends alice's weight to
    // pool1 (not pool2). Chasing chains would admit cycles.
    reg.AddUtxoDelegation(alice, pool1);
    reg.AddUtxoDelegation(pool1, pool2);
    BOOST_CHECK_EQUAL(reg.GetWeight(pool1), 1000U); // alice's, lent one hop
    BOOST_CHECK_EQUAL(reg.GetWeight(pool2), 500U);  // pool1's own, lent onward
    BOOST_CHECK_EQUAL(reg.GetWeight(alice), 0U);

    // A cycle must terminate rather than hang. Give pool2 stake of its own, then
    // close the loop: alice->pool1->pool2->alice. Each hop resolves exactly once.
    reg.AddUtxoStake(pool2, 700);
    reg.AddUtxoDelegation(pool2, alice);
    BOOST_CHECK_EQUAL(reg.GetWeight(alice), 700U);  // pool2's own weight, one hop
    BOOST_CHECK_EQUAL(reg.GetWeight(pool1), 1000U); // alice's
    BOOST_CHECK_EQUAL(reg.GetWeight(pool2), 500U);  // pool1's

    reg.Clear();
}


// SEQUENTIA payout policies: how a producer's fee reward is paid.
BOOST_AUTO_TEST_CASE(pos_payout_script_roundtrip)
{
    CPubKey signer = MakeKey();

    PosPayoutPolicy direct;
    direct.mode = PosPayoutMode::DIRECT;
    direct.activation = 5000;
    direct.script = CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 0x07) << OP_EQUALVERIFY << OP_CHECKSIG;
    auto pd = ParsePayoutScript(BuildPayoutScript(signer, direct));
    BOOST_REQUIRE(pd.has_value());
    BOOST_CHECK(pd->first == signer);
    BOOST_CHECK(pd->second == direct);

    for (uint32_t bp : {0u, 1u, 16u, 500u, 9999u, 10000u}) {
        PosPayoutPolicy lot;
        lot.mode = PosPayoutMode::LOTTERY;
        lot.activation = 1;
        lot.commission_bp = bp;
        auto pl = ParsePayoutScript(BuildPayoutScript(signer, lot));
        BOOST_REQUIRE_MESSAGE(pl.has_value(), strprintf("bp=%u", bp));
        BOOST_CHECK(pl->second == lot);
    }

    // Not confusable with the other bare templates.
    BOOST_CHECK(!ParseStakeScript(BuildPayoutScript(signer, direct)).has_value());
    BOOST_CHECK(!ParseDelegationScript(BuildPayoutScript(signer, direct)).has_value());
    BOOST_CHECK(!ParsePayoutScript(BuildDelegationScript(signer, signer)).has_value());
    BOOST_CHECK(!ParsePayoutScript(BuildStakeScript(signer, 10)).has_value());
    // It carries no stake weight and is not a delegation.
    CTxOut rec(CConfidentialAsset(::policyAsset), CConfidentialValue(1000), BuildPayoutScript(signer, direct));
    BOOST_CHECK(!StakeFromTxOut(rec).has_value());
    BOOST_CHECK(PayoutFromTxOut(rec).has_value());

    // Rejections: zero/negative activation, unknown mode, commission > 100%,
    // an empty or oversized direct script, trailing junk.
    PosPayoutPolicy bad = direct;
    bad.activation = 0;
    BOOST_CHECK(!ParsePayoutScript(BuildPayoutScript(signer, bad)).has_value());
    PosPayoutPolicy big_bp;
    big_bp.mode = PosPayoutMode::LOTTERY;
    big_bp.activation = 1;
    big_bp.commission_bp = POS_COMMISSION_DENOM + 1;
    BOOST_CHECK(!ParsePayoutScript(BuildPayoutScript(signer, big_bp)).has_value());
    PosPayoutPolicy empty_spk = direct;
    empty_spk.script = CScript();
    BOOST_CHECK(!ParsePayoutScript(BuildPayoutScript(signer, empty_spk)).has_value());
    PosPayoutPolicy huge = direct;
    const std::vector<unsigned char> huge_bytes(111, 0x51); // one past MAX_PAYOUT_SCRIPT_SIZE
    huge.script = CScript(huge_bytes.begin(), huge_bytes.end());
    BOOST_CHECK(!ParsePayoutScript(BuildPayoutScript(signer, huge)).has_value());
    CScript trailing = BuildPayoutScript(signer, direct);
    trailing << OP_TRUE;
    BOOST_CHECK(!ParsePayoutScript(trailing).has_value());
    // Mode 0 (LEADER) is not an announceable policy.
    BOOST_CHECK(!ParsePayoutScript(CScript() << std::vector<unsigned char>{'S','E','Q','P','A','Y'} << OP_DROP
                                             << (int64_t)5000 << OP_DROP << (int64_t)0 << OP_DROP
                                             << (int64_t)0 << OP_DROP
                                             << ToByteVector(signer) << OP_CHECKSIG).has_value());
}

// The policy in force at a height is the announced policy with the greatest
// activation <= that height, so a pending policy does not bind during its
// notice period and the one it replaces stays in force until it does.
BOOST_AUTO_TEST_CASE(pos_payout_notice_period_lookup)
{
    StakeRegistry& reg = StakeRegistry::GetInstance();
    reg.Clear();
    CPubKey signer = MakeKey();

    BOOST_CHECK(!reg.PayoutFor(signer, 100).has_value()); // none announced

    PosPayoutPolicy first;
    first.mode = PosPayoutMode::LOTTERY;
    first.activation = 100;
    first.commission_bp = 500;
    reg.AddUtxoPayout(signer, first);

    BOOST_CHECK(!reg.PayoutFor(signer, 99).has_value());  // still pending
    BOOST_REQUIRE(reg.PayoutFor(signer, 100).has_value()); // binds exactly at activation
    BOOST_CHECK(*reg.PayoutFor(signer, 100) == first);
    BOOST_CHECK(*reg.PayoutFor(signer, 5000) == first);

    // A hostile change announced for height 200 does NOT bind before then: the
    // old policy stays in force, which is the whole point of the notice period.
    PosPayoutPolicy hostile;
    hostile.mode = PosPayoutMode::DIRECT;
    hostile.activation = 200;
    hostile.script = CScript() << OP_TRUE;
    reg.AddUtxoPayout(signer, hostile);
    BOOST_CHECK(*reg.PayoutFor(signer, 150) == first);
    BOOST_CHECK(*reg.PayoutFor(signer, 199) == first);
    BOOST_CHECK(*reg.PayoutFor(signer, 200) == hostile);

    BOOST_CHECK(reg.HasPayoutAt(signer, 200));
    BOOST_CHECK(!reg.HasPayoutAt(signer, 201));

    // Spending the pending record (before it binds) reverts to the old policy.
    reg.SubUtxoPayout(signer, hostile);
    BOOST_CHECK(*reg.PayoutFor(signer, 250) == first);
    reg.SubUtxoPayout(signer, first);
    BOOST_CHECK(!reg.PayoutFor(signer, 250).has_value());
    reg.Clear();
}

// The coinbase payee: default, DIRECT redirect, and the LOTTERY draw. The draw
// must be deterministic (every node computes the same winner), unbiasable by the
// leader (the seed comes from Bitcoin), and proportional to stake over time.
BOOST_AUTO_TEST_CASE(pos_payout_required_coinbase_script)
{
    StakeRegistry& reg = StakeRegistry::GetInstance();
    reg.Clear();
    CPubKey pool = MakeKey(), alice = MakeKey(), bob = MakeKey();
    const uint256 seed = ComputePosSeed(uint256S("0xbeef"), 7);

    // No policy: the leader is paid, exactly as before this feature existed.
    BOOST_CHECK(PosRequiredCoinbaseScript(pool, 10, seed) == PosLeaderFeeScript(pool));

    // DIRECT: the committed script, whatever it is.
    PosPayoutPolicy direct;
    direct.mode = PosPayoutMode::DIRECT;
    direct.activation = 100;
    direct.script = CScript() << OP_TRUE;
    reg.AddUtxoPayout(pool, direct);
    BOOST_CHECK(PosRequiredCoinbaseScript(pool, 99, seed) == PosLeaderFeeScript(pool)); // pending
    BOOST_CHECK(PosRequiredCoinbaseScript(pool, 100, seed) == direct.script);           // bound
    reg.SubUtxoPayout(pool, direct);

    // LOTTERY with no commission and two delegators, 3:1 by stake.
    PosPayoutPolicy lot;
    lot.mode = PosPayoutMode::LOTTERY;
    lot.activation = 1;
    lot.commission_bp = 0;
    reg.AddUtxoPayout(pool, lot);
    reg.AddUtxoStake(alice, 3000);
    reg.AddUtxoStake(bob, 1000);
    reg.AddUtxoDelegation(alice, pool);
    reg.AddUtxoDelegation(bob, pool);

    // Deterministic: the same seed always yields the same winner.
    const CScript w1 = PosRequiredCoinbaseScript(pool, 10, seed);
    BOOST_CHECK(PosRequiredCoinbaseScript(pool, 10, seed) == w1);
    BOOST_CHECK(w1 == PosLeaderFeeScript(alice) || w1 == PosLeaderFeeScript(bob));

    // Proportional: across many seeds, alice (3x bob's stake) wins ~75%.
    int alice_wins = 0, bob_wins = 0, other = 0;
    const int trials = 4000;
    for (int i = 0; i < trials; ++i) {
        const CScript w = PosRequiredCoinbaseScript(pool, 10, ComputePosSeed(uint256(), i));
        if (w == PosLeaderFeeScript(alice)) alice_wins++;
        else if (w == PosLeaderFeeScript(bob)) bob_wins++;
        else other++;
    }
    BOOST_CHECK_EQUAL(other, 0);                       // the pool never pays itself: 0 commission
    BOOST_CHECK_EQUAL(alice_wins + bob_wins, trials);
    BOOST_CHECK(alice_wins > trials * 0.70);           // expect ~0.75
    BOOST_CHECK(alice_wins < trials * 0.80);

    // Commission: the operator keeps ~20% of blocks outright.
    reg.SubUtxoPayout(pool, lot);
    lot.commission_bp = 2000;
    reg.AddUtxoPayout(pool, lot);
    int pool_wins = 0;
    for (int i = 0; i < trials; ++i) {
        if (PosRequiredCoinbaseScript(pool, 10, ComputePosSeed(uint256(), i)) == PosLeaderFeeScript(pool)) pool_wins++;
    }
    BOOST_CHECK(pool_wins > trials * 0.15);
    BOOST_CHECK(pool_wins < trials * 0.25);

    // A pool with nothing delegated to it pays itself rather than nobody.
    CPubKey lonely = MakeKey();
    PosPayoutPolicy lot2 = lot;
    lot2.commission_bp = 0;
    reg.AddUtxoPayout(lonely, lot2);
    BOOST_CHECK(PosRequiredCoinbaseScript(lonely, 10, seed) == PosLeaderFeeScript(lonely));

    reg.Clear();
}

// Fix A: the block producer backs its new block's anchor down below any
// parent-chain height a live competing branch is contesting, but never for
// forks that have already fallen out of the contest window.
BOOST_AUTO_TEST_CASE(anchor_uncontested_height)
{
    using Br = std::vector<std::pair<int, int>>; // {tip height, branchlen}

    // No competing branches: the whole tip is uncontested.
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, 2, {}), 1000);

    // A rival at the same height forking 3 blocks back: everything from the fork
    // point (997) upward is contested, so back off to 997.
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, 2, Br{{1000, 3}}), 997);

    // The deepest live fork wins the back-off (lowest fork point among rivals):
    // 1000-3=997 vs 999-1=998 -> 997.
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, 2, Br{{1000, 3}, {999, 1}}), 997);

    // Today's testnet4 shape: several equal-height rivals with deep branches.
    // Lowest fork point is 1000-23=977.
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, 2, Br{{1000, 23}, {1000, 18}, {1000, 1}}), 977);

    // A rival whose tip is further than the window behind the active tip is
    // losing the race and ignored: with window 2, a tip at 997 (3 behind) does
    // not lower the anchor, so the tip stays uncontested.
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, 2, Br{{997, 1}}), 1000);
    // Exactly at the window edge (2 behind) still counts.
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, 2, Br{{998, 1}}), 997);

    // branchlen 0 (shares the active chain, e.g. a stray active-looking entry)
    // is never a fork.
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, 2, Br{{1000, 0}}), 1000);

    // Window 0: only rivals reaching the exact active height contest it.
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, 0, Br{{1000, 4}, {999, 1}}), 996);

    // Negative window is clamped to 0 (never widens acceptance oddly).
    BOOST_CHECK_EQUAL(AnchorUncontestedHeight(1000, -5, Br{{1000, 4}}), 996);
}

BOOST_AUTO_TEST_SUITE_END()
