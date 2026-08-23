// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: rebuilding a SeqDEX covenant's FILL leaf, from the node.
//
// The test that matters is the last one. Its terms are a REAL resting offer
// from the public testnet relay, and its expected answer is the scriptPubKey of
// the output that offer is actually funded by, read from the chain. If this
// node's leaf builder is off by a single byte -- an opcode, a push encoding, an
// asset id written the wrong way round -- the Taproot output key comes out
// different and the check fails. Nothing else about a covenant fill is worth
// testing until that holds, because everything else is downstream of it.

#include <wallet/covenantfill.h>

#include <core_io.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(covenantfill_tests, BasicTestingSetup)

namespace {
//! A real resting offer, https://sequentiatestnet.com/seqob, GOLD/USDX.
//! The asset ids are INTERNAL byte order, exactly as the relay serves them and
//! the leaf bakes them.
SeqobCovenant RealCovenant()
{
    SeqobCovenant c;
    c.txid = uint256S("cafa56d04a06f9d51f8aafb756a2bdb9421e05f448612a4790e877b8c350abbe");
    c.vout = 2;
    c.asset_a = CAsset(uint256(ParseHex("136d24918c4b615a9f5b7cc27cbf82eba1110d40d988906756615118e78596e3")));
    c.asset_b = CAsset(uint256(ParseHex("483fd6dc2e293b66e714e0890259b51e6d52e05555def13ecc94f5efb0a6df57")));
    c.rate_num = 65309937;
    c.rate_den = 3209855123;
    c.min_lot = 3209855;
    c.maker_prog = ParseHex("061dc08a690bb7ce03dcea70ef1d1fc52144bacbe73abb2c1913c5497a3c41ac");
    c.maker_x = ParseHex("061dc08a690bb7ce03dcea70ef1d1fc52144bacbe73abb2c1913c5497a3c41ac");
    c.internal_key = ParseHex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0");
    c.merkle_path = {ParseHex("bd04a893d0ff14e3d351831437e0c1e2a7da881c729cb770227eab3eb7aecfda")};
    c.expiry_locktime = 106273;
    return c;
}

//! What that covenant is actually funded by, read from the chain.
const char* kOnChainSpk = "5120f3c06eb3cc22bcbfe69b31880ade88f082ff755b590183e477c0b8faf7292df6";
const CAmount kLocked = 3209855123;
} // namespace

BOOST_AUTO_TEST_CASE(the_fill_leaf_is_a_script_not_a_signature)
{
    const SeqobCovenant c = RealCovenant();
    const CScript leaf = BuildSeqobFillLeaf(c.asset_a, c.asset_b, c.rate_num, c.rate_den,
                                            c.min_lot, c.maker_prog);
    BOOST_CHECK(!leaf.empty());
    // It spends nothing and signs nothing: no CHECKSIG anywhere. That is what
    // lets this node take a resting offer with the maker offline, and why the
    // witness is just the leaf and its control block.
    for (auto it = leaf.begin(); it != leaf.end();) {
        opcodetype op;
        std::vector<unsigned char> data;
        BOOST_REQUIRE(leaf.GetOp(it, op, data));
        BOOST_CHECK(op != OP_CHECKSIG);
        BOOST_CHECK(op != OP_CHECKSIGVERIFY);
    }
}

BOOST_AUTO_TEST_CASE(the_control_block_carries_version_key_and_path)
{
    const SeqobCovenant c = RealCovenant();
    const auto cb = BuildSeqobControlBlock(c.internal_key, c.merkle_path, /*parity=*/false);
    BOOST_CHECK_EQUAL(cb.size(), 33U + 32U);            // header + key + one sibling
    BOOST_CHECK_EQUAL(cb[0], SEQOB_LEAF_VERSION);
    const auto with_parity = BuildSeqobControlBlock(c.internal_key, c.merkle_path, /*parity=*/true);
    BOOST_CHECK_EQUAL(with_parity[0], SEQOB_LEAF_VERSION | 1);
}

BOOST_AUTO_TEST_CASE(a_fill_must_clear_the_minimum_lot_on_both_sides)
{
    const SeqobCovenant c = RealCovenant();
    // A full fill is fine.
    const auto full = PlanSeqobFill(c, kLocked, kLocked);
    BOOST_REQUIRE(full.has_value());
    BOOST_CHECK(!full->partial);
    BOOST_CHECK_EQUAL(full->remainder, 0);
    BOOST_CHECK_EQUAL(full->credit, SeqobCovenantPrice(kLocked, c.rate_num, c.rate_den));

    // So is a partial that leaves a healthy remainder.
    const auto half = PlanSeqobFill(c, kLocked, kLocked / 2);
    BOOST_REQUIRE(half.has_value());
    BOOST_CHECK(half->partial);
    BOOST_CHECK_EQUAL(half->filled + half->remainder, kLocked);

    // A fill below the minimum lot is not a fill.
    BOOST_CHECK(!PlanSeqobFill(c, kLocked, c.min_lot - 1).has_value());
    // Nor is one that would strand the REMAINDER below it: the leaf floors both
    // sides, so this transaction would simply be rejected.
    BOOST_CHECK(!PlanSeqobFill(c, kLocked, kLocked - (c.min_lot - 1)).has_value());
    // Nor is taking more than is there.
    BOOST_CHECK(!PlanSeqobFill(c, kLocked, kLocked + 1).has_value());
}

BOOST_AUTO_TEST_CASE(the_rebuilt_covenant_is_the_one_on_chain)
{
    // THE test. Rebuild the maker's committed terms and derive the Taproot
    // output they must produce; it has to be the output the offer is really
    // funded by. A wrong opcode, a wrong push encoding, or an asset id written
    // the wrong way round all fail here and nowhere else -- and would otherwise
    // fail as a rejected transaction with nothing useful to read.
    const SeqobCovenant c = RealCovenant();
    const auto scripts = BuildSeqobFillScripts(c);
    BOOST_REQUIRE(scripts.has_value());
    BOOST_CHECK_EQUAL(HexStr(scripts->spk), kOnChainSpk);
    BOOST_CHECK_EQUAL(scripts->control_block.size(), 65U);
    BOOST_CHECK(!scripts->fill_leaf.empty());
}

BOOST_AUTO_TEST_CASE(nonsense_terms_are_refused_rather_than_guessed)
{
    SeqobCovenant c = RealCovenant();
    c.internal_key.clear();
    BOOST_CHECK(!BuildSeqobFillScripts(c).has_value());

    c = RealCovenant();
    c.rate_den = 0;
    BOOST_CHECK(!BuildSeqobFillScripts(c).has_value());

    c = RealCovenant();
    c.merkle_path.push_back(ParseHex("00"));   // not a 32-byte hash
    BOOST_CHECK(!BuildSeqobFillScripts(c).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
