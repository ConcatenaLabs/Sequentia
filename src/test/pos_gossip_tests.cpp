// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls.h>
#include <chainparams.h>
#include <key.h>
#include <pos.h>
#include <pos_producer.h>
#include <vrf.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

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
    const uint256 block_hash = InsecureRand256();
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

    // A's genuine share for a block this node knows nothing about is valid,
    // just not worth relaying.
    BOOST_CHECK(producer.OnShare(make_share(key_a, bls_a, /*good_sig=*/true)) == PosGossipAction::Ignore);

    reg.Clear();
    g_pos_public_committee = saved_public;
}

BOOST_AUTO_TEST_SUITE_END()
