// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vrf.h>

#include <key.h>
#include <test/util/setup_common.h>

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
    // Tampered proof (flip a byte in each of gamma, c, s).
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

BOOST_AUTO_TEST_SUITE_END()
