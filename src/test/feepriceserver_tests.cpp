// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <asset.h>
#include <chainparamsbase.h>
#include <feepriceserver.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <string>

BOOST_FIXTURE_TEST_SUITE(feepriceserver_tests, BasicTestingSetup)

static const std::string ID_A(64, 'a');
static const std::string ID_B = std::string(63, '0') + "b";

static bool Parse(const std::string& body, std::string& error)
{
    std::map<CAsset, CAmount> rates;
    int64_t published{0};
    return ParsePriceServerFeeList(body, rates, published, error);
}

//! A price server's published list is taken as it stands.
BOOST_AUTO_TEST_CASE(a_published_list_is_taken)
{
    std::map<CAsset, CAmount> rates;
    int64_t published{0};
    std::string error;
    const std::string body = "{\"updated\": 1794735668.66, \"quote_currency\": \"USD\", \"rates\": {\"" + ID_A +
                             "\": 17527902, \"" + ID_B + "\": 100000000}, \"decisions\": []}";
    BOOST_REQUIRE_MESSAGE(ParsePriceServerFeeList(body, rates, published, error), error);
    BOOST_CHECK_EQUAL(rates.size(), 2U);
    BOOST_CHECK_EQUAL(rates[CAsset(uint256S(ID_A))], 17527902);
    BOOST_CHECK_EQUAL(rates[CAsset(uint256S(ID_B))], 100000000);
    BOOST_CHECK_EQUAL(published, 1794735668);
}

//! Nothing goes from a price feed API to the whitelist: a feed's answer, or the
//! registry's index, is not a price server's list.
BOOST_AUTO_TEST_CASE(a_price_feed_is_not_a_price_server)
{
    std::string error;
    BOOST_CHECK(!Parse("{\"SEQ\": {\"price\": 0.17, \"market_cap\": 4.8e9, \"volume_24h\": 2.5e8}, "
                       "\"USDX\": {\"price\": 1.0}, \"_meta\": {\"updated\": 1794735668}}", error));
    BOOST_CHECK(error.find("not a price server") != std::string::npos);
    BOOST_CHECK(!Parse("{\"" + ID_A + "\": [\"sequentia.io\", \"tSEQ\", \"Sequence\", 8, 1, 0]}", error));
    BOOST_CHECK(error.find("not a price server") != std::string::npos);
    // A price server whose first poll has not completed: a price server, not yet publishing.
    BOOST_CHECK(!Parse("{\"updated\": null, \"rates\": {}, \"decisions\": []}", error));
    BOOST_CHECK(error.find("has not published a list yet") != std::string::npos);
    // A feed that happens to have a "rates" key still lacks the publication time.
    BOOST_CHECK(!Parse("{\"rates\": {\"" + ID_A + "\": 1}}", error));
    BOOST_CHECK(error.find("not a price server") != std::string::npos);
}

//! A list is taken whole or not at all.
BOOST_AUTO_TEST_CASE(a_list_is_refused_whole)
{
    std::string error;
    BOOST_CHECK(!Parse("not json", error));
    BOOST_CHECK(!Parse("[]", error));
    BOOST_CHECK(!Parse("{\"updated\": 1, \"rates\": {}}", error));
    BOOST_CHECK(error.find("empty") != std::string::npos);
    BOOST_CHECK(!Parse("{\"updated\": 1, \"rates\": {\"tSEQ\": 100000000}}", error));
    BOOST_CHECK(error.find("64-hex") != std::string::npos);
    BOOST_CHECK(!Parse("{\"updated\": 1, \"rates\": {\"" + std::string(64, '0') + "\": 1}}", error));
    for (const std::string& rate : {"0", "-5", "1.5", "1000000000000000001", "\"100\"", "1e30"}) {
        BOOST_CHECK_MESSAGE(!Parse("{\"updated\": 1, \"rates\": {\"" + ID_A + "\": " + rate + ", \"" + ID_B + "\": 5}}", error), rate);
    }
    BOOST_CHECK(Parse("{\"updated\": 1, \"rates\": {\"" + ID_A + "\": 1000000000000000000}}", error));
    std::string many = "{\"updated\": 1, \"rates\": {";
    for (int i = 1; i <= 1001; ++i) many += strprintf("%s\"%064x\": 1", i > 1 ? "," : "", i);
    many += "}}";
    BOOST_CHECK(!Parse(many, error));
    BOOST_CHECK(error.find("1001 entries") != std::string::npos);
}

//! The testnet's default is the network's price server; the mainnet has none;
//! a custom chain has one only when it names it.
BOOST_AUTO_TEST_CASE(the_default_price_server_by_chain)
{
    {
        ArgsManager args;
        args.ForceSetArg("-chain", CBaseChainParams::TESTNET);
        BOOST_CHECK_EQUAL(DefaultFeePriceServer(args), "http://sequentiatestnet.com/feewhitelist");
    }
    for (const std::string& chain : {CBaseChainParams::SEQUENTIA, CBaseChainParams::MAIN, CBaseChainParams::REGTEST}) {
        ArgsManager args;
        args.ForceSetArg("-chain", chain);
        args.ForceSetArg("-con_default_fee_price_server", "http://127.0.0.1:1/x");
        BOOST_CHECK_MESSAGE(DefaultFeePriceServer(args).empty(), chain);
    }
    {
        ArgsManager args;
        args.ForceSetArg("-chain", "elementsregtest");
        BOOST_CHECK(DefaultFeePriceServer(args).empty());
        args.ForceSetArg("-con_default_fee_price_server", "http://127.0.0.1:1/x");
        BOOST_CHECK_EQUAL(DefaultFeePriceServer(args), "http://127.0.0.1:1/x");
    }
}

BOOST_AUTO_TEST_SUITE_END()
