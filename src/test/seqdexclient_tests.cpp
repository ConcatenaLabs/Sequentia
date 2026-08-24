// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: parsing a SeqDEX relay order book, and walking it.
//
// The fixture below is a REAL response, captured verbatim from the public
// testnet relay. That matters more than a hand-written one would: the field
// names, the decimal-string amounts and -- above all -- the BYTE ORDER of the
// covenant asset ids are exactly the things a hand-written fixture would get
// wrong in the same way the parser did, and so agree with it.

#include <seqdexclient.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(seqdexclient_tests, BasicTestingSetup)

namespace {
//! Two offers from https://sequentiatestnet.com/seqob, untouched.
const char* kBook = R"BOOK({"pair":{"base_asset":"e39685e718516156679088d9400d11a1eb82bf7cc27c5b9f5a614b8c91246d13","quote_asset":"57dfa6b0eff594cc3ef1de5555e0526d1eb5590289e014e7663b292edcd63f48","confidential":false},"offers":[{"offer_id":"93810770848a24ed2777e723246490b6","schema_version":1,"pair":{"base_asset":"e39685e718516156679088d9400d11a1eb82bf7cc27c5b9f5a614b8c91246d13","quote_asset":"57dfa6b0eff594cc3ef1de5555e0526d1eb5590289e014e7663b292edcd63f48","confidential":false},"trade_dir":"TRADE_DIR_SELL","base_amount":"3209855123","offer_amount":"3209855123","offer_asset":"e39685e718516156679088d9400d11a1eb82bf7cc27c5b9f5a614b8c91246d13","want_amount":"65309937","want_asset":"57dfa6b0eff594cc3ef1de5555e0526d1eb5590289e014e7663b292edcd63f48","allow_partial":true,"min_fill":"0","created_at_unix":"1787404144","expires_at_unix":"1788005344","maker_pubkey":"032c2662dd2b05e030d66175ff5caa523858cd7a0c818ed1c688180c0eb91ae9b8","fee_asset_hint":"57dfa6b0eff594cc3ef1de5555e0526d1eb5590289e014e7663b292edcd63f48","min_anchor_depth":0,"maker_ln_node_pubkey":"","ln_connect_hints":[],"time_in_force":"TIME_IN_FORCE_UNSPECIFIED","confidential":false,"covenant":{"covenant_txid":"cafa56d04a06f9d51f8aafb756a2bdb9421e05f448612a4790e877b8c350abbe","covenant_vout":2,"asset_a":"136d24918c4b615a9f5b7cc27cbf82eba1110d40d988906756615118e78596e3","asset_b":"483fd6dc2e293b66e714e0890259b51e6d52e05555def13ecc94f5efb0a6df57","rate_num":"65309937","rate_den":"3209855123","maker_prog":"Bh3AimkLt84D3Opw7x0fxSFEusvnOrssGRPFSXo8Qaw=","maker_prog_ver":1,"min_lot":"3209855","expiry_locktime":106273,"maker_x":"Bh3AimkLt84D3Opw7x0fxSFEusvnOrssGRPFSXo8Qaw=","internal_key":"UJKbdMGgSVS3i0tgNel6XgeKWg8o7JbVR7/ums6AOsA=","merkle_path":["vQSok9D/FOPTUYMUN+DB4qfaiBxynLdwIn6rPreuz9o="]},"maker_sig":"MEQCIHYYf3PoaIrDPlBiQgNO1Vdb+kYcyuCkuEiwNZzO2hePAiAG++w7+0t3tWXYqTygdQWKyzELtxW+YBLODRM/snqn6g=="},{"offer_id":"36c47b7a5a0180be712d6cd40fece801","schema_version":1,"pair":{"base_asset":"e39685e718516156679088d9400d11a1eb82bf7cc27c5b9f5a614b8c91246d13","quote_asset":"57dfa6b0eff594cc3ef1de5555e0526d1eb5590289e014e7663b292edcd63f48","confidential":false},"trade_dir":"TRADE_DIR_SELL","base_amount":"8991265628","offer_amount":"8991265628","offer_asset":"e39685e718516156679088d9400d11a1eb82bf7cc27c5b9f5a614b8c91246d13","want_amount":"181669783","want_asset":"57dfa6b0eff594cc3ef1de5555e0526d1eb5590289e014e7663b292edcd63f48","allow_partial":true,"min_fill":"0","created_at_unix":"1787512868","expires_at_unix":"1788114068","maker_pubkey":"032c2662dd2b05e030d66175ff5caa523858cd7a0c818ed1c688180c0eb91ae9b8","fee_asset_hint":"57dfa6b0eff594cc3ef1de5555e0526d1eb5590289e014e7663b292edcd63f48","min_anchor_depth":0,"maker_ln_node_pubkey":"","ln_connect_hints":[],"time_in_force":"TIME_IN_FORCE_UNSPECIFIED","confidential":false,"covenant":{"covenant_txid":"a5b60dde2ff572aa80a224dc74880a57258f7397d1a71cb1e7745046ef5fc70b","covenant_vout":1,"asset_a":"136d24918c4b615a9f5b7cc27cbf82eba1110d40d988906756615118e78596e3","asset_b":"483fd6dc2e293b66e714e0890259b51e6d52e05555def13ecc94f5efb0a6df57","rate_num":"181669783","rate_den":"8991265628","maker_prog":"Bh3AimkLt84D3Opw7x0fxSFEusvnOrssGRPFSXo8Qaw=","maker_prog_ver":1,"min_lot":"8991265","expiry_locktime":146157,"maker_x":"Bh3AimkLt84D3Opw7x0fxSFEusvnOrssGRPFSXo8Qaw=","internal_key":"UJKbdMGgSVS3i0tgNel6XgeKWg8o7JbVR7/ums6AOsA=","merkle_path":["J6vKgefGJG0VVxe/WyXrSJQPiPz2AUK52ueMEqgZI+c="]},"maker_sig":"MEQCIHc52dsIM8un0A5MWGAhpA2lAblnKx5rLxeDC4aL986RAiA6tPMYBIegIB6c0F120SkgLqmjN8Gw7aPnvpduW70lNQ=="}]})BOOK";

CAsset AssetDisplay(const std::string& hex) { return CAsset(uint256S(hex)); }
} // namespace

BOOST_AUTO_TEST_CASE(a_real_orderbook_parses)
{
    UniValue j;
    BOOST_REQUIRE(j.read(kBook));
    const auto offers = SeqobParseBook(j);
    BOOST_REQUIRE_EQUAL(offers.size(), 2U);

    const SeqobOffer& o = offers[0];
    BOOST_CHECK_EQUAL(o.offer_id, "93810770848a24ed2777e723246490b6");
    // Amounts cross the wire as decimal STRINGS; a parser that read them as
    // JSON numbers would lose the low bits of anything large.
    BOOST_CHECK_EQUAL(o.offer_amount, 3209855123);
    BOOST_CHECK_EQUAL(o.want_amount, 65309937);
    BOOST_CHECK(o.offer_asset == AssetDisplay("e39685e718516156679088d9400d11a1eb82bf7cc27c5b9f5a614b8c91246d13"));
    BOOST_CHECK(o.want_asset == AssetDisplay("57dfa6b0eff594cc3ef1de5555e0526d1eb5590289e014e7663b292edcd63f48"));
}

BOOST_AUTO_TEST_CASE(a_covenants_asset_ids_are_internal_byte_order)
{
    // THE trap. The offer says asset "e396...6d13"; its covenant says
    // "136d...96e3" -- the same id written the other way round. Parse the
    // covenant's the display way and it reverses, every covenant then fails its
    // own asset check, and the whole passive book reads as empty. Which is
    // indistinguishable from "nobody is making a market".
    UniValue j;
    BOOST_REQUIRE(j.read(kBook));
    const auto offers = SeqobParseBook(j);
    BOOST_REQUIRE(offers[0].covenant.has_value());
    const SeqobCovenant& c = *offers[0].covenant;
    BOOST_CHECK(c.asset_a == offers[0].offer_asset);
    BOOST_CHECK(c.asset_b == offers[0].want_asset);
    BOOST_CHECK_EQUAL(c.min_lot, 3209855);
    BOOST_CHECK_EQUAL(c.rate_num, 65309937);
    BOOST_CHECK_EQUAL(c.rate_den, 3209855123);
    // Covenant `bytes` fields arrive base64, not hex.
    BOOST_CHECK_EQUAL(c.maker_prog.size(), 32U);
    BOOST_CHECK_EQUAL(c.internal_key.size(), 32U);
}

BOOST_AUTO_TEST_CASE(walking_the_book_prices_a_sale)
{
    UniValue j;
    BOOST_REQUIRE(j.read(kBook));
    const auto offers = SeqobParseBook(j);
    const CAsset sell = offers[0].want_asset;    // we pay what the maker wants
    const CAsset want = offers[0].offer_asset;   // and receive what it offers

    const auto w = SeqobWalkBook(offers, sell, want, offers[0].want_amount);
    BOOST_REQUIRE(w.has_value());
    BOOST_CHECK_GT(w->receives, 0);
    BOOST_CHECK(!w->legs.empty());
    // The reference is priced at the BEST offer, so a fill that only touches
    // that offer can never beat it.
    BOOST_CHECK_LE(w->receives, w->reference);
}

BOOST_AUTO_TEST_CASE(a_pair_with_no_market_is_nullopt_not_an_error)
{
    UniValue j;
    BOOST_REQUIRE(j.read(kBook));
    const auto offers = SeqobParseBook(j);
    const CAsset nothing = AssetDisplay(std::string(64, 'a'));
    BOOST_CHECK(!SeqobWalkBook(offers, nothing, offers[0].offer_asset, 1000).has_value());
    // ...and neither is asking for more depth than rests.
    BOOST_CHECK(!SeqobWalkBook(offers, offers[0].want_asset, offers[0].offer_asset,
                               offers[0].want_amount * 1000).has_value());
    // Nor is a nonsensical size.
    BOOST_CHECK(!SeqobWalkBook(offers, offers[0].want_asset, offers[0].offer_asset, 0).has_value());
}

BOOST_AUTO_TEST_CASE(the_covenant_price_rounds_the_way_the_script_does)
{
    // ceil(filled*num/den). Rounding the other way builds a transaction the
    // covenant rejects -- a failure with no error message worth reading.
    BOOST_CHECK_EQUAL(SeqobCovenantPrice(100, 1, 3), 34);     // 33.33 -> 34
    BOOST_CHECK_EQUAL(SeqobCovenantPrice(300, 1, 3), 100);    // exact
    BOOST_CHECK_EQUAL(SeqobCovenantPrice(1, 1, 1), 1);
    BOOST_CHECK_EQUAL(SeqobCovenantPrice(0, 1, 1), 0);
    // A rate that would overflow is refused, not silently mispriced.
    BOOST_CHECK_EQUAL(SeqobCovenantPrice(4611686018427387904LL, 1000, 1), 0);
}

BOOST_AUTO_TEST_SUITE_END()
