// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <pos.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(sequentia_chainparams_tests, BasicTestingSetup)

//! Every Sequentia network must have Taproot buried-active from genesis.
//!
//! Two things ride on this. SCRIPT_VERIFY_TAPROOT is gated on the deployment
//! (GetBlockScriptFlags), so while Taproot is inactive a witness-v1 output is an
//! unencumbered anyone-can-spend; and tapscript gates the introspection opcodes
//! that every covenant we ship is built from.
//!
//! The mainnet chain used to carry Bitcoin's inherited BIP9 window
//! (nStartTime=1619222400, nTimeout=1628640000, min_activation_height=709632).
//! Under elements_mode those are read as block HEIGHTS, not times (GetBIP9Time),
//! so Taproot would have begun signalling at height ~1.6 billion: never.
static void CheckTaprootAlwaysActive(const std::string& chain)
{
    ArgsManager args;
    const auto params = CreateChainParams(args, chain);
    const auto& taproot = params->GetConsensus().vDeployments[Consensus::DEPLOYMENT_TAPROOT];

    BOOST_CHECK_EQUAL(taproot.nStartTime, Consensus::BIP9Deployment::ALWAYS_ACTIVE);
    BOOST_CHECK_EQUAL(taproot.nTimeout, Consensus::BIP9Deployment::NO_TIMEOUT);
    BOOST_CHECK_EQUAL(taproot.min_activation_height, 0);
}

BOOST_AUTO_TEST_CASE(taproot_always_active_on_sequentia_networks)
{
    CheckTaprootAlwaysActive(CBaseChainParams::SEQUENTIA);
    CheckTaprootAlwaysActive(CBaseChainParams::TESTNET);
    CheckTaprootAlwaysActive(CBaseChainParams::REGTEST);

    // The chain-params constructors mutate process globals (MAX_MONEY,
    // g_pos_min_stake, g_pos_slot_interval). Restore the fixture's chain so we
    // do not leak Sequentia's caps into whichever test runs next.
    SelectParams(CBaseChainParams::REGTEST);
}

//! The Bitcoin soft forks Sequentia depends on are buried-active on the mainnet
//! chain. CSV in particular: without BIP112 the staking output's unbonding lock
//! is unenforceable (nothing-at-stake), and CLTV backs any absolute-timelock
//! vesting construction.
BOOST_AUTO_TEST_CASE(sequentia_soft_forks_buried_active)
{
    ArgsManager args;
    const auto params = CreateChainParams(args, CBaseChainParams::SEQUENTIA);
    const Consensus::Params& consensus = params->GetConsensus();

    BOOST_CHECK_EQUAL(consensus.BIP65Height, 1); // OP_CHECKLOCKTIMEVERIFY
    BOOST_CHECK_EQUAL(consensus.BIP66Height, 1);
    BOOST_CHECK_EQUAL(consensus.BIP34Height, 1);
    BOOST_CHECK_EQUAL(consensus.CSVHeight, 1);   // OP_CHECKSEQUENCEVERIFY / BIP68
    BOOST_CHECK_EQUAL(consensus.SegwitHeight, 0);

    SelectParams(CBaseChainParams::REGTEST);
}

//! The consensus rules of the Sequentia network are PINNED, not configurable.
//! A node that disagrees about the committee, the quorum, the unbonding delay or
//! the payout notice forks off silently, so these must not be arg-readable.
BOOST_AUTO_TEST_CASE(sequentia_pos_consensus_rules_are_pinned)
{
    ArgsManager args;
    const auto params = CreateChainParams(args, CBaseChainParams::SEQUENTIA);
    BOOST_CHECK(g_pos_bls);                       // BLS aggregate certification
    BOOST_CHECK(g_pos_public_committee);          // public fixed-size committee
    BOOST_CHECK_EQUAL(g_pos_committee_size, 250); // quorum 126 at the cap
    BOOST_CHECK_EQUAL(PosPublicQuorum(250), 126);
    BOOST_CHECK_EQUAL(g_pos_min_stake, 4000000000000ULL);
    BOOST_CHECK_EQUAL(g_pos_slot_interval, 30);
    BOOST_CHECK_EQUAL(g_pos_unbonding_period, 43200U);
    BOOST_CHECK_EQUAL(g_pos_payout_notice, DEFAULT_POS_PAYOUT_NOTICE);

    // Any two quorums of the committee overlap in at least two members, so no
    // two blocks at a height can be certified without two signers on both.
    BOOST_CHECK(2 * PosPublicQuorum(250) - 250 >= 2);

    // Passing a consensus rule as a flag must be refused, not silently ignored.
    for (const char* flag : {"-poscommitteesize", "-pospubliccommittee", "-posbls",
                             "-posunbonding", "-posminstake", "-pospayoutnotice",
                             "-poscheckpointdepth"}) {
        ArgsManager bad;
        bad.ForceSetArg(flag, "1");
        BOOST_CHECK_THROW(CreateChainParams(bad, CBaseChainParams::SEQUENTIA), std::runtime_error);
    }
    SelectParams(CBaseChainParams::REGTEST);
}

//! chain=test is configurable (local operators bootstrap small committees), so
//! its PoS params are arg-READABLE rather than pinned. But the DEFAULTS must
//! match the live public testnet, or a node built from a bare config disagrees
//! about the committee model and forks off in silence: every network header
//! fails CheckProof with "block-proof-invalid", unlogged at the default level.
//! That was the root cause of issue #3. Before the fix the defaults were the
//! paper's pre-re-genesis values (public committee OFF, size 100), and the live
//! testnet only agreed because every node passed the right values explicitly.
BOOST_AUTO_TEST_CASE(testnet_pos_defaults_match_live_network)
{
    ArgsManager empty;
    const auto params = CreateChainParams(empty, CBaseChainParams::TESTNET);
    BOOST_CHECK_EQUAL(params->NetworkIDString(), CBaseChainParams::TESTNET);

    BOOST_CHECK(g_pos_bls);                       // BLS aggregate certification
    BOOST_CHECK(g_pos_public_committee);          // public fixed-size committee
    BOOST_CHECK_EQUAL(g_pos_committee_size, 250); // 126-of-250 quorum at the cap
    BOOST_CHECK_EQUAL(PosPublicQuorum(250), 126);

    SelectParams(CBaseChainParams::REGTEST);
}

//! chain=test is a SHARED PUBLIC network, so its PoS parameters are consensus
//! rules and a conflicting value must be refused rather than honoured: a node
//! computing a different quorum rejects the network's blocks and forks in
//! silence (issue #3). This used to assert the opposite — that the defaults were
//! freely overridable, so an operator could bootstrap a small local committee on
//! the same chain — which is exactly the footgun. Small local committees belong
//! on regtest or a custom chain, which stay configurable (see the case below).
BOOST_AUTO_TEST_CASE(testnet_pos_consensus_flags_are_refused_when_conflicting)
{
    ArgsManager args;
    args.ForceSetArg("-pospubliccommittee", "0");
    args.ForceSetArg("-poscommitteesize", "3");
    BOOST_CHECK_THROW(CreateChainParams(args, CBaseChainParams::TESTNET), std::runtime_error);

    SelectParams(CBaseChainParams::REGTEST);
}

//! A value EQUAL to the network's is still accepted. Existing testnet configs
//! legitimately pin these — they had to, before the defaults matched the live
//! network — and refusing those too would break every current node's config on
//! upgrade for no safety gain.
BOOST_AUTO_TEST_CASE(testnet_pos_matching_values_are_accepted)
{
    ArgsManager args;
    args.ForceSetArg("-pospubliccommittee", "1");
    args.ForceSetArg("-poscommitteesize", "250");
    const auto params = CreateChainParams(args, CBaseChainParams::TESTNET);
    BOOST_CHECK_EQUAL(params->NetworkIDString(), CBaseChainParams::TESTNET);

    BOOST_CHECK(g_pos_public_committee);
    BOOST_CHECK_EQUAL(g_pos_committee_size, 250);

    SelectParams(CBaseChainParams::REGTEST);
}

//! The one-time treasury UTXO recovery is pinned to ONE chain and ONE height.
//!
//! Everything about this rewrite is a constant in CTestNetParams, and a
//! transcription slip in any of them would give every node a different UTXO set
//! rather than an error. So the constants are asserted here against the values
//! the owner authorised, not merely against themselves.
BOOST_AUTO_TEST_CASE(testnet_treasury_utxo_recovery_is_pinned)
{
    ArgsManager empty;
    const auto params = CreateChainParams(empty, CBaseChainParams::TESTNET);
    const Consensus::Params& consensus = params->GetConsensus();
    const Consensus::UtxoRecovery& rec = consensus.utxo_recovery;

    BOOST_CHECK_EQUAL(rec.height, 89500);
    // Bound to THIS chain: the re-genesis testnet genesis, and no other.
    BOOST_CHECK_EQUAL(rec.chain_genesis.GetHex(),
                      "ddd11d54c87a2bd94400fd31ce05d8e1110bb4b78e7103f738342086fc4ea92e");
    BOOST_CHECK_EQUAL(rec.chain_genesis.GetHex(), consensus.hashGenesisBlock.GetHex());

    // Retired: the two outputs whose keys and blinding key were destroyed.
    BOOST_CHECK_EQUAL(rec.retire.size(), 2U);
    BOOST_CHECK_EQUAL(rec.retire[0].first.GetHex(),
                      "910fcd65f2096051ea2fd823b21838b73a538d54e3c42c4c0474e140fed11953");
    BOOST_CHECK_EQUAL(rec.retire[0].second, 0U);
    BOOST_CHECK_EQUAL(rec.retire[1].first.GetHex(),
                      "6d7b68f5ea109eba1a03c688698e4a92debe3e7208c43fdc34e5ef052977dc7d");
    BOOST_CHECK_EQUAL(rec.retire[1].second, 1U);

    // Created: both to the replacement treasury wallet's P2WPKH, both EXPLICIT.
    const std::string treasury2026 = "0014d2c45b413aac4cebfd80fd662199c0c143467665";
    BOOST_CHECK_EQUAL(rec.create.size(), 2U);
    BOOST_CHECK_EQUAL(rec.create[0].asset.GetHex(),
                      "c8eccacf0953e1931cd31e434d8319101cc36e6c38b0e2104d8687552fae3e40");
    BOOST_CHECK_EQUAL(rec.create[0].amount, 39800000000000000LL); // 398,000,000 tSEQ
    BOOST_CHECK_EQUAL(HexStr(rec.create[0].scriptPubKey), treasury2026);
    BOOST_CHECK_EQUAL(rec.create[1].asset.GetHex(),
                      "2afc53ebcd3f3179c60f97e4e7f23755ff2519b308914d3b51ee97fb1c8557e5");
    BOOST_CHECK_EQUAL(rec.create[1].amount, 100000000LL); // 1.0 USDX reissuance token
    BOOST_CHECK_EQUAL(HexStr(rec.create[1].scriptPubKey), treasury2026);

    // The policy-asset row really is this chain's policy asset, and the amount
    // recreated is under the chain's cap. (Also asserted at construction, so a
    // wrong byte order cannot even start a node -- pinned here so a reader can
    // see the claim being checked rather than take it on trust.)
    BOOST_CHECK(rec.create[0].asset == consensus.subsidy_asset);
    BOOST_CHECK(rec.create[0].amount <= MAX_MONEY);

    // The height gate is exact: the rewrite belongs to one block, not a range.
    BOOST_CHECK(consensus.UtxoRecoveryAppliesAt(89500));
    BOOST_CHECK(!consensus.UtxoRecoveryAppliesAt(89499));
    BOOST_CHECK(!consensus.UtxoRecoveryAppliesAt(89501));

    // The created coins' outpoints are a pure function of the table, so this
    // txid is recomputable by anyone and must not drift: a change here silently
    // moves the recovered funds to different outpoints. It is not a real
    // transaction and will never be found by getrawtransaction; the coins are
    // reachable via gettxout / scantxoutset.
    const CTransactionRef tx = BuildUtxoRecoveryTransaction(rec);
    BOOST_CHECK_EQUAL(tx->vin.size(), 2U);
    BOOST_CHECK_EQUAL(tx->vout.size(), 2U);
    BOOST_CHECK(tx->vout[0].nAsset.IsExplicit());
    BOOST_CHECK(tx->vout[0].nValue.IsExplicit());
    BOOST_CHECK_EQUAL(tx->vout[0].nValue.GetAmount(), 39800000000000000LL);
    BOOST_CHECK_EQUAL(tx->GetHash().GetHex(),
                      "618981449a50c460c1dcd7c0dae693674294a2c58e930e128d6ef56e82eecae7");

    SelectParams(CBaseChainParams::REGTEST);
}

//! A fresh chain must never carry someone else's accident. Mainnet, regtest and
//! Bitcoin main have no recovery table at all, and no height can make one fire.
BOOST_AUTO_TEST_CASE(fresh_chains_have_no_utxo_recovery)
{
    for (const std::string& chain : {std::string(CBaseChainParams::SEQUENTIA),
                                     std::string(CBaseChainParams::REGTEST),
                                     std::string(CBaseChainParams::MAIN)}) {
        ArgsManager empty;
        const auto params = CreateChainParams(empty, chain);
        const Consensus::Params& consensus = params->GetConsensus();
        BOOST_CHECK_MESSAGE(consensus.utxo_recovery.IsNull(), chain << " has a UTXO recovery table");
        BOOST_CHECK(!consensus.UtxoRecoveryAppliesAt(0));
        BOOST_CHECK(!consensus.UtxoRecoveryAppliesAt(89500));
    }
    SelectParams(CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_SUITE_END()
