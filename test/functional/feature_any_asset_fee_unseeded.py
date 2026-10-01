#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A node on a chain that does not seed the fee whitelist starts accepting nothing.

That is how the Sequentia mainnet behaves: no asset is accepted for fees until
the operator writes the whitelist, by hand or through a price server, so no
asset is accepted merely by default. Exercised here on a custom chain with
-con_seed_fee_whitelist=0.

What must hold for such a node:
  * it validates and stores blocks, so its wallet RECEIVES;
  * it refuses to SEND, and says the whitelist is what is missing;
  * it relays nothing;
  * once the operator writes a whitelist it sends normally;
  * the state survives a restart in both directions: an untouched node comes
    back empty, a configured one comes back configured.
"""

import json
import os
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

NOT_SET_UP = "This node accepts no asset for transaction fees yet"
STARTUP_NOTICE = (
    "Warning: This node accepts no asset for transaction fees, so it will not send or relay "
    "transactions. It still validates the chain, and its wallet can still receive. To change "
    "that, write the fee whitelist: list assets and prices in exchangerates.json or with "
    "setfeeexchangerates, or set up a price server to keep it current."
)


class AnyAssetFeeUnseededTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        common = [
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
        ]
        # node0 is an ordinary seeded node: it holds the coins and produces the
        # blocks. node1 is the one under test: its whitelist is not seeded.
        self.extra_args = [common + ["-anyonecanspendaremine=1"],
                           common + ["-con_seed_fee_whitelist=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def whitelist_file(self, node):
        return os.path.join(node.datadir, self.chain, "exchangerates.json")

    def restart_unconfigured(self):
        """Restart node1 while its whitelist is empty: it says so on stderr each time."""
        self.stop_node(1, expected_stderr=STARTUP_NOTICE)
        self.start_node(1)
        self.connect_nodes(0, 1)

    def run_test(self):
        seeded, unseeded = self.nodes

        self.log.info("A seeded node accepts the policy asset; an unseeded one accepts nothing")
        policy = list(seeded.getfeeexchangerates())[0]
        assert_equal(seeded.getfeeexchangerates(), {policy: 100000000})
        assert_equal(unseeded.getfeeexchangerates(), {})
        assert_equal(unseeded.getfeeacceptancepolicy(), {})
        with open(self.whitelist_file(unseeded), encoding="utf8") as f:
            assert_equal(json.load(f), {})

        self.log.info("It says so at startup, on stderr and in the log")
        self.stop_node(1, expected_stderr=STARTUP_NOTICE)
        with unseeded.assert_debug_log(["Warning: This node accepts no asset for transaction fees"]):
            self.start_node(1)
        self.connect_nodes(0, 1)

        self.log.info("It still receives: a payment confirmed in someone else's block arrives")
        self.generate(seeded, 101)
        addr = unseeded.getnewaddress()
        seeded.sendtoaddress(address=addr, amount=10, fee_asset_label=policy)
        self.generate(seeded, 1)
        self.sync_blocks()
        assert_equal(unseeded.getbalance()[policy], 10)

        self.log.info("It does not relay: a neighbour's transaction never enters its mempool")
        txid = seeded.sendtoaddress(address=seeded.getnewaddress(), amount=1, fee_asset_label=policy)
        assert txid in seeded.getrawmempool()
        time.sleep(3)  # long enough for the announcement to have arrived and been refused
        assert_equal(unseeded.getrawmempool(), [])
        self.generate(seeded, 1)
        self.sync_blocks()

        self.log.info("It refuses to send, and names the whitelist as what is missing")
        assert_raises_rpc_error(-6, NOT_SET_UP, unseeded.sendtoaddress,
                                address=seeded.getnewaddress(), amount=1, fee_asset_label=policy)

        self.log.info("An untouched node comes back empty after a restart")
        self.restart_unconfigured()
        assert_equal(unseeded.getfeeexchangerates(), {})

        self.log.info("Once the operator writes the whitelist, it sends")
        unseeded.setfeeexchangerates({policy: 100000000})
        txid = unseeded.sendtoaddress(address=seeded.getnewaddress(), amount=1, fee_asset_label=policy)
        self.sync_mempools()
        assert txid in seeded.getrawmempool()
        self.generate(seeded, 1)
        self.sync_blocks()
        assert_equal(unseeded.gettransaction(txid)["confirmations"], 1)

        self.log.info("A configured node comes back configured, and says nothing")
        self.stop_node(1, expected_stderr=STARTUP_NOTICE)  # this run began unconfigured
        self.start_node(1)
        assert_equal(unseeded.getfeeexchangerates(), {policy: 100000000})


if __name__ == '__main__':
    AnyAssetFeeUnseededTest().main()
