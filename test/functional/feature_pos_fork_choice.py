#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the PoS same-height fork choice (whitepaper §3.8).

Among two valid blocks at the same height (equal "work"), the chain must prefer
the one with MORE committee countersignatures — so a full-threshold block beats
an escaping-stall sub-threshold one, and a better-certified block beats a
lesser-certified sibling — regardless of which arrived first. This is enforced
by CBlockIndexWorkComparator using the per-block keys set at acceptance.

Here we build two competing aggregate-committee blocks on the same parent: one
certified by 2 members, one by 3. We make the 2-member block the tip first
(arrival order favouring the weaker block), then expose the 3-member block, and
assert the node reorganizes onto the 3-member block.
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


class PosForkChoiceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # 3 unit-weight stakers, committee size 3 ⇒ all always sortition-eligible,
        # quorum = 2. So a block can be certified by 2 or by 3 members; both are
        # valid (>= quorum), letting us compare certification strength directly.
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1",
            "-poscommitteesize=3", "-posslotinterval=1",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [common]

    def run_test(self):
        node = self.nodes[0]
        leader = self.stakers[0][0]
        m1 = self.stakers[1][0]
        m2 = self.stakers[2][0]

        # Parent block at height 1.
        node.generateposblock(leader, [m1, m2])
        assert_equal(node.getblockcount(), 1)
        parent = node.getbestblockhash()

        # The strongly-certified candidate (3 members) at height 2.
        res3 = node.generateposblock(leader, [m1, m2])
        b3 = res3['hash']
        assert_equal(res3['countersignatures'], 3)
        assert_equal(node.getbestblockhash(), b3)

        # Bury it so we can build a competing sibling on the same parent, with
        # arrival order favouring the WEAKER block (it becomes the tip first).
        node.invalidateblock(b3)
        assert_equal(node.getbestblockhash(), parent)

        res2 = node.generateposblock(leader, [m1])
        b2 = res2['hash']
        assert_equal(res2['countersignatures'], 2)
        assert_equal(node.getbestblockhash(), b2)
        assert_equal(node.getblockheader(b2)['previousblockhash'], parent)

        # Expose the 3-member block again: same height/work as the current
        # 2-member tip, but more countersignatures, so the fork choice must
        # reorganize onto it.
        node.reconsiderblock(b3)
        assert_equal(node.getbestblockhash(), b3)
        assert_equal(node.getblockcount(), 2)
        assert_equal(node.getblockheader(b3)['previousblockhash'], parent)


if __name__ == '__main__':
    PosForkChoiceTest().main()
