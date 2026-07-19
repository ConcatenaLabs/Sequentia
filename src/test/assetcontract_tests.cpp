// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetcontract.h>

#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(assetcontract_tests, BasicTestingSetup)

namespace {

UniValue ParseContract(const std::string& json)
{
    UniValue v;
    BOOST_REQUIRE(v.read(json));
    return v;
}

// The GOLD demo asset's contract, exactly as the public Sequentia asset registry
// stores it, together with the contract_hash that registry computed for it with
// its own (Node) canonicalise-and-hash implementation. This pins our
// serialisation against theirs: if these two ever disagree, assets issued by
// this software derive a different id than the registry expects and can never be
// verified. Fetched from https://sequentiatestnet.com/registry/<asset id>.
const std::string GOLD_CONTRACT_JSON =
    R"json({"name":"Gold (troy ounce)","ticker":"GOLD","precision":8,)json"
    R"json("entity":{"domain":"sequentia.io"},)json"
    R"json("issuer_pubkey":"020000000000000000000000000000000000000000000000000000000000000000",)json"
    R"json("version":0})json";

const std::string GOLD_CANONICAL =
    R"json({"entity":{"domain":"sequentia.io"},)json"
    R"json("issuer_pubkey":"020000000000000000000000000000000000000000000000000000000000000000",)json"
    R"json("name":"Gold (troy ounce)","precision":8,"ticker":"GOLD","version":0})json";

const std::string GOLD_CONTRACT_HASH = "646f250b0a99f8fd25a932e0db3a3a6ba777e1e86798064f0129cb7fc8f71956";

} // namespace

BOOST_AUTO_TEST_CASE(canonical_json_matches_the_registry)
{
    BOOST_CHECK_EQUAL(AssetContract::CanonicalJson(ParseContract(GOLD_CONTRACT_JSON)), GOLD_CANONICAL);
}

BOOST_AUTO_TEST_CASE(contract_hash_matches_the_registry)
{
    // HexStr prints the digest in its natural byte order, which is the order the
    // issuance commits and the registry re-hashes. uint256::GetHex() would print
    // it reversed -- that reversal is the trap this whole module exists to keep
    // issuers away from, so assert the natural order explicitly.
    BOOST_CHECK_EQUAL(HexStr(AssetContract::Hash(ParseContract(GOLD_CONTRACT_JSON))), GOLD_CONTRACT_HASH);
}

BOOST_AUTO_TEST_CASE(canonical_json_is_independent_of_key_order)
{
    // Same contract, keys written in a different order: the hash must not move.
    const std::string shuffled =
        R"json({"version":0,"issuer_pubkey":"020000000000000000000000000000000000000000000000000000000000000000",)json"
        R"json("ticker":"GOLD","entity":{"domain":"sequentia.io"},"precision":8,"name":"Gold (troy ounce)"})json";
    BOOST_CHECK_EQUAL(AssetContract::CanonicalJson(ParseContract(shuffled)), GOLD_CANONICAL);
    BOOST_CHECK_EQUAL(HexStr(AssetContract::Hash(ParseContract(shuffled))), GOLD_CONTRACT_HASH);
}

BOOST_AUTO_TEST_CASE(build_produces_the_registry_contract)
{
    const UniValue built = AssetContract::Build(
        "Gold (troy ounce)", "GOLD", 8, "sequentia.io",
        "020000000000000000000000000000000000000000000000000000000000000000");
    BOOST_CHECK_EQUAL(AssetContract::CanonicalJson(built), GOLD_CANONICAL);
}

BOOST_AUTO_TEST_CASE(build_folds_domain_and_pubkey_to_lower_case)
{
    const UniValue built = AssetContract::Build(
        "Mixed", "MIX", 2, "Example.COM",
        "02AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    BOOST_CHECK_EQUAL(built["entity"]["domain"].get_str(), "example.com");
    BOOST_CHECK_EQUAL(built["issuer_pubkey"].get_str(),
                      "02aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    // The ticker is left alone: the registry accepts mixed case there (tSEQ).
    BOOST_CHECK_EQUAL(built["ticker"].get_str(), "MIX");
    BOOST_CHECK(AssetContract::Validate(built).empty());
}

BOOST_AUTO_TEST_CASE(json_string_escaping_follows_json_stringify)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("s", std::string("a\"b\\c\nd\te"));
    BOOST_CHECK_EQUAL(AssetContract::CanonicalJson(o), R"json({"s":"a\"b\\c\nd\te"})json");

    // A C0 control character with no shorthand gets the \u form, lower-case hex,
    // as JSON.stringify writes it.
    UniValue c(UniValue::VOBJ);
    c.pushKV("s", std::string(1, '\x0b'));
    BOOST_CHECK_EQUAL(AssetContract::CanonicalJson(c), "{\"s\":\"\\u000b\"}");

    // Non-ASCII passes through as raw UTF-8 rather than being \u-escaped.
    UniValue u(UniValue::VOBJ);
    u.pushKV("s", std::string("caff\xc3\xa8"));
    BOOST_CHECK_EQUAL(AssetContract::CanonicalJson(u), "{\"s\":\"caff\xc3\xa8\"}");
}

BOOST_AUTO_TEST_CASE(validate_rejects_the_placeholder_pubkey)
{
    // This is why the seeded demo assets cannot be registered: their contract
    // carries an all-zeros placeholder key.
    const std::vector<std::string> errors = AssetContract::Validate(ParseContract(GOLD_CONTRACT_JSON));
    BOOST_CHECK_EQUAL(errors.size(), 1U);
    BOOST_CHECK(errors[0].find("issuer_pubkey") != std::string::npos);

    // ... including the x-only spelling of the same placeholder.
    UniValue xonly = AssetContract::Build("N", "N", 0, "example.com", std::string(64, '0'));
    BOOST_CHECK(!AssetContract::Validate(xonly).empty());
}

BOOST_AUTO_TEST_CASE(validate_accepts_a_well_formed_contract)
{
    const UniValue good = AssetContract::Build(
        "Alberto's Test Token", "ADLT", 8, "albertodeluigi.com",
        "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    BOOST_CHECK(AssetContract::Validate(good).empty());

    // An x-only key is equally acceptable (OpenAMP enclave keys are BIP340).
    const UniValue xonly = AssetContract::Build(
        "Restricted", "RSTR", 2, "example.com",
        "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    BOOST_CHECK(AssetContract::Validate(xonly).empty());
}

BOOST_AUTO_TEST_CASE(validate_checks_each_field)
{
    const std::string key = "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
    auto invalid = [&](const UniValue& c) { return !AssetContract::Validate(c).empty(); };

    BOOST_CHECK(invalid(AssetContract::Build("", "TICK", 8, "example.com", key)));                    // empty name
    BOOST_CHECK(invalid(AssetContract::Build(std::string(256, 'x'), "TICK", 8, "example.com", key)));  // name too long
    BOOST_CHECK(invalid(AssetContract::Build("N", "", 8, "example.com", key)));                        // empty ticker
    BOOST_CHECK(invalid(AssetContract::Build("N", "TOOLONGATICKER", 8, "example.com", key)));          // ticker > 12
    BOOST_CHECK(invalid(AssetContract::Build("N", "BAD TICK", 8, "example.com", key)));                // space in ticker
    BOOST_CHECK(invalid(AssetContract::Build("N", "TICK", 9, "example.com", key)));                    // precision > 8
    BOOST_CHECK(invalid(AssetContract::Build("N", "TICK", -1, "example.com", key)));                   // precision < 0
    BOOST_CHECK(invalid(AssetContract::Build("N", "TICK", 8, "example.com", "not-hex")));              // bad key
    BOOST_CHECK(invalid(AssetContract::Build("N", "TICK", 8, "example.com", key.substr(0, 60))));      // wrong length

    // A name may hold spaces, punctuation and non-ASCII.
    BOOST_CHECK(AssetContract::Validate(AssetContract::Build("Caff\xc3\xa8 & Co. (2026)", "CAF", 8, "example.com", key)).empty());

    // version must be 0, and unknown top-level fields are refused so the hash
    // stays well defined.
    UniValue bad_version = AssetContract::Build("N", "TICK", 8, "example.com", key);
    bad_version.pushKV("version", 1);
    BOOST_CHECK(invalid(bad_version));

    UniValue extra = AssetContract::Build("N", "TICK", 8, "example.com", key);
    extra.pushKV("surprise", "field");
    BOOST_CHECK(invalid(extra));
}

BOOST_AUTO_TEST_CASE(validate_checks_the_domain)
{
    const std::string key = "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
    auto domain_ok = [&](const std::string& d) {
        return AssetContract::Validate(AssetContract::Build("N", "TICK", 8, d, key)).empty();
    };

    BOOST_CHECK(domain_ok("example.com"));
    BOOST_CHECK(domain_ok("sub.example.com"));
    BOOST_CHECK(domain_ok("my-asset.example.co.uk"));
    BOOST_CHECK(domain_ok("x1.io"));

    BOOST_CHECK(!domain_ok(""));
    BOOST_CHECK(!domain_ok("example"));          // no TLD: names no registrable domain
    BOOST_CHECK(!domain_ok("example.c"));        // one-letter TLD
    BOOST_CHECK(!domain_ok("example.c0m"));      // digit in TLD
    BOOST_CHECK(!domain_ok(".example.com"));     // leading dot
    BOOST_CHECK(!domain_ok("example.com."));     // trailing dot
    BOOST_CHECK(!domain_ok("exa mple.com"));     // space
    BOOST_CHECK(!domain_ok("-example.com"));     // label starts with a hyphen
    BOOST_CHECK(!domain_ok("example-.com"));     // label ends with a hyphen
    BOOST_CHECK(!domain_ok("exa--mple.com"));    // doubled hyphen
    BOOST_CHECK(!domain_ok("https://example.com"));
    BOOST_CHECK(!domain_ok("example.com/path"));
}

BOOST_AUTO_TEST_CASE(proof_line_and_url)
{
    const std::string asset = "3a0f9192219db59f8d7f87d93ac6311095dfe1255d149727b87baaa7d2cc71a1";
    BOOST_CHECK_EQUAL(AssetContract::AssetProofLine("sequentia.io", asset),
                      "Authorize linking the domain name sequentia.io to the Sequentia asset " + asset);
    BOOST_CHECK_EQUAL(AssetContract::AssetProofPath(asset), ".well-known/sequentia-asset-proof-" + asset);
    BOOST_CHECK_EQUAL(AssetContract::AssetProofUrl("sequentia.io", asset),
                      "https://sequentia.io/.well-known/sequentia-asset-proof-" + asset);
}

BOOST_AUTO_TEST_SUITE_END()
