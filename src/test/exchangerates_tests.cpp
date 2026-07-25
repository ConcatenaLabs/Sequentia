// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <exchangerates.h>
#include <policy/policy.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>

/**
 * SEQUENTIA: the reference unit is an abstract factor and is never itself a
 * token. There is no mode in which an asset IS the reference unit, so no asset
 * is valued without being listed in the whitelist, the policy asset included.
 * What bootstraps a never configured node is the SEED that the map is
 * constructed with, not a special case in the converters.
 */
namespace {
//! Two distinct, non-null assets so "policy asset" and "some other asset" are
//! genuinely different keys.
const CAsset ASSET_POLICY{uint256S("01")};
const CAsset ASSET_OTHER{uint256S("02")};

//! The whitelist is a process-wide singleton, so leave it and ::policyAsset as
//! we found them for whatever test runs next in this binary.
struct ExchangeRatesTestingSetup : public BasicTestingSetup {
    CAsset m_saved_policy_asset{::policyAsset};
    ~ExchangeRatesTestingSetup()
    {
        ::policyAsset = m_saved_policy_asset;
        ExchangeRateMap::GetInstance().ResetToBootstrapRates();
    }
};

//! Put the singleton back into the state a freshly started node has, for the
//! ::policyAsset in force right now. Set ::policyAsset before calling this.
ExchangeRateMap& FreshMap()
{
    ExchangeRateMap& rates = ExchangeRateMap::GetInstance();
    rates.ResetToBootstrapRates();
    return rates;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(exchangerates_tests, ExchangeRatesTestingSetup)

//! The seed, not a special case, is what lets a fresh node accept policy-asset
//! fees out of the box: constructing/resetting the map lists the policy asset at
//! exchange_rate_scale, and it converts 1:1 both ways.
BOOST_AUTO_TEST_CASE(bootstrap_seed_accepts_the_policy_asset)
{
    ::policyAsset = ASSET_POLICY;
    ExchangeRateMap& rates = FreshMap();

    BOOST_CHECK_EQUAL(rates.size(), 1U);
    BOOST_CHECK(rates.count(::policyAsset) == 1);
    BOOST_CHECK_EQUAL(rates.at(::policyAsset).m_scaled_value, exchange_rate_scale);

    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 1000);
    BOOST_CHECK_EQUAL(rates.ConvertValueToAmount(CValue(1000), ::policyAsset), 1000);

    // The seed lists ONLY the policy asset; every other asset starts unlisted.
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ASSET_OTHER).GetValue(), 0);
    BOOST_CHECK_EQUAL(rates.ConvertValueToAmount(CValue(1000), ASSET_OTHER), 0);
}

//! An unlisted policy asset is unlisted like any other asset: no implicit 1:1.
//! Replacing the whitelist replaces the seed too, so a producer whose policy
//! omits the policy asset refuses fees paid in it.
BOOST_AUTO_TEST_CASE(unlisted_policy_asset_values_to_zero)
{
    ::policyAsset = ASSET_POLICY;
    ExchangeRateMap& rates = FreshMap();

    rates.SetRates({{ASSET_OTHER, exchange_rate_scale}});

    BOOST_CHECK(rates.count(::policyAsset) == 0);
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 0);
    BOOST_CHECK_EQUAL(rates.ConvertValueToAmount(CValue(1000), ::policyAsset), 0);

    // The listed asset is unaffected: this is about being listed, not about
    // which asset it is.
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ASSET_OTHER).GetValue(), 1000);

    // An empty whitelist accepts nothing at all, policy asset included.
    rates.SetRates({});
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 0);
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ASSET_OTHER).GetValue(), 0);
}

//! An explicit rate for the policy asset overrides the seeded one, in both
//! directions, and is treated exactly like any other asset's rate.
BOOST_AUTO_TEST_CASE(explicit_rate_overrides_the_seed)
{
    ::policyAsset = ASSET_POLICY;
    ExchangeRateMap& rates = FreshMap();

    // Half the reference value per atom.
    rates.SetRates({{::policyAsset, exchange_rate_scale / 2}});
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 500);
    BOOST_CHECK_EQUAL(rates.ConvertValueToAmount(CValue(500), ::policyAsset), 1000);

    // Twice the reference value per atom.
    rates.SetRates({{::policyAsset, 2 * exchange_rate_scale}});
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 2000);
    BOOST_CHECK_EQUAL(rates.ConvertValueToAmount(CValue(2000), ::policyAsset), 1000);
}

//! A rate of 0 is an explicit refusal and holds for the policy asset too: it is
//! how a producer declines an asset on the record rather than by omission.
BOOST_AUTO_TEST_CASE(explicit_zero_rate_refuses)
{
    ::policyAsset = ASSET_POLICY;
    ExchangeRateMap& rates = FreshMap();

    rates.SetRates({{::policyAsset, 0}, {ASSET_OTHER, 0}});
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 0);
    BOOST_CHECK_EQUAL(rates.ConvertValueToAmount(CValue(1000), ::policyAsset), 0);
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ASSET_OTHER).GetValue(), 0);
    BOOST_CHECK_EQUAL(rates.ConvertValueToAmount(CValue(1000), ASSET_OTHER), 0);
}

//! getfeeacceptancepolicy reports exactly the accept set. With no implicit
//! default left there is nothing to materialise, so an omitted policy asset is
//! genuinely absent and a refused one is present with rate 0.
BOOST_AUTO_TEST_CASE(acceptance_policy_reports_exactly_the_accept_set)
{
    ::policyAsset = ASSET_POLICY;
    ExchangeRateMap& rates = FreshMap();

    // Out of the box: the seeded policy asset and nothing else.
    UniValue seeded = rates.AcceptancePolicyToJSON();
    BOOST_CHECK_EQUAL(seeded.size(), 1U);
    BOOST_CHECK(seeded.exists(::policyAsset.GetHex()));
    BOOST_CHECK_EQUAL(seeded[::policyAsset.GetHex()]["rate"].get_int64(), exchange_rate_scale);

    // Omitted: absent from the report, and refused by the converter. The two now
    // agree, where before the converter accepted what the report did not list.
    rates.SetRates({{ASSET_OTHER, exchange_rate_scale}});
    UniValue omitted = rates.AcceptancePolicyToJSON();
    BOOST_CHECK_EQUAL(omitted.size(), 1U);
    BOOST_CHECK(!omitted.exists(::policyAsset.GetHex()));
    BOOST_CHECK(omitted.exists(ASSET_OTHER.GetHex()));
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 0);

    // Explicitly refused: listed at 0, which is the same acceptance outcome
    // stated on the record.
    rates.SetRates({{::policyAsset, 0}});
    UniValue refused = rates.AcceptancePolicyToJSON();
    BOOST_CHECK_EQUAL(refused.size(), 1U);
    BOOST_CHECK_EQUAL(refused[::policyAsset.GetHex()]["rate"].get_int64(), 0);
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 0);
}

//! ClearRates empties the whitelist outright. It leaves no residual policy-asset
//! entry, and ResetToBootstrapRates is what gets back to the out-of-box state.
BOOST_AUTO_TEST_CASE(clear_rates_leaves_nothing_accepted)
{
    ::policyAsset = ASSET_POLICY;
    ExchangeRateMap& rates = FreshMap();

    rates.ClearRates();
    BOOST_CHECK_EQUAL(rates.size(), 0U);
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 0);
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ASSET_OTHER).GetValue(), 0);

    rates.ResetToBootstrapRates();
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ::policyAsset).GetValue(), 1000);
}

//! The bootstrap seed follows ::policyAsset, whatever -feeasset selected: it is
//! "the fee asset this node was configured with", not a hard-coded token.
BOOST_AUTO_TEST_CASE(bootstrap_seed_follows_the_configured_fee_asset)
{
    ::policyAsset = ASSET_OTHER;
    ExchangeRateMap& rates = FreshMap();

    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ASSET_OTHER).GetValue(), 1000);
    BOOST_CHECK_EQUAL(rates.ConvertAmountToValue(1000, ASSET_POLICY).GetValue(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
