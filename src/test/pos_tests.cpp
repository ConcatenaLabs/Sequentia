// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos.h>
#include <anchor.h>

#include <key.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <vrf.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>

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

// The committee is the first g_pos_committee_size entries of the schedule.
BOOST_AUTO_TEST_CASE(pos_committee_is_schedule_prefix)
{
    StakeRegistry reg;
    for (int i = 0; i < 8; ++i) reg.SetStake(MakeKey(), 1);
    uint256 seed = ComputePosSeed(uint256S("0x77"), uint256(), 3);

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
        uint256 seed = ComputePosSeed(uint256S("0xef"), uint256(), i);
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
        uint256 b = ComputePosSeed(uint256S("0x99"), uint256(), i);
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
        uint256 b = ComputePosSeed(uint256S("0x55"), uint256(), i);
        BOOST_CHECK(PosVrfIsCommitteeMember(b, 1, 4));
    }

    // Statistical expected size: 4 unit-weight stakers, T=2 => each member
    // with probability 1/2; expected #members per slot = 2.
    g_pos_committee_size = 2;
    int members = 0;
    const int slots = 200;
    for (int i = 0; i < slots; ++i) {
        for (int k = 0; k < 4; ++k) {
            uint256 b = ComputePosSeed(uint256S("0x66"), uint256S(strprintf("0x%x", k)), i);
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

    // Round-trips for smallint (OP_1..16) and CScriptNum-push CSV values.
    for (uint32_t csv : {1u, 5u, 16u, 17u, 144u, 65535u}) {
        CScript script = BuildStakeScript(staker, csv);
        auto parsed = ParseStakeScript(script);
        BOOST_REQUIRE_MESSAGE(parsed.has_value(), strprintf("csv=%u", csv));
        BOOST_CHECK(parsed->first == staker);
        BOOST_CHECK_EQUAL(parsed->second, csv);
    }

    // Rejections: zero/oversized csv, wrong opcodes, trailing data, not a key.
    BOOST_CHECK(!ParseStakeScript(CScript() << (int64_t)0 << OP_CHECKSEQUENCEVERIFY << OP_DROP << ToByteVector(staker) << OP_CHECKSIG).has_value());
    BOOST_CHECK(!ParseStakeScript(CScript() << (int64_t)0x10000 << OP_CHECKSEQUENCEVERIFY << OP_DROP << ToByteVector(staker) << OP_CHECKSIG).has_value());
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
