// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vrf.h>

#include <key.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <set>

BOOST_FIXTURE_TEST_SUITE(vrf_tests, BasicTestingSetup)

static CKey MakeKey()
{
    CKey k;
    k.MakeNewKey(true);
    return k;
}

static std::vector<unsigned char> Bytes(const std::string& s)
{
    return std::vector<unsigned char>(s.begin(), s.end());
}

// A proof produced by VrfProve verifies under the corresponding public key and
// yields a 32-byte output equal to VrfProofToHash.
BOOST_AUTO_TEST_CASE(vrf_prove_verify_roundtrip)
{
    CKey key = MakeKey();
    CPubKey pub = key.GetPubKey();
    std::vector<unsigned char> alpha = Bytes("sequentia-slot-seed");

    auto proof = VrfProve(key, alpha);
    BOOST_REQUIRE(proof.has_value());
    BOOST_CHECK_EQUAL(proof->size(), VRF_PROOF_SIZE);

    uint256 beta;
    BOOST_CHECK(VrfVerify(pub, alpha, *proof, beta));
    auto beta2 = VrfProofToHash(*proof);
    BOOST_REQUIRE(beta2.has_value());
    BOOST_CHECK(beta == *beta2);
}

// Verification fails under the wrong key, the wrong input, or a tampered proof.
BOOST_AUTO_TEST_CASE(vrf_verify_rejects_bad_inputs)
{
    CKey key = MakeKey();
    CPubKey pub = key.GetPubKey();
    std::vector<unsigned char> alpha = Bytes("input-A");
    auto proof = VrfProve(key, alpha);
    BOOST_REQUIRE(proof.has_value());

    uint256 beta;
    // Wrong public key.
    BOOST_CHECK(!VrfVerify(MakeKey().GetPubKey(), alpha, *proof, beta));
    // Wrong input.
    BOOST_CHECK(!VrfVerify(pub, Bytes("input-B"), *proof, beta));
    // Tampered proof (flip a byte in each of gamma[0..32], c[33..48], s[49..80]).
    for (size_t idx : {0u, 5u, 40u, 70u}) {
        auto bad = *proof;
        bad[idx] ^= 0x01;
        BOOST_CHECK(!VrfVerify(pub, alpha, bad, beta));
    }
    // Wrong length.
    auto truncated = *proof;
    truncated.pop_back();
    BOOST_CHECK(!VrfVerify(pub, alpha, truncated, beta));
}

// Uniqueness/determinism: the output is the same across repeated proofs for a
// fixed (key, input), and differs across keys and across inputs.
BOOST_AUTO_TEST_CASE(vrf_output_is_unique_and_deterministic)
{
    CKey key = MakeKey();
    CPubKey pub = key.GetPubKey();
    std::vector<unsigned char> alpha = Bytes("fixed-input");

    auto p1 = VrfProve(key, alpha);
    auto p2 = VrfProve(key, alpha);
    BOOST_REQUIRE(p1 && p2);
    // Deterministic nonce ⇒ identical proof, hence identical output.
    BOOST_CHECK(*p1 == *p2);

    uint256 b1, b2;
    BOOST_CHECK(VrfVerify(pub, alpha, *p1, b1));
    BOOST_CHECK(VrfVerify(pub, alpha, *p2, b2));
    BOOST_CHECK(b1 == b2);

    // Different input ⇒ different output.
    auto p3 = VrfProve(key, Bytes("other-input"));
    uint256 b3;
    BOOST_REQUIRE(p3.has_value());
    BOOST_CHECK(VrfVerify(pub, Bytes("other-input"), *p3, b3));
    BOOST_CHECK(b1 != b3);

    // Different key, same input ⇒ different output (overwhelmingly).
    CKey key2 = MakeKey();
    auto p4 = VrfProve(key2, alpha);
    uint256 b4;
    BOOST_REQUIRE(p4.has_value());
    BOOST_CHECK(VrfVerify(key2.GetPubKey(), alpha, *p4, b4));
    BOOST_CHECK(b1 != b4);
}

// Pseudorandomness (smoke test): outputs over many distinct inputs are all
// distinct and the top bit is roughly balanced.
BOOST_AUTO_TEST_CASE(vrf_output_looks_random)
{
    CKey key = MakeKey();
    CPubKey pub = key.GetPubKey();
    std::set<uint256> outputs;
    int high_bit_set = 0;
    const int n = 256;
    for (int i = 0; i < n; ++i) {
        std::vector<unsigned char> alpha = {(unsigned char)i, (unsigned char)(i >> 8)};
        auto proof = VrfProve(key, alpha);
        BOOST_REQUIRE(proof.has_value());
        uint256 beta;
        BOOST_REQUIRE(VrfVerify(pub, alpha, *proof, beta));
        outputs.insert(beta);
        if (beta.begin()[31] & 0x80) high_bit_set++;
    }
    // All outputs distinct.
    BOOST_CHECK_EQUAL(outputs.size(), (size_t)n);
    // Roughly balanced high bit (binomial; very loose bounds).
    BOOST_CHECK(high_bit_set > n / 4);
    BOOST_CHECK(high_bit_set < 3 * n / 4);
}

// Golden known-answer vectors pinning the VRF construction. Generated from the
// implementation itself; their purpose is to catch *accidental* changes to the
// scheme (suite byte, hash-to-curve, challenge, proof-to-hash). If the
// construction is deliberately revised (e.g. to an RFC 9381 ciphersuite), these
// must be regenerated. Private key = 0x01 repeated 32 times.
BOOST_AUTO_TEST_CASE(vrf_known_answer_vectors)
{
    CKey key;
    const std::vector<unsigned char> keydata(32, 0x01);
    key.Set(keydata.begin(), keydata.end(), /*fCompressedIn=*/true);
    BOOST_REQUIRE(key.IsValid());
    CPubKey pub = key.GetPubKey();
    BOOST_CHECK_EQUAL(HexStr(pub), "031b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f");

    struct KAT { std::string alpha_hex; std::string proof_hex; std::string output_hex; };
    const KAT kats[] = {
        {"00",
         "02faadfd917ac0dad8579987655afb4fae84cbe3b50854bdfae86434c98c543f6b8a3d09f751c7c1d9654b741efdbe0a685850e93597fc47968c5c1a6da89cd2c83e1a8676afe53d03cd9b1a72f2f0479b",
         "6c161979216cc6712a3c8d1d2cbded0dcac38d54e98a148bd650cfe0b23e671d"},
        {"deadbeef",
         "033a2f69ffea623598b0a6d3013cd416f8f691562c7cc553dc74f68717a6e6faa6ac094f695f8eb7effcc2d4ed2b298e107dc16679023f599bda68d731297162ee97dbb7701c9d27ef528119f95ad855fd",
         "fea30b726d0052ea49764e169ec4440b1ae65ba1658201396ded2d22a273515c"},
    };
    for (const KAT& kat : kats) {
        std::vector<unsigned char> alpha = ParseHex(kat.alpha_hex);
        auto proof = VrfProve(key, alpha);
        BOOST_REQUIRE(proof.has_value());
        BOOST_CHECK_EQUAL(HexStr(*proof), kat.proof_hex);
        uint256 output;
        BOOST_CHECK(VrfVerify(pub, alpha, *proof, output));
        BOOST_CHECK_EQUAL(output.GetHex(), kat.output_hex);
        // And the pinned proof bytes verify directly.
        uint256 output2;
        BOOST_CHECK(VrfVerify(pub, alpha, ParseHex(kat.proof_hex), output2));
        BOOST_CHECK_EQUAL(output2.GetHex(), kat.output_hex);
    }
}

BOOST_AUTO_TEST_SUITE_END()
