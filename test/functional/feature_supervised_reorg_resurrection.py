#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Supervision eviction across a reorg.

`feature_supervised_assets.py` proves that a record evicts the spends it
invalidated when the record CONFIRMS, and that a reorg takes a freeze back with
it. Neither covers the direction a reorg opens up: a spend that was already
MINED, on a branch that loses, arriving back at the mempool through the
disconnect pool while a pause is in force on the branch that won.

That is the path where an eviction sweep cannot help, because nothing is being
connected that carries a record. The only thing standing between a
pause-invalidated spend and the mempool is that resurrection goes through
AcceptToMemoryPool, and that ATMP reads the supervision registry. This test
asserts that empirically rather than by reading the code.

Three cases:

  1. Baseline. A spend sits in the mempool, a pause record confirms, the spend
     is evicted. (ConnectTip -> removeStaleSupervision, which only runs at all
     when the connecting block carries a record.)
  2. The record block is disconnected and reconnected. Lifting the pause makes
     the spend acceptable again; restoring it must evict again.
  3. The disconnect-pool direction. A spend mined on the losing branch, a pause
     mined on the winning branch, and the reorg must NOT admit the spend.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

# Reuse the record-building machinery rather than reimplementing the signing
# dance: it is the same issuer flow and it is already proven by that test.
from feature_supervised_assets import SupervisedAssetsTest, make_key


class SupervisedReorgResurrectionTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        # Same chain configuration as feature_supervised_assets.py: this chain
        # has no coinbase issuance, so the coins come from initialfreecoins.
        self.extra_args = [[
            "-con_default_blinded_addresses=0",
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-anyonecanspendaremine=1",
            "-txindex=1",
            "-supervisedassetsheight=1",
            # The incident this test exists for was invisible because rejections
            # are not logged by default. A test about a rejection should be able
            # to tell "refused" from "never offered", so turn it on here.
            "-debug=mempoolrej",
            "-debug=mempool",
        ]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # -- borrowed helpers -------------------------------------------------

    funding_outpoint = SupervisedAssetsTest.funding_outpoint
    send_raw = SupervisedAssetsTest.send_raw
    record_tx = SupervisedAssetsTest.record_tx
    spend_asset = SupervisedAssetsTest.spend_asset
    issue_asset = SupervisedAssetsTest.issue_asset

    def run_test(self):
        self.operational, self.op_pub = make_key(0x1111)
        self.recovery, self.rec_pub = make_key(0x2222)

        node = self.nodes[0]
        self.policy_asset = node.dumpassetlabels()["bitcoin"]

        self.generate(node, 1)
        self.sync_all()

        asset = self.issue_asset(node, pause=True)
        holder = self.nodes[1].getnewaddress()
        self.spend_asset(node, asset, Decimal("100"), holder)
        # Fee asset for node1, so a refusal is never merely "no funds".
        self.spend_asset(node, self.policy_asset, 10, self.nodes[1].getnewaddress())
        self.generate(node, 1)
        self.sync_all()

        self.case_1_baseline_eviction(node, asset)
        self.case_2_disconnect_and_reconnect(node, asset)
        self.case_3_disconnect_pool(node, asset)

    # -- 1 ----------------------------------------------------------------

    def case_1_baseline_eviction(self, node, asset):
        self.log.info("1. a pause record confirming evicts the spend it invalidated")
        spend = self.spend_asset(self.nodes[1], asset, Decimal("10"),
                                 node.getnewaddress())
        self.sync_mempools()
        assert spend in node.getrawmempool(), "the spend should be resident before the pause"

        self.record_tx(node, "pause", asset, None, self.operational)
        self.generate(node, 1)
        self.sync_all()

        assert_equal([a for a in node.getsupervisedassets()
                      if a["asset"] == asset][0]["paused"], True)
        assert spend not in node.getrawmempool(), \
            "a confirmed pause must evict the spend it invalidated"
        assert spend not in self.nodes[1].getrawmempool(), \
            "eviction is not the submitting node's private opinion"
        self.log.info("   evicted on both nodes")
        self.paused_record_tip = node.getbestblockhash()

    # -- 2 ----------------------------------------------------------------

    def case_2_disconnect_and_reconnect(self, node, asset):
        self.log.info("2. disconnecting the record lifts the pause; reconnecting evicts again")
        record_block = self.paused_record_tip

        node.invalidateblock(record_block)
        assert_equal([a for a in node.getsupervisedassets()
                      if a["asset"] == asset][0]["paused"], False)

        # With the pause gone the spend is ordinary again, so it must be
        # ACCEPTABLE. This is the half that catches an over-eager registry: a
        # rollback that fails to lift leaves holders frozen by a record that is
        # no longer on the chain.
        spend = self.spend_asset(self.nodes[0], asset, Decimal("5"),
                                 self.nodes[1].getnewaddress())
        assert spend in node.getrawmempool(), \
            "with the record disconnected the pause must be lifted"
        self.log.info("   pause lifted, spend accepted again")

        node.reconsiderblock(record_block)
        assert_equal(node.getbestblockhash(), record_block)
        assert_equal([a for a in node.getsupervisedassets()
                      if a["asset"] == asset][0]["paused"], True)
        assert spend not in node.getrawmempool(), \
            "reconnecting the record must evict the spend again"
        self.log.info("   pause restored, spend evicted again")

    # -- 3 ----------------------------------------------------------------

    def case_3_disconnect_pool(self, node, asset):
        self.log.info("3. a spend mined on the losing branch must not be resurrected "
                      "into a paused chain")
        # A fresh asset, so this case does not inherit the chain state the first
        # two left behind. It starts unpaused, which is the precondition: the
        # spend has to be valid when it is mined, and invalid only afterwards,
        # because of a record it never shared a branch with.
        asset2 = self.issue_asset(node, pause=True)
        self.spend_asset(node, self.policy_asset, 5, self.nodes[1].getnewaddress())
        self.generate(node, 1)
        self.sync_all()
        assert_equal([a for a in node.getsupervisedassets()
                      if a["asset"] == asset2][0]["paused"], False)

        self.disconnect_nodes(0, 1)

        # LOSING branch (node0): the spend is MINED, not merely resident. That
        # is what puts it in the disconnect pool later; an evicted mempool entry
        # would simply be gone.
        spend = self.spend_asset(node, asset2, Decimal("7"),
                                 self.nodes[1].getnewaddress())
        spend_raw = node.getrawtransaction(spend)
        # A CONTROL in the same block: an ordinary spend of an asset nothing
        # freezes. Without it a green here would be vacuous -- "not in the
        # mempool" is also what you see if resurrection never happens at all,
        # and MaybeUpdateMempoolForReorg is called with fAddToMempool=false on
        # two of its four call sites. The control has to come BACK.
        control = self.spend_asset(node, self.policy_asset, Decimal("2"),
                                   self.nodes[1].getnewaddress())
        self.generate(node, 1, sync_fun=self.no_op)
        mined = node.getblock(node.getbestblockhash())["tx"]
        assert spend in mined, "the spend must be mined on the branch that will lose"
        assert control in mined, "the control must share the losing block"
        assert spend not in node.getrawmempool()
        losing_tip = node.getbestblockhash()

        # WINNING branch (node1): a pause for the same asset, and enough work to
        # win. node1 never saw the spend, so the pause and the spend have never
        # been on one branch together.
        self.record_tx(self.nodes[1], "pause", asset2, None, self.operational)
        self.generate(self.nodes[1], 4, sync_fun=self.no_op)

        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(node.getbestblockhash(), self.nodes[1].getbestblockhash())
        assert node.getbestblockhash() != losing_tip, "node0 must have reorged"
        assert_equal([a for a in node.getsupervisedassets()
                      if a["asset"] == asset2][0]["paused"], True)

        # THE ASSERTION. The spend came back through the disconnect pool onto a
        # chain where its asset is paused. No eviction sweep can reach this: the
        # sweep runs on CONNECT when the connecting block carries a record, and
        # here the entry arrives from the DISCONNECT side afterwards. The only
        # thing refusing it is that resurrection goes through AcceptToMemoryPool
        # and ATMP consults the supervision registry.
        # The control proves the path is live: it shared a block with the spend,
        # was disconnected by the same reorg, and came back.
        assert control in node.getrawmempool(), (
            "the control spend was not resurrected either, so this reorg "
            "resurrected nothing and the case below proves nothing")
        self.log.info("   control resurrected, so the disconnect pool is live")

        assert spend not in node.getrawmempool(), (
            "a pause-invalidated spend was resurrected from the disconnect pool; "
            "it would sit in the mempool unminable, and a miner building on it "
            "would produce a block every node rejects")
        self.log.info("   the paused spend was NOT resurrected, while the control "
                      "beside it was: ATMP read the registry")

        # And the refusal is the pause, not the transaction having quietly gone
        # missing for some unrelated reason. Offering the IDENTICAL bytes back
        # to the node must be refused, by name. testmempoolaccept rather than
        # the wallet, so nothing depends on which coins the wallet considers
        # spendable after a reorg.
        verdict = node.testmempoolaccept([spend_raw])[0]
        assert_equal(verdict["allowed"], False)
        assert "frozen" in verdict["reject-reason"], \
            "expected the pause to be the reason, got: %s" % verdict["reject-reason"]
        self.log.info("   resubmitting the same bytes is refused: %s",
                      verdict["reject-reason"])


if __name__ == '__main__':
    SupervisedReorgResurrectionTest().main()
