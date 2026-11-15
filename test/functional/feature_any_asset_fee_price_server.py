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
  * two states: dynamic is connected to a price server, static is written by
    hand. A price server never writes to a node: setfeeexchangerates with
    persist=false is refused and changes nothing. A hand write ends the
    connection; setfeepriceserver connects again, or disconnects keeping the
    list. Operator writes are serialised: of a connect and a hand write, the
    later one is in force and recorded;
  * who decides, at every start: the command line, then the choice recorded in
    settings.json (unless the configuration file changed since), then the
    configuration file, then the network's default, which yields to a list
    written by hand; getfeepriceserver says which (decided_by);
  * upgrades and downgrades: a file holding only the old seed, or nothing, is
    not a choice: the default connects over it, and a static node does not load
    it, unless this node recorded writing exactly that list; startup never
    writes the file; a deleted settings.json is decided again by the file;
  * your own price server, the real one, is connected by its /feewhitelist
    like any other;
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
    get_rpc_proxy,
)

WAITING = "is connected to the price server at"
EMPTY_STATIC = "Warning: This node accepts no asset for transaction fees"
EMPTY_CONNECTED = "Warning: This node is connected to the price server at"
NOT_A_PRICE_SERVER = "is not a price server"


class StandIn:
    """A stand-in price server, price feed and registry on one port. Each path
    answers what `routes` holds for it: (status, body), or (status, body,
    seconds to wait first)."""

    def __init__(self):
        self.routes = {}
        self.hits = {}
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                outer.hits[self.path] = outer.hits.get(self.path, 0) + 1
                route = outer.routes.get(self.path, (404, "{}"))
                status, body = route[0], route[1]
                if len(route) > 2:
                    time.sleep(route[2])
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
        self.serve_and_expect_refusal(" " * (1 << 20) + "{}", "the reply is too large")

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

        self.log.info("A price server never writes to a node: persist=false is refused and changes nothing")
        self.standin.publish("/feewhitelist", self.list1)
        self.wait_until(lambda: self.rates() == self.list1, timeout=20)
        pushed = {self.policy: 70000000}
        assert_raises_rpc_error(-8, "persist=false is refused: a price server never writes to a node",
                                self.n1().setfeeexchangerates, pushed, False)
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], True)
        assert_equal(state["decided_by"], "default")
        assert "connection" not in state
        assert_equal(self.rates(), self.list1)

        self.log.info("A hand write is static, and stays so across a restart, decided by the recorded choice")
        mine = {self.policy: 80000000}
        self.n1().setfeeexchangerates(mine)
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], False)
        assert_equal(state["whitelist"], "static")
        assert_equal(state["disconnected_because"], "the fee whitelist was written by hand")
        self.restart1()
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], False)
        assert_equal(state["decided_by"], "settings")
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

        self.log.info("-nofeepriceserver writes nothing at startup, so the default connects next time")
        assert not os.path.exists(self.rates_file())
        self.stop1(empty=True)
        self.start_node(1)
        assert_equal(self.n1().getfeepriceserver()["connected"], True)
        assert_equal(self.rates(), self.list1)
        assert not os.path.exists(self.rates_file())

        self.log.info("Downgrade and upgrade: the old seed an older binary saves is not a choice")
        self.stop1()
        self.reset_datadir_fee_state()
        seed = json.dumps({self.policy: 100000000})
        with open(self.rates_file(), "w", encoding="utf8") as f:
            f.write(seed)
        self.start_node(1)
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], True)
        assert_equal(state["decided_by"], "default")
        assert_equal(self.rates(), self.list1)
        with open(self.rates_file(), encoding="utf8") as f:
            assert_equal(f.read(), seed)  # left as it was: startup writes nothing

        self.log.info("...and -nofeepriceserver does not load it: never a one-asset list nobody chose")
        with self.n1().assert_debug_log(["which no one chose, so it is not loaded"]):
            self.restart1(["-nofeepriceserver"])
        assert_equal(self.rates(), {})
        self.restart1(empty=True)

        self.log.info("The same list written by hand is a choice, kept across a restart")
        self.n1().setfeeexchangerates({self.policy: 100000000})
        self.restart1()
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], False)
        assert_equal(self.rates(), {self.policy: 100000000})
        self.restart1(["-nofeepriceserver"])
        assert_equal(self.rates(), {self.policy: 100000000})

        self.log.info("A deleted settings.json: decided again by what the file holds")
        self.stop1()
        os.remove(os.path.join(self.n1().datadir, self.chain, "settings.json"))
        self.start_node(1)
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], True)
        assert_equal(state["decided_by"], "default")
        assert_equal(self.rates(), self.list1)

        self.log.info("Upgrade: a list the operator wrote is kept, static")
        self.stop1()
        self.reset_datadir_fee_state()
        with open(self.rates_file(), "w", encoding="utf8") as f:
            json.dump({self.policy: 100000000, self.other: 5}, f)
        self.start_node(1)
        assert_equal(self.n1().getfeepriceserver()["connected"], False)
        assert_equal(self.rates(), {self.policy: 100000000, self.other: 5})

        state = self.n1().getfeepriceserver()
        assert_equal(state["decided_by"], "whitelist file")

        self.log.info("A URL the operator gives is connected even over a list the operator wrote")
        self.restart1(["-feepriceserver=%s" % self.standin.url("/feewhitelist")])
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], True)
        assert_equal(state["decided_by"], "command line")
        assert_equal(self.rates(), self.list1)

        self.log.info("The configuration file against settings.json: the last explicit choice wins")
        self.configuration_file_against_settings()

        self.log.info("Operator writes are serialised: a hand write during a connect lands after it")
        self.connect_races_a_hand_write()

        self.log.info("Your own price server: the real one, with no login to the node, connected by address")
        self.own_price_server()

    def set_conf(self, line):
        conf = os.path.join(self.n1().datadir, "elements.conf")
        with open(conf, encoding="utf8") as f:
            kept = [l for l in f.read().splitlines() if not l.startswith("feepriceserver=")]
        if line:
            kept.insert(0, line)  # before any [section], so it applies to every chain
        with open(conf, "w", encoding="utf8") as f:
            f.write("\n".join(kept) + "\n")

    def configuration_file_against_settings(self):
        self.stop1()
        self.reset_datadir_fee_state()
        self.standin.publish("/list2", {self.other: 400000000})
        self.set_conf("feepriceserver=%s" % self.standin.url("/feewhitelist"))
        self.start_node(1)
        state = self.n1().getfeepriceserver()
        assert_equal((state["connected"], state["decided_by"]), (True, "configuration file"))
        # A hand write is the later choice: it holds at the next start, and the
        # status says where it came from.
        mine = {self.policy: 90000000}
        self.n1().setfeeexchangerates(mine)
        self.restart1()
        state = self.n1().getfeepriceserver()
        assert_equal((state["connected"], state["decided_by"]), (False, "settings"))
        assert_equal(self.rates(), mine)
        # Editing the configuration file afterwards is the later choice again.
        self.stop1()
        self.set_conf("feepriceserver=%s" % self.standin.url("/list2"))
        self.start_node(1)
        state = self.n1().getfeepriceserver()
        assert_equal((state["connected"], state["decided_by"], state["url"]),
                     (True, "configuration file", self.standin.url("/list2")))
        assert_equal(self.rates(), {self.other: 400000000})
        self.stop1()
        self.set_conf(None)
        self.start_node(1)

    def connect_races_a_hand_write(self):
        self.standin.routes["/slow"] = (200, json.dumps({"updated": time.time(), "rates": {self.other: 1}}), 3)
        self.n1().setfeeexchangerates({self.policy: 1})
        result = {}

        def connect():
            rpc = get_rpc_proxy(self.n1().url, 1, timeout=60)
            result["connect"] = rpc.setfeepriceserver(self.standin.url("/slow"))
        thread = threading.Thread(target=connect)
        thread.start()
        time.sleep(1)  # the connect is asking the slow server now
        hand = {self.policy: 123456789}
        self.n1().setfeeexchangerates(hand)
        thread.join()
        state = self.n1().getfeepriceserver()
        assert_equal(state["connected"], False)
        assert_equal(self.rates(), hand)
        self.restart1()
        assert_equal(self.n1().getfeepriceserver()["connected"], False)
        assert_equal(self.rates(), hand)

    def own_price_server(self):
        """Run contrib/price-server/price_server.py against the stand-in's feed
        and registry, with no node RPC in its config, and connect the node to
        its /feewhitelist, as the desktop wallet's "Run my own price server"
        and the documented way do."""
        import subprocess
        gold = "33" * 32
        self.standin.routes["/registry"] = (200, json.dumps({
            self.policy: ["example.test", "SEQ", "Sequence", 8, 1, 0],
            gold: ["example.test", "GOLD", "Gold", 8, 1, 0]}))
        self.standin.routes["/feed"] = (200, json.dumps({
            "SEQ": {"price": 0.5, "market_cap": 1e9, "volume_24h": 1e7},
            "GOLD": {"price": 3000.0, "market_cap": 1e9, "volume_24h": 1e7}}))
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config = os.path.join(self.options.tmpdir, "own-price-server.json")
        with open(config, "w", encoding="utf8") as f:
            json.dump({"poll_interval_secs": 2, "registry_url": self.standin.url("/registry"),
                       "source": {"url": self.standin.url("/feed"), "quote_currency": "USD", "format": "sequentia"},
                       "feed_aliases": {}, "default_thresholds": {"require": "all"}, "exceptions": {},
                       "ui": {}}, f)
        script = os.path.join(self.config["environment"]["SRCDIR"], "contrib", "price-server", "price_server.py")
        log = open(os.path.join(self.options.tmpdir, "own-price-server.log"), "w", encoding="utf8")
        proc = subprocess.Popen(["python3", "-I", script, "--config", config, "--ui-port", str(port)],
                                stdout=log, stderr=subprocess.STDOUT)
        try:
            url = "http://127.0.0.1:%d/feewhitelist" % port

            def connects():
                try:
                    self.n1().setfeepriceserver(url)
                    return True
                except Exception:
                    return False
            self.wait_until(connects, timeout=60)
            state = self.n1().getfeepriceserver()
            assert_equal(state["connected"], True)
            assert_equal(state["whitelist"], "dynamic")
            assert_equal(state["url"], url)
            assert_equal(self.rates(), {self.policy: 50000000, gold: 300000000000})
        finally:
            proc.terminate()
            proc.wait(timeout=10)
            log.close()

    def reset_datadir_fee_state(self):
        """Forget every fee-list choice node1 has made, as a fresh data directory
        would: the operator's file, the cache and the recorded setting."""
        chain_dir = os.path.join(self.n1().datadir, self.chain)
        for name in ("exchangerates.json", "feepriceserver.json"):
            path = os.path.join(chain_dir, name)
            if os.path.exists(path):
                os.remove(path)
        settings = os.path.join(chain_dir, "settings.json")
        if os.path.exists(settings):
            with open(settings, encoding="utf8") as f:
                data = json.load(f)
            for key in ("feepriceserver", "feepriceserverconf", "feelisthand"):
                data.pop(key, None)
            with open(settings, "w", encoding="utf8") as f:
                json.dump(data, f)


if __name__ == '__main__':
    AnyAssetFeePriceServerTest().main()
