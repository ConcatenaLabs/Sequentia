// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// SEQUENTIA: the node-side rules that let a Simplicity covenant pay for its own
// execution without hauling inert padding through every block.
//
//  - the budget a witness byte buys (SIMPLICITY_BUDGET_PER_WITNESS_BYTE),
//  - the height gate that turns it into a flag day on the running testnet, and
//  - the annex, standard on a Simplicity leaf and nowhere else.
//
// The end-to-end proof that a pad-free covenant spends lives in the opendamp
// crate's regtest suite; what is pinned here is the arithmetic and the
// standardness predicate, which are cheap to get wrong and silent when wrong.

#include <chainparams.h>
#include <coins.h>
#include <consensus/params.h>
#include <consensus/tx_verify.h>
#include <key.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(simplicity_policy_tests, BasicTestingSetup)

namespace {

//! The budget a Simplicity spend is granted, mirroring interpreter.cpp.
int64_t SimplicityBudget(size_t serialized_witness_size, bool wide = true)
{
    return std::min<int64_t>(
        (int64_t)serialized_witness_size * (wide ? SIMPLICITY_BUDGET_PER_WITNESS_BYTE : 1) + VALIDATION_WEIGHT_OFFSET,
        SIMPLICITY_BUDGET_MAX);
}

//! A taproot output paying an arbitrary (untweaked, never spent for real) key.
CScript P2TRScript()
{
    std::vector<unsigned char> program(WITNESS_V1_TAPROOT_SIZE, 0x02);
    return CScript() << OP_1 << program;
}

//! Build a one-input spend of `prevout_script` carrying `stack` as its witness.
//! `nonce` only distinguishes the funding transactions so each case gets its own
//! unspent coin.
CMutableTransaction SpendWith(CCoinsViewCache& coins, const CScript& prevout_script,
                              const std::vector<std::vector<unsigned char>>& stack,
                              int nonce)
{
    CMutableTransaction create;
    create.vin.emplace_back(uint256{}, (uint32_t)nonce);
    create.vout.emplace_back(CConfidentialAsset(CAsset{}), CConfidentialValue(CAmount(1000)), prevout_script);

    CMutableTransaction spend;
    spend.vin.emplace_back(create.GetHash(), 0);
    spend.vout.emplace_back(CConfidentialAsset(CAsset{}), CConfidentialValue(CAmount(900)), CScript() << OP_TRUE);
    spend.witness.vtxinwit.resize(1);
    spend.witness.vtxinwit[0].scriptWitness.stack = stack;

    AddCoins(coins, CTransaction{create}, 0, false);
    return spend;
}

//! Witness stack of a Simplicity script-path spend: [witness, CMR, control].
std::vector<std::vector<unsigned char>> SimplicityStack()
{
    return {
        std::vector<unsigned char>(64, 0x11),  // Simplicity witness blob
        std::vector<unsigned char>(32, 0x22),  // the 32-byte CMR, which is the "script"
        std::vector<unsigned char>(33, TAPROOT_LEAF_TAPSIMPLICITY),  // control block, depth 0
    };
}

//! The same shape at tapscript's leaf version.
std::vector<std::vector<unsigned char>> TapscriptStack()
{
    return {
        std::vector<unsigned char>(64, 0x11),
        std::vector<unsigned char>{OP_TRUE},
        std::vector<unsigned char>(33, TAPROOT_LEAF_TAPSCRIPT),
    };
}

std::vector<unsigned char> Annex(size_t len)
{
    std::vector<unsigned char> annex(len, 0x00);
    annex[0] = ANNEX_TAG;
    return annex;
}

} // namespace

BOOST_AUTO_TEST_CASE(simplicity_budget_scales_with_the_witness)
{
    // Four weight units of execution per witness byte, plus the flat offset.
    BOOST_CHECK_EQUAL(SIMPLICITY_BUDGET_PER_WITNESS_BYTE, 4);
    BOOST_CHECK_EQUAL(SimplicityBudget(0), VALIDATION_WEIGHT_OFFSET);
    BOOST_CHECK_EQUAL(SimplicityBudget(1000), 4050);

    // This is what makes padding unnecessary: the OpenDAMP verifier's static
    // cost bound is ~11.7M milli-weight-units and its functional witness is
    // about 4,300 bytes, which under a multiplier of one bought only 4,350 and
    // under four buys 17,400.
    BOOST_CHECK(SimplicityBudget(4300) * 1000 > 11'700'000);
    BOOST_CHECK((int64_t)(4300 + VALIDATION_WEIGHT_OFFSET) * 1000 < 11'700'000);

    // The interpreter requires budget <= BUDGET_MAX, so a witness large enough
    // to exceed it must clamp rather than trip that precondition.
    BOOST_CHECK_EQUAL(SimplicityBudget(SIMPLICITY_BUDGET_MAX), SIMPLICITY_BUDGET_MAX);
    BOOST_CHECK_EQUAL(SimplicityBudget(1'000'000), SIMPLICITY_BUDGET_MAX);
    BOOST_CHECK(SimplicityBudget(999'000) < SIMPLICITY_BUDGET_MAX);

    // Before the flag day the old budget applies, unchanged, which is what
    // makes running the new binary early safe.
    BOOST_CHECK_EQUAL(SimplicityBudget(1000, /*wide=*/false), 1050);
    BOOST_CHECK((int64_t)SimplicityBudget(4300, /*wide=*/false) * 1000 < 11'700'000);
}

BOOST_AUTO_TEST_CASE(the_wider_budget_is_a_flag_day_on_a_running_chain_only)
{
    Consensus::Params p;
    p.hashGenesisBlock = uint256::ONE;

    // A fresh chain: no height set, so the rule is in force from genesis. This
    // is regtest, mainnet, and any re-genesised testnet.
    BOOST_CHECK(p.SimplicityBudget4ActiveAt(0));
    BOOST_CHECK(p.SimplicityBudget4ActiveAt(1'000'000));

    // A running chain with a flag day: the OLD budget until the height, so a
    // node that upgrades early cannot split the chain, and the date is fixed
    // rather than set by whoever first broadcasts an unpadded spend.
    p.simplicity_budget4_height = 101810;
    p.simplicity_budget4_chain_genesis = uint256::ONE;
    BOOST_CHECK(!p.SimplicityBudget4ActiveAt(0));
    BOOST_CHECK(!p.SimplicityBudget4ActiveAt(101809));
    BOOST_CHECK(p.SimplicityBudget4ActiveAt(101810));
    BOOST_CHECK(p.SimplicityBudget4ActiveAt(101811));

    // A different chain that happens to reach the same height must NOT inherit
    // someone else's flag day: it has no history and no other operators, so it
    // gets the rule from genesis. This is what a re-genesis relies on.
    p.hashGenesisBlock = uint256::ZERO;
    BOOST_CHECK(p.SimplicityBudget4ActiveAt(0));
    BOOST_CHECK(p.SimplicityBudget4ActiveAt(101809));
}

BOOST_AUTO_TEST_CASE(only_the_live_testnet_carries_a_flag_day)
{
    // Every fresh chain has the rule from genesis; only the chain that is
    // already running, with operators to coordinate, waits.
    for (const auto& chain : {CBaseChainParams::SEQUENTIA, CBaseChainParams::REGTEST}) {
        const auto params = CreateChainParams(ArgsManager{}, chain);
        BOOST_CHECK_MESSAGE(params->GetConsensus().SimplicityBudget4ActiveAt(0),
                            chain + " must have the wider budget from genesis");
    }
    const auto testnet = CreateChainParams(ArgsManager{}, CBaseChainParams::TESTNET);
    const auto& tp = testnet->GetConsensus();
    BOOST_CHECK(!tp.SimplicityBudget4ActiveAt(tp.simplicity_budget4_height - 1));
    BOOST_CHECK(tp.SimplicityBudget4ActiveAt(tp.simplicity_budget4_height));
    BOOST_CHECK_EQUAL(tp.simplicity_budget4_chain_genesis, tp.hashGenesisBlock);
}

BOOST_AUTO_TEST_CASE(annex_is_standard_only_on_a_simplicity_leaf)
{
    CCoinsView dummy;
    CCoinsViewCache coins(&dummy);
    const CScript spk{P2TRScript()};

    // Baseline: both leaf versions are standard without an annex.
    {
        CMutableTransaction tx{SpendWith(coins, spk, SimplicityStack(), 1)};
        BOOST_CHECK(IsWitnessStandard(CTransaction{tx}, coins));
    }
    {
        CMutableTransaction tx{SpendWith(coins, spk, TapscriptStack(), 2)};
        BOOST_CHECK(IsWitnessStandard(CTransaction{tx}, coins));
    }

    // A Simplicity spend may carry an annex: that is how it buys execution
    // budget without buying bytes the program has to read.
    {
        auto stack{SimplicityStack()};
        stack.push_back(Annex(4096));
        CMutableTransaction tx{SpendWith(coins, spk, stack, 3)};
        BOOST_CHECK(IsWitnessStandard(CTransaction{tx}, coins));
    }

    // Right up to the policy ceiling, and not past it.
    {
        auto stack{SimplicityStack()};
        stack.push_back(Annex(MAX_STANDARD_SIMPLICITY_ANNEX_SIZE));
        CMutableTransaction tx{SpendWith(coins, spk, stack, 4)};
        BOOST_CHECK(IsWitnessStandard(CTransaction{tx}, coins));
    }
    {
        auto stack{SimplicityStack()};
        stack.push_back(Annex(MAX_STANDARD_SIMPLICITY_ANNEX_SIZE + 1));
        CMutableTransaction tx{SpendWith(coins, spk, stack, 5)};
        BOOST_CHECK(!IsWitnessStandard(CTransaction{tx}, coins));
    }

    // Everywhere else the annex still has no defined meaning: tapscript...
    {
        auto stack{TapscriptStack()};
        stack.push_back(Annex(64));
        CMutableTransaction tx{SpendWith(coins, spk, stack, 6)};
        BOOST_CHECK(!IsWitnessStandard(CTransaction{tx}, coins));
    }
    // ...and the key path, where no leaf runs at all.
    {
        std::vector<std::vector<unsigned char>> stack{
            std::vector<unsigned char>(64, 0x33),  // signature
            Annex(64),
        };
        CMutableTransaction tx{SpendWith(coins, spk, stack, 7)};
        BOOST_CHECK(!IsWitnessStandard(CTransaction{tx}, coins));
    }

    // Stripping the annex must not change how the rest of the stack is read:
    // an oversized tapscript stack item is still non-standard behind an annex
    // it is not allowed to have, and standard-sized items still pass.
    {
        auto stack{TapscriptStack()};
        stack[0] = std::vector<unsigned char>(MAX_STANDARD_TAPSCRIPT_STACK_ITEM_SIZE + 1, 0x44);
        CMutableTransaction tx{SpendWith(coins, spk, stack, 8)};
        BOOST_CHECK(!IsWitnessStandard(CTransaction{tx}, coins));
    }
    // The same oversized item is fine under Simplicity, which has no such cap.
    {
        auto stack{SimplicityStack()};
        stack[0] = std::vector<unsigned char>(MAX_STANDARD_TAPSCRIPT_STACK_ITEM_SIZE + 1, 0x44);
        stack.push_back(Annex(128));
        CMutableTransaction tx{SpendWith(coins, spk, stack, 9)};
        BOOST_CHECK(IsWitnessStandard(CTransaction{tx}, coins));
    }
}

BOOST_AUTO_TEST_SUITE_END()
