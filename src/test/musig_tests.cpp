// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <musig.h>

#include <key.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(musig_tests, BasicTestingSetup)

static std::vector<unsigned char> Msg(unsigned char b)
{
    return std::vector<unsigned char>(32, b);
}

// An aggregate signature by all signers verifies under the aggregate key, and
// the aggregate key depends only on the *set* of pubkeys, not their order.
BOOST_AUTO_TEST_CASE(musig_aggregate_sign_verify)
{
    const size_t n = 5;
    std::vector<CKey> keys;
    std::vector<CPubKey> pubs;
    for (size_t i = 0; i < n; ++i) {
        CKey k; k.MakeNewKey(true);
        keys.push_back(k);
        pubs.push_back(k.GetPubKey());
    }
    std::vector<unsigned char> msg = Msg(0x42);

    auto agg = MuSigAggregatePubkey(pubs);
    BOOST_REQUIRE(agg.has_value());
    BOOST_CHECK_EQUAL(agg->size(), 32U);

    // Order-independence of the aggregate key.
    std::vector<CPubKey> shuffled = pubs;
    std::swap(shuffled.front(), shuffled.back());
    auto agg2 = MuSigAggregatePubkey(shuffled);
    BOOST_REQUIRE(agg2.has_value());
    BOOST_CHECK(*agg == *agg2);

    auto sig = MuSigSign(keys, pubs, msg);
    BOOST_REQUIRE(sig.has_value());
    BOOST_CHECK_EQUAL(sig->size(), 64U);
    BOOST_CHECK(MuSigVerify(pubs, msg, *sig));
    // Signing with the keys in a different order yields a signature valid
    // under the same (set-defined) aggregate key.
    std::vector<CKey> keys_rev(keys.rbegin(), keys.rend());
    std::vector<CPubKey> pubs_rev(pubs.rbegin(), pubs.rend());
    auto sig_rev = MuSigSign(keys_rev, pubs_rev, msg);
    BOOST_REQUIRE(sig_rev.has_value());
    BOOST_CHECK(MuSigVerify(pubs, msg, *sig_rev));
}

// Verification fails for the wrong message, a tampered signature, or a
// different signer set.
BOOST_AUTO_TEST_CASE(musig_verify_rejects)
{
    std::vector<CKey> keys;
    std::vector<CPubKey> pubs;
    for (size_t i = 0; i < 3; ++i) {
        CKey k; k.MakeNewKey(true);
        keys.push_back(k);
        pubs.push_back(k.GetPubKey());
    }
    std::vector<unsigned char> msg = Msg(0x07);
    auto sig = MuSigSign(keys, pubs, msg);
    BOOST_REQUIRE(sig.has_value());

    BOOST_CHECK(!MuSigVerify(pubs, Msg(0x08), *sig));      // wrong message
    auto bad = *sig; bad[10] ^= 0x01;
    BOOST_CHECK(!MuSigVerify(pubs, msg, bad));             // tampered sig

    // A different signer set (drop one) has a different aggregate key.
    std::vector<CPubKey> fewer(pubs.begin(), pubs.end() - 1);
    BOOST_CHECK(!MuSigVerify(fewer, msg, *sig));

    // Adding an unrelated key changes the aggregate key too.
    CKey extra; extra.MakeNewKey(true);
    std::vector<CPubKey> more = pubs; more.push_back(extra.GetPubKey());
    BOOST_CHECK(!MuSigVerify(more, msg, *sig));
}

// A q-of-m quorum is realized by aggregating exactly the q signing members:
// the signature verifies under the q-aggregate, not the full-m aggregate.
BOOST_AUTO_TEST_CASE(musig_quorum_subset)
{
    const size_t m = 5;
    std::vector<CKey> all_keys;
    std::vector<CPubKey> all_pubs;
    for (size_t i = 0; i < m; ++i) {
        CKey k; k.MakeNewKey(true);
        all_keys.push_back(k);
        all_pubs.push_back(k.GetPubKey());
    }
    std::vector<unsigned char> msg = Msg(0x99);

    // The signing quorum is the first 3 members.
    std::vector<CKey> q_keys(all_keys.begin(), all_keys.begin() + 3);
    std::vector<CPubKey> q_pubs(all_pubs.begin(), all_pubs.begin() + 3);
    auto sig = MuSigSign(q_keys, q_pubs, msg);
    BOOST_REQUIRE(sig.has_value());

    BOOST_CHECK(MuSigVerify(q_pubs, msg, *sig));   // under the quorum aggregate
    BOOST_CHECK(!MuSigVerify(all_pubs, msg, *sig)); // not under the full set

    // One signature, regardless of quorum size — the scaling property.
    BOOST_CHECK_EQUAL(sig->size(), 64U);
}

// MuSig2 is n-of-n: every aggregated key must sign. A missing signer (keys a
// strict subset of pubkeys) is rejected at the API boundary.
BOOST_AUTO_TEST_CASE(musig_requires_all_signers)
{
    std::vector<CKey> keys;
    std::vector<CPubKey> pubs;
    for (size_t i = 0; i < 4; ++i) {
        CKey k; k.MakeNewKey(true);
        if (i < 3) keys.push_back(k);  // only 3 keys for 4 pubkeys
        pubs.push_back(k.GetPubKey());
    }
    BOOST_CHECK(!MuSigSign(keys, pubs, Msg(0x01)).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
