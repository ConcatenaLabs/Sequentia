// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// SEQUENTIA: a Simplicity input the checker has no transaction data for.
//
// A Simplicity program may read the spent output of every input, so it can only
// run once the precomputed transaction data holds all of them. Block and mempool
// validation always have them. The signing RPCs often do not: they are handed
// transactions with inputs whose parents the node has never seen. A checker
// without the data refuses the input with a script error of its own, and
// SignTransaction reports it, instead of the node aborting.

#include <coins.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/script_error.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <simplicity/elements/env.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(simplicity_checker_tests, BasicTestingSetup)

namespace {

//! The Simplicity program `unit`: it accepts any transaction, with an empty witness.
const std::vector<unsigned char> UNIT_PROGRAM{0x24};
const std::vector<unsigned char> UNIT_CMR{ParseHex("c40a10263f7436b4160acbef1c36fba4be4d95df181a968afeab5eac247adff7")};
//! BIP341's nothing-up-my-sleeve point: no key path.
const std::vector<unsigned char> NUMS{ParseHex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")};

constexpr unsigned int FLAGS = STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_SIMPLICITY;

//! A transaction spending two coins that each pay `unit` in a Simplicity leaf,
//! both inputs carrying the program and its empty witness.
struct TwoUnitSpends {
    CScript spk;
    std::vector<CTxOut> spent;
    CMutableTransaction tx;

    TwoUnitSpends()
    {
        const CScript leaf(UNIT_CMR.begin(), UNIT_CMR.end());
        TaprootBuilder builder;
        builder.Add(/*depth=*/0, leaf, TAPROOT_LEAF_TAPSIMPLICITY);
        builder.Finalize(XOnlyPubKey{NUMS});
        spk = GetScriptForDestination(builder.GetOutput());
        const std::vector<unsigned char> control = *builder.GetSpendData().scripts.at({leaf, TAPROOT_LEAF_TAPSIMPLICITY}).begin();

        for (uint32_t n = 0; n < 2; ++n) {
            tx.vin.emplace_back(COutPoint{uint256::ONE, n});
            spent.emplace_back(CConfidentialAsset{CAsset{}}, CConfidentialValue{CAmount{1000}}, spk);
        }
        tx.vout.emplace_back(CConfidentialAsset{CAsset{}}, CConfidentialValue{CAmount{2000}}, CScript() << OP_TRUE);
        tx.witness.vtxinwit.resize(2);
        for (auto& in : tx.witness.vtxinwit) {
            in.scriptWitness.stack = {{}, UNIT_PROGRAM, UNIT_CMR, control};
        }
    }

    ScriptError Verify(unsigned int n, const MutableTransactionSignatureChecker& checker) const
    {
        ScriptError err = SCRIPT_ERR_UNKNOWN_ERROR;
        VerifyScript(tx.vin[n].scriptSig, spk, &tx.witness.vtxinwit[n].scriptWitness, FLAGS, checker, &err);
        return err;
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(simplicity_without_txdata_is_a_script_error)
{
    const TwoUnitSpends s;

    // A checker built without any precomputed data.
    const MutableTransactionSignatureChecker no_txdata{&s.tx, 0, s.spent[0].nValue, MissingDataBehavior::FAIL};
    BOOST_CHECK_EQUAL(s.Verify(0, no_txdata), SCRIPT_ERR_SIMPLICITY_TX_DATA_MISSING);

    rawElementsTapEnv tap_env{};
    ScriptError err = SCRIPT_ERR_OK;
    BOOST_CHECK(!no_txdata.CheckSimplicity(UNIT_PROGRAM, {}, tap_env, /*budget=*/1000, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_SIMPLICITY_TX_DATA_MISSING);

    // The shape the signing RPCs build when a parent is unknown: forced
    // precomputation with no spent outputs, so no Simplicity data.
    PrecomputedTransactionData partial{uint256::ONE};
    partial.Init(s.tx, /*spent_outputs=*/{}, /*force=*/true);
    BOOST_CHECK(!partial.m_simplicity_tx_data);
    const MutableTransactionSignatureChecker partial_checker{&s.tx, 0, s.spent[0].nValue, partial, MissingDataBehavior::FAIL};
    BOOST_CHECK_EQUAL(s.Verify(0, partial_checker), SCRIPT_ERR_SIMPLICITY_TX_DATA_MISSING);

    // Control: with every spent output known, the program runs and accepts.
    PrecomputedTransactionData full{uint256::ONE};
    full.Init(s.tx, std::vector<CTxOut>{s.spent});
    BOOST_CHECK(full.m_simplicity_tx_data);
    for (unsigned int n = 0; n < 2; ++n) {
        const MutableTransactionSignatureChecker checker{&s.tx, n, s.spent[n].nValue, full, MissingDataBehavior::FAIL};
        BOOST_CHECK_EQUAL(s.Verify(n, checker), SCRIPT_ERR_OK);
    }

    BOOST_CHECK_EQUAL(ScriptErrorString(SCRIPT_ERR_SIMPLICITY_TX_DATA_MISSING),
                      "Simplicity input cannot be checked without the spent output of every input");
}

BOOST_AUTO_TEST_CASE(sign_transaction_reports_an_uncheckable_simplicity_input)
{
    const TwoUnitSpends s;
    const FillableSigningProvider keystore;

    // One parent unknown: that input is not found, and the Simplicity input
    // beside it is reported as uncheckable.
    {
        CMutableTransaction tx{s.tx};
        std::map<COutPoint, Coin> coins;
        coins.emplace(tx.vin[0].prevout, Coin{s.spent[0], 1, false});
        std::map<int, bilingual_str> errors;
        BOOST_CHECK(!SignTransaction(tx, &keystore, coins, SIGHASH_ALL, uint256::ONE, errors));
        BOOST_CHECK_EQUAL(errors.size(), 2U);
        BOOST_CHECK_EQUAL(errors.at(0).original, "Simplicity input cannot be checked without the spent output of every input");
        BOOST_CHECK_EQUAL(errors.at(1).original, "Input not found or already spent");
        // The witness the input already carried is left in place.
        BOOST_CHECK(tx.witness.vtxinwit[0].scriptWitness.stack == s.tx.witness.vtxinwit[0].scriptWitness.stack);
    }

    // Every parent known: both inputs check and the transaction is complete.
    {
        CMutableTransaction tx{s.tx};
        std::map<COutPoint, Coin> coins;
        for (unsigned int n = 0; n < 2; ++n) coins.emplace(tx.vin[n].prevout, Coin{s.spent[n], 1, false});
        std::map<int, bilingual_str> errors;
        BOOST_CHECK(SignTransaction(tx, &keystore, coins, SIGHASH_ALL, uint256::ONE, errors));
        BOOST_CHECK(errors.empty());
    }
}

BOOST_AUTO_TEST_SUITE_END()
