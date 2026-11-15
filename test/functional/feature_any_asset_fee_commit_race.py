#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""One fee whitelist decides a wallet's send from the choice of its fee to its commit.

A connected node reads its price server's list every minute, and an operator
can write the list by hand at any time, so the whitelist can change while the
wallet builds a transaction. The wallet notes the list's generation before it
chooses the fee and commits only if it is unchanged, under the lock every
writer of the list takes, held for the commit alone.

The race is forced: -testfeedecisiondelay holds sendtoaddress between building
the transaction and committing it, and the list is written by hand in that
gap. For each change (the fee asset removed, set to zero, its rate raised so
the fee is worth less, its rate lowered so it is worth more) nothing is sent
and the wallet holds no trace of it; sent again, the fee is chosen under the
list in force and the node takes it. The same list written again changes
nothing, and the send goes through. An issuance races the same way. The hand write does not wait for the held
send. The switch is refused on a chain that does not mine on demand.
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
        self.base = {self.policy: 100000000, self.asset: 200000000}

        for name, rate in (("removed", None), ("zero", 0), ("higher rate", 200000000 * 1000000),
                           ("lower rate", 200000000 // 2), ("the same list written again", "same")):
            self.log.info("The fee list changes during the send: %s" % name)
            self.race(rate)

        self.log.info("An issuance whose fee asset is removed during it sends nothing")
        node = self.nodes[0]
        node.setfeeexchangerates(self.base)
        before = {t["txid"] for t in node.listtransactions("*", 1000)}
        result = {}

        def issue():
            rpc = get_rpc_proxy(node.url, 1, timeout=90)
            try:
                result["ok"] = rpc.issueasset(assetamount=Decimal("1"), tokenamount=0, blind=False, fee_asset=self.asset)
            except Exception as e:
                result["error"] = str(e)
        thread = threading.Thread(target=issue)
        thread.start()
        time.sleep(DELAY_MS / 3000)
        node.setfeeexchangerates({self.policy: 100000000})
        thread.join()
        assert "fee whitelist changed while the transaction was being built" in result.get("error", ""), result
        assert_equal({t["txid"] for t in node.listtransactions("*", 1000)}, before)
        node.setfeeexchangerates(self.base)

        self.log.info("The switch is refused where blocks are not mined on demand")
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(
            extra_args=self.extra_args[0] + ["-chain=sequentia"],
            expected_msg="Error: -testfeedecisiondelay is for test chains only",
            match=ErrorMatch.PARTIAL_REGEX)

    def race(self, new_rate):
        node = self.nodes[0]
        node.setfeeexchangerates(self.base)
        changed = dict(self.base)
        if new_rate is None:
            del changed[self.asset]
        elif new_rate != "same":
            changed[self.asset] = new_rate
        address = node.getnewaddress()
        before = {t["txid"] for t in node.listtransactions("*", 1000)}
        result = {}

        def send():
            rpc = get_rpc_proxy(node.url, 1, timeout=90)
            try:
                result["txid"] = rpc.sendtoaddress(address=address, amount=Decimal("1"), assetlabel=self.asset,
                                                   fee_asset_label=self.asset)
            except Exception as e:
                result["error"] = str(e)
        thread = threading.Thread(target=send)
        thread.start()
        time.sleep(DELAY_MS / 3000)  # the send has built and signed, and waits to commit
        started = time.monotonic()
        node.setfeeexchangerates(changed)
        waited = time.monotonic() - started
        thread.join()
        # The writer never waits on a send being built.
        self.log.info("  the hand write took %.3f s while the send was held" % waited)
        assert waited < DELAY_MS / 3000, waited
        if new_rate == "same":
            assert "error" not in result, result
            assert result["txid"] in node.getrawmempool()
        else:
            assert "error" in result, result
            assert "fee whitelist changed while the transaction was being built" in result["error"], result["error"]
            assert "Nothing was sent" in result["error"], result["error"]
            after = {t["txid"] for t in node.listtransactions("*", 1000)}
            assert_equal(after, before)
            if new_rate is not None and new_rate > 0:
                # Sent again, the fee is chosen under the list in force.
                result["txid"] = node.sendtoaddress(address=address, amount=Decimal("1"), assetlabel=self.asset,
                                                    fee_asset_label=self.asset)
                assert result["txid"] in node.getrawmempool()
        if "txid" in result:
            decoded = node.decoderawtransaction(node.gettransaction(result["txid"])["hex"])
            fee_outputs = [o for o in decoded["vout"] if o["scriptPubKey"]["type"] == "fee"]
            assert_equal([o["asset"] for o in fee_outputs], [self.asset])
            self.generate(node, 1)
            assert_equal(node.gettransaction(result["txid"])["confirmations"], 1)
        node.setfeeexchangerates(self.base)


if __name__ == '__main__':
    AnyAssetFeeCommitRaceTest().main()
