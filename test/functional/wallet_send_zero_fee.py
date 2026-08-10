#!/usr/bin/env python3
# Copyright (c) 2017-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A zero fee rate builds a transaction with no fee output, and this chain refuses it.

SEQUENTIA: upstream this test asserted that a zero fee_rate send confirms and moves
the balances, because a transaction carrying no fee output was an ordinary shape. On
a chain with the open fee market it is not. The fee output is what NAMES the asset a
fee is denominated in, so a transaction carrying none names no fee asset at all, and
the mempool -- which has to account for fees per asset -- rejects it as
bad-txns-no-fee (the M5 audit check, src/validation.cpp).

The test is aimed at what is true on this chain rather than at a configuration no
Sequentia chain runs, so it does not reach for -con_any_asset_fees=0 to keep the old
assertion alive. It still pins the wallet's behaviour exactly -- a zero fee rate does
build a two-output transaction with no fee output and an empty fee map -- and then
asserts the consequence: the network refuses that transaction, so it never confirms
and the recipient never receives.
"""
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)

class WalletTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.extra_args = [[
            "-blindedaddresses=1",
            "-minrelaytxfee=0",
            "-blockmintxfee=0",
            "-mintxfee=0",
        ]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        # initial state when setup_clean_chain is False
        assert_equal(self.nodes[0].getbalance(), {'bitcoin': Decimal('1250')})
        assert_equal(self.nodes[1].getbalance(), {'bitcoin': Decimal('1250')})
        assert_equal(self.nodes[2].getbalance(), {'bitcoin': Decimal('1250')})
        assert_equal(self.nodes[0].getblockchaininfo()["blocks"], 200)

        # SEQUENTIA: an open-fee-market chain has no default fee asset.
        self.nodes[0].sendtoaddress(address=self.nodes[1].getnewaddress(), amount=10,
                                    fee_asset_label='bitcoin')
        self.nodes[0].sendtoaddress(address=self.nodes[2].getnewaddress(), amount=20,
                                    fee_asset_label='bitcoin')
        self.generate(self.nodes[0], 1)
        assert_equal(self.nodes[0].getblockchaininfo()["blocks"], 201)
        assert_equal(self.nodes[0].getbalance(), {'bitcoin': Decimal('1269.99897200')})
        assert_equal(self.nodes[1].getbalance(), {'bitcoin': Decimal('1260')})
        assert_equal(self.nodes[2].getbalance(), {'bitcoin': Decimal('1270')})

        # A zero fee_rate still builds a transaction that pays no fee, and so adds no
        # fee output. The wallet's side of this is unchanged.
        addr = self.nodes[1].getnewaddress()
        txid = self.nodes[0].sendtoaddress(address=addr, amount=1, fee_rate=0,
                                           fee_asset_label='bitcoin')
        wtx = self.nodes[0].gettransaction(txid, True, True)
        # there should be no fees
        assert "bitcoin" not in wtx["fee"]
        assert_equal(wtx["fee"], {})
        # and no fee output
        decoded = wtx["decoded"]
        assert_equal(len(decoded["vout"]), 2)
        for output in decoded["vout"]:
            assert output["scriptPubKey"]["type"] != "fee"

        # ... and this chain refuses it, because a transaction with no fee output names
        # no asset for its fee to be denominated in. It never reaches the mempool.
        assert_equal(self.nodes[0].getrawmempool(), [])
        assert_equal(self.nodes[0].testmempoolaccept([wtx["hex"]])[0]["reject-reason"],
                     "bad-txns-no-fee")

        # So it cannot be mined either: a block later it is still unconfirmed, and the
        # recipient's balance has not moved.
        self.generate(self.nodes[0], 1)
        assert_equal(self.nodes[0].getblockchaininfo()["blocks"], 202)
        assert_equal(self.nodes[0].gettransaction(txid)["confirmations"], 0)

        # Node 0's spendable balance is unchanged in total, but not for a trivial
        # reason: the refused transaction still ties up the 50 it spends, and exactly
        # 50 matured in the block just generated.
        assert_equal(self.nodes[0].getbalance(), {'bitcoin': Decimal('1269.99897200')})
        assert_equal(self.nodes[1].getbalance(), {'bitcoin': Decimal('1260')})
        assert_equal(self.nodes[2].getbalance(), {'bitcoin': Decimal('1270')})

if __name__ == '__main__':
    WalletTest().main()
