// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: the hashed timelock both legs of a cross-chain swap are built on.
//
// A swap where the two sides disagree about a single push encoding is a swap
// where one side's money sits in a script the other cannot spend. So the shape
// is pinned to the byte, against the same vector SWK's `seqdex_htlc` builds --
// not against whatever this file happens to produce.

#include <wallet/htlc.h>
#include <wallet/xchainconvert.h>
#include <crypto/sha256.h>
#include <key.h>
#include <script/standard.h>

#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(htlc_tests, BasicTestingSetup)

namespace {
const char* kHash = "0101010101010101010101010101010101010101010101010101010101010101";
// Two real compressed points: the generator, and 2G.
const char* kClaim = "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
const char* kRefund = "02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";
} // namespace

BOOST_AUTO_TEST_CASE(the_redeem_script_is_the_agreed_bytes)
{
    const auto s = BuildHtlcRedeemScript(ParseHex(kHash), ParseHex(kClaim), ParseHex(kRefund), 700000);
    BOOST_REQUIRE(s.has_value());

    // OP_IF OP_SIZE <32> OP_EQUALVERIFY OP_SHA256 <H> OP_EQUALVERIFY <claim>
    // OP_CHECKSIG OP_ELSE <700000> OP_CLTV OP_DROP <refund> OP_CHECKSIG OP_ENDIF
    const std::string expected =
        "63"                                     // OP_IF
        "82" "0120" "88"                         // OP_SIZE <32> OP_EQUALVERIFY
        "a8" "20" + std::string(kHash) + "88"    // OP_SHA256 <H> OP_EQUALVERIFY
        + "21" + std::string(kClaim) + "ac"      // <claim> OP_CHECKSIG
        + "67"                                   // OP_ELSE
        "03" "60ae0a"                            // <700000>, minimal little-endian
        "b1" "75"                                // OP_CLTV OP_DROP
        + "21" + std::string(kRefund) + "ac"     // <refund> OP_CHECKSIG
        + "68";                                  // OP_ENDIF
    BOOST_CHECK_EQUAL(HexStr(*s), expected);
}

BOOST_AUTO_TEST_CASE(the_size_guard_is_present_and_is_not_decoration)
{
    // Without OP_SIZE <32> a preimage of some other length can satisfy the hash
    // on one chain and not the other, and the atomicity quietly stops being
    // atomic. If this ever disappears the swap is unsafe, not merely different.
    const auto s = BuildHtlcRedeemScript(ParseHex(kHash), ParseHex(kClaim), ParseHex(kRefund), 1);
    BOOST_REQUIRE(s.has_value());
    const std::string hex = HexStr(*s);
    BOOST_CHECK(hex.find("82012088") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(the_terms_read_back_out)
{
    for (uint32_t locktime : {1u, 16u, 17u, 127u, 128u, 700000u, 500000000u}) {
        const auto s = BuildHtlcRedeemScript(ParseHex(kHash), ParseHex(kClaim), ParseHex(kRefund), locktime);
        BOOST_REQUIRE(s.has_value());
        const auto t = ParseHtlcRedeemScript(*s);
        BOOST_REQUIRE_MESSAGE(t.has_value(), "locktime " << locktime << " did not round-trip");
        BOOST_CHECK_EQUAL(HexStr(t->hash), kHash);
        BOOST_CHECK_EQUAL(HexStr(t->claim_pub), kClaim);
        BOOST_CHECK_EQUAL(HexStr(t->refund_pub), kRefund);
        BOOST_CHECK_EQUAL(t->locktime, locktime);
    }
}

BOOST_AUTO_TEST_CASE(nonsense_terms_are_refused)
{
    // A hash of the wrong length, or a key that is not a point, is a script
    // nobody can spend from -- worth refusing before it is funded, not after.
    BOOST_CHECK(!BuildHtlcRedeemScript(ParseHex("0101"), ParseHex(kClaim), ParseHex(kRefund), 1).has_value());
    BOOST_CHECK(!BuildHtlcRedeemScript(ParseHex(kHash), ParseHex("02"), ParseHex(kRefund), 1).has_value());
    const std::string not_a_point = "02" + std::string(64, 'f');
    BOOST_CHECK(!BuildHtlcRedeemScript(ParseHex(kHash), ParseHex(not_a_point), ParseHex(kRefund), 1).has_value());
    // An uncompressed key is the right length for nothing here.
    const std::string uncompressed = "04" + std::string(128, '0');
    BOOST_CHECK(!BuildHtlcRedeemScript(ParseHex(kHash), ParseHex(uncompressed), ParseHex(kRefund), 1).has_value());
}

BOOST_AUTO_TEST_CASE(something_that_is_not_an_htlc_does_not_parse_as_one)
{
    BOOST_CHECK(!ParseHtlcRedeemScript(CScript()).has_value());
    BOOST_CHECK(!ParseHtlcRedeemScript(CScript() << OP_TRUE).has_value());
    // A script of the right length but the wrong shape.
    CScript wrong;
    for (int i = 0; i < 16; ++i) wrong << OP_NOP;
    BOOST_CHECK(!ParseHtlcRedeemScript(wrong).has_value());
}

BOOST_AUTO_TEST_CASE(both_chains_pay_the_same_script_their_own_way)
{
    const auto s = BuildHtlcRedeemScript(ParseHex(kHash), ParseHex(kClaim), ParseHex(kRefund), 700000);
    BOOST_REQUIRE(s.has_value());
    // Sequentia's leg is P2SH, Bitcoin's is P2WSH: the same terms, committed to
    // differently, which is why the two hexes must not be interchangeable.
    const CScript p2sh = HtlcP2shSpk(*s);
    const CScript p2wsh = HtlcP2wshSpk(*s);
    BOOST_CHECK_EQUAL(p2sh.size(), 23U);     // OP_HASH160 <20> OP_EQUAL
    BOOST_CHECK_EQUAL(p2wsh.size(), 34U);    // OP_0 <32>
    BOOST_CHECK(HexStr(p2sh) != HexStr(p2wsh));
}

//! The parent-chain JSON-RPC envelope.
//!
//! This is here because getting it wrong is silent. CallMainChainRPC hands back
//! the whole reply; reading "feerate" or "vout" straight off it finds nothing,
//! throws nothing, and leaves the caller with a default. That shipped once: the
//! fee estimate quietly fell back to 2 sat/vB while the parent chain wanted 100,
//! and the maker's Bitcoin lock could not be read at all.
BOOST_AUTO_TEST_CASE(mainchain_payload_unwraps_the_envelope)
{
    UniValue inner(UniValue::VOBJ);
    inner.pushKV("feerate", 0.00100513);

    UniValue reply(UniValue::VOBJ);
    reply.pushKV("result", inner);
    reply.pushKV("error", NullUniValue);
    reply.pushKV("id", 1);

    const UniValue got = MainChainPayload("estimatesmartfee", reply);
    BOOST_CHECK(got.isObject());
    BOOST_CHECK(got.exists("feerate"));

    // The mistake itself: the payload handed in where the envelope belongs. It
    // must not read as an empty-but-fine answer.
    BOOST_CHECK_THROW(MainChainPayload("estimatesmartfee", inner), std::runtime_error);

    // An error in the envelope is an error, not an empty result.
    UniValue bad(UniValue::VOBJ);
    bad.pushKV("result", NullUniValue);
    UniValue e(UniValue::VOBJ);
    e.pushKV("code", -5);
    e.pushKV("message", "No such mempool transaction");
    bad.pushKV("error", e);
    BOOST_CHECK_THROW(MainChainPayload("getrawtransaction", bad), std::runtime_error);
}


//! Every way the maker's Bitcoin lock can fail to be the agreed one.
//!
//! This is the check the staker's money rests on. Everything before it is talk;
//! the next thing that happens once it says yes is that an asset gets locked in
//! a contract. So each refusal here is a way a maker could take an asset and
//! give nothing back, and each one gets a test that watches it refuse -- a
//! fund-safety check nobody has seen say no is a check nobody knows works.
namespace {
struct MakerLegFixture {
    XchainSwap swap;
    CScript script;
    CKey claim_key, refund_key;

    MakerLegFixture()
    {
        claim_key.MakeNewKey(true);
        refund_key.MakeNewKey(true);
        const std::vector<unsigned char> preimage(32, 0x11);
        unsigned char digest[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(preimage.data(), preimage.size()).Finalize(digest);

        swap.hash_h.assign(digest, digest + 32);
        const CPubKey cp = claim_key.GetPubKey();
        const CPubKey rp = refund_key.GetPubKey();
        swap.taker_btc_claim_pub.assign(cp.begin(), cp.end());
        swap.maker_btc_refund_pub.assign(rp.begin(), rp.end());
        swap.btc_locktime = 800000;
        swap.btc_amount = 100000;

        script = *BuildHtlcRedeemScript(swap.hash_h, swap.taker_btc_claim_pub,
                                        swap.maker_btc_refund_pub, swap.btc_locktime);
        swap.btc_leg_script.assign(script.begin(), script.end());
    }

    ParentOut GoodOutput() const
    {
        ParentOut o;
        o.found = true;
        o.spk = HtlcP2wshSpk(script);
        o.value = 100000;
        o.height = 900;
        o.confirmations = 6;
        return o;
    }
};
}  // namespace

BOOST_AUTO_TEST_CASE(maker_btc_leg_accepts_the_agreed_lock)
{
    MakerLegFixture f;
    BOOST_CHECK_EQUAL(CheckMakerBtcLeg(f.swap, f.GoodOutput()), "");

    // More than agreed is fine. A maker that overpays is not a problem.
    ParentOut generous = f.GoodOutput();
    generous.value = 100001;
    BOOST_CHECK_EQUAL(CheckMakerBtcLeg(f.swap, generous), "");
}

BOOST_AUTO_TEST_CASE(maker_btc_leg_refuses_a_script_that_is_not_the_agreed_one)
{
    // Not an HTLC at all.
    {
        MakerLegFixture f;
        const CScript junk = CScript() << OP_TRUE;
        f.swap.btc_leg_script.assign(junk.begin(), junk.end());
        BOOST_CHECK(!CheckMakerBtcLeg(f.swap, f.GoodOutput()).empty());
    }

    // Locks a different secret: we could never claim it, only the maker could.
    {
        MakerLegFixture f;
        auto other = f.swap.hash_h;
        other[0] ^= 0xff;
        const CScript s = *BuildHtlcRedeemScript(other, f.swap.taker_btc_claim_pub,
                                                 f.swap.maker_btc_refund_pub, f.swap.btc_locktime);
        f.swap.btc_leg_script.assign(s.begin(), s.end());
        BOOST_CHECK(!CheckMakerBtcLeg(f.swap, f.GoodOutput()).empty());
    }

    // Claimable by somebody else. The nastiest of the lot: the secret is right,
    // the amount is right, and revealing the secret hands the asset over for a
    // contract that pays a stranger.
    {
        MakerLegFixture f;
        CKey thief;
        thief.MakeNewKey(true);
        const CPubKey tp = thief.GetPubKey();
        const std::vector<unsigned char> theirs(tp.begin(), tp.end());
        const CScript s = *BuildHtlcRedeemScript(f.swap.hash_h, theirs,
                                                 f.swap.maker_btc_refund_pub, f.swap.btc_locktime);
        f.swap.btc_leg_script.assign(s.begin(), s.end());
        BOOST_CHECK(!CheckMakerBtcLeg(f.swap, f.GoodOutput()).empty());
    }

    // A shorter timelock than agreed: the maker could take its Bitcoin back
    // before we had any reason to claim it.
    {
        MakerLegFixture f;
        const CScript s = *BuildHtlcRedeemScript(f.swap.hash_h, f.swap.taker_btc_claim_pub,
                                                 f.swap.maker_btc_refund_pub, 700000);
        f.swap.btc_leg_script.assign(s.begin(), s.end());
        BOOST_CHECK(!CheckMakerBtcLeg(f.swap, f.GoodOutput()).empty());
    }
}

BOOST_AUTO_TEST_CASE(maker_btc_leg_refuses_what_the_chain_does_not_bear_out)
{
    MakerLegFixture f;

    // Unreadable is not "absent, therefore fine". A parent daemon that is
    // momentarily unavailable must never be mistaken for a verified lock.
    {
        ParentOut missing;
        missing.found = false;
        BOOST_CHECK(!CheckMakerBtcLeg(f.swap, missing).empty());
    }

    // The script is the agreed one, but the output does not pay it. This is the
    // check that makes the script check mean anything: a maker can describe a
    // perfect contract and fund something else entirely.
    {
        ParentOut elsewhere = f.GoodOutput();
        CKey other;
        other.MakeNewKey(true);
        elsewhere.spk = GetScriptForDestination(PKHash(other.GetPubKey()));
        BOOST_CHECK(!CheckMakerBtcLeg(f.swap, elsewhere).empty());
    }

    // Underfunded by one satoshi is still underfunded.
    {
        ParentOut short_paid = f.GoodOutput();
        short_paid.value = 99999;
        BOOST_CHECK(!CheckMakerBtcLeg(f.swap, short_paid).empty());
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
