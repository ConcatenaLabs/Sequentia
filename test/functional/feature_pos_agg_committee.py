#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests MuSig2 aggregate committee certification (-posaggcommittee).

With aggregation, the block challenge commits to the leader key plus a single
32-byte MuSig2 aggregate of the committee member set (OP_1 <leader> <aggkey>),
and the block carries one 64-byte BIP340 signature by all named members instead
of one ECDSA signature per member — lifting the script-multisig committee cap
from 16 to the paper's 100. Membership is still proven per member by coinbase
SEQCMT VRF eligibility commitments; validators re-aggregate the named set and
require it to match the challenge's aggregate key. See doc/sequentia/04-proof-of-stake.md §6.

As in feature_pos_vrf_committee.py, 4 unit-weight stakers with committee_size 4
make sortition deterministic: every staker is always selected; quorum is 3.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

COMMITTEE_SIZE = 4
QUORUM = 3  # strict majority of the expected committee size


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosAggCommitteeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(4)]
        self.wifs = [w for w, _ in self.stakers]

        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posaggcommittee=1",
            "-poscommitteesize=%d" % COMMITTEE_SIZE,
            "-posslotinterval=2",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [list(common), list(common)]

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]

        # --- Below quorum: too few sortition-selected members to aggregate.
        # (Without anchoring there is no escaping-stall relief, so consensus
        # rejects the sub-quorum block — surfaced from CreateNewBlock's
        # TestBlockValidity as bad-posvrf-agg-quorum.) ---
        assert_raises_rpc_error(-1, "fewer named committee members than the certification quorum",
                                n0.generateposblock, self.wifs[0])
        assert_raises_rpc_error(-1, "fewer named committee members than the certification quorum",
                                n0.generateposblock, self.wifs[0], self.wifs[1:2])

        # --- At quorum: one aggregate signature certifies the block ---
        res = n0.generateposblock(self.wifs[0], self.wifs[1:3])
        assert 'vrf_output' in res
        assert res['countersignatures'] >= QUORUM
        assert_equal(n0.getblockcount(), 1)

        # The coinbase carries the leader proof and one eligibility commitment
        # per *named* member ("SEQVRF" + 3x "SEQCMT"); the challenge is the
        # aggregate form OP_1 (0x51) <leader 33> <aggkey 32>.
        block = n0.getblock(n0.getbestblockhash(), 2)
        hexes = [o['scriptPubKey']['hex'] for o in block['tx'][0]['vout']
                 if o['scriptPubKey']['asm'].startswith('OP_RETURN')]
        assert_equal(len([h for h in hexes if '534551565246' in h]), 1)  # SEQVRF
        assert_equal(len([h for h in hexes if '534551434d54' in h]), 3)  # SEQCMT
        challenge = block['signblock_challenge']
        assert challenge.startswith('51'), "expected OP_1-versioned aggregate challenge"
        assert_equal(len(challenge), 2 * (1 + 1 + 33 + 1 + 32))
        # The solution is two pushes — a DER leader signature and a 64-byte
        # BIP340 aggregate signature — and stays this size no matter how many
        # members signed (vs one ECDSA signature per member with multisig).
        assert len(block['signblock_witness_hex']) // 2 < 200

        # The peer fully validates leader + member proofs, re-aggregates the
        # named member set, checks it matches the challenge key, and verifies
        # the single Schnorr signature.
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())

        # --- A run with all four members; both nodes stay converged ---
        for i in range(6):
            leader = self.wifs[i % 4]
            others = [w for w in self.wifs if w != leader]
            r = n0.generateposblock(leader, others)
            assert r['countersignatures'] >= QUORUM
        assert_equal(n0.getblockcount(), 7)
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())

        # --- Unregistered keys contribute nothing toward the quorum ---
        outsider_wif, _ = make_staker()
        assert_raises_rpc_error(-1, "fewer named committee members than the certification quorum",
                                n0.generateposblock, self.wifs[0],
                                [self.wifs[1], outsider_wif])

        # --- Committee sizes beyond the multisig cap of 16 are accepted only
        # with aggregation enabled ---
        self.stop_node(1)
        big = [a for a in self.extra_args[1] if not a.startswith("-poscommitteesize")]
        self.start_node(1, extra_args=big + ["-poscommitteesize=40"])
        self.stop_node(1)
        no_agg = [a for a in big if a != "-posaggcommittee=1"]
        n1.assert_start_raises_init_error(
            no_agg + ["-poscommitteesize=40"],
            "poscommitteesize must be between 1 and 16",
            match=ErrorMatch.PARTIAL_REGEX)


if __name__ == '__main__':
    PosAggCommitteeTest().main()
