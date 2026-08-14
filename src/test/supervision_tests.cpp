// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <issuance.h>
#include <key.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
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

/* ---------------------------------------------------------------------------
 * Records: freezing, unfreezing and rotation.
 * ------------------------------------------------------------------------ */

static CKey MakePrivKey(uint8_t seed)
{
    CKey key;
    unsigned char bytes[32];
    memset(bytes, seed, sizeof(bytes));
    key.Set(bytes, bytes + 32, true);
    BOOST_REQUIRE(key.IsValid());
    return key;
}

static const COutPoint RECORD_INPUT{uint256S("0x11"), 3};

//! A signed record of the given kind, as an issuer would build it.
static SupervisionRecord SignRecord(SupervisionRecordKind kind, const CAsset& asset,
                                    const uint256& target, const XOnlyPubKey& old_key,
                                    const CKey& signer, const COutPoint& first_input)
{
    SupervisionRecord record;
    record.kind = kind;
    record.asset = asset;
    record.target = target;
    record.old_key = old_key;
    const uint256 sighash = SupervisionRecordSigHash(record, first_input);
    record.signature.resize(64);
    BOOST_REQUIRE(signer.SignSchnorr(sighash, record.signature, nullptr, uint256()));
    return record;
}

static CMutableTransaction RecordTx(const SupervisionRecord& record)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(RECORD_INPUT);
    tx.vout.emplace_back(record.asset, CConfidentialValue(0),
                         BuildSupervisionRecordScript(record));
    return tx;
}

//! Put one supervised asset in a registry under the given keys.
static SupervisionDeclaration RegisterAsset(SupervisionRegistry& registry, const CKey& op,
                                            const CKey& rec)
{
    SupervisionDeclaration decl;
    decl.descriptor.operational_key = XOnlyPubKey(op.GetPubKey());
    decl.descriptor.recovery_key = XOnlyPubKey(rec.GetPubKey());
    uint256 entropy;
    GenerateSupervisedAssetEntropy(entropy, ISSUANCE_PREVOUT, CONTRACT_HASH, decl.descriptor);
    CalculateAsset(decl.asset, entropy);
    registry.AddAsset(decl);
    return decl;
}

BOOST_AUTO_TEST_CASE(record_roundtrips_through_its_output)
{
    const CKey op = MakePrivKey(11);
    const SupervisionRecord freeze =
        SignRecord(SupervisionRecordKind::FREEZE, CAsset(uint256S("0xaa")), uint256S("0xbb"),
                   XOnlyPubKey(), op, RECORD_INPUT);

    const CScript script = BuildSupervisionRecordScript(freeze);
    const auto parsed = ParseSupervisionRecordScript(script);
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK(parsed->kind == freeze.kind);
    BOOST_CHECK(parsed->asset == freeze.asset);
    BOOST_CHECK(parsed->target == freeze.target);
    BOOST_CHECK(parsed->signature == freeze.signature);
    // Retained in the UTXO set, like a declaration and for the same reason: the
    // freeze set is a function of the UTXO set and cannot be if it is pruned.
    BOOST_CHECK(!script.IsUnspendable());

    const SupervisionRecord rotation =
        SignRecord(SupervisionRecordKind::ROTATE_OPERATIONAL, CAsset(uint256S("0xaa")),
                   SupervisionKeyAsTarget(XOnlyPubKey(MakePrivKey(12).GetPubKey())), XOnlyPubKey(op.GetPubKey()),
                   op, RECORD_INPUT);
    const auto parsed_rot = ParseSupervisionRecordScript(BuildSupervisionRecordScript(rotation));
    BOOST_REQUIRE(parsed_rot.has_value());
    BOOST_CHECK(parsed_rot->old_key == rotation.old_key);
    BOOST_CHECK(parsed_rot->NewKey() == rotation.NewKey());

    // An unknown kind is not forward compatibility; it is a record whose
    // meaning this node cannot know, so it must not parse.
    BOOST_CHECK(!ParseSupervisionRecordScript(CScript() << SUPERVISION_RECORD_MARKER << OP_DROP
                                                        << (int64_t)9 << OP_DROP)
                     .has_value());
}

//! The rule that makes a record authoritative. Anyone can put a record-shaped
//! output in a transaction; only a signature by the asset's current key gets it
//! admitted.
BOOST_AUTO_TEST_CASE(records_need_the_current_key)
{
    SupervisionRegistry registry;
    const CKey op = MakePrivKey(21), rec = MakePrivKey(22), stranger = MakePrivKey(23);
    const SupervisionDeclaration decl = RegisterAsset(registry, op, rec);
    const uint256 target = uint256S("0xbeef");
    std::string err;

    const auto good = SignRecord(SupervisionRecordKind::FREEZE, decl.asset, target, XOnlyPubKey(),
                                 op, RECORD_INPUT);
    BOOST_CHECK_MESSAGE(CheckSupervisionRecords(CTransaction(RecordTx(good)), registry, err), err);

    // Signed by somebody else.
    const auto forged = SignRecord(SupervisionRecordKind::FREEZE, decl.asset, target,
                                   XOnlyPubKey(), stranger, RECORD_INPUT);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(RecordTx(forged)), registry, err));

    // Signed by the RECOVERY key, which may rotate but must never freeze.
    const auto by_recovery = SignRecord(SupervisionRecordKind::FREEZE, decl.asset, target,
                                        XOnlyPubKey(), rec, RECORD_INPUT);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(RecordTx(by_recovery)), registry, err));

    // An asset nobody declared supervised.
    const auto unknown_asset = SignRecord(SupervisionRecordKind::FREEZE, CAsset(uint256S("0xdead")),
                                          target, XOnlyPubKey(), op, RECORD_INPUT);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(RecordTx(unknown_asset)), registry, err));

    // The signature binds the transaction's first input, so it cannot be lifted
    // off the chain and replayed to re-freeze what the issuer unfroze.
    CMutableTransaction replayed = RecordTx(good);
    replayed.vin[0].prevout = COutPoint(uint256S("0x99"), 1);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(replayed), registry, err));
}

//! Alberto's point 2, the asymmetry that makes two keys worth having: the
//! operational key cannot rotate anything, not even itself, so a thief who has
//! it can grief but can never take the authority away from its owner.
BOOST_AUTO_TEST_CASE(only_the_recovery_key_rotates)
{
    SupervisionRegistry registry;
    const CKey op = MakePrivKey(31), rec = MakePrivKey(32), fresh = MakePrivKey(33);
    const SupervisionDeclaration decl = RegisterAsset(registry, op, rec);
    const XOnlyPubKey old_op(op.GetPubKey()), new_op(fresh.GetPubKey());
    std::string err;

    const auto by_operational = SignRecord(SupervisionRecordKind::ROTATE_OPERATIONAL, decl.asset,
                                           SupervisionKeyAsTarget(new_op), old_op, op, RECORD_INPUT);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(RecordTx(by_operational)), registry, err));

    const auto by_recovery = SignRecord(SupervisionRecordKind::ROTATE_OPERATIONAL, decl.asset,
                                        SupervisionKeyAsTarget(new_op), old_op, rec, RECORD_INPUT);
    BOOST_CHECK_MESSAGE(CheckSupervisionRecords(CTransaction(RecordTx(by_recovery)), registry, err),
                        err);

    // Naming a key that is not the current one is a stale rotation.
    const auto stale = SignRecord(SupervisionRecordKind::ROTATE_OPERATIONAL, decl.asset,
                                  SupervisionKeyAsTarget(new_op), XOnlyPubKey(MakePrivKey(34).GetPubKey()), rec,
                                  RECORD_INPUT);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(RecordTx(stale)), registry, err));

    // Rotating the operational key onto the recovery key would collapse the
    // separation the two keys exist to create.
    const auto collapse =
        SignRecord(SupervisionRecordKind::ROTATE_OPERATIONAL, decl.asset,
                   SupervisionKeyAsTarget(XOnlyPubKey(rec.GetPubKey())), old_op, rec, RECORD_INPUT);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(RecordTx(collapse)), registry, err));

    BOOST_REQUIRE(registry.Rotate(decl.asset, true, old_op, new_op));
    // ...and after the rotation the old key signs nothing.
    const auto after = SignRecord(SupervisionRecordKind::FREEZE, decl.asset, uint256S("0x1"),
                                  XOnlyPubKey(), op, RECORD_INPUT);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(RecordTx(after)), registry, err));
    const auto after_new = SignRecord(SupervisionRecordKind::FREEZE, decl.asset, uint256S("0x1"),
                                      XOnlyPubKey(), fresh, RECORD_INPUT);
    BOOST_CHECK_MESSAGE(CheckSupervisionRecords(CTransaction(RecordTx(after_new)), registry, err),
                        err);
}

//! THE UNFREEZE TRAP. After a rotation a stolen old key must not be able to
//! lift the freezes it set, or rotating after a compromise achieves nothing.
BOOST_AUTO_TEST_CASE(unfreeze_needs_the_current_key_not_the_records_own)
{
    SupervisionRegistry registry;
    const CKey op = MakePrivKey(41), rec = MakePrivKey(42), fresh = MakePrivKey(43);
    const SupervisionDeclaration decl = RegisterAsset(registry, op, rec);
    const uint256 target = uint256S("0xcafe");
    std::string err;

    const auto record = SignRecord(SupervisionRecordKind::FREEZE, decl.asset, target, XOnlyPubKey(),
                                   op, RECORD_INPUT);
    const CTxOut record_out(decl.asset, CConfidentialValue(0),
                            BuildSupervisionRecordScript(record));
    const COutPoint record_outpoint(uint256S("0x77"), 0);

    const uint256 sighash = SupervisionUnfreezeSigHash(record_outpoint, decl.asset, target);
    std::vector<unsigned char> sig(64);
    BOOST_REQUIRE(op.SignSchnorr(sighash, sig, nullptr, uint256()));

    CTxIn input(record_outpoint);
    input.scriptSig = CScript() << sig;
    BOOST_CHECK_MESSAGE(CheckSupervisionRecordSpend(input, record_out, registry, err), err);

    // Rotate, and the very same spend stops working: the record still names the
    // old key in its own bytes, but consensus asks the registry.
    BOOST_REQUIRE(registry.Rotate(decl.asset, true, XOnlyPubKey(op.GetPubKey()),
                                  XOnlyPubKey(fresh.GetPubKey())));
    BOOST_CHECK(!CheckSupervisionRecordSpend(input, record_out, registry, err));

    // The new key can.
    std::vector<unsigned char> new_sig(64);
    BOOST_REQUIRE(fresh.SignSchnorr(sighash, new_sig, nullptr, uint256()));
    CTxIn new_input(record_outpoint);
    new_input.scriptSig = CScript() << new_sig;
    BOOST_CHECK_MESSAGE(CheckSupervisionRecordSpend(new_input, record_out, registry, err), err);

    // A signature for a different record does not travel.
    const uint256 other_sighash =
        SupervisionUnfreezeSigHash(COutPoint(uint256S("0x78"), 0), decl.asset, target);
    std::vector<unsigned char> other_sig(64);
    BOOST_REQUIRE(fresh.SignSchnorr(other_sighash, other_sig, nullptr, uint256()));
    CTxIn wrong_input(record_outpoint);
    wrong_input.scriptSig = CScript() << other_sig;
    BOOST_CHECK(!CheckSupervisionRecordSpend(wrong_input, record_out, registry, err));

    // No signature at all.
    CTxIn bare(record_outpoint);
    BOOST_CHECK(!CheckSupervisionRecordSpend(bare, record_out, registry, err));
}

//! A rotation is a statement about the past. Spending one would erase it from
//! the UTXO set and take the key change with it, leaving the registry
//! unrebuildable.
BOOST_AUTO_TEST_CASE(rotation_records_cannot_be_spent)
{
    SupervisionRegistry registry;
    const CKey op = MakePrivKey(51), rec = MakePrivKey(52), fresh = MakePrivKey(53);
    const SupervisionDeclaration decl = RegisterAsset(registry, op, rec);
    std::string err;

    const auto rotation = SignRecord(SupervisionRecordKind::ROTATE_OPERATIONAL, decl.asset,
                                     SupervisionKeyAsTarget(XOnlyPubKey(fresh.GetPubKey())),
                                     XOnlyPubKey(op.GetPubKey()), rec, RECORD_INPUT);
    const CTxOut out(decl.asset, CConfidentialValue(0),
                     BuildSupervisionRecordScript(rotation));
    CTxIn input(COutPoint(uint256S("0x77"), 0));
    input.scriptSig = CScript() << std::vector<unsigned char>(64, 0x00);
    BOOST_CHECK(!CheckSupervisionRecordSpend(input, out, registry, err));
}

//! The registry must be an EXACT function of the UTXO set: two records may name
//! one target, and the freeze lifts only when the last of them is spent.
BOOST_AUTO_TEST_CASE(freezes_are_counted_not_set)
{
    SupervisionRegistry registry;
    const SupervisionDeclaration decl = RegisterAsset(registry, MakePrivKey(61), MakePrivKey(62));
    const uint256 target = uint256S("0x5150");

    BOOST_CHECK(!registry.IsFrozen(decl.asset, target));
    registry.AddFreeze(decl.asset, target);
    registry.AddFreeze(decl.asset, target);
    BOOST_CHECK(registry.IsFrozen(decl.asset, target));
    registry.SubFreeze(decl.asset, target);
    BOOST_CHECK(registry.IsFrozen(decl.asset, target));
    registry.SubFreeze(decl.asset, target);
    BOOST_CHECK(!registry.IsFrozen(decl.asset, target));
    // Erased, not left at zero, so the map does not grow without bound.
    BOOST_CHECK(registry.FrozenTargets(decl.asset).empty());

    // An unsupervised asset is never frozen, and saying so must not create it.
    registry.AddFreeze(CAsset(uint256S("0xdead")), target);
    BOOST_CHECK(!registry.IsFrozen(CAsset(uint256S("0xdead")), target));
}

//! The record script must actually execute, and leave a CLEAN stack.
//!
//! This is the test that justifies the OP_2DROP. A record ending OP_DROP
//! OP_TRUE would satisfy consensus and leave two items on the stack, failing
//! SCRIPT_VERIFY_CLEANSTACK, so every unfreeze would be a valid transaction
//! that no node relays -- indistinguishable, in practice, from a freeze that
//! cannot be lifted.
BOOST_AUTO_TEST_CASE(unfreeze_spend_executes_under_standard_flags)
{
    const CKey op = MakePrivKey(71);
    const SupervisionRecord record =
        SignRecord(SupervisionRecordKind::FREEZE, CAsset(uint256S("0xaa")), uint256S("0xbb"),
                   XOnlyPubKey(), op, RECORD_INPUT);
    const CScript record_script = BuildSupervisionRecordScript(record);

    ScriptError serror = SCRIPT_ERR_UNKNOWN_ERROR;
    const CScriptWitness empty_witness;

    // Exactly one item: passes, with nothing left over.
    const CScript one_item = CScript() << std::vector<unsigned char>(64, 0x01);
    BOOST_CHECK_MESSAGE(VerifyScript(one_item, record_script, &empty_witness,
                                     STANDARD_SCRIPT_VERIFY_FLAGS, BaseSignatureChecker(), &serror),
                        ScriptErrorString(serror));

    // No item: the OP_2DROP has nothing to remove, so the output cannot be
    // spent by accident or by a transaction that forgot the signature.
    BOOST_CHECK(!VerifyScript(CScript(), record_script, &empty_witness,
                              STANDARD_SCRIPT_VERIFY_FLAGS, BaseSignatureChecker(), &serror));

    // Two items: valid script, dirty stack, so it would never relay. Refusing
    // it here keeps one encoding of an unfreeze, matching the consensus check.
    const CScript two_items = CScript() << std::vector<unsigned char>(64, 0x01)
                                        << std::vector<unsigned char>(64, 0x02);
    BOOST_CHECK(!VerifyScript(two_items, record_script, &empty_witness,
                              STANDARD_SCRIPT_VERIFY_FLAGS, BaseSignatureChecker(), &serror));
    BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_CLEANSTACK);

    // And a declaration is unspendable however it is approached.
    const CScript declaration =
        BuildSupervisionScript(CAsset(uint256S("0xabc")), MakeDescriptor());
    BOOST_CHECK(!VerifyScript(one_item, declaration, &empty_witness,
                              STANDARD_SCRIPT_VERIFY_FLAGS, BaseSignatureChecker(), &serror));
}

//! A record output must be denominated in the asset it governs, carrying zero.
//! The dust rule applies only to the fee asset, so a record denominated in the
//! policy asset would be dust and would never relay: a freeze the issuer could
//! sign and never publish.
BOOST_AUTO_TEST_CASE(record_outputs_carry_zero_of_their_own_asset)
{
    SupervisionRegistry registry;
    const CKey op = MakePrivKey(81), rec = MakePrivKey(82);
    const SupervisionDeclaration decl = RegisterAsset(registry, op, rec);
    const auto record = SignRecord(SupervisionRecordKind::FREEZE, decl.asset, uint256S("0x1234"),
                                   XOnlyPubKey(), op, RECORD_INPUT);
    std::string err;

    BOOST_CHECK_MESSAGE(CheckSupervisionRecords(CTransaction(RecordTx(record)), registry, err),
                        err);

    CMutableTransaction other_asset = RecordTx(record);
    other_asset.vout[0].nAsset = CConfidentialAsset(CAsset(uint256S("0xfee")));
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(other_asset), registry, err));

    CMutableTransaction funded = RecordTx(record);
    funded.vout[0].nValue = CConfidentialValue(1);
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(funded), registry, err));

    CMutableTransaction blinded = RecordTx(record);
    blinded.vout[0].nAsset.vchCommitment.assign(33, 0x00);
    blinded.vout[0].nAsset.vchCommitment[0] = 10;
    BOOST_CHECK(!CheckSupervisionRecords(CTransaction(blinded), registry, err));
}

/* ---------------------------------------------------------------------------
 * Enforcement: which spends a freeze can reach.
 * ------------------------------------------------------------------------ */

//! Build a transaction with one input spending `spk`, with the given witness
//! and scriptSig.
static CMutableTransaction SpendOf(const CScript& script_sig,
                                   const std::vector<std::vector<unsigned char>>& witness)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint(uint256S("0x55"), 0));
    tx.vin[0].scriptSig = script_sig;
    tx.witness.vtxinwit.resize(1);
    tx.witness.vtxinwit[0].scriptWitness.stack = witness;
    return tx;
}

static const std::vector<unsigned char> SIG64(64, 0x01);
static const std::vector<unsigned char> PUBKEY33(33, 0x02);

//! Single-owner scripts are freezable. This is the permissive half, and every
//! entry here is a form in which somebody can hold a supervised asset outright.
BOOST_AUTO_TEST_CASE(single_owner_spends_are_freezable)
{
    const std::vector<unsigned char> hash20(20, 0x03), hash32(32, 0x04);

    // P2WPKH: <sig> <pubkey>.
    const CScript p2wpkh = CScript() << OP_0 << hash20;
    BOOST_CHECK(IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {SIG64, PUBKEY33})), 0, p2wpkh));

    // P2TR key path: one witness element, and one more if there is an annex.
    const CScript p2tr = CScript() << OP_1 << hash32;
    BOOST_CHECK(IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {SIG64})), 0, p2tr));
    const std::vector<unsigned char> annex{0x50, 0x00};
    BOOST_CHECK(IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {SIG64, annex})), 0, p2tr));

    // Legacy forms, unambiguous from the output alone.
    const CScript p2pkh = CScript() << OP_DUP << OP_HASH160 << hash20 << OP_EQUALVERIFY
                                    << OP_CHECKSIG;
    BOOST_CHECK(IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {})), 0, p2pkh));
    const CScript p2pk = CScript() << PUBKEY33 << OP_CHECKSIG;
    BOOST_CHECK(IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {})), 0, p2pk));

    // P2SH wrapping P2WPKH, which the spend reveals. Covered so that a
    // legacy-wrapped address is not an evasion route.
    CScript witness_program;
    witness_program << OP_0 << hash20;
    const CScript p2sh = CScript() << OP_HASH160 << hash20 << OP_EQUAL;
    const CScript redeem_push =
        CScript() << std::vector<unsigned char>(witness_program.begin(), witness_program.end());
    BOOST_CHECK(
        IsSingleOwnerSpend(CTransaction(SpendOf(redeem_push, {SIG64, PUBKEY33})), 0, p2sh));
}

//! THE RULE THAT KEEPS A FREEZE FROM TAKING HOSTAGES. Everything here is a
//! script somebody else may have a claim on, so freezing it would strand a
//! counterparty who did nothing, and freeze a contract whose timelocks keep
//! running. This is why supervised assets work on Lightning and on the DEX.
BOOST_AUTO_TEST_CASE(shared_scripts_are_never_freezable)
{
    const std::vector<unsigned char> hash20(20, 0x03), hash32(32, 0x04);

    // P2WSH: a Lightning funding output, an HTLC, a covenant.
    const CScript p2wsh = CScript() << OP_0 << hash32;
    BOOST_CHECK(!IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {SIG64, SIG64, {}})), 0,
                                    p2wsh));

    // P2TR SCRIPT path: the control block makes the witness longer than one.
    const CScript p2tr = CScript() << OP_1 << hash32;
    const std::vector<unsigned char> control(33, 0xc0);
    BOOST_CHECK(!IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {SIG64, SIG64, control})), 0,
                                    p2tr));

    // Bare multisig.
    const CScript multisig = CScript() << OP_2 << PUBKEY33 << PUBKEY33 << OP_2 << OP_CHECKMULTISIG;
    BOOST_CHECK(!IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {})), 0, multisig));

    // P2SH that does NOT wrap P2WPKH.
    const CScript p2sh = CScript() << OP_HASH160 << hash20 << OP_EQUAL;
    const CScript redeem_push = CScript() << std::vector<unsigned char>{OP_TRUE};
    BOOST_CHECK(!IsSingleOwnerSpend(CTransaction(SpendOf(redeem_push, {SIG64, PUBKEY33})), 0,
                                    p2sh));

    // A future witness version nobody has defined yet.
    const CScript unknown_version = CScript() << OP_2 << hash32;
    BOOST_CHECK(!IsSingleOwnerSpend(CTransaction(SpendOf(CScript(), {SIG64})), 0,
                                    unknown_version));

    // A supervision record itself, which is bare and consensus-gated.
    const CKey op = MakePrivKey(91);
    const auto record = SignRecord(SupervisionRecordKind::FREEZE, CAsset(uint256S("0xaa")),
                                   uint256S("0xbb"), XOnlyPubKey(), op, RECORD_INPUT);
    BOOST_CHECK(!IsSingleOwnerSpend(CTransaction(SpendOf(CScript() << SIG64, {})), 0,
                                    BuildSupervisionRecordScript(record)));

    // A witness the transaction does not carry at all.
    CMutableTransaction no_witness;
    no_witness.vin.emplace_back(COutPoint(uint256S("0x55"), 0));
    const CScript p2wpkh = CScript() << OP_0 << hash20;
    BOOST_CHECK(!IsSingleOwnerSpend(CTransaction(no_witness), 0, p2wpkh));
    // ...and an input index that does not exist.
    BOOST_CHECK(!IsSingleOwnerSpend(CTransaction(no_witness), 7, p2wpkh));
}

//! A freeze names a scriptPubKey by its hash, so the registry answer must
//! depend on the exact script and nothing else.
BOOST_AUTO_TEST_CASE(freeze_targets_are_script_hashes)
{
    const CScript a = CScript() << OP_0 << std::vector<unsigned char>(20, 0x03);
    const CScript b = CScript() << OP_0 << std::vector<unsigned char>(20, 0x04);
    BOOST_CHECK(SupervisionTargetHash(a) == SupervisionTargetHash(a));
    BOOST_CHECK(SupervisionTargetHash(a) != SupervisionTargetHash(b));
}

BOOST_AUTO_TEST_SUITE_END()
