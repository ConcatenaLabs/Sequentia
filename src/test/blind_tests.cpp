// Copyright (c) 2013-2019 The Elements Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <blind.h>
#include <coins.h>
#include <random.h>
#include <script/sigcache.h>
#include <uint256.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <secp256k1.h>

// For elements serialization rules
struct ElementsSetup : public TestingSetup {
        ElementsSetup() : TestingSetup("custom") {}
};

BOOST_FIXTURE_TEST_SUITE(blind_tests, ElementsSetup)

// TODO: Make deterministic blinding wrapper function, test caching more exactly

BOOST_AUTO_TEST_CASE(naive_blinding_test)
{
    CKey key1;
    CKey key2;
    CKey keyDummy;

    // Any asset id will do
    CAsset bitcoinID(GetRandHash());
    CAsset otherID(GetRandHash());
    CAsset unblinded_id;
    uint256 asset_blind;
    CScript op_true(OP_TRUE);
    std::vector<CKey> vDummy;

    unsigned char k1[32] = {1,2,3};
    unsigned char k2[32] = {22,33,44};
    unsigned char kDummy[32] = {133,144,155};
    key1.Set(&k1[0], &k1[32], true);
    key2.Set(&k2[0], &k2[32], true);
    keyDummy.Set(&kDummy[0], &kDummy[32], true);
    CPubKey pubkey1 = key1.GetPubKey();
    CPubKey pubkey2 = key2.GetPubKey();
    CPubKey pubkeyDummy = keyDummy.GetPubKey();

    uint256 blind3, blind4, blindDummy;

    std::vector<CTxOut> inputs;
    CTxOut btc_oo(bitcoinID, 11, CScript());
    CTxOut btc_ooo(bitcoinID, 111, CScript());
    CTxOut other_fzz(otherID, 500, CScript());
    CTxOut blind_ozz; // Will be computed later

    {
        inputs.clear();
        inputs.push_back(btc_oo);
        inputs.push_back(btc_ooo);

        // Build a transaction that spends 2 unblinded coins (11, 111), and produces a single blinded one (100) and fee (22).
        CMutableTransaction tx3;
        tx3.vin.resize(2);
        tx3.vin[0].prevout.hash = ArithToUint256(1);

        tx3.vin[0].prevout.n = 0;
        tx3.vin[1].prevout.hash = ArithToUint256(2);
        tx3.vin[1].prevout.n = 0;
        tx3.vout.resize(0);
        tx3.vout.push_back(CTxOut(bitcoinID, 100, CScript() << OP_TRUE));
        // Fee outputs are blank scriptpubkeys, and unblinded value/asset
        tx3.vout.push_back(CTxOut(bitcoinID, 22, CScript()));
        BOOST_CHECK(VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));

        // Malleate the output and check for correct handling of bad commitments
        // These will fail IsValid checks
        std::vector<unsigned char> asset_copy(tx3.vout[0].nAsset.vchCommitment);
        std::vector<unsigned char> value_copy(tx3.vout[0].nValue.vchCommitment);
        tx3.vout[0].nAsset.vchCommitment[0] = 122;
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));
        tx3.vout[0].nAsset.vchCommitment = asset_copy;
        tx3.vout[0].nValue.vchCommitment[0] = 122;
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));
        tx3.vout[0].nValue.vchCommitment = value_copy;

        // Make sure null values are handled correctly
        tx3.vout[0].nAsset.SetNull();
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));
        tx3.vout[0].nAsset.vchCommitment = asset_copy;
        tx3.vout[0].nValue.SetNull();
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));
        tx3.vout[0].nValue.vchCommitment = value_copy;

        // Bad nonce values will result in failure to deserialize
        tx3.vout[0].nNonce.SetNull();
        BOOST_CHECK(VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));
        tx3.vout[0].nNonce.vchCommitment = tx3.vout[0].nValue.vchCommitment;
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));

        // Try to blind with a single non-fee output, which fails as its blinding factor ends up being zero.
        std::vector<uint256> input_blinds;
        std::vector<uint256> input_asset_blinds;
        std::vector<CAsset> input_assets;
        std::vector<CAmount> input_amounts;
        std::vector<uint256> output_blinds;
        std::vector<uint256> output_asset_blinds;
        std::vector<CPubKey> output_pubkeys;
        input_blinds.push_back(uint256());
        input_blinds.push_back(uint256());
        input_asset_blinds.push_back(uint256());
        input_asset_blinds.push_back(uint256());
        input_assets.push_back(bitcoinID);
        input_assets.push_back(bitcoinID);
        input_amounts.push_back(11);
        input_amounts.push_back(111);
        output_pubkeys.push_back(pubkey1);
        output_pubkeys.push_back(CPubKey());
        BOOST_CHECK(BlindTransaction(input_blinds, input_asset_blinds, input_assets, input_amounts, output_blinds, output_asset_blinds, output_pubkeys, vDummy, vDummy, tx3) == 0);

        // Add a dummy output. Must be unspendable since it's 0-valued.
        tx3.vout.push_back(CTxOut(bitcoinID, 0, CScript() << OP_RETURN));
        output_pubkeys.push_back(pubkeyDummy);
        BOOST_CHECK(BlindTransaction(input_blinds, input_asset_blinds, input_assets, input_amounts, output_blinds, output_asset_blinds, output_pubkeys, vDummy, vDummy, tx3) == 2);
        BOOST_CHECK(!tx3.vout[0].nValue.IsExplicit());
        BOOST_CHECK(!tx3.vout[2].nValue.IsExplicit());
        BOOST_CHECK(VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));

        CAmount unblinded_amount;
        BOOST_CHECK(UnblindConfidentialPair(key2, tx3.vout[0].nValue, tx3.vout[0].nAsset, tx3.vout[0].nNonce, op_true, tx3.witness.vtxoutwit[0].vchRangeproof, unblinded_amount, blind3, unblinded_id, asset_blind) == 0);
        // Saving unblinded_id and asset_blind for later since we need for input
        BOOST_CHECK(UnblindConfidentialPair(key1, tx3.vout[0].nValue, tx3.vout[0].nAsset, tx3.vout[0].nNonce, op_true, tx3.witness.vtxoutwit[0].vchRangeproof, unblinded_amount, blind3, unblinded_id, asset_blind) == 1);
        BOOST_CHECK(unblinded_amount == 100);
        BOOST_CHECK(unblinded_id == bitcoinID);
        CAsset temp_asset;
        uint256 temp_asset_blinder;
        BOOST_CHECK(UnblindConfidentialPair(keyDummy, tx3.vout[2].nValue, tx3.vout[2].nAsset, tx3.vout[2].nNonce, CScript() << OP_RETURN, tx3.witness.vtxoutwit[2].vchRangeproof, unblinded_amount, blindDummy, temp_asset, temp_asset_blinder) == 1);
        BOOST_CHECK(unblinded_amount == 0);

        // Storing for next section
        BOOST_CHECK(tx3.vout[0].nValue.IsCommitment());
        BOOST_CHECK(tx3.vout[0].nAsset.IsCommitment());
        blind_ozz = tx3.vout[0];

        tx3.vout[1].nValue = CConfidentialValue(tx3.vout[1].nValue.GetAmount() - 1);
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx3), false, nullptr, nullptr, false));
    }

    {
        inputs.clear();
        inputs.push_back(btc_ooo);
        inputs.push_back(blind_ozz);

        // Build a transactions that spends an unblinded (111) and blinded (100) coin, and produces only unblinded coins (impossible)
        CMutableTransaction tx4;
        tx4.vin.resize(2);
        tx4.vin[0].prevout.hash = ArithToUint256(2);
        tx4.vin[0].prevout.n = 0;
        tx4.vin[1].prevout.hash = ArithToUint256(3);
        tx4.vin[1].prevout.n = 0;
        tx4.vout.push_back(CTxOut(bitcoinID, 30, CScript() << OP_TRUE));
        tx4.vout.push_back(CTxOut(bitcoinID, 40, CScript() << OP_TRUE));
        tx4.vout.push_back(CTxOut(bitcoinID, 111+100-30-40, CScript()));
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx4), false, nullptr, nullptr, false)); // Spends a blinded coin with no blinded outputs to compensate.

        std::vector<uint256> input_blinds;
        std::vector<uint256> input_asset_blinds;
        std::vector<CAsset> input_assets;
        std::vector<CAmount> input_amounts;
        std::vector<uint256> output_blinds;
        std::vector<uint256> output_asset_blinds;
        std::vector<CPubKey> output_pubkeys;
        input_blinds.push_back(uint256());
        input_blinds.push_back(blind3);
        input_asset_blinds.push_back(uint256());
        input_asset_blinds.push_back(asset_blind);
        input_amounts.push_back(111);
        input_amounts.push_back(100);
        input_assets.push_back(unblinded_id);
        input_assets.push_back(unblinded_id);
        output_pubkeys.push_back(CPubKey());
        output_pubkeys.push_back(CPubKey());
        output_pubkeys.push_back(CPubKey());
        BOOST_CHECK(BlindTransaction(input_blinds, input_asset_blinds, input_assets, input_amounts, output_blinds, output_asset_blinds, output_pubkeys, vDummy, vDummy, tx4) == 0); // Blinds nothing
    }

    {
        inputs.clear();
        inputs.push_back(btc_ooo);
        inputs.push_back(blind_ozz);

        // Build a transactions that spends an unblinded (111) and blinded (100) coin, and produces a blinded (30), unblinded (40), and blinded (50) coin and fee (91)
        CMutableTransaction tx4;
        tx4.vin.resize(2);
        tx4.vin[0].prevout.hash = ArithToUint256(2);
        tx4.vin[0].prevout.n = 0;
        tx4.vin[1].prevout.hash = ArithToUint256(3);
        tx4.vin[1].prevout.n = 0;
        tx4.vout.push_back(CTxOut(bitcoinID, 30, CScript() << OP_TRUE));
        tx4.vout.push_back(CTxOut(bitcoinID, 40, CScript() << OP_TRUE));
        tx4.vout.push_back(CTxOut(bitcoinID, 50, CScript() << OP_TRUE));
        // Fee
        tx4.vout.push_back(CTxOut(bitcoinID, 111+100-30-40-50, CScript()));
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx4), false, nullptr, nullptr, false)); // Spends a blinded coin with no blinded outputs to compensate.

        std::vector<uint256> input_blinds;
        std::vector<uint256> input_asset_blinds;
        std::vector<CAsset> input_assets;
        std::vector<CAmount> input_amounts;
        std::vector<uint256> output_blinds;
        std::vector<uint256> output_asset_blinds;
        std::vector<CPubKey> output_pubkeys;

        input_blinds.push_back(uint256());
        input_blinds.push_back(blind3);
        input_asset_blinds.push_back(uint256());
        input_asset_blinds.push_back(asset_blind);
        input_amounts.push_back(111);
        input_amounts.push_back(100);
        input_assets.push_back(unblinded_id);
        input_assets.push_back(unblinded_id);

        output_pubkeys.push_back(pubkey2);
        output_pubkeys.push_back(CPubKey());
        output_pubkeys.push_back(pubkey2);
        output_pubkeys.push_back(CPubKey());

        BOOST_CHECK(BlindTransaction(input_blinds, input_asset_blinds, input_assets, input_amounts, output_blinds, output_asset_blinds, output_pubkeys, vDummy, vDummy, tx4) == 2);
        BOOST_CHECK(!tx4.vout[0].nValue.IsExplicit());
        BOOST_CHECK(tx4.vout[1].nValue.IsExplicit());
        BOOST_CHECK(!tx4.vout[2].nValue.IsExplicit());
        // This one broken
        BOOST_CHECK(VerifyAmounts(inputs, CTransaction(tx4), false, nullptr, nullptr, false));

        CAmount unblinded_amount;
        CAsset asset_out;
        uint256 asset_blinder_out;
        BOOST_CHECK(UnblindConfidentialPair(key1, tx4.vout[0].nValue, tx4.vout[0].nAsset, tx4.vout[0].nNonce, op_true, tx4.witness.vtxoutwit[0].vchRangeproof, unblinded_amount, blind4, asset_out, asset_blinder_out) == 0);
        BOOST_CHECK(UnblindConfidentialPair(key2, tx4.vout[0].nValue, tx4.vout[0].nAsset, tx4.vout[0].nNonce, op_true, tx4.witness.vtxoutwit[0].vchRangeproof, unblinded_amount, blind4, asset_out, asset_blinder_out) == 1);
        BOOST_CHECK(unblinded_amount == 30);
        BOOST_CHECK(asset_out == unblinded_id);
        BOOST_CHECK(UnblindConfidentialPair(key2, tx4.vout[2].nValue, tx4.vout[2].nAsset, tx4.vout[2].nNonce, op_true, tx4.witness.vtxoutwit[2].vchRangeproof, unblinded_amount, blind4, asset_out, asset_blinder_out) == 1);
        BOOST_CHECK(asset_out == unblinded_id);
        BOOST_CHECK(unblinded_amount == 50);

        // Commit to the wrong script in the rangeproof
        BOOST_CHECK(UnblindConfidentialPair(key2, tx4.vout[2].nValue, tx4.vout[2].nAsset, tx4.vout[2].nNonce, CScript() << OP_FALSE, tx4.witness.vtxoutwit[2].vchRangeproof, unblinded_amount, blind4, asset_out, asset_blinder_out) == 0);

        // Make invalid public keys in nonce commitment, first of right size
        tx4.vout[2].nNonce.vchCommitment = std::vector<unsigned char>(33, 0);
        tx4.vout[2].nNonce.vchCommitment[0] = 0x03;
        BOOST_CHECK(UnblindConfidentialPair(key2, tx4.vout[2].nValue, tx4.vout[2].nAsset, tx4.vout[2].nNonce, op_true, tx4.witness.vtxoutwit[2].vchRangeproof, unblinded_amount, blind4, asset_out, asset_blinder_out) == 0);

        // Next, leading byte claiming to be 33 bytes in size
        tx4.vout[2].nNonce.vchCommitment.resize(1);
        BOOST_CHECK(UnblindConfidentialPair(key2, tx4.vout[2].nValue, tx4.vout[2].nAsset, tx4.vout[2].nNonce, op_true, tx4.witness.vtxoutwit[2].vchRangeproof, unblinded_amount, blind4, asset_out, asset_blinder_out) == 0);

        // Last, blank nonce commitment
        tx4.vout[2].nNonce.vchCommitment.clear();
        BOOST_CHECK(UnblindConfidentialPair(key2, tx4.vout[2].nValue, tx4.vout[2].nAsset, tx4.vout[2].nNonce, op_true, tx4.witness.vtxoutwit[2].vchRangeproof, unblinded_amount, blind4, asset_out, asset_blinder_out) == 0);

        tx4.vout[3].nValue = CConfidentialValue(tx4.vout[3].nValue.GetAmount() - 1);
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx4), false, nullptr, nullptr, false));

        // Check wallet borromean-based rangeproof results against expected args
        size_t proof_size = DEFAULT_RANGEPROOF_SIZE;
        BOOST_CHECK_EQUAL(tx4.witness.vtxoutwit[2].vchRangeproof.size(), proof_size);
        secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
        int exp = 0;
        int mantissa = 0;
        uint64_t min_value = 0;
        uint64_t max_value = 0;
        BOOST_CHECK(secp256k1_rangeproof_info(ctx, &exp, &mantissa, &min_value, &max_value, tx4.witness.vtxoutwit[2].vchRangeproof.data(), proof_size) == 1);
        BOOST_CHECK_EQUAL(exp, 0);
        BOOST_CHECK_EQUAL(mantissa, 52); // 52 bit default
        BOOST_CHECK_EQUAL(min_value, 1ULL);
        BOOST_CHECK_EQUAL(max_value, 4503599627370496ULL);
        secp256k1_context_destroy(ctx);
    }
    {
        inputs.clear();
        inputs.push_back(blind_ozz);
        inputs.push_back(other_fzz);

        // Spends 100 blinded bitcoin, 500 of unblinded "other"
        CMutableTransaction tx5;
        tx5.vin.resize(0);
        tx5.vout.resize(0);
        tx5.vin.push_back(CTxIn(COutPoint(ArithToUint256(3), 0)));
        tx5.vin.push_back(CTxIn(COutPoint(ArithToUint256(5), 0)));
        tx5.vout.push_back(CTxOut(bitcoinID, 29, CScript() << OP_TRUE));
        tx5.vout.push_back(CTxOut(bitcoinID, 70, CScript() << OP_TRUE));
        tx5.vout.push_back(CTxOut(otherID, 250, CScript() << OP_TRUE));
        tx5.vout.push_back(CTxOut(otherID, 249, CScript() << OP_TRUE));
        // Fees
        tx5.vout.push_back(CTxOut(bitcoinID, 1, CScript()));
        tx5.vout.push_back(CTxOut(otherID, 1, CScript()));

        // Blinds don't balance
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(tx5), false, nullptr, nullptr, false));

        // Blinding setup stuff
        std::vector<uint256> input_blinds;
        std::vector<uint256> input_asset_blinds;
        std::vector<CAsset> input_assets;
        std::vector<CAmount> input_amounts;
        std::vector<uint256> output_blinds;
        std::vector<uint256> output_asset_blinds;
        std::vector<CPubKey> output_pubkeys;
        input_blinds.push_back(blind3);
        input_blinds.push_back(uint256()); //
        input_asset_blinds.push_back(asset_blind);
        input_asset_blinds.push_back(uint256());
        input_amounts.push_back(100);
        input_amounts.push_back(500);
        input_assets.push_back(bitcoinID);
        input_assets.push_back(otherID);
        for (unsigned int i = 0; i < 6; i++) {
            output_pubkeys.push_back(pubkey2);
        }

        CMutableTransaction txtemp(tx5);

        // No blinding keys for fees, bails out blinding nothing, still invalid due to imbalance
        BOOST_CHECK(BlindTransaction(input_blinds, input_asset_blinds, input_assets, input_amounts, output_blinds, output_asset_blinds, output_pubkeys, vDummy, vDummy, txtemp) == -1);
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(txtemp), false, nullptr, nullptr, false));
        // Last will be implied blank keys
        output_pubkeys.resize(4);

        // Blind transaction, verify amounts
        txtemp = tx5;
        BOOST_CHECK(BlindTransaction(input_blinds, input_asset_blinds, input_assets, input_amounts, output_blinds, output_asset_blinds, output_pubkeys, vDummy, vDummy, txtemp) == 4);
        BOOST_CHECK(VerifyAmounts(inputs, CTransaction(txtemp), false, nullptr, nullptr, false));

        // Transaction may not have spendable 0-value output
        txtemp.vout.push_back(CTxOut(CAsset(), 0, CScript() << OP_TRUE));
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(txtemp), false, nullptr, nullptr, false));

        // Create imbalance by removing fees, should still be able to blind
        txtemp = tx5;
        txtemp.vout.resize(5);
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(txtemp), false, nullptr, nullptr, false));
        txtemp.vout.resize(4);
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(txtemp), false, nullptr, nullptr, false));
        BOOST_CHECK(BlindTransaction(input_blinds, input_asset_blinds, input_assets, input_amounts, output_blinds, output_asset_blinds, output_pubkeys, vDummy, vDummy, txtemp) == 4);
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(txtemp), false, nullptr, nullptr, false));

        txtemp = tx5;
        // Remove other input, make surjection proof impossible for 2 "otherID" outputs
        std::vector<uint256> t_input_blinds;
        std::vector<uint256> t_input_asset_blinds;
        std::vector<CAsset> t_input_assets;
        std::vector<CAmount> t_input_amounts;

        t_input_blinds = input_blinds;
        t_input_asset_blinds = input_asset_blinds;
        t_input_assets = input_assets;
        t_input_amounts = input_amounts;
        txtemp.vin.resize(1);
        inputs.resize(1);
        t_input_blinds.resize(1);
        t_input_asset_blinds.resize(1);
        t_input_assets.resize(1);
        t_input_amounts.resize(1);
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(txtemp), false, nullptr, nullptr, false));
        BOOST_CHECK(BlindTransaction(t_input_blinds, t_input_asset_blinds, t_input_assets, t_input_amounts, output_blinds, output_asset_blinds, output_pubkeys, vDummy, vDummy, txtemp) == 2);
        BOOST_CHECK(!VerifyAmounts(inputs, CTransaction(txtemp), false, nullptr, nullptr, false));
    }
}
// Build a valid rangeproof for `amount` of `asset` locked to `scriptPubKey`,
// returning the serialised value and asset commitments alongside it.
static void MakeRangeproof(std::vector<unsigned char>& rangeproof, CConfidentialValue& conf_value, CConfidentialAsset& conf_asset, const CAsset& asset, const CAmount amount, const CScript& scriptPubKey)
{
    uint256 value_blind = GetRandHash();
    uint256 asset_blind = GetRandHash();

    secp256k1_generator asset_gen;
    BlindAsset(conf_asset, asset_gen, asset, asset_blind.begin());

    secp256k1_pedersen_commitment value_commit;
    CreateValueCommitment(conf_value, value_commit, value_blind.begin(), asset_gen, amount);

    std::vector<unsigned char*> value_blindptrs{value_blind.begin()};
    std::vector<const unsigned char*> asset_blindptrs{asset_blind.begin()};
    BOOST_CHECK(GenerateRangeproof(rangeproof, value_blindptrs, GetRandHash(), amount, scriptPubKey, value_commit, asset_gen, asset, asset_blindptrs));
}

// The rangeproof cache must key on every argument secp256k1_rangeproof_verify
// is given. It used to key on the proof and the value commitment alone, so a
// proof accepted for one asset and script was replayed, unverified, under any
// other asset and script -- which is enough to hand an output a value nobody
// ever proved to be in range.
BOOST_AUTO_TEST_CASE(rangeproof_cache_binding_test)
{
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY | SECP256K1_CONTEXT_SIGN);
    BOOST_CHECK(ctx != nullptr);

    const CachingRangeProofChecker checker(true /* store */);

    const CAsset asset(GetRandHash());
    const CScript script = CScript() << OP_1 << OP_EQUAL;
    const CScript other_script = CScript() << OP_2 << OP_EQUAL;

    std::vector<unsigned char> rangeproof;
    CConfidentialValue conf_value;
    CConfidentialAsset conf_asset;
    MakeRangeproof(rangeproof, conf_value, conf_asset, asset, 1000, script);

    // The genuine article verifies, and lands in the cache.
    BOOST_CHECK(checker.VerifyRangeProof(rangeproof, conf_value.vchCommitment, conf_asset.vchCommitment, script, ctx));
    BOOST_CHECK(checker.VerifyRangeProof(rangeproof, conf_value.vchCommitment, conf_asset.vchCommitment, script, ctx));

    // Same proof, same value commitment, different asset commitment: the proof
    // says nothing about this generator, so the cache must not answer for it.
    CConfidentialAsset other_conf_asset;
    secp256k1_generator other_gen;
    uint256 other_asset_blind = GetRandHash();
    BlindAsset(other_conf_asset, other_gen, CAsset(GetRandHash()), other_asset_blind.begin());
    BOOST_CHECK(!checker.VerifyRangeProof(rangeproof, conf_value.vchCommitment, other_conf_asset.vchCommitment, script, ctx));

    // Same proof, different script: the script is the proof's extra commitment,
    // and it also decides whether a zero minimum value is allowed.
    BOOST_CHECK(!checker.VerifyRangeProof(rangeproof, conf_value.vchCommitment, conf_asset.vchCommitment, other_script, ctx));

    // The shape the issuance path hands us: an issuance rangeproof commits to
    // an empty (unspendable) script, so it is allowed a minimum value of zero.
    // Replaying it on a spendable output is how a reissuance token gets
    // conjured out of nothing.
    std::vector<unsigned char> issuance_rangeproof;
    CConfidentialValue issuance_value;
    CConfidentialAsset issuance_asset;
    MakeRangeproof(issuance_rangeproof, issuance_value, issuance_asset, CAsset(GetRandHash()), 0, CScript());
    BOOST_CHECK(checker.VerifyRangeProof(issuance_rangeproof, issuance_value.vchCommitment, issuance_asset.vchCommitment, CScript(), ctx));
    BOOST_CHECK(!checker.VerifyRangeProof(issuance_rangeproof, issuance_value.vchCommitment, issuance_asset.vchCommitment, script, ctx));

    secp256k1_context_destroy(ctx);
}

// The rangeproof cache key must not be a raw, length-undelimited concatenation
// of its fields. If it were, two DISTINCT argument tuples whose fields
// concatenate to the same byte stream would map to the same cache entry, and a
// positive result cached for one would be returned for the other without
// verification. Length-prefixed serialization removes that: distinct tuples
// cannot share an encoding. This is the exact "boundary shift" collision class.
BOOST_AUTO_TEST_CASE(rangeproof_cache_length_prefix_test)
{
    // Two 33-byte commitment blobs (contents irrelevant; this exercises the key
    // derivation, not proof verification).
    std::vector<unsigned char> cc(33), ac(33);
    for (int i = 0; i < 33; ++i) { cc[i] = (unsigned char)(0x40 + i); ac[i] = (unsigned char)(0x80 + i); }

    // Tuple A: proof={0xAA}, comm=cc, asset=ac, script={0xBB}
    std::vector<unsigned char> proofA{0xAA};
    std::vector<unsigned char> commA = cc;
    std::vector<unsigned char> assetA = ac;
    std::vector<unsigned char> sbytesA{0xBB};
    CScript scriptA(sbytesA.begin(), sbytesA.end());

    // Tuple B: one byte shifted across each field boundary, so the raw
    // concatenation proof|comm|asset|script is byte-for-byte identical to A,
    // while the tuple itself is different.
    std::vector<unsigned char> proofB{0xAA, cc[0]};
    std::vector<unsigned char> commB(cc.begin() + 1, cc.end());   // cc[1..32]
    commB.push_back(ac[0]);                                       // + ac[0]  -> 33 bytes
    std::vector<unsigned char> assetB(ac.begin() + 1, ac.end());  // ac[1..32]
    assetB.push_back(0xBB);                                       // + 0xBB   -> 33 bytes
    std::vector<unsigned char> sbytesB;                           // empty
    CScript scriptB(sbytesB.begin(), sbytesB.end());

    // Sanity: the raw concatenations really are identical (the pre-fix collision).
    auto raw = [](const std::vector<unsigned char>& p, const std::vector<unsigned char>& c,
                  const std::vector<unsigned char>& a, const std::vector<unsigned char>& s) {
        std::vector<unsigned char> r;
        r.insert(r.end(), p.begin(), p.end());
        r.insert(r.end(), c.begin(), c.end());
        r.insert(r.end(), a.begin(), a.end());
        r.insert(r.end(), s.begin(), s.end());
        return r;
    };
    BOOST_CHECK(raw(proofA, commA, assetA, sbytesA) == raw(proofB, commB, assetB, sbytesB));
    // ...but the tuples are genuinely different.
    BOOST_CHECK(proofA != proofB);

    uint256 entryA, entryB;
    TestComputeEntryRangeProof(entryA, proofA, commA, assetA, scriptA);
    TestComputeEntryRangeProof(entryB, proofB, commB, assetB, scriptB);
    // The whole point: identical raw stream, different keys.
    BOOST_CHECK(entryA != entryB);

    // And the key is still deterministic for identical inputs (the cache works).
    uint256 entryA2;
    TestComputeEntryRangeProof(entryA2, proofA, commA, assetA, scriptA);
    BOOST_CHECK(entryA == entryA2);
}

// The surjection cache key must depend on the target generator set (vTags):
// two verifications that differ only in vTags must not share a cache entry.
BOOST_AUTO_TEST_CASE(surjection_cache_vtags_test)
{
    uint256 wtxid = GetRandHash();
    std::vector<unsigned char> proof{1, 2, 3};
    std::vector<unsigned char> commitment(64, 0x07);

    secp256k1_generator g1, g2;
    memset(g1.data, 0x11, sizeof(g1.data));
    memset(g2.data, 0x22, sizeof(g2.data));
    std::vector<secp256k1_generator> tags1{g1};
    std::vector<secp256k1_generator> tags2{g1, g2};

    uint256 e1, e2, e1b;
    TestComputeEntrySurjectionProof(e1, wtxid, proof, commitment, tags1);
    TestComputeEntrySurjectionProof(e2, wtxid, proof, commitment, tags2);
    TestComputeEntrySurjectionProof(e1b, wtxid, proof, commitment, tags1);

    BOOST_CHECK(e1 != e2);   // different target set -> different key
    BOOST_CHECK(e1 == e1b);  // deterministic for identical inputs
}

BOOST_AUTO_TEST_SUITE_END()
