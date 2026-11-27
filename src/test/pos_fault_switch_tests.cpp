// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>

BOOST_FIXTURE_TEST_SUITE(pos_fault_switch_tests, BasicTestingSetup)

//! Every switch that makes a producer misbehave on purpose or weakens its own
//! checks, for tests only.
static const char* const FAULT_SWITCHES[] = {
    "-posbyzantineequivocate", "-posbyzantineinvalid", "-posbyzantinefakeanchor",
    "-posbyzantinebackdate", "-posbyzantinestampfloor", "-posdebugroundskewms",
    "-posdebugsharedelayms", "-posgossipnobodycheck", "-posgossipstaleonce",
};

BOOST_AUTO_TEST_CASE(fault_switches_are_refused_on_the_named_networks)
{
    for (const std::string& chain : {CBaseChainParams::TESTNET, CBaseChainParams::SEQUENTIA}) {
        for (const char* sw : FAULT_SWITCHES) {
            ArgsManager args;
            args.ForceSetArg(sw, "1");
            bool refused = false;
            try {
                CreateChainParams(args, chain);
            } catch (const std::runtime_error& e) {
                refused = std::string(e.what()).find("is a test-only fault injection and is refused") != std::string::npos;
            }
            BOOST_CHECK_MESSAGE(refused, std::string(sw) + " must be refused on the " + chain + " chain");
        }
    }
    // A custom chain, the kind the functional tests run on, takes every one.
    for (const char* sw : FAULT_SWITCHES) {
        ArgsManager args;
        args.ForceSetArg(sw, "1");
        BOOST_CHECK_NO_THROW(CreateChainParams(args, "elementsregtest"));
    }
    // Building chain parameters sets process-wide state; put back the fixture's.
    SelectParams(Params().NetworkIDString());
}

BOOST_AUTO_TEST_SUITE_END()
