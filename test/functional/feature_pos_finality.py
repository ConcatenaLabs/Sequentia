#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests immediate finality (whitepaper §3.8, doc 10 §7).

A quorum-certified block is FINAL: once a block at a height carries a committee
quorum, no SEQ-internal competitor may reorg it — not even one that later gathers
MORE signatures (committee equivocation). This is the property the conceptual
creator insisted on ("no reorg even if the same committee later signs another
block at the same height with 70/100"), and it is what makes the immediate-
finality model safe without a longest-chain fallback.

Contrast with feature_pos_fork_choice: there a higher-countersignature block,
exposed via the *manual* reconsiderblock operator override, reorganizes a
lower-countersignature tip (the comparator prefers more certification). Here the
higher-countersignature block instead arrives over the *network* (submitblock) as
a competitor to an already-finalized block — and must be REJECTED
(bad-fork-prior-to-pos-final), leaving the finalized tip untouched.
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


class PosFinalityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # 3 unit-weight stakers, committee size 3 ⇒ quorum 2. A block certified
        # by 2 members is final; one certified by 3 has strictly more
        # certification (the comparator would prefer it). Both nodes hold all
        # staker keys so each can independently certify a competing block.
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1",
            "-poscommitteesize=3", "-posslotinterval=1",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [common, common]

    def run_test(self):
        n0, n1 = self.nodes
        leader = self.stakers[0][0]
        m1 = self.stakers[1][0]
        m2 = self.stakers[2][0]

        # Shared parent at height 1, synced to both nodes.
        n0.generateposblock(leader, [m1, m2])
        self.sync_blocks()
        parent = n0.getbestblockhash()
        assert_equal(n1.getbestblockhash(), parent)

        # Split the network so each node builds its own height-2 block on the
        # shared parent (simulating the committee equivocating across a partition).
        self.disconnect_nodes(0, 1)

        # n0 finalizes A with a 2-member quorum (>= quorum 2 ⇒ final on n0).
        res_a = n0.generateposblock(leader, [m1])
        a = res_a['hash']
        assert_equal(res_a['countersignatures'], 2)
        assert_equal(n0.getbestblockhash(), a)

        # n1 finalizes B with the FULL 3-member committee — strictly MORE
        # certification than A. Built on the same parent (a competing sibling).
        res_b = n1.generateposblock(leader, [m1, m2])
        b = res_b['hash']
        assert_equal(res_b['countersignatures'], 3)
        assert_equal(n1.getbestblockhash(), b)
        assert_equal(n1.getblockheader(b)['previousblockhash'], parent)

        # Hand B to n0 directly (as the network would). Even though B carries MORE
        # countersignatures than n0's finalized A — so the fork-choice comparator
        # alone would reorganize onto it — the immediate-finality gate must reject
        # it (logging bad-fork-prior-to-pos-final): A is final and cannot be
        # overwritten. The log assertion proves the gate (not mere first-seen)
        # protected A.
        b_hex = n1.getblock(b, 0)
        with n0.assert_debug_log(["bad-fork-prior-to-pos-final"]):
            n0.submitblock(b_hex)
        assert_equal(n0.getbestblockhash(), a)        # A still final; NO reorg to the 3-member B
        assert_equal(n0.getblockcount(), 2)

        # Symmetrically, n0's A must not reorg n1's finalized B.
        a_hex = n0.getblock(a, 0)
        n1.submitblock(a_hex)
        assert_equal(n1.getbestblockhash(), b)


if __name__ == '__main__':
    PosFinalityTest().main()
