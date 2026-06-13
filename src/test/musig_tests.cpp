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

// The distributed (per-host) two-round session API produces a signature
// verifying under the same aggregate key as the local MuSigSign — modelling a
// committee whose members never share their keys.
BOOST_AUTO_TEST_CASE(musig_distributed_rounds)
{
    const size_t n = 4;
    std::vector<CKey> keys;
    std::vector<CPubKey> pubs;
    for (size_t i = 0; i < n; ++i) {
        CKey k; k.MakeNewKey(true);
        keys.push_back(k);
        pubs.push_back(k.GetPubKey());
    }
    std::vector<unsigned char> msg = Msg(0x55);

    // Round 1: each member, in its own session, produces a public nonce.
    std::vector<std::string> ids;
    std::vector<std::vector<unsigned char>> pubnonces;
    for (size_t i = 0; i < n; ++i) {
        std::string id = "sess-" + std::to_string(i);
        std::string err;
        auto pn = MuSigSessionNonce(id, keys[i], pubs, msg, err);
        BOOST_REQUIRE_MESSAGE(pn.has_value(), err);
        BOOST_CHECK_EQUAL(pn->size(), 66U);
        ids.push_back(id);
        pubnonces.push_back(*pn);
    }

    // Round 2: each member partial-signs given all the public nonces.
    std::vector<std::vector<unsigned char>> partials;
    for (size_t i = 0; i < n; ++i) {
        std::string err;
        auto ps = MuSigSessionPartialSign(ids[i], keys[i], pubs, pubnonces, msg, err);
        BOOST_REQUIRE_MESSAGE(ps.has_value(), err);
        BOOST_CHECK_EQUAL(ps->size(), 32U);
        partials.push_back(*ps);
    }

    // Aggregate (public) → a 64-byte signature valid under the set aggregate.
    auto sig = MuSigAggregatePartials(pubs, pubnonces, partials, msg);
    BOOST_REQUIRE(sig.has_value());
    BOOST_CHECK_EQUAL(sig->size(), 64U);
    BOOST_CHECK(MuSigVerify(pubs, msg, *sig));
}

// Safety: a session is single-use (round 2 consumes the secret nonce), a stale
// id can't be reused, and round 2 refuses a different message/set than round 1.
BOOST_AUTO_TEST_CASE(musig_session_safety)
{
    std::vector<CKey> keys;
    std::vector<CPubKey> pubs;
    for (size_t i = 0; i < 3; ++i) {
        CKey k; k.MakeNewKey(true);
        keys.push_back(k);
        pubs.push_back(k.GetPubKey());
    }
    std::vector<unsigned char> msg = Msg(0x11);
    std::string err;

    // Round 1 for all members.
    std::vector<std::vector<unsigned char>> pubnonces;
    for (size_t i = 0; i < 3; ++i) {
        auto pn = MuSigSessionNonce("s" + std::to_string(i), keys[i], pubs, msg, err);
        BOOST_REQUIRE(pn.has_value());
        pubnonces.push_back(*pn);
    }

    // A duplicate session id is refused (would reuse a secret nonce).
    BOOST_CHECK(!MuSigSessionNonce("s0", keys[0], pubs, msg, err).has_value());

    // Round 2 with a mismatched message is refused without consuming the session.
    BOOST_CHECK(!MuSigSessionPartialSign("s0", keys[0], pubs, pubnonces, Msg(0x12), err).has_value());

    // The correct round 2 succeeds...
    BOOST_CHECK(MuSigSessionPartialSign("s0", keys[0], pubs, pubnonces, msg, err).has_value());
    // ...and the session is now consumed: a second call fails.
    BOOST_CHECK(!MuSigSessionPartialSign("s0", keys[0], pubs, pubnonces, msg, err).has_value());

    // Abort cleans up the remaining sessions (no crash, idempotent).
    MuSigSessionAbort("s1");
    MuSigSessionAbort("s1");
    BOOST_CHECK(!MuSigSessionPartialSign("s1", keys[1], pubs, pubnonces, msg, err).has_value());
}

// Malformed inputs must fail cleanly, never reach a libsecp ARG_CHECK abort:
// mispaired keys/pubkeys, the wrong member's key in round 2, a key outside the
// member set, and empty or wrong-count nonce/partial vectors.
BOOST_AUTO_TEST_CASE(musig_rejects_malformed_inputs)
{
    std::vector<CKey> keys;
    std::vector<CPubKey> pubs;
    for (size_t i = 0; i < 3; ++i) {
        CKey k; k.MakeNewKey(true);
        keys.push_back(k);
        pubs.push_back(k.GetPubKey());
    }
    std::vector<unsigned char> msg = Msg(0x33);
    std::string err;

    // MuSigSign: same sets, mispaired indexes (keys[i] not the key for
    // pubkeys[i]) — must return nullopt, not abort.
    std::vector<CKey> mispaired = {keys[1], keys[0], keys[2]};
    BOOST_CHECK(!MuSigSign(mispaired, pubs, msg).has_value());

    // Round 1 with a key outside the member set is refused.
    CKey outsider; outsider.MakeNewKey(true);
    BOOST_CHECK(!MuSigSessionNonce("out", outsider, pubs, msg, err).has_value());

    // Run a proper round 1 for everyone.
    std::vector<std::vector<unsigned char>> pubnonces;
    for (size_t i = 0; i < 3; ++i) {
        auto pn = MuSigSessionNonce("m" + std::to_string(i), keys[i], pubs, msg, err);
        BOOST_REQUIRE(pn.has_value());
        pubnonces.push_back(*pn);
    }

    // Round 2 with a different member's key: refused, and the session is NOT
    // consumed — the right key still completes it afterwards.
    BOOST_CHECK(!MuSigSessionPartialSign("m0", keys[1], pubs, pubnonces, msg, err).has_value());
    // Round 2 with empty or wrong-count nonce vectors: refused, not consumed.
    BOOST_CHECK(!MuSigSessionPartialSign("m0", keys[0], pubs, {}, msg, err).has_value());
    std::vector<std::vector<unsigned char>> short_nonces(pubnonces.begin(), pubnonces.end() - 1);
    BOOST_CHECK(!MuSigSessionPartialSign("m0", keys[0], pubs, short_nonces, msg, err).has_value());

    std::vector<std::vector<unsigned char>> partials;
    for (size_t i = 0; i < 3; ++i) {
        auto ps = MuSigSessionPartialSign("m" + std::to_string(i), keys[i], pubs, pubnonces, msg, err);
        BOOST_REQUIRE_MESSAGE(ps.has_value(), err);
        partials.push_back(*ps);
    }

    // Aggregation: empty or wrong-count nonces/partials are refused cleanly.
    BOOST_CHECK(!MuSigAggregatePartials(pubs, {}, partials, msg).has_value());
    BOOST_CHECK(!MuSigAggregatePartials(pubs, pubnonces, {}, msg).has_value());
    std::vector<std::vector<unsigned char>> short_partials(partials.begin(), partials.end() - 1);
    BOOST_CHECK(!MuSigAggregatePartials(pubs, pubnonces, short_partials, msg).has_value());

    // The well-formed aggregation still succeeds and verifies.
    auto sig = MuSigAggregatePartials(pubs, pubnonces, partials, msg);
    BOOST_REQUIRE(sig.has_value());
    BOOST_CHECK(MuSigVerify(pubs, msg, *sig));
}

BOOST_AUTO_TEST_SUITE_END()
