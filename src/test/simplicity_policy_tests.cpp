// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// SEQUENTIA: the node-side rules that let a Simplicity covenant pay for its own
// execution without hauling inert padding through every block.
//
//  - the budget a witness byte buys (SIMPLICITY_BUDGET_PER_WITNESS_BYTE),
//  - the height gate that turns it into a flag day on the running testnet, and
//  - the annex, standard on a Simplicity leaf and nowhere else, and there only
//    when it is zeros and exactly as large as the program's cost needs.
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
#include <script/script_error.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <optional>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(simplicity_policy_tests, BasicTestingSetup)

namespace {

//! The budget a Simplicity spend is granted, mirroring interpreter.cpp.
int64_t MirroredBudget(size_t serialized_witness_size, bool wide = true)
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
    BOOST_CHECK_EQUAL(MirroredBudget(0), VALIDATION_WEIGHT_OFFSET);
    BOOST_CHECK_EQUAL(MirroredBudget(1000), 4050);

    // This is what makes padding unnecessary: the OpenDAMP verifier's static
    // cost bound is ~11.7M milli-weight-units and its functional witness is
    // about 4,300 bytes, which under a multiplier of one bought only 4,350 and
    // under four buys 17,400.
    BOOST_CHECK(MirroredBudget(4300) * 1000 > 11'700'000);
    BOOST_CHECK((int64_t)(4300 + VALIDATION_WEIGHT_OFFSET) * 1000 < 11'700'000);

    // The interpreter requires budget <= BUDGET_MAX, so a witness large enough
    // to exceed it must clamp rather than trip that precondition.
    BOOST_CHECK_EQUAL(MirroredBudget(SIMPLICITY_BUDGET_MAX), SIMPLICITY_BUDGET_MAX);
    BOOST_CHECK_EQUAL(MirroredBudget(1'000'000), SIMPLICITY_BUDGET_MAX);
    BOOST_CHECK(MirroredBudget(999'000) < SIMPLICITY_BUDGET_MAX);

    // Before the flag day the old budget applies, unchanged, which is what
    // makes running the new binary early safe.
    BOOST_CHECK_EQUAL(MirroredBudget(1000, /*wide=*/false), 1050);
    BOOST_CHECK((int64_t)MirroredBudget(4300, /*wide=*/false) * 1000 < 11'700'000);
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

namespace {

using Stack = std::vector<std::vector<unsigned char>>;

//! The serialized size of `stack`, as the interpreter measures it.
size_t StackSize(const Stack& stack) { return ::GetSerializeSize(stack, PROTOCOL_VERSION); }

//! `stack` with its annex replaced by one a byte shorter, or removed when it is
//! the tag alone: the reference SimplicityAnnexMinCost must agree with.
Stack OneByteShorter(Stack stack)
{
    if (stack.back().size() == 1) {
        stack.pop_back();
    } else {
        stack.back().pop_back();
    }
    return stack;
}

//! The Simplicity program `unit` (SimplicityHL `fn main() {}`): no budget needed.
const std::vector<unsigned char> UNIT_PROGRAM{0x24};
const std::vector<unsigned char> UNIT_CMR{ParseHex("c40a10263f7436b4160acbef1c36fba4be4d95df181a968afeab5eac247adff7")};

//! SHA-256 over the bytes 0x00..0xff in a 256-step loop, pruned (the example
//! of ElementsProject/elements#1539): a program whose
//! cost bound, 8,045,103 milli weight units, is far above what its witness earns.
const std::vector<unsigned char> HASH_LOOP_PROGRAM{ParseHex(
    "e8144eac81081420c48a0f9128a0590da107248150b21b79b8118720e30e3e070a85a02d8370c41c920542c86e2341c9"
    "20542c86e2a36e30e3f0b30e3f0e38542cc2d6371b1b8e4c39071871f038542d016c1b906839240a8590dc8a41c92054"
    "2c86e489b71871f90461c7e429c2a16616b1b93a839240a8590dca441c920542c86e559b71871f93861c7e4f9c2a1661"
    "6b1b96e6e3430e3f204c38fc8438542cc2d6373066e618c39071871f038542d016c1b99041c70b06aa0420507cb36504"
    "20506e678e2b5620a203801a00dc0708038980e33039001390ac5f8bdd59a0d0ed8d3bb22cb0ef50f71e3a577040de5b"
    "fe99608095e7d53356765e430b9101722c0661c40cc0e4804e4a9792a2e4b85c9a01907681901c9f03958139625e588b"
    "966172d80641e0c064072ec273005e6005cc105cc280c83c380c80e6280e6600e694273545e6a85cd605cd780c83c500"
    "6407368139b92f3722e6ec2e6f80641e30032039c3039d109ceb179d6173b0173b60320f1c81901cf004e790bcf38b9e"
    "60b9ea01907902064073d840a136940aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880042"
    "050831061c9160366ce8867390b3cffedf1a67f35f4e6e69c39210d09fddb9189a14c225a77e6c262e1806006616b66d"
    "d008c0212283f4060201c180e180740780")};
const std::vector<unsigned char> HASH_LOOP_CMR{ParseHex("988c6d7a1c50012028523debc8ec575ce96920c46a45f663051aa3309f6fc539")};
constexpr int64_t HASH_LOOP_COST_MILLIWU{8'045'103};

const std::vector<unsigned char> NUMS{ParseHex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")};

//! The mempool's flags, and a block's: the same less the policy-only annex rule.
constexpr unsigned int POLICY = STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_SIMPLICITY_BUDGET4;
constexpr unsigned int CONSENSUS = POLICY & ~SCRIPT_VERIFY_SIMPLICITY_ANNEX_EXACT;

//! One coin paying a single Simplicity leaf committing to `cmr`, and a spend of
//! it whose witness is `program`'s with an empty Simplicity witness and `annex`.
ScriptError Run(const std::vector<unsigned char>& program, const std::vector<unsigned char>& cmr,
                const std::optional<std::vector<unsigned char>>& annex, unsigned int flags)
{
    const CScript leaf(cmr.begin(), cmr.end());
    TaprootBuilder builder;
    builder.Add(/*depth=*/0, leaf, TAPROOT_LEAF_TAPSIMPLICITY);
    builder.Finalize(XOnlyPubKey{NUMS});
    const CScript spk = GetScriptForDestination(builder.GetOutput());
    const std::vector<unsigned char> control = *builder.GetSpendData().scripts.at({leaf, TAPROOT_LEAF_TAPSIMPLICITY}).begin();

    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{uint256::ONE, 0});
    tx.vout.emplace_back(CConfidentialAsset{CAsset{}}, CConfidentialValue{CAmount{900}}, CScript() << OP_TRUE);
    tx.witness.vtxinwit.resize(1);
    tx.witness.vtxinwit[0].scriptWitness.stack = {{}, program, cmr, control};
    if (annex) tx.witness.vtxinwit[0].scriptWitness.stack.push_back(*annex);

    const std::vector<CTxOut> spent{CTxOut{CConfidentialAsset{CAsset{}}, CConfidentialValue{CAmount{1000}}, spk}};
    PrecomputedTransactionData txdata{uint256::ONE};
    txdata.Init(tx, std::vector<CTxOut>{spent});
    const MutableTransactionSignatureChecker checker{&tx, 0, spent[0].nValue, txdata, MissingDataBehavior::FAIL};
    ScriptError err = SCRIPT_ERR_UNKNOWN_ERROR;
    VerifyScript(tx.vin[0].scriptSig, spk, &tx.witness.vtxinwit[0].scriptWitness, flags, checker, &err);
    return err;
}

} // namespace

BOOST_AUTO_TEST_CASE(annex_min_cost_is_the_budget_one_byte_shorter)
{
    // Every annex length where a length prefix changes size, at both budget
    // rules, against the stack actually rebuilt one byte shorter.
    for (const size_t len : {1, 2, 3, 252, 253, 254, 255, 0xfffe, 0xffff, 0x10000, 0x10001}) {
        for (const int64_t per_byte : {int64_t{1}, SIMPLICITY_BUDGET_PER_WITNESS_BYTE}) {
            Stack stack{SimplicityStack()};
            stack.push_back(Annex(len));
            const int64_t min_cost = SimplicityAnnexMinCost(stack, per_byte);
            BOOST_CHECK_EQUAL(min_cost, SimplicityBudget(StackSize(OneByteShorter(stack)), per_byte));
            BOOST_CHECK(min_cost < SimplicityBudget(StackSize(stack), per_byte));
        }
    }

    // The tag alone: the shorter witness has no annex, and one item fewer,
    // which matters where the item count's own prefix changes size.
    for (const size_t items : {3, 251, 252, 253, 254}) {
        Stack stack(items, std::vector<unsigned char>(1, 0x11));
        stack.push_back(Annex(1));
        BOOST_CHECK_EQUAL(SimplicityAnnexMinCost(stack, SIMPLICITY_BUDGET_PER_WITNESS_BYTE),
                          SimplicityBudget(StackSize(OneByteShorter(stack)), SIMPLICITY_BUDGET_PER_WITNESS_BYTE));
    }
    {
        // Crossing 253 items moves the count prefix from one byte to three.
        Stack stack(252, std::vector<unsigned char>(1, 0x11));
        stack.push_back(Annex(1));
        BOOST_CHECK_EQUAL(StackSize(stack) - StackSize(OneByteShorter(stack)), 2U + 2U);
    }

    // Past the cap both are the cap: an annex can buy nothing more, so a
    // program can never need it and none is standard there.
    Stack huge{SimplicityStack()};
    huge.push_back(Annex(1'100'000));
    BOOST_CHECK_EQUAL(SimplicityAnnexMinCost(huge, SIMPLICITY_BUDGET_PER_WITNESS_BYTE), SIMPLICITY_BUDGET_MAX);
    BOOST_CHECK_EQUAL(SimplicityBudget(StackSize(huge), SIMPLICITY_BUDGET_PER_WITNESS_BYTE), SIMPLICITY_BUDGET_MAX);
}

BOOST_AUTO_TEST_CASE(an_annex_is_standard_only_when_the_program_needs_all_of_it)
{
    // A program its witness already pays for: no annex is standard, not even
    // the tag alone, and every one remains valid by consensus.
    BOOST_CHECK_EQUAL(Run(UNIT_PROGRAM, UNIT_CMR, std::nullopt, POLICY), SCRIPT_ERR_OK);
    for (const size_t len : {1, 3, 1000, 100'000}) {
        BOOST_CHECK_EQUAL(Run(UNIT_PROGRAM, UNIT_CMR, Annex(len), POLICY), SCRIPT_ERR_SIMPLICITY_OVERWEIGHT);
        BOOST_CHECK_EQUAL(Run(UNIT_PROGRAM, UNIT_CMR, Annex(len), CONSENSUS), SCRIPT_ERR_OK);
    }
    BOOST_CHECK_EQUAL(Run(UNIT_PROGRAM, UNIT_CMR, ParseHex("50f111"), POLICY), SCRIPT_ERR_SIMPLICITY_ANNEX_NONZERO);
    BOOST_CHECK_EQUAL(Run(UNIT_PROGRAM, UNIT_CMR, ParseHex("50f111"), CONSENSUS), SCRIPT_ERR_OK);

    // A program that needs budget: the smallest annex that covers its cost,
    // computed the way a wallet does, is the only standard one.
    const Stack base{{}, HASH_LOOP_PROGRAM, HASH_LOOP_CMR, std::vector<unsigned char>(33, TAPROOT_LEAF_TAPSIMPLICITY)};
    BOOST_CHECK(SimplicityBudget(StackSize(base), SIMPLICITY_BUDGET_PER_WITNESS_BYTE) * 1000 < HASH_LOOP_COST_MILLIWU);
    size_t len = 1;
    while (true) {
        Stack padded{base};
        padded.push_back(Annex(len));
        if (SimplicityBudget(StackSize(padded), SIMPLICITY_BUDGET_PER_WITNESS_BYTE) * 1000 >= HASH_LOOP_COST_MILLIWU) break;
        ++len;
    }
    BOOST_CHECK_EQUAL(len, 1427U);
    // The node agrees: the shortest annex consensus runs the program with.
    size_t node_len = 1;
    while (Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, Annex(node_len), CONSENSUS) == SCRIPT_ERR_SIMPLICITY_EXEC_BUDGET) ++node_len;
    BOOST_CHECK_EQUAL(node_len, len);

    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, std::nullopt, CONSENSUS), SCRIPT_ERR_SIMPLICITY_EXEC_BUDGET);
    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, Annex(len - 1), CONSENSUS), SCRIPT_ERR_SIMPLICITY_EXEC_BUDGET);
    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, Annex(len - 1), POLICY), SCRIPT_ERR_SIMPLICITY_EXEC_BUDGET);
    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, Annex(len), POLICY), SCRIPT_ERR_OK);
    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, Annex(len), CONSENSUS), SCRIPT_ERR_OK);
    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, Annex(len + 1), POLICY), SCRIPT_ERR_SIMPLICITY_OVERWEIGHT);
    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, Annex(len + 1), CONSENSUS), SCRIPT_ERR_OK);
    auto nonzero{Annex(len)};
    nonzero.back() = 0x01;
    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, nonzero, POLICY), SCRIPT_ERR_SIMPLICITY_ANNEX_NONZERO);
    BOOST_CHECK_EQUAL(Run(HASH_LOOP_PROGRAM, HASH_LOOP_CMR, nonzero, CONSENSUS), SCRIPT_ERR_OK);

    // The rule is policy: no block's flags carry it.
    BOOST_CHECK(STANDARD_SCRIPT_VERIFY_FLAGS & SCRIPT_VERIFY_SIMPLICITY_ANNEX_EXACT);
    BOOST_CHECK(!(MANDATORY_SCRIPT_VERIFY_FLAGS & SCRIPT_VERIFY_SIMPLICITY_ANNEX_EXACT));
}

BOOST_AUTO_TEST_SUITE_END()
