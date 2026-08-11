#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A leader elected by the exp-race fork must not be dropped by the relay filter.

Regression test for the pos_exprace relay bug: PosProducer::OnProposal used to
run PosVrfIsCommitteeMember on the proposing leader. That helper is hardcoded to
the LEGACY PosVrfSlot and takes no height, so it could not be fork-gated.

The trap is arithmetic. Winning the exp-race requires -ln(U) to be small, i.e.
beta near its MAXIMUM; the legacy formula reads that same beta as a slot of
~total_weight/weight. So a leader holding less than 1/committee_size of the
stake wins its election legitimately and then has that very proposal discarded
by every peer -- with the relaying peer marked misbehaving -- which annuls the
split-neutral proportionality the fork exists to deliver.

Trigger arithmetic used here: three equal stakers (weight 1 each, total 3) and
-poscommitteesize=2, so total/weight = 3 > 2. The exp-race winner's U sits
around 0.7-0.9, giving a legacy slot of ~2-3, at or above the cap of 2 -- so
before the fix nearly every proposal is dropped and the committee cannot
certify. Consensus itself never had this check (CheckPosStakeRules requires
registered stake, the min-stake floor, a valid VRF proof and the time-gate --
never committee membership for the leader), so the filter was strictly stricter
than the rule it pre-filtered for.

This runs under -pospubliccommittee=1, matching mainnet (chainparams.cpp pins
g_pos_public_committee=true there): the original bug was NOT specific to
private threshold sortition, since OnProposal's check on the leader was
unconditional -- unlike the five other slot-formula call sites, none of which
gate the public-committee case. Committee members must carry a registered BLS
key (the bitfield certificate names signers by looking them up), derived via
getblsregistration and folded into the extended -staker spec; see
feature_pos_public_committee.py for the same two-phase idle-then-restart
pattern this test follows.

No node holds a quorum here (one key per host, quorum 2 of 2), so the chain can
only advance if proposals actually survive the relay filter and get
countersigned over gossip. Before the fix this test stalls; after it, the chain
advances.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosExpRaceRelayTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(3)]
        self.common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posbls=1",
            "-pospubliccommittee=1",
            # committee cap 2 with 3 equal stakers => total/weight = 3 > 2, so the
            # legacy membership gate fires on the exp-race winner. This is the
            # small-scale stand-in for "less than 1/250 of the stake on mainnet".
            "-poscommitteesize=2",
            "-posslotinterval=1",
            # Exp-race active from the first block: the fork is what makes the
            # winner's beta land near its maximum.
            "-posexpraceheight=1",
            "-con_max_block_sig_size=4000",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ]
        # Phase 1: idle nodes -- staker weights only, no BLS registration and no
        # producer, so no committee can form while we derive registrations.
        idle = self.common + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [list(idle) for _ in range(3)]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def run_test(self):
        # Derive each staker's BLS registration (pure key derivation, any idle
        # node will do) and restart every node with the extended -staker spec
        # plus its own producer key. One key per host: no node can certify
        # alone, so every block has to come through the gossip proposal path
        # that carries the bug.
        self.log.info("Deriving BLS registrations and restarting with the full config")
        specs = []
        for wif, pub in self.stakers:
            reg = self.nodes[0].getblsregistration(wif)["spec"]
            specs.append("-staker=%s:1%s" % (pub, reg))

        full = self.common + specs
        for i in range(3):
            self.restart_node(i, extra_args=full + ["-posproducer",
                                                     "-posproducerkey=%s" % self.stakers[i][0]])
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

        assert_equal(self.nodes[0].getposschedule()['total_weight'], 3)

        # The chain can only advance if proposals survive the relay filter and
        # get countersigned. With the legacy membership gate in OnProposal this
        # stalls at (or very near) height 0.
        self.log.info("Blocks must be produced despite total/weight exceeding the committee cap")
        self.wait_until(lambda: all(n.getblockcount() >= 6 for n in self.nodes), timeout=120)

        # All hosts converged on the same gossip-assembled chain.
        self.sync_blocks()
        h = min(n.getblockcount() for n in self.nodes) - 2
        expect = self.nodes[0].getblockhash(h)
        for n in self.nodes[1:]:
            assert_equal(n.getblockhash(h), expect)

        # Nobody was penalised for relaying a legitimate proposal: the peers all
        # stayed connected, and no ban was recorded.
        for i, n in enumerate(self.nodes):
            assert_equal(n.listbanned(), [])
            assert len(n.getpeerinfo()) >= 2, "node %d lost peers (misbehaviour scoring?)" % i

        self.log.info("exp-race leaders below the committee-size stake ratio were relayed, "
                      "certified and accepted; chain at height %d on all hosts"
                      % self.nodes[0].getblockcount())


if __name__ == '__main__':
    PosExpRaceRelayTest().main()
