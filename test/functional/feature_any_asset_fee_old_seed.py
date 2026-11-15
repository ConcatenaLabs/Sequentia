#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A node that once seeded its fee whitelist and no longer does keeps no seed.

The testnet used to start every node with the policy asset alone on its fee
whitelist, and the node saved its whitelist at every start, so every testnet
data directory holds an exchangerates.json with that one entry, written there
without anyone choosing it. The testnet no longer seeds. A node upgraded on such
a data directory must not load that file back: it would keep the one-asset list
the open fee market withholds. A list its operator did write is kept.

Reproduced on a custom chain: started seeded, then restarted with
-con_seed_fee_whitelist=0, which is what the testnet now does.

What must hold:
  * a file holding only the old seed is moved aside to exchangerates.json.old-seed,
    and the node starts accepting nothing, says so, and refuses to send;
  * it stays that way across another restart, and the seed never comes back;
  * a list the operator wrote is kept, even one holding the policy asset alone
    at another rate, or the policy asset at 1:1 beside another asset;
  * the check runs once per data directory (recorded in settings.json), so the
    policy asset alone at 1:1, written by the operator afterwards, is kept;
  * a file named with -initialexchangeratesjsonfile is loaded as written, even
    when it holds only the old seed: naming it is the operator's choice.
"""

import json
import os

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
UNSEEDED = ["-con_seed_fee_whitelist=0"]
OTHER_ASSET = "11" * 32


class AnyAssetFeeOldSeedTest(BitcoinTestFramework):
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
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def rates_file(self):
        return os.path.join(self.nodes[0].datadir, self.chain, "exchangerates.json")

    def read(self, path):
        with open(path, encoding="utf8") as f:
            return json.load(f)

    def write(self, path, data):
        with open(path, "w", encoding="utf8") as f:
            json.dump(data, f)

    def restart(self, extra, stopped_empty=False):
        self.stop_node(0, expected_stderr=STARTUP_NOTICE if stopped_empty else "")
        self.start_node(0, self.extra_args[0] + extra)

    def run_test(self):
        node = self.nodes[0]
        aside = self.rates_file() + ".old-seed"

        self.log.info("A seeded node saves the seed to its data directory, unasked")
        policy_label = list(node.getfeeexchangerates())[0]
        seed = {policy_label: 100000000}
        assert_equal(node.getfeeexchangerates(), seed)
        assert_equal(self.read(self.rates_file()), seed)
        self.generate(node, 101)  # the genesis outputs mature, so the last step can send

        self.log.info("Restarted on a chain that no longer seeds, it moves that file aside and lists nothing")
        self.stop_node(0)
        with node.assert_debug_log(["held only the old seeded fee list", "moved it to"]):
            self.start_node(0, self.extra_args[0] + UNSEEDED)
        assert_equal(node.getfeeexchangerates(), {})
        assert_equal(self.read(aside), seed)
        assert_equal(self.read(self.rates_file()), {})

        self.log.info("It refuses to send, and names the whitelist as what is missing")
        assert_raises_rpc_error(-6, NOT_SET_UP, node.sendtoaddress,
                                address=node.getnewaddress(), amount=1, fee_asset_label=policy_label)

        self.log.info("Another restart keeps it empty; the seed never comes back")
        self.restart(UNSEEDED, stopped_empty=True)
        assert_equal(node.getfeeexchangerates(), {})
        assert_equal(self.read(aside), seed)

        self.log.info("A list the operator wrote is kept: the policy asset alone at another rate")
        node.setfeeexchangerates({policy_label: 200000000})
        self.restart(UNSEEDED, stopped_empty=True)  # this run began empty
        assert_equal(node.getfeeexchangerates(), {policy_label: 200000000})

        self.log.info("...and the policy asset at 1:1 beside another asset")
        both = {policy_label: 100000000, OTHER_ASSET: 5000}
        node.setfeeexchangerates(both)
        self.restart(UNSEEDED)
        assert_equal(node.getfeeexchangerates(), both)

        self.log.info("An emptied list stays empty")
        node.setfeeexchangerates({})
        self.restart(UNSEEDED)
        assert_equal(node.getfeeexchangerates(), {})

        self.log.info("The check runs once: the policy asset alone at 1:1, written by the operator, is kept")
        node.setfeeexchangerates(seed)
        self.restart(UNSEEDED, stopped_empty=True)  # this run began with the emptied list
        assert_equal(node.getfeeexchangerates(), seed)
        assert_equal(self.read(self.rates_file()), seed)

        self.log.info("A file named with -initialexchangeratesjsonfile is loaded as written, the seed included")
        named = os.path.join(self.options.tmpdir, "named-rates.json")
        self.write(named, seed)
        node.setfeeexchangerates({})
        self.restart(UNSEEDED + ["-initialexchangeratesjsonfile=%s" % named])
        assert_equal(node.getfeeexchangerates(), seed)

        self.log.info("Once it lists an asset, it sends")
        txid = node.sendtoaddress(address=node.getnewaddress(), amount=1, fee_asset_label=policy_label)
        assert txid in node.getrawmempool()


if __name__ == '__main__':
    AnyAssetFeeOldSeedTest().main()
