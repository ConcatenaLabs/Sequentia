// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls.h>
#include <chainparams.h>
#include <key.h>
#include <pos.h>
#include <pos_producer.h>
#include <primitives/block.h>
#include <vrf.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pos_gossip_tests, TestingSetup)

// A posshare names its author (pubkey) separately from the BLS key that signed
// it. Anyone holding some BLS key can therefore sign a block and name another
// committee member as the author. Such a forgery must neither shadow the
// member's own share nor cost the receiver a pairing.
BOOST_AUTO_TEST_CASE(forged_share_never_shadows_a_member)
{
    const bool saved_public = g_pos_public_committee;
    g_pos_public_committee = true;
    StakeRegistry& reg = StakeRegistry::GetInstance();
    reg.Clear();

    CKey key_a, key_b;
    key_a.MakeNewKey(true);
    key_b.MakeNewKey(true);
    const std::vector<unsigned char> bls_a(BLS_SK_SIZE, 0x0a), bls_b(BLS_SK_SIZE, 0x0b);
    reg.SetStake(key_a.GetPubKey(), 1);
    reg.SetStake(key_b.GetPubKey(), 1);
    reg.SetBls(key_a.GetPubKey(), *BlsDerivePubKey(bls_a));
    reg.SetBls(key_b.GetPubKey(), *BlsDerivePubKey(bls_b));

    PosProducer producer(*m_node.chainman, *m_node.mempool, Params(), /*connman=*/nullptr, {});
    // A block the node holds as a round candidate: shares for it are judged
    // in full, and relayed when valid.
    auto block = std::make_shared<CBlock>();
    block->nTime = 1;
    producer.AddCandidateForTesting(block, key_a.GetPubKey());
    const uint256 block_hash = block->GetHash();
    const uint256 other_hash = InsecureRand256();
    const auto make_share = [&](const CKey& author, const std::vector<unsigned char>& bls_sk, bool good_sig) {
        PosShare share;
        share.block_hash = block_hash;
        share.pubkey = author.GetPubKey();
        share.vrf_proof.assign(VRF_PROOF_SIZE, 0);
        share.bls_pubkey = *BlsDerivePubKey(bls_sk);
        share.bls_pop = *BlsProvePossession(bls_sk);
        const uint256& signed_hash = good_sig ? block_hash : other_hash;
        share.bls_share = *BlsSign(bls_sk, Span<const unsigned char>(signed_hash.begin(), 32));
        return share;
    };

    // B's genuine signature relabelled as A's: cryptographically sound, but not
    // under A's registered key, so it is dropped and not relayed.
    BOOST_CHECK(producer.OnShare(make_share(key_a, bls_b, /*good_sig=*/true)) == PosGossipAction::Ignore);

    // A share really from A is still judged on its merits after the forgery:
    // with a bad signature it reaches verification and is Invalid. Had the
    // forgery been recorded under (block, A) this would be dropped unseen, and
    // so would A's real share.
    BOOST_CHECK(producer.OnShare(make_share(key_a, bls_a, /*good_sig=*/false)) == PosGossipAction::Invalid);

    // A forgery is rejected before any pairing: even a bad signature under the
    // wrong key is Ignore, not Invalid, because it never reaches verification.
    BOOST_CHECK(producer.OnShare(make_share(key_a, bls_b, /*good_sig=*/false)) == PosGossipAction::Ignore);

    // A member with no registered key cannot sign a certificate at all.
    CKey key_c;
    key_c.MakeNewKey(true);
    reg.SetStake(key_c.GetPubKey(), 1);
    BOOST_CHECK(producer.OnShare(make_share(key_c, bls_b, /*good_sig=*/true)) == PosGossipAction::Ignore);

    // A's genuine share is accepted and relayed.
    BOOST_CHECK(producer.OnShare(make_share(key_a, bls_a, /*good_sig=*/true)) == PosGossipAction::Relay);

    // A share for a block the node knows nothing about is dropped before any
    // pairing, so even a bad signature costs nothing and is not judged.
    PosShare unknown = make_share(key_b, bls_b, /*good_sig=*/false);
    unknown.block_hash = InsecureRand256();
    BOOST_CHECK(producer.OnShare(unknown) == PosGossipAction::Ignore);

    reg.Clear();
    g_pos_public_committee = saved_public;
}

// The leader's staging signature is taken in the one encoding CKey::Sign
// produces. CPubKey::Verify parses DER laxly, so each re-encoding below still
// verifies; only the strict one passes IsStrictLowSEcdsaSignature.
BOOST_AUTO_TEST_CASE(leader_signature_in_its_one_encoding)
{
    static const unsigned char ORDER[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
        0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B, 0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41};
    CKey key;
    key.MakeNewKey(true);
    const CPubKey pub = key.GetPubKey();
    for (int i = 0; i < 16; ++i) {
        const uint256 hash = InsecureRand256();
        std::vector<unsigned char> sig;
        BOOST_REQUIRE(key.Sign(hash, sig));
        BOOST_CHECK(sig.size() <= 72);
        BOOST_CHECK(IsStrictLowSEcdsaSignature(sig));
        const size_t rl = sig[3], sl = sig[5 + rl];
        const std::vector<unsigned char> r(sig.begin() + 4, sig.begin() + 4 + rl), s(sig.begin() + 6 + rl, sig.end());
        BOOST_REQUIRE_EQUAL(s.size(), sl);

        // R padded with zero bytes, indefinite and long-form lengths.
        std::vector<unsigned char> padded{0x30, 0x80, 0x02, 0x83, 0x00, 0x00, 0x00};
        const size_t pad = 256;
        padded[5] = (unsigned char)((pad + rl) >> 8);
        padded[6] = (unsigned char)((pad + rl) & 0xff);
        padded.insert(padded.end(), pad, 0x00);
        padded.insert(padded.end(), r.begin(), r.end());
        padded.push_back(0x02);
        padded.push_back((unsigned char)sl);
        padded.insert(padded.end(), s.begin(), s.end());
        BOOST_CHECK(pub.Verify(hash, padded));
        BOOST_CHECK(!IsStrictLowSEcdsaSignature(padded));

        // A 73-byte encoding: R with leading zeros up to 73 bytes in all.
        std::vector<unsigned char> r2(73 - sig.size(), 0x00);
        r2.insert(r2.end(), r.begin(), r.end());
        std::vector<unsigned char> long_sig{0x30, (unsigned char)(4 + r2.size() + sl), 0x02, (unsigned char)r2.size()};
        long_sig.insert(long_sig.end(), r2.begin(), r2.end());
        long_sig.push_back(0x02);
        long_sig.push_back((unsigned char)sl);
        long_sig.insert(long_sig.end(), s.begin(), s.end());
        BOOST_CHECK_EQUAL(long_sig.size(), 73U);
        BOOST_CHECK(pub.Verify(hash, long_sig));
        BOOST_CHECK(!IsStrictLowSEcdsaSignature(long_sig));

        // The high S: order - S, minimally encoded.
        std::vector<unsigned char> s32(32 - s.size() + (s[0] == 0 ? 1 : 0), 0x00);
        s32.insert(s32.end(), s.begin() + (s[0] == 0 ? 1 : 0), s.end());
        BOOST_REQUIRE_EQUAL(s32.size(), 32U);
        std::vector<unsigned char> hs(32);
        int borrow = 0;
        for (int b = 31; b >= 0; --b) {
            int d = ORDER[b] - s32[b] - borrow;
            borrow = d < 0;
            hs[b] = (unsigned char)(d + (borrow ? 256 : 0));
        }
        size_t lead = 0;
        while (lead < 31 && hs[lead] == 0) ++lead;
        std::vector<unsigned char> hs_der(hs.begin() + lead, hs.end());
        if (hs_der[0] & 0x80) hs_der.insert(hs_der.begin(), 0x00);
        std::vector<unsigned char> high{0x30, (unsigned char)(4 + rl + hs_der.size()), 0x02, (unsigned char)rl};
        high.insert(high.end(), r.begin(), r.end());
        high.push_back(0x02);
        high.push_back((unsigned char)hs_der.size());
        high.insert(high.end(), hs_der.begin(), hs_der.end());
        BOOST_CHECK(pub.Verify(hash, high));
        BOOST_CHECK(!IsStrictLowSEcdsaSignature(high));
    }
    BOOST_CHECK(!IsStrictLowSEcdsaSignature({}));
}

BOOST_AUTO_TEST_SUITE_END()
