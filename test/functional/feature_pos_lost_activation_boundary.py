#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The edges of the lost-block rule's height (doc 03, "Activation"), on the
network of feature_pos_lost_activation.py (-con_lost_blocks_height=H, -par=1):

  binds  a block L at H - 1 and its child at H are lost (Bitcoin reorganizes the
         chain from H back to H - 2). The block that replaces L at H - 1 is
         judged by the old rules: forced without L's transaction T and with a
         malformed record, it is accepted. The block at H is judged against all
         the evidence held, L included: the mempool refuses T's respend
         (txn-lost-conflict), the template carries T first, and forced blocks
         are refused with a malformed record (bad-lost-record), with the
         respend (bad-txns-lost-respend) and without T (bad-lost-replay);
  above  the same refusals for a block lost at H + 1;
  reindex
         a node rebuilt from disk reaches the same tip through the malformed
         record at H - 1, and reports the rule active.
"""

from decimal import Decimal

from feature_pos_lost_activation import H, LostActivationTest
from feature_pos_lost_blocks import ALL_KEYS, HONEST, N
from test_framework.util import assert_equal

GARBAGE = b"\x02not a record"


class LostActivationBoundaryTest(LostActivationTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        try:
            self.body()
        finally:
            self.stop_bitcoind()

    def expect_refused(self, case, what, rc, expect):
        n1 = self.nodes[1]
        before = n1.getbestblockhash()
        res = n1.submitblock(rc["hex"])
        assert res and expect in res, "%s: expected %r, got %r" % (what, expect, res)
        assert_equal(n1.getbestblockhash(), before)
        self.record(case, "forced: " + what, res)

    def refusals(self, case, t_hex, t_id, r_hex, height, filler):
        """At `height`, with T of a lost block due: the mempool, the template and three forced blocks."""
        n1 = self.nodes[1]
        why = n1.testmempoolaccept([r_hex])[0].get("reject-reason")
        self.record(case, "the respend in the mempool", why)
        assert_equal(why, "txn-lost-conflict")
        base, leader = self.produce_by()
        assert_equal(base["height"], height)
        assert_equal(n1.getblock(base["hash"])["tx"][1], t_id)
        self.record(case, "template", "node 1's block at %d carries T first" % height)
        raw = n1.getblock(base["hash"], 0)
        n1.invalidateblock(base["hash"])
        self.expect_refused(case, "a malformed record", self.forced(raw, leader, record=GARBAGE), "bad-lost-record")
        self.expect_refused(case, "the respend instead of T", self.forced(raw, leader, drop=[t_hex], add=[r_hex]),
                            "bad-txns-lost-respend")
        self.expect_refused(case, "without T", self.forced(raw, leader, drop=[t_hex], add=[filler]), "bad-lost-replay")
        n1.reconsiderblock(base["hash"])
        self.wait_until(lambda: n1.getbestblockhash() == base["hash"], timeout=30)
        self.sync_tips(HONEST)

    def body(self):
        self.wallets()
        n1, a = self.nodes[1], self.nodes[0]
        coins = [self.fund(Decimal("2")) for _ in range(5)]

        def filler(coin):
            raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],
                                         [{a.getnewaddress(): coin["amount"] - Decimal("0.003")}, {"fee": Decimal("0.003")}])
            return self.signed(a, raw)
        f1, f2, f3 = filler(coins[2]), filler(coins[3]), filler(coins[4])

        self.log.info("binds: a block lost below H binds the block at H")
        while n1.getblockcount() < H - 2:
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        t_hex, t_id, r_hex, h = self.lost_T("binds", coins[0])
        assert_equal(h, H - 1)
        self.active(False)
        base, leader = self.produce_by()
        assert_equal(base["height"], H - 1)
        raw = n1.getblock(base["hash"], 0)
        # Every node takes the produced block, then leaves it aside, so that all
        # of them follow the forced one (a certified block is final against a sibling).
        self.sync_tips(range(N))
        for node in self.nodes:
            node.invalidateblock(base["hash"])
        self.sync_tips(range(N))
        rc = self.forced(raw, leader, drop=[t_hex], add=[f1], record=GARBAGE)
        res = n1.submitblock(rc["hex"])
        assert res is None, res
        assert_equal(n1.getbestblockhash(), rc["hash"])
        self.record("binds", "forced at H - 1", "without T and with a malformed record: accepted")
        self.sync_tips(range(N))
        self.active(True)
        self.refusals("binds", t_hex, t_id, r_hex, H, f2)

        self.log.info("above: a block lost at H + 1")
        t_hex, t_id, r_hex, h = self.lost_T("above", coins[1])
        assert_equal(h, H + 1)
        self.refusals("above", t_hex, t_id, r_hex, H + 1, f3)

        self.log.info("reindex: node 2 rebuilds the chain from disk, through the malformed record at H - 1")
        tip = self.nodes[2].getbestblockhash()
        self.restart_node(2, extra_args=self.node_args(2) + ["-reindex"])
        self.wait_until(lambda: self.nodes[2].getbestblockhash() == tip, timeout=120)
        record = self.nodes[2].getblock(self.nodes[2].getblockhash(H - 1), 2)["tx"][0]["vout"]
        assert any(o["scriptPubKey"]["hex"].startswith("6a0453514c52") for o in record), "no record output at H - 1"
        d = self.nodes[2].getdeploymentinfo()["deployments"]["lost_blocks"]
        assert_equal((d["active"], d["height"]), (True, H))
        self.record("reindex", "node 2", "the same tip at height %d; lost_blocks %s" % (self.nodes[2].getblockcount(), d))
        for i in HONEST:
            if i != 2:
                self.connect_nodes(2, i)


if __name__ == '__main__':
    LostActivationBoundaryTest().main()
