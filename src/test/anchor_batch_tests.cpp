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

#include <map>
#include <set>

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

    // Any other error is the daemon failing to answer, not a verdict on the
    // block: RPC_IN_WARMUP while bitcoind restarts must never read as "this
    // anchor is off the chain", or the node turns on its own valid history.
    UniValue warmup(UniValue::VOBJ);
    UniValue w(UniValue::VOBJ);
    w.pushKV("code", -28);
    w.pushKV("message", "Loading block index...");
    warmup.pushKV("result", NullUniValue);
    warmup.pushKV("error", w);
    BOOST_CHECK(InterpretAnchorHeaderReply(warmup, 800000) == AnchorCheckResult::NO_CONNECTION);

    // A reply with no usable result is NOT quietly an OK, and not a verdict
    // either.
    UniValue empty(UniValue::VOBJ);
    empty.pushKV("result", NullUniValue);
    empty.pushKV("error", NullUniValue);
    BOOST_CHECK(InterpretAnchorHeaderReply(empty, 800000) == AnchorCheckResult::NO_CONNECTION);

    // Missing fields must not read as canonical either.
    UniValue no_conf(UniValue::VOBJ);
    UniValue h(UniValue::VOBJ);
    h.pushKV("height", (int64_t)800000);
    no_conf.pushKV("result", h);
    no_conf.pushKV("error", NullUniValue);
    BOOST_CHECK(InterpretAnchorHeaderReply(no_conf, 800000) == AnchorCheckResult::STALE);
}

BOOST_AUTO_TEST_CASE(an_answer_asked_before_a_parent_move_is_not_cached_after_it)
{
    // An OK the daemon gave just before Bitcoin reorganized, arriving after
    // the watcher dropped the caches for that reorganization, would otherwise
    // be kept as current until the next reorganization at or below it.
    const uint256 anchor = uint256S("aa");
    BOOST_CHECK(CacheAnchorOkForTesting(800000, anchor, [] {}));
    BOOST_CHECK(!CacheAnchorOkForTesting(800000, anchor, [] { DropAnchorCachesForTesting(799990); }));
    BOOST_CHECK(!CacheAnchorOkForTesting(800000, anchor, [] { DropAnchorCachesForTesting(-1); }));
    // Asked after the drop: kept.
    DropAnchorCachesForTesting(799990);
    BOOST_CHECK(CacheAnchorOkForTesting(800000, anchor, [] {}));
}

namespace {
UniValue Tip(const std::string& hash, int height, int branchlen, const std::string& status)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", height);
    o.pushKV("hash", hash);
    o.pushKV("branchlen", branchlen);
    o.pushKV("status", status);
    return o;
}
const std::string H_A2 = std::string(63, '0') + "1"; // the active tip
const std::string H_B1 = std::string(63, '0') + "2"; // a branch forking at 99
const std::string H_B2 = std::string(63, '0') + "3"; // B1 extended by one block
const std::string H_C = std::string(63, '0') + "4";  // an old fork at 50
} // namespace

BOOST_AUTO_TEST_CASE(chain_tips_parse_only_whole_replies)
{
    UniValue arr(UniValue::VARR);
    arr.push_back(Tip(H_A2, 101, 0, "active"));
    arr.push_back(Tip(H_B1, 100, 1, "valid-fork"));
    MainchainChainTips tips;
    BOOST_CHECK(ParseMainchainChainTips(arr, tips));
    BOOST_CHECK_EQUAL(tips.size(), 2U);
    BOOST_CHECK_EQUAL(tips[uint256S(H_B1)].height - tips[uint256S(H_B1)].branchlen, 99);

    // One unreadable tip makes the whole reply unreadable: a branch that cannot
    // be judged must not be passed over as if it were not there.
    MainchainChainTips untouched;
    UniValue bad = arr;
    bad.push_back(Tip("zz", 100, 1, "valid-fork"));
    BOOST_CHECK(!ParseMainchainChainTips(bad, untouched));
    BOOST_CHECK(untouched.empty());
    UniValue too_long(UniValue::VARR);
    too_long.push_back(Tip(H_B1, 5, 6, "valid-fork"));
    BOOST_CHECK(!ParseMainchainChainTips(too_long, untouched));
    UniValue no_status(UniValue::VARR);
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", 100);
    o.pushKV("hash", H_B1);
    o.pushKV("branchlen", 1);
    no_status.push_back(o);
    BOOST_CHECK(!ParseMainchainChainTips(no_status, untouched));
    BOOST_CHECK(!ParseMainchainChainTips(UniValue(UniValue::VOBJ), untouched));
    BOOST_CHECK(untouched.empty());
}

BOOST_AUTO_TEST_CASE(a_branch_the_daemon_may_have_stood_on_is_evidence_and_a_mere_fork_is_not)
{
    // The old tip has chainwork 100. Tips never connected are judged by theirs.
    const std::optional<arith_uint256> old_work = arith_uint256(100);
    std::map<uint256, arith_uint256> work;
    std::set<uint256> asked;
    const auto work_of = [&](const uint256& h) -> std::optional<arith_uint256> {
        asked.insert(h);
        const auto it = work.find(h);
        if (it == work.end()) return std::nullopt;
        return it->second;
    };

    MainchainChainTips before;
    before[uint256S(H_A2)] = {100, 0, "active"};
    before[uint256S(H_C)] = {60, 10, "valid-fork"};

    // Nothing new: the old fork at 50 was there already, and the active tip
    // moved on (that move is the tip comparison's to judge).
    MainchainChainTips same = before;
    same[uint256S(H_A2)] = {101, 0, "active"};
    ChangedBranches c = ClassifyChangedBranches(before, same, old_work, work_of);
    BOOST_CHECK(!c.recheck_above && !c.moved_below);

    // A branch the daemon stood on and left: re-check above its fork, and it
    // is evidence that Bitcoin moved off something.
    MainchainChainTips flipped = same;
    flipped[uint256S(H_B1)] = {100, 1, "valid-fork"};
    c = ClassifyChangedBranches(before, flipped, old_work, work_of);
    BOOST_CHECK_EQUAL(c.recheck_above.value_or(-2), 99);
    BOOST_CHECK_EQUAL(c.moved_below.value_or(-2), 99);

    // A tip already there whose status changed: an old branch revisited.
    MainchainChainTips revisited = same;
    revisited[uint256S(H_C)] = {60, 10, "invalid"};
    c = ClassifyChangedBranches(before, revisited, old_work, work_of);
    BOOST_CHECK_EQUAL(c.recheck_above.value_or(-2), 50);
    BOOST_CHECK_EQUAL(c.moved_below.value_or(-2), 50);

    // The same branch grown by a block never connected, with more work than
    // the old tip: evidence, whether its tip is new or was there already (the
    // daemon held its last block as a header before the branch went active).
    for (const char* status : {"valid-headers", "headers-only"}) {
        work[uint256S(H_B2)] = arith_uint256(150);
        MainchainChainTips grown = same;
        grown[uint256S(H_B2)] = {101, 2, status};
        c = ClassifyChangedBranches(before, grown, old_work, work_of);
        BOOST_CHECK_EQUAL(c.recheck_above.value_or(-2), 99);
        BOOST_CHECK_EQUAL(c.moved_below.value_or(-2), 99);
        MainchainChainTips held_before = before;
        held_before[uint256S(H_B2)] = {101, 2, status};
        c = ClassifyChangedBranches(held_before, grown, old_work, work_of);
        BOOST_CHECK_EQUAL(c.recheck_above.value_or(-2), 99);
        BOOST_CHECK_EQUAL(c.moved_below.value_or(-2), 99);
    }

    // A header-only fork with no more work than the old tip: nobody's best
    // chain was there. New, it is asked about again and is no evidence; seen
    // before, it is nothing at all.
    work[uint256S(H_B2)] = arith_uint256(100);
    MainchainChainTips light = same;
    light[uint256S(H_B2)] = {101, 2, "headers-only"};
    c = ClassifyChangedBranches(before, light, old_work, work_of);
    BOOST_CHECK_EQUAL(c.recheck_above.value_or(-2), 99);
    BOOST_CHECK(!c.moved_below);
    MainchainChainTips light_before = before;
    light_before[uint256S(H_B2)] = {101, 2, "headers-only"};
    c = ClassifyChangedBranches(light_before, light, old_work, work_of);
    BOOST_CHECK(!c.recheck_above && !c.moved_below);

    // An unknown chainwork, the tip's or the old tip's, counts as more.
    work.erase(uint256S(H_B2));
    c = ClassifyChangedBranches(light_before, light, old_work, work_of);
    BOOST_CHECK_EQUAL(c.moved_below.value_or(-2), 99);
    work[uint256S(H_B2)] = arith_uint256(1);
    c = ClassifyChangedBranches(light_before, light, std::nullopt, work_of);
    BOOST_CHECK_EQUAL(c.moved_below.value_or(-2), 99);

    // The lowest of several.
    MainchainChainTips both = flipped;
    both[uint256S(H_C)] = {60, 10, "invalid"};
    c = ClassifyChangedBranches(before, both, old_work, work_of);
    BOOST_CHECK_EQUAL(c.recheck_above.value_or(-2), 50);
    BOOST_CHECK_EQUAL(c.moved_below.value_or(-2), 50);

    // An invalid tip with no more work than the old tip proves no move: a
    // block with valid proof of work and a bad body is marked failed without
    // ever being connected. It is asked about again, and voids nothing; with
    // more work it is evidence.
    work[uint256S(H_C)] = arith_uint256(100);
    c = ClassifyChangedBranches(before, revisited, old_work, work_of);
    BOOST_CHECK_EQUAL(c.recheck_above.value_or(-2), 50);
    BOOST_CHECK(!c.moved_below);
    work[uint256S(H_C)] = arith_uint256(101);
    c = ClassifyChangedBranches(before, revisited, old_work, work_of);
    BOOST_CHECK_EQUAL(c.moved_below.value_or(-2), 50);

    // Chainwork is asked of tips never connected and of new invalid ones only,
    // and the active tip is never a branch, whatever it was before.
    BOOST_CHECK(!asked.count(uint256S(H_B1)) && asked.count(uint256S(H_C)));
    MainchainChainTips was_fork;
    was_fork[uint256S(H_C)] = {60, 10, "valid-fork"};
    MainchainChainTips returned;
    returned[uint256S(H_C)] = {60, 0, "active"};
    c = ClassifyChangedBranches(was_fork, returned, old_work, work_of);
    BOOST_CHECK(!c.recheck_above && !c.moved_below);
}

BOOST_AUTO_TEST_SUITE_END()
