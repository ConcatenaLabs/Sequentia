#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests VRF-sortitioned committee certification (-posvrf + -poscommitteesize).

The paper's full consensus shape: each block needs its leader's VRF sortition
proof AND countersignatures from a quorum of committee members, where committee
membership is itself proven by each member's own VRF proof over the slot seed
(threshold sortition: member iff PosVrfSlot(beta, w, W) < committee_size, so
the expected committee size is exactly committee_size, weight-proportionally).
The challenge script enforces the signatures; coinbase SEQCMT commitments
prove each claimed member's eligibility. See doc/sequentia/07-vrf.md.

For determinism this test uses 4 unit-weight stakers with committee_size 4:
membership probability is min(4*1/4, 1) = 1, so every staker is always
selected and the quorum is PosQuorum(4) = 3.
"""

from test_framework.test_framework import BitcoinTestFramework
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


class PosVrfCommitteeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(4)]
        self.wifs = [w for w, _ in self.stakers]

        common = [
            "-con_pos=1",
            "-posvrf=1",
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

        # --- Below quorum: the leader alone (1 eligible member) cannot
        # certify; neither can leader + one more (2 < 3). ---
        assert_raises_rpc_error(-1, "sortition-selected committee members",
                                n0.generateposblock, self.wifs[0])
        assert_raises_rpc_error(-1, "sortition-selected committee members",
                                n0.generateposblock, self.wifs[0], self.wifs[1:2])

        # --- At quorum: leader + 2 more eligible members certify the block ---
        res = n0.generateposblock(self.wifs[0], self.wifs[1:3])
        assert 'vrf_output' in res
        assert res['countersignatures'] >= QUORUM
        assert_equal(n0.getblockcount(), 1)

        # The coinbase carries the leader proof and one eligibility
        # commitment per claimed member ("SEQVRF" + 3x "SEQCMT")
        block = n0.getblock(n0.getbestblockhash(), 2)
        hexes = [o['scriptPubKey']['hex'] for o in block['tx'][0]['vout']
                 if o['scriptPubKey']['asm'].startswith('OP_RETURN')]
        assert_equal(len([h for h in hexes if '534551565246' in h]), 1)  # SEQVRF
        assert_equal(len([h for h in hexes if '534551434d54' in h]), 3)  # SEQCMT

        # The peer fully validates leader + member proofs and accepts
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
        assert_raises_rpc_error(-1, "sortition-selected committee members",
                                n0.generateposblock, self.wifs[0],
                                [self.wifs[1], outsider_wif])


if __name__ == '__main__':
    PosVrfCommitteeTest().main()
