// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <issuance.h>
#include <key.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <supervision.h>
#include <util/strencodings.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

//! Supervised assets exist only on elements-mode chains, and CTxOut serializes
//! differently there, so the whole suite needs a chain that has assets at all.
struct SupervisionSetup : public TestingSetup {
    SupervisionSetup() : TestingSetup("custom") {}
};

BOOST_FIXTURE_TEST_SUITE(supervision_tests, SupervisionSetup)

static XOnlyPubKey MakeKey(uint8_t seed)
{
    CKey key;
    unsigned char bytes[32];
    memset(bytes, seed, sizeof(bytes));
    key.Set(bytes, bytes + 32, true);
    BOOST_REQUIRE(key.IsValid());
    return XOnlyPubKey(key.GetPubKey());
}

static SupervisionDescriptor MakeDescriptor()
{
    SupervisionDescriptor desc;
    desc.operational_key = MakeKey(1);
    desc.recovery_key = MakeKey(2);
    return desc;
}

static const COutPoint ISSUANCE_PREVOUT{uint256S("0x01"), 0};
static const uint256 CONTRACT_HASH{uint256S("0x02")};

//! A transaction that satisfies every supervised-issuance rule, for the tests
//! below to break one rule at a time.
static CMutableTransaction MakeSupervisedIssuance(const SupervisionDescriptor& desc,
                                                  SupervisionDeclaration& decl)
{
    uint256 entropy;
    GenerateSupervisedAssetEntropy(entropy, ISSUANCE_PREVOUT, CONTRACT_HASH, desc);
    CAsset asset, token;
    CalculateAsset(asset, entropy);
    CalculateReissuanceToken(token, entropy, false);

    decl.asset = asset;
    decl.descriptor = desc;

    CMutableTransaction tx;
    tx.vin.emplace_back(ISSUANCE_PREVOUT);
    tx.vin[0].assetIssuance.assetEntropy = CONTRACT_HASH;
    tx.vin[0].assetIssuance.nAmount = CConfidentialValue(1000);
    tx.vin[0].assetIssuance.nInflationKeys = CConfidentialValue(1);

    tx.vout.emplace_back(asset, CConfidentialValue(1000), CScript() << OP_TRUE);
    tx.vout.emplace_back(token, CConfidentialValue(1), CScript() << OP_TRUE);
    tx.vout.emplace_back(asset, CConfidentialValue(0), BuildSupervisionScript(asset, desc));
    return tx;
}

//! A descriptor must survive the round trip through the declaration output byte
//! for byte. The asset id commits to these bytes, so a parser that accepted a
//! second encoding of the same descriptor would let one asset be described two
//! ways.
BOOST_AUTO_TEST_CASE(declaration_roundtrips_through_its_output)
{
    const SupervisionDescriptor desc = MakeDescriptor();
    const CAsset asset(uint256S("0xabc"));
    const CScript script = BuildSupervisionScript(asset, desc);

    const auto parsed = ParseSupervisionScript(script);
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK(parsed->descriptor == desc);
    BOOST_CHECK(parsed->asset == asset);
}

//! The load-bearing property of the script's shape. A leading OP_RETURN would
//! be pruned by CCoinsViewCache::AddCoin on sight, and the supervised-asset set
//! has to be recoverable from the UTXO set so it can be rebuilt at startup and
//! inverted on reorg. Trailing, the script still fails every execution.
BOOST_AUTO_TEST_CASE(declaration_output_is_kept_in_the_utxo_set)
{
    const CScript script = BuildSupervisionScript(CAsset(uint256S("0xabc")), MakeDescriptor());
    BOOST_CHECK(!script.IsUnspendable());
    BOOST_CHECK(script.back() == OP_RETURN);
}

BOOST_AUTO_TEST_CASE(non_declaration_scripts_are_not_mistaken_for_one)
{
    const SupervisionDescriptor desc = MakeDescriptor();
    const CAsset asset(uint256S("0xabc"));

    BOOST_CHECK(!ParseSupervisionScript(CScript()).has_value());
    BOOST_CHECK(!ParseSupervisionScript(CScript() << OP_RETURN).has_value());
    // Right shape, wrong marker.
    BOOST_CHECK(!ParseSupervisionScript(CScript()
                                        << std::vector<unsigned char>{'N', 'O', 'P', 'E'} << OP_DROP)
                     .has_value());
    // A staking-family record must never parse as a declaration, or one
    // registry would read another's records.
    BOOST_CHECK(!ParseSupervisionScript(CScript()
                                        << std::vector<unsigned char>{'S', 'E', 'Q', 'D', 'E', 'L'}
                                        << OP_DROP)
                     .has_value());

    // Trailing data after a well-formed declaration is a second encoding of the
    // same thing; refuse it, because the asset id commits to one.
    CScript trailing = BuildSupervisionScript(asset, desc);
    trailing << std::vector<unsigned char>{0xff};
    BOOST_CHECK(!ParseSupervisionScript(trailing).has_value());

    // A null asset names nothing.
    BOOST_CHECK(!ParseSupervisionScript(BuildSupervisionScript(CAsset(), desc)).has_value());
}

//! Both keys must be real and distinct, and no reserved bit may be claimed.
//! These are checked at issuance because the descriptor is in the asset id: a
//! malformed one cannot be repaired, only abandoned.
BOOST_AUTO_TEST_CASE(descriptor_validation_rejects_the_unusable)
{
    std::string err;

    SupervisionDescriptor good = MakeDescriptor();
    BOOST_CHECK(ValidateSupervisionDescriptor(good, err));

    SupervisionDescriptor same_keys = MakeDescriptor();
    same_keys.recovery_key = same_keys.operational_key;
    BOOST_CHECK(!ValidateSupervisionDescriptor(same_keys, err));

    SupervisionDescriptor reserved_bit = MakeDescriptor();
    reserved_bit.feature_bits = SUPERVISION_FEATURE_PAUSE;
    BOOST_CHECK(!ValidateSupervisionDescriptor(reserved_bit, err));

    SupervisionDescriptor unknown_bit = MakeDescriptor();
    unknown_bit.feature_bits = 0x8000;
    BOOST_CHECK(!ValidateSupervisionDescriptor(unknown_bit, err));

    SupervisionDescriptor bad_version = MakeDescriptor();
    bad_version.version = SUPERVISION_VERSION + 1;
    BOOST_CHECK(!ValidateSupervisionDescriptor(bad_version, err));
}

//! The whole point of committing the descriptor: a supervised asset is a
//! different asset from the unsupervised one it would otherwise have been, and
//! changing any committed field changes the asset again. This is what makes
//! supervision impossible to add, remove or alter after issuance.
BOOST_AUTO_TEST_CASE(descriptor_changes_the_asset_id)
{
    const SupervisionDescriptor desc = MakeDescriptor();

    uint256 plain_entropy;
    GenerateAssetEntropy(plain_entropy, ISSUANCE_PREVOUT, CONTRACT_HASH);
    CAsset plain_asset, plain_token;
    CalculateAsset(plain_asset, plain_entropy);
    CalculateReissuanceToken(plain_token, plain_entropy, false);

    uint256 sup_entropy;
    GenerateSupervisedAssetEntropy(sup_entropy, ISSUANCE_PREVOUT, CONTRACT_HASH, desc);
    CAsset sup_asset, sup_token;
    CalculateAsset(sup_asset, sup_entropy);
    CalculateReissuanceToken(sup_token, sup_entropy, false);

    BOOST_CHECK(sup_entropy != plain_entropy);
    BOOST_CHECK(sup_asset != plain_asset);
    // The token descends from the same entropy, so it is bound too, for free.
    BOOST_CHECK(sup_token != plain_token);

    // Rotating the operational key at issuance yields a different asset, which
    // is why rotation needs its own on-chain mechanism rather than a re-issue.
    SupervisionDescriptor other = desc;
    other.operational_key = MakeKey(3);
    uint256 other_entropy;
    GenerateSupervisedAssetEntropy(other_entropy, ISSUANCE_PREVOUT, CONTRACT_HASH, other);
    BOOST_CHECK(other_entropy != sup_entropy);

    // The recovery key is committed just as firmly.
    SupervisionDescriptor other_recovery = desc;
    other_recovery.recovery_key = MakeKey(4);
    uint256 other_recovery_entropy;
    GenerateSupervisedAssetEntropy(other_recovery_entropy, ISSUANCE_PREVOUT, CONTRACT_HASH,
                                   other_recovery);
    BOOST_CHECK(other_recovery_entropy != sup_entropy);

    // Derivation is deterministic.
    uint256 again;
    GenerateSupervisedAssetEntropy(again, ISSUANCE_PREVOUT, CONTRACT_HASH, desc);
    BOOST_CHECK(again == sup_entropy);
}

//! Pinned vectors. These are not a restatement of the code: an asset id is
//! permanent, so any refactor that changes a byte of the descriptor encoding or
//! the entropy construction renames every supervised asset ever issued and
//! orphans its holders. That failure is silent without this test.
BOOST_AUTO_TEST_CASE(derivation_vectors_are_pinned)
{
    const SupervisionDescriptor desc = MakeDescriptor();

    BOOST_CHECK_EQUAL(HexStr(desc.operational_key),
                      "1b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f");
    BOOST_CHECK_EQUAL(HexStr(desc.recovery_key),
                      "4d4b6cd1361032ca9bd2aeb9d900aa4d45d9ead80ac9423374c451a7254d0766");

    // version(1) || feature_bits(2, LE) || operational(32) || recovery(32)
    std::vector<unsigned char> encoded;
    CVectorWriter(SER_NETWORK, PROTOCOL_VERSION, encoded, 0, desc);
    BOOST_CHECK_EQUAL(encoded.size(), SUPERVISION_DESCRIPTOR_SIZE);
    BOOST_CHECK_EQUAL(HexStr(encoded),
                      "0100001b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f"
                      "4d4b6cd1361032ca9bd2aeb9d900aa4d45d9ead80ac9423374c451a7254d0766");

    BOOST_CHECK_EQUAL(SupervisionDescriptorHash(desc).GetHex(),
                      "65d9d215734fdb77f8b58a40a48489e79523c2bf6986170c4020b8b5181ac1c6");

    uint256 entropy;
    GenerateSupervisedAssetEntropy(entropy, ISSUANCE_PREVOUT, CONTRACT_HASH, desc);
    BOOST_CHECK_EQUAL(entropy.GetHex(),
                      "459bff71b8ea611b8eb06e0a3594177478465a94c17586fae464a2a5daaad516");

    CAsset asset, token;
    CalculateAsset(asset, entropy);
    CalculateReissuanceToken(token, entropy, false);
    BOOST_CHECK_EQUAL(asset.GetHex(),
                      "22f416a1580c768b6cbb35d173530a22cbc5160201f6c5882fedb3ecac786224");
    BOOST_CHECK_EQUAL(token.GetHex(),
                      "e78d2c59026e8860f60a3b9b250c2c8f3d5ff05f71acbd35b292c72060de0e9a");
}

//! One declaration governs one asset, so a second has nothing to govern.
BOOST_AUTO_TEST_CASE(two_declarations_in_one_transaction_are_malformed)
{
    SupervisionDeclaration decl;
    const SupervisionDescriptor desc = MakeDescriptor();
    CMutableTransaction tx = MakeSupervisedIssuance(desc, decl);

    bool malformed = false;
    const auto one = SupervisionFromTx(CTransaction(tx), malformed);
    BOOST_REQUIRE(one.has_value());
    BOOST_CHECK(!malformed);
    BOOST_CHECK(*one == decl);

    SupervisionDescriptor other = desc;
    other.operational_key = MakeKey(5);
    tx.vout.emplace_back(decl.asset, CConfidentialValue(0),
                         BuildSupervisionScript(decl.asset, other));
    BOOST_CHECK(!SupervisionFromTx(CTransaction(tx), malformed).has_value());
    BOOST_CHECK(malformed);

    // A transaction with no declaration is the ordinary case, not an error.
    CMutableTransaction plain;
    plain.vout.emplace_back(CAsset(), CConfidentialValue(0), CScript() << OP_TRUE);
    BOOST_CHECK(!SupervisionFromTx(CTransaction(plain), malformed).has_value());
    BOOST_CHECK(!malformed);
}

BOOST_AUTO_TEST_CASE(supervised_issuance_accepts_a_well_formed_transaction)
{
    SupervisionDeclaration decl;
    const CMutableTransaction tx = MakeSupervisedIssuance(MakeDescriptor(), decl);

    uint256 entropy;
    std::string err;
    BOOST_CHECK_MESSAGE(CheckSupervisedIssuance(CTransaction(tx), decl, entropy, err), err);

    CAsset derived;
    CalculateAsset(derived, entropy);
    BOOST_CHECK(derived == decl.asset);
}

//! A declaration cannot lie about its own terms, because the asset id is
//! derived over them: claim a different key and you have named an asset the
//! issuance did not create.
BOOST_AUTO_TEST_CASE(supervised_issuance_rejects_a_declaration_that_lies)
{
    SupervisionDeclaration decl;
    const SupervisionDescriptor desc = MakeDescriptor();
    CMutableTransaction tx = MakeSupervisedIssuance(desc, decl);

    SupervisionDescriptor forged = desc;
    forged.operational_key = MakeKey(6);
    SupervisionDeclaration forged_decl{decl.asset, forged};
    tx.vout.back() = CTxOut(decl.asset, CConfidentialValue(0),
                            BuildSupervisionScript(decl.asset, forged));

    uint256 entropy;
    std::string err;
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(tx), forged_decl, entropy, err));

    // Naming some other asset fails the same way.
    const SupervisionDeclaration wrong_asset{CAsset(uint256S("0xdead")), desc};
    tx.vout.back() = CTxOut(wrong_asset.asset, CConfidentialValue(0),
                            BuildSupervisionScript(wrong_asset.asset, desc));
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(tx), wrong_asset, entropy, err));
}

//! Alberto's point 3: seize and burn are never consensus machinery, so they
//! fall out economically as freeze-as-burn and freeze-plus-reissue-as-seize.
//! Reissuability is the enabling condition, so an asset that cannot be reissued
//! must not be able to call itself supervised.
BOOST_AUTO_TEST_CASE(supervised_issuance_requires_reissuance_tokens)
{
    SupervisionDeclaration decl;
    CMutableTransaction tx = MakeSupervisedIssuance(MakeDescriptor(), decl);
    tx.vin[0].assetIssuance.nInflationKeys = CConfidentialValue(0);

    uint256 entropy;
    std::string err;
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(tx), decl, entropy, err));
}

//! Consensus cannot read a blinded output's asset, so a supervised asset in one
//! would be unfreezable and unauditable. It has to be every output, not merely
//! the supervised ones, because which asset a blinded output carries is exactly
//! what cannot be determined.
BOOST_AUTO_TEST_CASE(supervised_issuance_rejects_blinded_outputs)
{
    SupervisionDeclaration decl;
    const SupervisionDescriptor desc = MakeDescriptor();
    uint256 entropy;
    std::string err;

    // Prefix 10 or 11 marks a confidential asset commitment; 8 or 9, a value.
    CMutableTransaction blinded_asset = MakeSupervisedIssuance(desc, decl);
    blinded_asset.vout[0].nAsset.vchCommitment.assign(33, 0x00);
    blinded_asset.vout[0].nAsset.vchCommitment[0] = 10;
    BOOST_REQUIRE(blinded_asset.vout[0].nAsset.IsCommitment());
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(blinded_asset), decl, entropy, err));

    CMutableTransaction blinded_value = MakeSupervisedIssuance(desc, decl);
    blinded_value.vout[0].nValue.vchCommitment.assign(33, 0x00);
    blinded_value.vout[0].nValue.vchCommitment[0] = 8;
    BOOST_REQUIRE(blinded_value.vout[0].nValue.IsCommitment());
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(blinded_value), decl, entropy, err));

    CMutableTransaction blinded_issuance = MakeSupervisedIssuance(desc, decl);
    blinded_issuance.vin[0].assetIssuance.nAmount.vchCommitment.assign(33, 0x00);
    blinded_issuance.vin[0].assetIssuance.nAmount.vchCommitment[0] = 8;
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(blinded_issuance), decl, entropy, err));
}

BOOST_AUTO_TEST_CASE(supervised_issuance_rejects_structural_mistakes)
{
    SupervisionDeclaration decl;
    const SupervisionDescriptor desc = MakeDescriptor();
    uint256 entropy;
    std::string err;

    // A declaration with nothing to declare.
    CMutableTransaction no_issuance = MakeSupervisedIssuance(desc, decl);
    no_issuance.vin[0].assetIssuance.SetNull();
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(no_issuance), decl, entropy, err));

    // Two issuances, one declaration: no honest answer to which it governs.
    CMutableTransaction two_issuances = MakeSupervisedIssuance(desc, decl);
    two_issuances.vin.emplace_back(COutPoint(uint256S("0x03"), 1));
    two_issuances.vin[1].assetIssuance.assetEntropy = CONTRACT_HASH;
    two_issuances.vin[1].assetIssuance.nAmount = CConfidentialValue(5);
    two_issuances.vin[1].assetIssuance.nInflationKeys = CConfidentialValue(1);
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(two_issuances), decl, entropy, err));

    // The declaration output is unspendable, so anything it carries is burnt.
    CMutableTransaction funded_declaration = MakeSupervisedIssuance(desc, decl);
    funded_declaration.vout.back().nValue = CConfidentialValue(500);
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(funded_declaration), decl, entropy, err));

    // A descriptor that could never have been produced honestly.
    SupervisionDescriptor same_keys = desc;
    same_keys.recovery_key = same_keys.operational_key;
    SupervisionDeclaration bad_decl;
    CMutableTransaction bad_keys = MakeSupervisedIssuance(same_keys, bad_decl);
    BOOST_CHECK(!CheckSupervisedIssuance(CTransaction(bad_keys), bad_decl, entropy, err));
}

//! The gate is what keeps a node that has crossed the activation height and one
//! that has not on the same chain: below it a declaration is inert data and the
//! issuance derives plainly, which is what a node without this code does.
BOOST_AUTO_TEST_CASE(activation_gate_follows_the_height)
{
    const int saved = g_supervision_height;

    g_supervision_height = 0;
    BOOST_CHECK(!SupervisionActive(0));
    BOOST_CHECK(!SupervisionActive(1000000));

    g_supervision_height = 100;
    BOOST_CHECK(!SupervisionActive(99));
    BOOST_CHECK(SupervisionActive(100));
    BOOST_CHECK(SupervisionActive(101));

    g_supervision_height = saved;
}

BOOST_AUTO_TEST_SUITE_END()
