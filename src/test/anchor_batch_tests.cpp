// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: batching the anchor watcher's questions to the parent chain.
//
// Batching turns thousands of round trips into a handful, and it has exactly
// one way of being silently wrong: pairing an answer with the wrong question.
// A slow batch is a nuisance; a mismatched one judges an anchor by another
// anchor's verdict and then invalidates a block on the strength of it. So the
// matching is a pure function, and this file feeds it the orderings a daemon
// is ALLOWED to send -- which are not the ones it usually does.

#include <anchor.h>
#include <mainchainrpc.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(anchor_batch_tests, BasicTestingSetup)

namespace {
//! A reply carrying nothing but its id, which is all the matcher looks at.
UniValue Reply(int id)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("result", NullUniValue);
    o.pushKV("error", NullUniValue);
    o.pushKV("id", id);
    return o;
}

//! What bitcoind returns for a block that IS on the best chain.
UniValue HeaderReply(int64_t height, int64_t confirmations)
{
    UniValue h(UniValue::VOBJ);
    h.pushKV("height", height);
    h.pushKV("confirmations", confirmations);
    UniValue o(UniValue::VOBJ);
    o.pushKV("result", h);
    o.pushKV("error", NullUniValue);
    return o;
}
}  // namespace

BOOST_AUTO_TEST_CASE(a_batch_reply_is_reordered_by_id)
{
    // In order: the easy case, and the one that would hide a position-based bug.
    {
        UniValue arr(UniValue::VARR);
        for (int i = 0; i < 4; ++i) arr.push_back(Reply(i));
        const auto out = MatchBatchReplies(arr, 4, "getblockheader");
        BOOST_REQUIRE_EQUAL(out.size(), 4U);
        for (int i = 0; i < 4; ++i) BOOST_CHECK_EQUAL(find_value(out[i], "id").get_int(), i);
    }

    // Backwards. A daemon may answer a batch in any order it likes, and this is
    // the case that separates matching from indexing.
    {
        UniValue arr(UniValue::VARR);
        for (int i = 3; i >= 0; --i) arr.push_back(Reply(i));
        const auto out = MatchBatchReplies(arr, 4, "getblockheader");
        BOOST_REQUIRE_EQUAL(out.size(), 4U);
        for (int i = 0; i < 4; ++i) BOOST_CHECK_EQUAL(find_value(out[i], "id").get_int(), i);
    }

    // Shuffled.
    {
        UniValue arr(UniValue::VARR);
        for (int i : {2, 0, 3, 1}) arr.push_back(Reply(i));
        const auto out = MatchBatchReplies(arr, 4, "getblockheader");
        for (int i = 0; i < 4; ++i) BOOST_CHECK_EQUAL(find_value(out[i], "id").get_int(), i);
    }
}

BOOST_AUTO_TEST_CASE(a_batch_reply_that_cannot_be_matched_is_refused)
{
    // Every one of these could otherwise leave a slot holding somebody else's
    // answer, or holding nothing while looking like an answer.
    UniValue dup(UniValue::VARR);
    dup.push_back(Reply(0));
    dup.push_back(Reply(0));
    BOOST_CHECK_THROW(MatchBatchReplies(dup, 2, "getblockheader"), std::runtime_error);

    UniValue oob(UniValue::VARR);
    oob.push_back(Reply(0));
    oob.push_back(Reply(9));
    BOOST_CHECK_THROW(MatchBatchReplies(oob, 2, "getblockheader"), std::runtime_error);

    UniValue negative(UniValue::VARR);
    negative.push_back(Reply(0));
    negative.push_back(Reply(-1));
    BOOST_CHECK_THROW(MatchBatchReplies(negative, 2, "getblockheader"), std::runtime_error);

    UniValue no_id(UniValue::VARR);
    no_id.push_back(Reply(0));
    UniValue bare(UniValue::VOBJ);
    bare.pushKV("result", NullUniValue);
    no_id.push_back(bare);
    BOOST_CHECK_THROW(MatchBatchReplies(no_id, 2, "getblockheader"), std::runtime_error);

    UniValue short_arr(UniValue::VARR);
    short_arr.push_back(Reply(0));
    BOOST_CHECK_THROW(MatchBatchReplies(short_arr, 2, "getblockheader"), std::runtime_error);

    UniValue not_an_array(UniValue::VOBJ);
    not_an_array.pushKV("result", NullUniValue);
    BOOST_CHECK_THROW(MatchBatchReplies(not_an_array, 1, "getblockheader"), std::runtime_error);

    UniValue not_objects(UniValue::VARR);
    not_objects.push_back("nope");
    BOOST_CHECK_THROW(MatchBatchReplies(not_objects, 1, "getblockheader"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(one_headers_reply_yields_one_verdict)
{
    // The batched and single-call paths share this, so it is the definition of
    // an anchor verdict rather than one path's opinion of it.
    BOOST_CHECK(InterpretAnchorHeaderReply(HeaderReply(800000, 6), 800000) == AnchorCheckResult::OK);

    // confirmations == -1 is bitcoind for "this block is not on the best
    // chain", which is precisely the reorganised-away anchor this whole
    // subsystem exists to catch.
    BOOST_CHECK(InterpretAnchorHeaderReply(HeaderReply(800000, -1), 800000) == AnchorCheckResult::STALE);
    BOOST_CHECK(InterpretAnchorHeaderReply(HeaderReply(800000, 0), 800000) == AnchorCheckResult::STALE);

    // On the best chain, but not where the anchor claimed it was.
    BOOST_CHECK(InterpretAnchorHeaderReply(HeaderReply(799999, 6), 800000) == AnchorCheckResult::HEIGHT_MISMATCH);

    // The daemon has never heard of it.
    UniValue err(UniValue::VOBJ);
    UniValue e(UniValue::VOBJ);
    e.pushKV("code", -5);
    e.pushKV("message", "Block not found");
    err.pushKV("result", NullUniValue);
    err.pushKV("error", e);
    BOOST_CHECK(InterpretAnchorHeaderReply(err, 800000) == AnchorCheckResult::NOT_FOUND);

    // A reply with no usable result is NOT quietly an OK.
    UniValue empty(UniValue::VOBJ);
    empty.pushKV("result", NullUniValue);
    empty.pushKV("error", NullUniValue);
    BOOST_CHECK(InterpretAnchorHeaderReply(empty, 800000) == AnchorCheckResult::NOT_FOUND);

    // Missing fields must not read as canonical either.
    UniValue no_conf(UniValue::VOBJ);
    UniValue h(UniValue::VOBJ);
    h.pushKV("height", (int64_t)800000);
    no_conf.pushKV("result", h);
    no_conf.pushKV("error", NullUniValue);
    BOOST_CHECK(InterpretAnchorHeaderReply(no_conf, 800000) == AnchorCheckResult::STALE);
}

BOOST_AUTO_TEST_SUITE_END()
