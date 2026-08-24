// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: the courier's key agreement.
//
// Two sides that derive different keys do not fail loudly. Every message the
// other sends simply fails to open and is discarded, and the session looks
// exactly like a maker that never answered -- so the convention is pinned here
// rather than discovered in the field.

#include <seqobcourier.h>

#include <crypto/sha256.h>
#include <key.h>
#include <pubkey.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(seqobcourier_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(both_sides_derive_the_same_key)
{
    // The property the whole session rests on: ECDH is symmetric, so the maker
    // and the taker reach the same key from opposite halves.
    CKey a, b;
    do { a.MakeNewKey(true); } while (!a.IsValid());
    do { b.MakeNewKey(true); } while (!b.IsValid());
    const CPubKey pa = a.GetPubKey(), pb = b.GetPubKey();

    unsigned char ka[32], kb[32];
    BOOST_REQUIRE(SeqobCourierSharedKey(std::vector<unsigned char>(pb.begin(), pb.end()),
                                        (const unsigned char*)a.begin(), ka));
    BOOST_REQUIRE(SeqobCourierSharedKey(std::vector<unsigned char>(pa.begin(), pa.end()),
                                        (const unsigned char*)b.begin(), kb));
    BOOST_CHECK_EQUAL(HexStr(Span<const unsigned char>(ka, 32)), HexStr(Span<const unsigned char>(kb, 32)));
}

BOOST_AUTO_TEST_CASE(the_key_is_sha256_of_x_with_the_parity_byte_dropped)
{
    // A scalar of 1 leaves the peer's point where it is, so the shared secret
    // is that point's own x coordinate. That makes the expected key computable
    // by hand, and pins the two things that could differ between
    // implementations: which coordinate is hashed, and whether the compressed
    // form's parity byte goes in with it.
    unsigned char one[32] = {0};
    one[31] = 1;

    // The generator, in compressed form.
    const std::vector<unsigned char> g = ParseHex("0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    unsigned char key[32];
    BOOST_REQUIRE(SeqobCourierSharedKey(g, one, key));

    unsigned char expected[32];
    CSHA256().Write(g.data() + 1, 32).Finalize(expected);   // x only, no parity byte
    BOOST_CHECK_EQUAL(HexStr(Span<const unsigned char>(key, 32)),
                      HexStr(Span<const unsigned char>(expected, 32)));
}

BOOST_AUTO_TEST_CASE(a_key_that_is_not_a_point_is_refused)
{
    unsigned char one[32] = {0};
    one[31] = 1;
    unsigned char key[32];
    BOOST_CHECK(!SeqobCourierSharedKey({}, one, key));
    BOOST_CHECK(!SeqobCourierSharedKey(ParseHex("02"), one, key));
    BOOST_CHECK(!SeqobCourierSharedKey(ParseHex("02" + std::string(64, 'f')), one, key));

    // ...and so is a scalar that is not one: multiplying by zero has no
    // shared secret to speak of.
    unsigned char zero[32] = {0};
    const std::vector<unsigned char> g = ParseHex("0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    BOOST_CHECK(!SeqobCourierSharedKey(g, zero, key));
}

BOOST_AUTO_TEST_SUITE_END()
