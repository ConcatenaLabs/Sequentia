#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The minimum block spacing is a CONSENSUS rule, not a habit of the producer.

Sequentia's cadence used to live only in PosProducer::Step, producer-side, where
no validator checks it: a modified producer ignored it and every node accepted
the result. Producers are paid in the fees of the blocks they lead and in
nothing else (there is no block subsidy), so the incentive to shorten the
cadence is permanent and grows with congestion, and more blocks in the same time
is more disk, more bandwidth and more validation for every node for ever.

Consensus::Params::pos_block_spacing closes that. Two independent halves, and
the test drives both, because either alone is useless:

  THE RULE     a block stamped closer than the spacing to its parent is invalid
               (bad-pos-spacing), from pos_block_spacing_height onward and not
               before -- the running testnet has 2,186 blocks that violate it.

  THE CLAMP    a producer never STAMPS closer than the spacing, whatever its own
               clock says, and follows the spacing VALUE rather than its
               activation height. This is the part that is easy to leave out and
               must not be: on the live chain 2,183 blocks sit at exactly 29 s
               after their parent, produced by honest nodes whose clock trailed
               the previous producer's by a second (the producer waited
               correctly, then the assembler stamped GetAdjustedTime()). Turning
               the rule on without the clamp would invalidate blocks honest
               producers emit right now, over and over.

The spacing is deliberately NOT the slot-gate unit (-posslotinterval). They
answer different questions -- how fast may the chain run, versus in what order
may leaders propose -- and this test pins them at different values so that
folding them back into one number breaks it.

Topology: node0 sets no spacing at all, so it is the unconstrained producer
standing in for a modified binary; node1 enforces; node2 has the value but the
rule parked out of reach, isolating the clamp. Blocks move between nodes with
submitblock rather than p2p, so no assertion can race block propagation.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal

SPACING = 10          # -posblockspacing on the enforcing node
ACTIVATION = 8        # -posblockspacingheight: blocks 1..7 exempt, 8 onward bound
FAST_STEP = 1         # seconds the unconstrained producer advances per block


class PosBlockSpacingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.mocktime = int(time.time())
        common = [
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
            "-initialfreecoins=500000000",
            # Pinned away from SPACING on purpose: the spacing must not be
            # taken from the slot-gate unit, and a test that used the same
            # number for both would not notice if it were.
            "-posslotinterval=3",
        ]
        self.extra_args = [
            # node0: no spacing -> stamps whatever its clock says. The modified
            # producer the rule exists to stop.
            common + ["-posblockspacing=0"],
            # node1: enforces, from ACTIVATION.
            common + ["-posblockspacing=%d" % SPACING,
                      "-posblockspacingheight=%d" % ACTIVATION],
            # node2: same spacing VALUE but the rule parked out of reach, so
            # only the clamp can be responsible for anything seen here.
            common + ["-posblockspacing=%d" % SPACING,
                      "-posblockspacingheight=999999"],
        ]

    def setup_network(self):
        # No p2p links: every block is handed over explicitly with submitblock.
        self.setup_nodes()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # --- helpers ----------------------------------------------------------

    def gaps(self, node, lo, hi):
        """Seconds between consecutive block timestamps over heights lo..hi.

        Starts at lo >= 1 deliberately: the genesis timestamp is pinned in the
        chain parameters, so the genesis-to-1 gap says nothing about spacing.
        """
        t = [node.getblockheader(node.getblockhash(h))["time"] for h in range(lo, hi + 1)]
        return [b - a for a, b in zip(t, t[1:])]

    def mine_fast(self, node, addr, n):
        """Mine n blocks on `node`, advancing its clock by only FAST_STEP each."""
        for _ in range(n):
            self.mocktime += FAST_STEP
            node.setmocktime(self.mocktime)
            self.generatetoaddress(node, 1, addr, sync_fun=self.no_op)

    def hand_over(self, src, dst, height):
        """Give dst the block src has at `height`. Returns submitblock's verdict."""
        return dst.submitblock(src.getblock(src.getblockhash(height), 0))

    # --- the test ---------------------------------------------------------

    def run_test(self):
        node0, node1, node2 = self.nodes
        for n in self.nodes:
            n.setmocktime(self.mocktime)
            n.createwallet(wallet_name="w", descriptors=True)
        # A named wallet is only reachable through its own /wallet/<name> path;
        # the node-level proxy has no default wallet to fall back on.
        addr0, addr1, addr2 = (n.get_wallet_rpc("w").getnewaddress() for n in self.nodes)

        self.log.info("node0 (no spacing) packs a chain tighter than the spacing")
        self.mine_fast(node0, addr0, ACTIVATION + 3)
        tight = self.gaps(node0, 1, ACTIVATION + 3)
        assert_equal(set(tight), {FAST_STEP})
        self.log.info("  %d blocks, every gap %ds, spacing would be %ds",
                      ACTIVATION + 3, FAST_STEP, SPACING)

        self.log.info("below the activation height the tight blocks are ACCEPTED")
        for h in range(1, ACTIVATION):
            res = self.hand_over(node0, node1, h)
            assert res is None, "block %d rejected below the gate: %s" % (h, res)
        assert_equal(node1.getblockcount(), ACTIVATION - 1)
        self.log.info("  heights 1..%d accepted, exactly as the live chain's "
                      "2,186 too-close blocks must stay valid", ACTIVATION - 1)

        self.log.info("at the activation height the same block is REJECTED")
        assert_equal(self.hand_over(node0, node1, ACTIVATION), "bad-pos-spacing")
        assert_equal(node1.getblockcount(), ACTIVATION - 1)
        self.log.info("  bad-pos-spacing, tip stayed at %d", ACTIVATION - 1)

        self.log.info("the refusal is about the GAP, not the height: node1 makes "
                      "its own block at the same height and it stands")
        # node1's clock has only crept forward by FAST_STEP per block, so this
        # can succeed only if the clamp pushed its stamp out to parent+SPACING.
        self.mocktime += FAST_STEP
        node1.setmocktime(self.mocktime)
        self.generatetoaddress(node1, 1, addr1, sync_fun=self.no_op)
        assert_equal(node1.getblockcount(), ACTIVATION)
        own = self.gaps(node1, ACTIVATION - 1, ACTIVATION)
        assert_greater_than_or_equal(own[0], SPACING)
        self.log.info("  accepted at height %d with a gap of %ds", ACTIVATION, own[0])

        self.log.info("the clamp follows the spacing VALUE, not its height: "
                      "node2 has the rule parked at 999999 and still cannot "
                      "build a too-close block")
        self.mine_fast(node2, addr2, 6)
        clamped = self.gaps(node2, 1, 6)
        assert all(g >= SPACING for g in clamped), \
            "clamp let a block through: gaps %s, spacing %d" % (clamped, SPACING)
        self.log.info("  6 blocks, gaps %s, all >= %ds while its own clock "
                      "advanced only %ds per block", clamped, SPACING, FAST_STEP)

        self.log.info("and with no spacing configured nothing is clamped: node0's "
                      "gaps stayed at %ds throughout", FAST_STEP)
        assert_equal(set(self.gaps(node0, 1, ACTIVATION + 3)), {FAST_STEP})


if __name__ == '__main__':
    PosBlockSpacingTest().main()
