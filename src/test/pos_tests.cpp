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
    uint64_t a = 0x01;             // parent leader VRF score (VRF-output chain)
    uint256 b = uint256S("0x02");  // parent Bitcoin anchor hash
    BOOST_CHECK(ComputePosSeed(a, b, 5) == ComputePosSeed(a, b, 5));
    BOOST_CHECK(ComputePosSeed(a, b, 5) != ComputePosSeed(a, b, 6));
    // The VRF score (first input) is part of the seed.
    BOOST_CHECK(ComputePosSeed(a, b, 5) != ComputePosSeed(0x99, b, 5));
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
    uint256 seed = ComputePosSeed(0xab, uint256(), 1);
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
        uint256 seed = ComputePosSeed(0xcd, uint256(), i);
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
    uint256 seed = ComputePosSeed(0xfeed, uint256(), 1);

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
// rejected. See doc/sequentia/07-vrf.md §6.
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
    uint256 seed = ComputePosSeed(0x77, uint256(), 3);

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
        uint256 seed = ComputePosSeed(0xef, uint256(), i);
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
        uint256 b = ComputePosSeed(0x99, uint256(), i);
        sum_big += PosVrfSlot(b, 9, 10);
        sum_small += PosVrfSlot(b, 1, 10);
    }
    BOOST_CHECK(sum_big * 3 < sum_small);
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
        uint256 b = ComputePosSeed(0x55, uint256(), i);
        BOOST_CHECK(PosVrfIsCommitteeMember(b, 1, 4));
    }

    // Statistical expected size: 4 unit-weight stakers, T=2 => each member
    // with probability 1/2; expected #members per slot = 2.
    g_pos_committee_size = 2;
    int members = 0;
    const int slots = 200;
    for (int i = 0; i < slots; ++i) {
        for (int k = 0; k < 4; ++k) {
            uint256 b = ComputePosSeed(0x66, uint256S(strprintf("0x%x", k)), i);
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

BOOST_AUTO_TEST_SUITE_END()
