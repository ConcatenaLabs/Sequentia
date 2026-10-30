#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests for the testnet market-data feed (mock-price-api.py).
Run:  python3 contrib/price-server/test_mock_price_api.py
Stdlib only, no network: Yahoo is stubbed and the server runs on a free local port.
What is pinned here:
  * a ticker read from a source is never served before its first live reading;
  * every ticker carries `as_of`, which stops moving while its source fails;
  * a reading more than 25% from the price served is held until three in a row agree,
    and one more than 5x away, or not a positive number, is rejected;
  * the mock tSEQ price moves at most about 0.1% a tick and stays within +/-10%.
"""
import importlib.util
import json
import os
import threading
import time
import unittest
import urllib.request
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))


def load():
    spec = importlib.util.spec_from_file_location("mock_price_api", os.path.join(HERE, "mock-price-api.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class Judge(unittest.TestCase):
    def setUp(self):
        self.m = load()

    def test_first_reading_is_taken(self):
        self.assertEqual(self.m._judge("GOLD", 4000.0), "take")

    def test_small_moves_are_taken(self):
        self.m._set("GOLD", 4000.0)
        self.assertEqual(self.m._judge("GOLD", 4900.0), "take")
        self.assertEqual(self.m._judge("GOLD", 3300.0), "take")

    def test_a_jump_is_held_until_three_readings_agree(self):
        self.m._set("GOLD", 4000.0)
        self.assertEqual(self.m._judge("GOLD", 2350.0), "hold")
        self.assertEqual(self.m._judge("GOLD", 2351.0), "hold")
        self.assertEqual(self.m._judge("GOLD", 2352.0), "take")

    def test_a_reading_back_in_band_ends_the_hold(self):
        self.m._set("GOLD", 4000.0)
        self.assertEqual(self.m._judge("GOLD", 2350.0), "hold")
        self.assertEqual(self.m._judge("GOLD", 2350.0), "hold")
        self.assertEqual(self.m._judge("GOLD", 4010.0), "take")
        self.assertEqual(self.m._judge("GOLD", 2350.0), "hold")

    def test_held_readings_that_disagree_are_not_taken(self):
        self.m._set("GOLD", 4000.0)
        for v in (2350.0, 1500.0, 2350.0):
            self.assertEqual(self.m._judge("GOLD", v), "hold")

    def test_wild_and_invalid_readings_are_rejected(self):
        self.m._set("GOLD", 4000.0)
        for v in (40.0, 400000.0, 0.0, -1.0, float("nan"), float("inf"), "x", None):
            self.assertEqual(self.m._judge("GOLD", v), "reject", v)


class Served(unittest.TestCase):
    def setUp(self):
        self.m = load()
        self.reads = {"GC=F": [4021.5]}

        def yahoo(sym, timeout=8):
            q = self.reads.get(sym)
            if not q:
                raise OSError("source down")
            return q.pop(0)
        self.m._yahoo = yahoo
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), self.m.Handler)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d/prices" % self.srv.server_address[1]

    def tearDown(self):
        self.srv.shutdown()

    def get(self):
        with urllib.request.urlopen(self.url, timeout=5) as r:
            return json.load(r)

    def tick(self):
        """One pass of the refresh loop, without its sleep."""
        def stop(_):
            raise StopIteration
        sleep, self.m.time.sleep = self.m.time.sleep, stop
        try:
            self.m._refresh(0)
        except StopIteration:
            pass
        finally:
            self.m.time.sleep = sleep

    def test_nothing_read_from_a_source_is_served_before_its_first_reading(self):
        self.m._fixed()
        self.m._walk_seq()
        d = self.get()
        self.assertEqual(sorted(d), ["SEQ", "USDC", "USDX"])
        self.tick()
        d = self.get()
        self.assertEqual(d["GOLD"]["price"], 4021.5)
        self.assertNotIn("SILVR", d)   # its source never answered
        for t, v in d.items():
            self.assertIsInstance(v["as_of"], int, t)

    def test_as_of_stops_while_the_source_fails(self):
        self.tick()
        at = self.get()["GOLD"]["as_of"]
        time.sleep(1.1)
        self.tick()                    # GC=F has no more readings: it fails
        d = self.get()
        self.assertEqual(d["GOLD"]["price"], 4021.5)
        self.assertEqual(d["GOLD"]["as_of"], at)
        self.assertGreater(d["SEQ"]["as_of"], at)

    def test_a_seeded_price_is_static(self):
        body = json.dumps({"ticker": "ACME", "price": 2.5}).encode()
        req = urllib.request.Request(self.url.replace("/prices", "/seed"), data=body, method="POST")
        urllib.request.urlopen(req, timeout=5).read()
        d = self.get()["ACME"]
        self.assertTrue(d["static"])
        self.assertIsInstance(d["as_of"], int)


class Walk(unittest.TestCase):
    def test_tseq_walks_slowly_within_its_band(self):
        m = load()
        m._walk_seq()
        prev, top = m._state["SEQ"]["price"], 0.0
        for _ in range(20000):
            m._walk_seq()
            cur = m._state["SEQ"]["price"]
            top = max(top, abs(cur / prev - 1))
            self.assertTrue(m.SEQ_DAY1 * 0.9 <= cur <= m.SEQ_DAY1 * 1.1)
            prev = cur
        self.assertLess(top, 0.001)


if __name__ == "__main__":
    unittest.main(verbosity=2)
