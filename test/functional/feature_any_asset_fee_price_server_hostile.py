#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Whatever a price server, or anyone on the path to it, sends, the node goes on.

Every testnet node is connected to a price server by default, and asks it over
plain HTTP, so its answers are untrusted input. What must hold:
  * a server that answers one byte at a time never holds block connection,
    startup, shutdown or an RPC beyond the request's whole deadline;
  * a number out of any range (an "updated" of 1e9999) is refused like any bad
    answer, and the node does not stop;
  * a list dated ahead of the node's clock is refused and never cached, so it
    cannot make every honest list after it read as older;
  * a reply's headers are capped as well as its body;
  * the display reference-price feed is bounded the same way: a price of 1e9999
    stops nothing.
Run on a custom chain set up as the testnet is (-con_seed_fee_whitelist=0,
-con_default_fee_price_server pointing at a stand-in).
"""

import http.server
import json
import threading
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

TIMEOUT = 5


class StandIn:
    """Answers each path with routes[path]: (status, body), or "drip" (one byte
    a second, never finishing), or ("header", n) (a header line of n bytes)."""

    def __init__(self):
        self.routes = {}
        self.stopping = False
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                route = outer.routes.get(self.path, (404, "{}"))
                try:
                    if route == "drip":
                        self.wfile.write(b"HTTP/1.1 200 OK\r\n")
                        self.wfile.flush()
                        while not outer.stopping:
                            self.wfile.write(b"X")
                            self.wfile.flush()
                            time.sleep(1)
                        return
                    if route[0] == "header":
                        self.wfile.write(b"HTTP/1.1 200 OK\r\nX-Filler: " + b"a" * route[1] + b"\r\n")
                        self.wfile.flush()
                        return
                    status, body = route
                    data = body.encode()
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def url(self, path):
        return "http://127.0.0.1:%d%s" % (self.port, path)

    def publish(self, path, rates, updated=None):
        self.routes[path] = (200, json.dumps({"updated": time.time() if updated is None else updated, "rates": rates}))

    def stop(self):
        self.stopping = True
        self.httpd.shutdown()
        self.httpd.server_close()


class AnyAssetFeePriceServerHostileTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.standin = StandIn()
        self.base = [
            "-blindedaddresses=0",
            "-con_any_asset_fees=1",
            "-con_seed_fee_whitelist=0",
            "-con_default_fee_price_server=%s" % self.standin.url("/feewhitelist"),
            "-feepriceserverpoll=1",
            "-feepriceservertimeout=%d" % TIMEOUT,
        ]
        self.extra_args = [self.base]

    def setup_nodes(self):
        self.add_nodes(self.num_nodes, self.extra_args)
        self.list1 = {"22" * 32: 100000000}
        self.standin.publish("/feewhitelist", self.list1)
        self.start_node(0)

    def run_test(self):
        try:
            self.body()
        finally:
            self.standin.stop()

    def rates(self):
        return {k: v["rate"] for k, v in self.nodes[0].getfeeassetinfo().items() if v["listed"]}

    def wait_error(self, needle):
        self.wait_until(lambda: needle in self.nodes[0].getfeepriceserver().get("last_error", ""), timeout=30)

    def timed(self, fn):
        start = time.time()
        fn()
        return time.time() - start

    def stop(self, empty=False):
        node = self.nodes[0]
        node.stderr.seek(0)
        said = node.stderr.read().decode("utf-8").strip()
        self.stop_node(0, expected_stderr=said)

    def body(self):
        node = self.nodes[0]
        assert_equal(self.rates(), self.list1)

        self.log.info("A slow server never holds block connection, which waits on the scheduler's queue")
        self.standin.routes["/feewhitelist"] = "drip"
        time.sleep(2)  # a poll is now in flight, and stays in flight until its deadline
        took = self.timed(lambda: self.generate(node, 15, sync_fun=self.no_op))
        self.log.info("  generatetoaddress 15 took %.1f s", took)
        assert took < TIMEOUT, took
        took = self.timed(node.syncwithvalidationinterfacequeue)
        assert took < 2, took
        self.wait_error("no complete answer within %d s" % TIMEOUT)
        assert_equal(self.rates(), self.list1)

        self.log.info("...nor an RPC beyond its whole deadline")
        took = self.timed(lambda: assert_raises_rpc_error(-8, "no complete answer within", node.setfeepriceserver, self.standin.url("/feewhitelist")))
        self.log.info("  setfeepriceserver took %.1f s", took)
        assert took < TIMEOUT + 3, took

        self.log.info("...nor shutdown, with a request in flight")
        time.sleep(1.5)
        took = self.timed(lambda: self.stop())
        self.log.info("  shutdown took %.1f s", took)
        assert took < 5, took

        self.log.info("...nor startup beyond its 5 s")
        took = self.timed(lambda: self.start_node(0))
        self.log.info("  startup took %.1f s", took)
        assert took < 5 + 5, took
        assert_equal(self.rates(), self.list1)  # its cache

        self.log.info("A number out of any range is refused, and the node goes on")
        self.standin.routes["/feewhitelist"] = (200, '{"updated":1e9999,"rates":{"' + "aa" * 32 + '":100000000}}')
        self.wait_error("is not a time")
        assert_equal(self.rates(), self.list1)
        assert node.getblockcount() >= 0
        self.standin.routes["/feewhitelist"] = (200, '{"updated":-1e9999,"rates":{"' + "aa" * 32 + '":100000000}}')
        self.wait_error("is not a time")
        self.standin.routes["/feewhitelist"] = (200, '{"updated":1e30,"rates":{"' + "aa" * 32 + '":100000000}}')
        self.wait_error("ahead of this node's clock")

        self.log.info("A list dated ahead of the clock is refused and never cached")
        self.standin.publish("/feewhitelist", {"33" * 32: 5}, updated=time.time() + 3600)
        self.wait_error("ahead of this node's clock")
        assert_equal(self.rates(), self.list1)
        list2 = {"44" * 32: 7}
        self.standin.publish("/feewhitelist", list2)
        self.wait_until(lambda: self.rates() == list2, timeout=30)
        self.standin.routes["/feewhitelist"] = (503, "{}")
        self.stop()
        self.start_node(0)
        assert_equal(self.rates(), list2)

        self.log.info("A reply's headers are capped as well as its body")

        def rss_kb():
            with open("/proc/%d/status" % node.process.pid, encoding="utf8") as f:
                return int(next(line for line in f if line.startswith("VmRSS:")).split()[1])
        before = rss_kb()
        self.standin.routes["/feewhitelist"] = ("header", 64 << 20)
        self.wait_error("larger than %d bytes" % (16 * 1024))
        grew = rss_kb() - before
        self.log.info("  resident memory grew by %d kB for a 64 MiB header", grew)
        assert grew < 16 * 1024, grew
        assert_equal(self.rates(), list2)

        self.log.info("The display reference-price feed: a price of 1e9999 stops nothing")
        self.standin.routes["/prices"] = (200, '{"SEQ": {"price": 1e9999}, "GOLD": 1e9999}')
        self.standin.publish("/feewhitelist", list2)
        self.stop()
        with node.assert_debug_log(["ReferencePrices: the answer could not be read"], timeout=30):
            self.start_node(0, self.base + ["-referencepricesurl=%s" % self.standin.url("/prices"), "-referencepricespoll=1"])
        assert node.getblockcount() >= 0
        assert_equal(self.rates(), list2)


if __name__ == '__main__':
    AnyAssetFeePriceServerHostileTest().main()
