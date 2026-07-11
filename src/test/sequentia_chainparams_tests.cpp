// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <pos.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <util/system.h>

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

//! The chain=test defaults must stay OVERRIDABLE (unlike the pinned mainnet
//! chain), so a single operator can still bootstrap a small local committee, or
//! run VRF sortition instead of the public committee, on the same configurable
//! testnet. Nodes that pass values explicitly are unaffected by the new default.
BOOST_AUTO_TEST_CASE(testnet_pos_defaults_are_overridable)
{
    ArgsManager args;
    args.ForceSetArg("-pospubliccommittee", "0");
    args.ForceSetArg("-poscommitteesize", "3");
    const auto params = CreateChainParams(args, CBaseChainParams::TESTNET);
    BOOST_CHECK_EQUAL(params->NetworkIDString(), CBaseChainParams::TESTNET);

    BOOST_CHECK(!g_pos_public_committee);
    BOOST_CHECK_EQUAL(g_pos_committee_size, 3);

    SelectParams(CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_SUITE_END()
