#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A wallet's send is judged against the fee whitelist in force when it commits.

A connected node reads its price server's list every minute, and an operator
can write the list by hand at any time, so the whitelist can change while the
wallet builds a transaction, or while its user reviews it. At commit, under the
lock every writer of the list takes, the transaction is judged as the mempool
judges it: its fee asset still accepted, and its fee at least what the rate in
force requires. If so it is sent, whatever else in the list changed; if not,
nothing is sent and the wallet says why.

The race is forced: -testfeedecisiondelay holds the send between building the
transaction and committing it, and the list is written by hand in that gap.
  * the fee asset removed, or set to zero: nothing sent;
  * its rate raised so the fee is worth less than required: nothing sent;
  * its rate lowered (the fee is worth more): sent;
  * another asset's rate changed, another asset added or removed, the same
    list written again: sent;
  * an issuance: the same.
The hand write does not wait for the held send. The switch is refused on a
chain that does not mine on demand.
"""

import threading
import time
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, get_rpc_proxy

DELAY_MS = 3000


class AnyAssetFeeCommitRaceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
            "-anyonecanspendaremine=1",
            "-testfeedecisiondelay=%d" % DELAY_MS,
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        self.policy = node.getsidechaininfo()["pegged_asset"]
        issued = node.issueasset(assetamount=Decimal("1000"), tokenamount=0, blind=False, fee_asset=self.policy)
        self.asset = issued["asset"]
        self.generate(node, 1)
        other, extra = "77" * 32, "88" * 32
        self.base = {self.policy: 100000000, self.asset: 200000000, other: 100000000}

        def changed(**kw):
            new = dict(self.base)
            new.update(kw.get("set", {}))
            for k in kw.get("drop", []):
                del new[k]
            return new

        cases = (
            ("the fee asset removed", changed(drop=[self.asset]), "no longer accepts"),
            ("the fee asset's rate set to zero", changed(set={self.asset: 0}), "no longer accepts"),
            ("the requirement rose above what it pays", changed(set={self.asset: 200}), "below what this node requires"),
            ("the requirement fell", changed(set={self.asset: 200000000 * 2}), None),
            ("another asset's rate changed", changed(set={other: 300000000}), None),
            ("another asset added", changed(set={extra: 100000000}), None),
            ("another asset removed", changed(drop=[other]), None),
            ("the same list written again", changed(), None),
        )
        for name, new_list, refusal in cases:
            self.log.info("Send: %s" % name)
            self.race(self.send, new_list, refusal)
        for name, new_list, refusal in (cases[0], cases[4]):
            self.log.info("Issuance: %s" % name)
            self.race(self.issue, new_list, refusal)

        self.log.info("The switch is refused where blocks are not mined on demand")
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(
            extra_args=self.extra_args[0] + ["-chain=sequentia"],
            expected_msg="Error: -testfeedecisiondelay is for test chains only",
            match=ErrorMatch.PARTIAL_REGEX)

    def send(self, rpc):
        return rpc.sendtoaddress(address=self.nodes[0].getnewaddress(), amount=Decimal("1"), assetlabel=self.asset,
                                 fee_asset_label=self.asset)

    def issue(self, rpc):
        return rpc.issueasset(assetamount=Decimal("1"), tokenamount=0, blind=False, fee_asset=self.asset)["txid"]

    def race(self, call, new_list, refusal):
        node = self.nodes[0]
        node.setfeeexchangerates(self.base)
        before = {t["txid"] for t in node.listtransactions("*", 1000)}
        result = {}

        def run():
            rpc = get_rpc_proxy(node.url, 1, timeout=90)
            try:
                result["txid"] = call(rpc)
            except Exception as e:
                result["error"] = str(e)
        thread = threading.Thread(target=run)
        thread.start()
        time.sleep(DELAY_MS / 3000)  # built and signed, waiting to commit
        started = time.monotonic()
        node.setfeeexchangerates(new_list)
        waited = time.monotonic() - started
        thread.join()
        self.log.info("  the hand write took %.3f s while the send was held" % waited)
        assert waited < DELAY_MS / 3000, waited
        if refusal is None:
            assert "error" not in result, result
            assert result["txid"] in node.getrawmempool()
            decoded = node.decoderawtransaction(node.gettransaction(result["txid"])["hex"])
            assert_equal([o["asset"] for o in decoded["vout"] if o["scriptPubKey"]["type"] == "fee"], [self.asset])
            node.setfeeexchangerates(self.base)
            self.generate(node, 1)
            assert_equal(node.gettransaction(result["txid"])["confirmations"], 1)
        else:
            assert "error" in result, result
            assert refusal in result["error"] and "Nothing was sent" in result["error"], result["error"]
            assert_equal({t["txid"] for t in node.listtransactions("*", 1000)}, before)
            node.setfeeexchangerates(self.base)


if __name__ == '__main__':
    AnyAssetFeeCommitRaceTest().main()
