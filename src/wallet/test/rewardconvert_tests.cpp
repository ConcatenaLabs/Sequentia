// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA reward auto-conversion, layer 2: which staking rewards to sell.
//
// These are the SAME cases SWK's `lwk_wollet::staking_rewards` tests pin, in
// the same order and with the same numbers, on purpose. A node wallet and a
// light wallet watching one staker's keys must reach the same verdict about
// that staker's coins; two implementations that were never compared are two
// implementations that have already drifted.

#include <wallet/rewardconvert.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(rewardconvert_tests, BasicTestingSetup)

namespace {
CAsset Asset(uint8_t n)
{
    std::vector<unsigned char> v(32, n);
    return CAsset(uint256(v));
}

COutPoint Out(uint8_t n, uint32_t vout = 0)
{
    std::vector<unsigned char> v(32, n);
    return COutPoint(uint256(v), vout);
}

RewardCoin Mature(uint8_t txid, const CAsset& asset, CAmount amount)
{
    RewardCoin c;
    c.outpoint = Out(txid);
    c.asset = asset;
    c.amount = amount;
    c.mature = true;
    c.spent = false;
    return c;
}

RewardConvertSettings On(std::optional<CAsset> target = std::nullopt)
{
    RewardConvertSettings s;
    s.enabled = true;
    s.target = target;
    return s;
}

RewardBatch Batch(const CAsset& asset, CAmount value)
{
    RewardBatch b;
    b.asset = asset;
    b.value = value;
    b.inputs.push_back(Out(1));
    return b;
}
} // namespace

BOOST_AUTO_TEST_CASE(batches_group_by_asset_and_sum)
{
    const std::vector<RewardCoin> rewards{
        Mature(1, Asset(1), 100),
        Mature(2, Asset(1), 250),
        Mature(3, Asset(2), 900),
    };
    const auto b = RewardBatches(rewards, On(), {});
    BOOST_CHECK_EQUAL(b.size(), 2U);
    // Biggest first.
    BOOST_CHECK(b[0].asset == Asset(2));
    BOOST_CHECK_EQUAL(b[0].value, 900);
    BOOST_CHECK_EQUAL(b[1].value, 350);
    BOOST_CHECK_EQUAL(b[1].inputs.size(), 2U);
}

BOOST_AUTO_TEST_CASE(immature_spent_and_already_converted_are_never_batched)
{
    RewardCoin immature = Mature(1, Asset(1), 100);
    immature.mature = false;
    RewardCoin spent = Mature(2, Asset(1), 100);
    spent.spent = true;
    const RewardCoin done = Mature(3, Asset(1), 100);

    const std::set<COutPoint> already{done.outpoint};
    const auto b = RewardBatches({immature, spent, done}, On(), already);
    BOOST_CHECK(b.empty());
}

BOOST_AUTO_TEST_CASE(the_target_and_excluded_assets_are_left_alone)
{
    RewardConvertSettings s = On(Asset(1));
    s.exclude.insert(Asset(2));
    const auto b = RewardBatches({Mature(1, Asset(1), 100),   // the target itself
                                  Mature(2, Asset(2), 100),   // excluded
                                  Mature(3, Asset(3), 100)}, s, {});
    BOOST_CHECK_EQUAL(b.size(), 1U);
    BOOST_CHECK(b[0].asset == Asset(3));
}

BOOST_AUTO_TEST_CASE(a_batch_converts_when_the_book_clears_the_floor)
{
    const RewardQuote q{20000, 20000};
    const auto d = DecideRewardConversion(Batch(Asset(3), 100), q, On());
    BOOST_CHECK(d.Converts());
    BOOST_CHECK_EQUAL(d.receives, 20000);
}

BOOST_AUTO_TEST_CASE(no_market_is_a_wait_not_an_error)
{
    auto d = DecideRewardConversion(Batch(Asset(3), 100), std::nullopt, On());
    BOOST_CHECK(d.kind == RewardDecisionKind::NoMarket);
    // A market with no depth to deliver anything is the same situation.
    const RewardQuote empty{0, 20000};
    d = DecideRewardConversion(Batch(Asset(3), 100), empty, On());
    BOOST_CHECK(d.kind == RewardDecisionKind::NoMarket);
}

BOOST_AUTO_TEST_CASE(a_batch_below_the_floor_waits_for_more_rewards)
{
    const RewardQuote q{9999, 9999};
    const auto d = DecideRewardConversion(Batch(Asset(3), 100), q, On());
    BOOST_CHECK(d.kind == RewardDecisionKind::BelowFloor);
    BOOST_CHECK_EQUAL(d.receives, 9999);
    BOOST_CHECK_EQUAL(d.floor, 10000);
}

BOOST_AUTO_TEST_CASE(a_badly_quoted_market_is_refused_before_the_floor)
{
    // Would clear the floor, but 40% below the reference price.
    const RewardQuote q{60000, 100000};
    const auto d = DecideRewardConversion(Batch(Asset(3), 100), q, On());
    BOOST_CHECK(d.kind == RewardDecisionKind::SlippageTooHigh);
    BOOST_CHECK_EQUAL(d.slippage_bp, 4000);
    BOOST_CHECK_EQUAL(d.cap_bp, 200);
}

BOOST_AUTO_TEST_CASE(a_fill_better_than_reference_is_zero_slippage)
{
    const RewardQuote q{120000, 100000};
    BOOST_CHECK_EQUAL(q.SlippageBp(), 0);
}

BOOST_AUTO_TEST_CASE(nothing_converts_while_the_setting_is_off)
{
    RewardConvertSettings off;
    BOOST_CHECK(!off.enabled);
    const RewardQuote q{1000000, 1000000};
    const auto d = DecideRewardConversion(Batch(Asset(3), 100), q, off);
    BOOST_CHECK(d.kind == RewardDecisionKind::Disabled);
}

BOOST_AUTO_TEST_CASE(native_btc_is_the_default_target_and_settles_cross_chain)
{
    const RewardConvertSettings s;
    BOOST_CHECK(s.TargetIsNativeBtc());
    RewardConvertSettings a = On(Asset(1));
    BOOST_CHECK(!a.TargetIsNativeBtc());
}

BOOST_AUTO_TEST_CASE(a_whole_htlc_offer_is_clamped_to_the_batch)
{
    // The cross-chain rail picks the smallest offer that COVERS the request, so
    // the offer is routinely bigger than what staking paid. Taking it whole
    // would sell coins that were never rewards.
    BOOST_CHECK_EQUAL(RewardSliceForWholeHtlc(5000, 1000), 1000);
    BOOST_CHECK_EQUAL(RewardSliceForWholeHtlc(600, 1000), 600);
    BOOST_CHECK_EQUAL(RewardSliceForWholeHtlc(1000, 1000), 1000);
    // Nothing to trade is NOT "take everything".
    BOOST_CHECK_EQUAL(RewardSliceForWholeHtlc(0, 1000), 0);
    BOOST_CHECK_EQUAL(RewardSliceForWholeHtlc(5000, 0), 0);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
