#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""One fee whitelist decides a wallet's send from the choice of its fee to its commit.

A connected node reads its price server's list every minute, and an operator
can write the list by hand at any time, so the whitelist can change while the
wallet builds a transaction. The wallet holds the fee decision from fee
selection through commit, and every writer of the whitelist waits for it: a
change that arrives in between lands after the commit, and the node accepts
the transaction under the list its fee was chosen under.

The race is forced: -testfeedecisiondelay holds sendtoaddress between building
the transaction and committing it, and the list is written by hand in that
gap. For each change (the fee asset removed, set to zero, its rate raised so
the fee is worth less, its rate lowered so the fee is worth more) the send
returns a transaction the node took, and the hand write completes only after
it.
"""

import threading
import time
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
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
        self.base = {self.policy: 100000000, self.asset: 200000000}

        for name, rate in (("removed", None), ("zero", 0), ("higher rate", 200000000 * 1000000),
                           ("lower rate", 200000000 // 2)):
            self.log.info("The fee asset's rate changes during the send: %s" % name)
            self.race(rate)

    def race(self, new_rate):
        node = self.nodes[0]
        node.setfeeexchangerates(self.base)
        changed = dict(self.base)
        if new_rate is None:
            del changed[self.asset]
        else:
            changed[self.asset] = new_rate
        address = node.getnewaddress()
        result = {}

        def send():
            rpc = get_rpc_proxy(node.url, 1, timeout=90)
            try:
                result["txid"] = rpc.sendtoaddress(address=address, amount=Decimal("1"), assetlabel=self.asset,
                                                   fee_asset_label=self.asset)
            except Exception as e:
                result["error"] = str(e)
            result["sent_at"] = time.monotonic()
        thread = threading.Thread(target=send)
        thread.start()
        time.sleep(DELAY_MS / 3000)  # the send has built and signed, and waits to commit
        node.setfeeexchangerates(changed)
        written_at = time.monotonic()
        thread.join()
        assert "error" not in result, result["error"]
        # The hand write waited for the commit.
        assert written_at >= result["sent_at"], (written_at, result["sent_at"])
        tx = node.gettransaction(result["txid"])
        assert_equal(tx.get("abandoned", False), False)
        assert_equal(tx["details"][0]["abandoned"] if "abandoned" in tx["details"][0] else False, False)
        # The node took it under the list its fee was chosen under.
        decoded = node.decoderawtransaction(tx["hex"])
        fee_outputs = [o for o in decoded["vout"] if o["scriptPubKey"]["type"] == "fee"]
        assert_equal(len(fee_outputs), 1)
        assert_equal(fee_outputs[0]["asset"], self.asset)
        assert fee_outputs[0]["value"] > 0
        # A change that refuses the fee evicts the transaction afterwards, as
        # it does any other: under the old list again, it is offered once more
        # and mined.
        node.setfeeexchangerates(self.base)
        if result["txid"] not in node.getrawmempool():
            node.sendrawtransaction(tx["hex"])
        self.generate(node, 1)
        assert_equal(node.gettransaction(result["txid"])["confirmations"], 1)


if __name__ == '__main__':
    AnyAssetFeeCommitRaceTest().main()
