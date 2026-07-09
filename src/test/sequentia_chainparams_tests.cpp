// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
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

BOOST_AUTO_TEST_SUITE_END()
