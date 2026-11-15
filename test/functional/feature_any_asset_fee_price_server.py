#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A node connected to a price server: its fee whitelist is dynamic.

A fee whitelist is static when it is written by hand, and dynamic when a price
server updates it. On the testnet a node is connected by default to the
network's price server and takes the list that server publishes, so its
whitelist is populated as it starts and kept current; the mainnet has no default
price server. Reproduced on a custom chain with -con_seed_fee_whitelist=0 and
-con_default_fee_price_server pointing at a stand-in price server.

What must hold:
  * the first list is asked for at startup, so it is in force before the wallet
    can be asked to send, and no asset is listed before it;
  * until a list arrives the node lists nothing and its wallet says it is waiting
    for the price server; nothing falls back to one asset;
  * a list is taken whole or not at all: an empty, malformed, oversized or older
    list, or one with an invalid entry, is refused and the list in force is kept;
  * a price feed API's answer or the registry's is never taken as a whitelist:
    only a price server's published list is, and the error says the address is
    not a price server;
  * the server unreachable: the last list is kept, and so is its cache across a
    restart;
  * one writer: the first write by anyone else ends the connection for good,
    across restarts, and an emptied list stays empty; setfeepriceserver connects
    again, or disconnects keeping the list;
  * upgrades: a data directory holding only the old seed connects; one whose
    operator wrote a list keeps it, static; an operator's own price server
    pushing to the node takes over at its first push;
  * the display reference-price feed never writes the whitelist.
"""

import http.server
import json
import os
import socket
import threading
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

WAITING = "is connected to the price server at"
EMPTY_STATIC = "Warning: This node accepts no asset for transaction fees"
EMPTY_CONNECTED = "Warning: This node is connected to the price server at"
NOT_A_PRICE_SERVER = "is not a price server"


class StandIn:
    """A stand-in price server, price feed and registry on one port. Each path
    answers what `routes` holds for it: (status, body)."""

    def __init__(self):
        self.routes = {}
        self.hits = {}
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                outer.hits[self.path] = outer.hits.get(self.path, 0) + 1
                status, body = outer.routes.get(self.path, (404, "{}"))
                data = body.encode() if isinstance(body, str) else body
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # the node hangs up on an answer over its size cap, as it should

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:%d%s" % (self.port, path)

    def publish(self, path, rates, updated=None, **extra):
        body = {"updated": time.time() if updated is None else updated, "quote_currency": "USD",
                "reference_unit": {"api_units_per_reference_unit": 1.0}, "rates": rates, "decisions": []}
        body.update(extra)
        self.routes[path] = (200, json.dumps(body))

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class AnyAssetFeePriceServerTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.standin = StandIn()
        common = [
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-con_any_asset_fees=1",
        ]
        # node0 holds the coins and produces the blocks, seeded as custom chains
        # are. node1 is the one under test, set up as the testnet is: no seed,
        # and a default price server.
        self.testnet_like = ["-con_seed_fee_whitelist=0",
                             "-con_default_fee_price_server=%s" % self.standin.url("/feewhitelist"),
                             "-feepriceserverpoll=1"]
        self.extra_args = [common + ["-anyonecanspendaremine=1"], common + self.testnet_like]
        self.common = common

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_nodes(self):
        # node1 must find a list published at its first start, and the list
        # names the policy asset, which only a running node can say.
        self.add_nodes(self.num_nodes, self.extra_args)
        self.start_node(0)
        self.policy = self.nodes[0].getsidechaininfo()["pegged_asset"]
        self.other = "22" * 32
        self.list1 = {self.policy: 50000000, self.other: 300000000}
        self.standin.publish("/feewhitelist", self.list1)
        self.start_node(1)
        self.import_deterministic_coinbase_privkeys()

    def run_test(self):
        try:
            self.body()
        finally:
            self.standin.stop()

    # -- helpers ------------------------------------------------------------

    def n1(self):
        return self.nodes[1]

    def rates(self):
        """node1's whitelist keyed by hex id."""
        out = {}
        for key, entry in self.n1().getfeeassetinfo().items():
            if entry["listed"]:
                out[key] = entry["rate"]
        return out

    def rates_file(self):
        return os.path.join(self.n1().datadir, self.chain, "exchangerates.json")

    def stop1(self, empty=False):
        """Stop node1. A run that began with an empty whitelist said so on
        stderr, in one of the two forms; any other run said nothing."""
        node = self.n1()
        node.stderr.seek(0)
        said = node.stderr.read().decode("utf-8").strip()
        if empty:
            assert said.startswith(EMPTY_STATIC) or said.startswith(EMPTY_CONNECTED), said
        self.stop_node(1, expected_stderr=said if empty else "")

    def restart1(self, extra=None, empty=False):
        self.stop1(empty)
        self.start_node(1, self.extra_args[1] + (extra or []))
        self.connect_nodes(0, 1)

    def wait_error(self, needle):
        self.wait_until(lambda: needle in self.n1().getfeepriceserver().get("last_error", ""), timeout=20)

    def serve_and_expect_refusal(self, body, needle, status=200):
        before = self.rates()
        self.standin.routes["/feewhitelist"] = (status, body)
        self.wait_error(needle)
        assert_equal(self.rates(), before)

    # -- the test -----------------------------------------------------------

    def body(self):
        node0 = self.nodes[0]
        self.generate(node0, 101)
        self.sync_blocks()

        self.log.info("A fresh node is connected to the default price server and has its list as it starts")
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], True)
        assert_equal(state["url"], self.standin.url("/feewhitelist"))
        assert_equal(state["default"], True)
        assert_equal(state["whitelist"], "dynamic")
        assert state["list_written"] > 0
        assert_equal(self.rates(), self.list1)
        assert_equal(state["assets"], 2)

        self.log.info("It sends with a fee in a listed asset")
        addr = self.n1().getnewaddress()
        node0.sendtoaddress(address=addr, amount=10, fee_asset_label=self.policy)
        self.generate(node0, 1)
        self.sync_blocks()
        txid = self.n1().sendtoaddress(address=node0.getnewaddress(), amount=1, fee_asset_label=self.policy)
        self.sync_mempools()
        assert txid in node0.getrawmempool()
        self.generate(node0, 1)
        self.sync_blocks()
        assert_equal(self.n1().gettransaction(txid)["confirmations"], 1)

        self.log.info("A list is taken whole or not at all")
        self.serve_and_expect_refusal(json.dumps({"updated": time.time(), "rates": {}}), "the list is empty")
        self.serve_and_expect_refusal("not json", "not a JSON object")
        self.serve_and_expect_refusal(json.dumps({"updated": time.time(), "rates": {self.policy: 0}}), "is not an integer in")
        self.serve_and_expect_refusal(json.dumps({"updated": time.time(), "rates": {self.policy: 10**19}}), "is not an integer in")
        self.serve_and_expect_refusal(json.dumps({"updated": time.time(), "rates": {self.policy: 1.5}}), "is not an integer in")
        self.serve_and_expect_refusal(json.dumps({"updated": time.time(), "rates": {"bitcoin": 100000000}}), "is not a 64-hex asset id")
        many = {"%064x" % (i + 1): 100000000 for i in range(1001)}
        self.serve_and_expect_refusal(json.dumps({"updated": time.time(), "rates": many}), "more than the 1000 accepted")
        self.serve_and_expect_refusal(json.dumps({"updated": 1.0, "rates": {self.policy: 1}}), "is older")
        self.serve_and_expect_refusal(" " * (1 << 20) + "{}", "larger than")

        self.log.info("A price feed API's answer, or the registry's, is never a whitelist")
        feed = {"SEQ": {"price": 0.17, "market_cap": 4.8e9, "volume_24h": 2.5e8},
                "rates": {"price": 1.0}, "_meta": {"updated": time.time()}}
        self.serve_and_expect_refusal(json.dumps(feed), NOT_A_PRICE_SERVER)
        registry = {self.policy: ["sequentia.io", "tSEQ", "Sequence", 8, 1, 0]}
        self.serve_and_expect_refusal(json.dumps(registry), NOT_A_PRICE_SERVER)
        self.standin.routes["/prices"] = (200, json.dumps(feed))
        assert_raises_rpc_error(-8, NOT_A_PRICE_SERVER, self.n1().setfeepriceserver, self.standin.url("/prices"))
        assert_equal(self.rates(), self.list1)

        self.log.info("The server unreachable: the last list is kept")
        self.serve_and_expect_refusal("{}", "HTTP 503", status=503)
        assert_equal(self.n1().getfeepriceserver()["connected"], True)

        self.log.info("A new list replaces the whole of the old one")
        list2 = {self.policy: 60000000}
        self.standin.publish("/feewhitelist", list2)
        self.wait_until(lambda: self.rates() == list2, timeout=20)
        assert_equal(self.n1().getfeepriceserver()["last_error"], "")

        self.log.info("Its cache is in force at a restart while the server cannot be reached")
        self.standin.routes["/feewhitelist"] = (503, "{}")
        self.restart1()
        assert_equal(self.rates(), list2)
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], True)
        assert "HTTP 503" in state["last_error"]

        self.log.info("The display reference-price feed writes nothing into the whitelist")
        self.restart1(["-referencepricesurl=%s" % self.standin.url("/prices"), "-referencepricespoll=1"])
        self.wait_until(lambda: self.standin.hits.get("/prices", 0) >= 2, timeout=20)
        assert_equal(self.rates(), list2)

        self.log.info("One writer: the first write by the operator ends the connection, for good")
        self.standin.publish("/feewhitelist", self.list1)
        self.wait_until(lambda: self.rates() == self.list1, timeout=20)
        mine = {self.policy: 70000000}
        self.n1().setfeeexchangerates(mine, False)  # as an operator's own price server pushes
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], False)
        assert_equal(state["whitelist"], "static")
        assert "written by" in state["disconnected_because"]
        hits = self.standin.hits.get("/feewhitelist", 0)
        time.sleep(3)
        assert_equal(self.standin.hits.get("/feewhitelist", 0), hits)  # it no longer asks
        assert_equal(self.rates(), mine)

        self.log.info("...and stays ended across a restart")
        self.n1().setfeeexchangerates(mine)  # by hand, kept in exchangerates.json
        self.restart1()
        assert_equal(self.n1().getfeepriceserver()["connected"], False)
        assert_equal(self.rates(), mine)

        self.log.info("setfeepriceserver connects it again; its list replaces the operator's")
        self.n1().setfeepriceserver(self.standin.url("/feewhitelist"))
        assert_equal(self.n1().getfeepriceserver()["connected"], True)
        assert_equal(self.rates(), self.list1)
        self.restart1()
        assert_equal(self.n1().getfeepriceserver()["connected"], True)
        assert_equal(self.n1().getfeepriceserver()["default"], False)

        self.log.info("An emptied list stays empty")
        self.n1().setfeeexchangerates({})
        assert_equal(self.rates(), {})
        self.restart1()
        assert_equal(self.rates(), {})
        assert_equal(self.n1().getfeepriceserver()["connected"], False)

        self.log.info("setfeepriceserver \"\" disconnects and keeps the list in force as the operator's")
        self.n1().setfeepriceserver(self.standin.url("/feewhitelist"))
        assert_equal(self.rates(), self.list1)
        self.n1().setfeepriceserver("")
        assert_equal(self.n1().getfeepriceserver()["connected"], False)
        assert_equal(self.rates(), self.list1)
        self.restart1(empty=True)  # the run that ends began with the emptied list
        assert_equal(self.rates(), self.list1)

        self.log.info("Until the first list arrives, nothing is listed and sends wait for the price server")
        self.standin.routes["/feewhitelist"] = (503, "{}")
        self.stop1()
        self.reset_datadir_fee_state()
        self.start_node(1)
        self.connect_nodes(0, 1)
        assert_equal(self.rates(), {})
        assert_equal(self.n1().getfeepriceserver()["connected"], True)
        assert_raises_rpc_error(-6, WAITING, self.n1().sendtoaddress,
                                address=node0.getnewaddress(), amount=1, fee_asset_label=self.policy)
        self.standin.publish("/feewhitelist", self.list1)
        self.wait_until(lambda: self.rates() == self.list1, timeout=30)
        txid = self.n1().sendtoaddress(address=node0.getnewaddress(), amount=1, fee_asset_label=self.policy)
        assert txid in self.n1().getrawmempool()

        self.log.info("-nofeepriceserver: static and empty, on a chain with a default")
        self.stop1(empty=True)
        self.reset_datadir_fee_state()
        self.start_node(1, self.extra_args[1] + ["-nofeepriceserver"])
        assert_equal(self.n1().getfeepriceserver()["connected"], False)
        assert_equal(self.rates(), {})

        self.log.info("Upgrade: a data directory holding only the old seed connects, and the seed is moved aside")
        self.stop1(empty=True)
        self.reset_datadir_fee_state()
        with open(self.rates_file(), "w", encoding="utf8") as f:
            json.dump({self.policy: 100000000}, f)
        with self.n1().assert_debug_log(["held only the old seeded fee list"]):
            self.start_node(1)
        assert_equal(self.n1().getfeepriceserver()["connected"], True)
        assert_equal(self.rates(), self.list1)
        assert os.path.exists(self.rates_file() + ".old-seed")

        self.log.info("Upgrade: a list the operator wrote is kept, static")
        self.stop1()
        self.reset_datadir_fee_state()
        with open(self.rates_file(), "w", encoding="utf8") as f:
            json.dump({self.policy: 100000000, self.other: 5}, f)
        self.start_node(1)
        assert_equal(self.n1().getfeepriceserver()["connected"], False)
        assert_equal(self.rates(), {self.policy: 100000000, self.other: 5})

        self.log.info("A URL the operator gives is connected even over a list the operator wrote")
        self.restart1(["-feepriceserver=%s" % self.standin.url("/feewhitelist")])
        assert_equal(self.n1().getfeepriceserver()["connected"], True)
        assert_equal(self.rates(), self.list1)

    def reset_datadir_fee_state(self):
        """Forget every fee-list choice node1 has made, as a fresh data directory
        would: the operator's file, the cache and the recorded setting."""
        chain_dir = os.path.join(self.n1().datadir, self.chain)
        for name in ("exchangerates.json", "exchangerates.json.old-seed", "feepriceserver.json"):
            path = os.path.join(chain_dir, name)
            if os.path.exists(path):
                os.remove(path)
        settings = os.path.join(chain_dir, "settings.json")
        if os.path.exists(settings):
            with open(settings, encoding="utf8") as f:
                data = json.load(f)
            data.pop("feepriceserver", None)
            data.pop("feelistseedchecked", None)
            with open(settings, "w", encoding="utf8") as f:
                json.dump(data, f)


if __name__ == '__main__':
    AnyAssetFeePriceServerTest().main()
