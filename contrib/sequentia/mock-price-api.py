#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Mock market-data API for the Sequentia open-fee-market testnet demo.

Serves RANDOMIZED (random-walk) prices for the demo test assets in exactly the
JSON shape the price server's `jsonapi` source expects, so the price server can
poll it like any real exchange/aggregator and push the resulting rates into each
node's fee-asset whitelist via setfeeexchangerates (persist=false).

This is NOT real market data — it's a deterministic-free random walk for testing
the open fee market end to end.

Endpoints:
  GET /price/<TICKER>  -> {"price":..,"market_cap":..,"volume_24h":..}
  GET /prices          -> {"<TICKER>": {price,market_cap,volume_24h}, ...}
  GET /healthz         -> {"ok": true, "assets": [...]}

Stdlib only. Usage: mock-price-api.py [port] [tick_secs]
"""
import json
import random
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Base prices in the reference unit the node prices fees in (e.g. USD). The
# random walk drifts around these and mean-reverts so rates stay plausible.
BASE = {
    "SEQ": 0.05,       # tSEQ itself, priced like any market asset (USD ref)
    "USDX": 1.00,      # USD stablecoin
    "EURX": 1.08,      # EUR stablecoin
    "GOLD": 2350.0,    # gold ounce
    "WBTC": 64000.0,   # wrapped BTC
    "SILVR": 29.0,     # silver ounce
    "OILX": 78.0,      # oil barrel
}

_lock = threading.Lock()
_state = {
    t: {"price": p,
        "market_cap": random.uniform(5e8, 5e9),
        "volume_24h": random.uniform(5e6, 5e8)}
    for t, p in BASE.items()
}


def _walk(tick_secs):
    while True:
        time.sleep(tick_secs)
        with _lock:
            for t, s in _state.items():
                drift = random.uniform(-0.02, 0.02)              # +/- 2% per tick
                revert = (BASE[t] - s["price"]) / BASE[t] * 0.05  # gentle mean reversion
                s["price"] = max(1e-6, s["price"] * (1 + drift + revert))
                s["market_cap"] = random.uniform(5e8, 5e9)
                s["volume_24h"] = random.uniform(5e6, 5e8)


class Handler(BaseHTTPRequestHandler):
    def _json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        with _lock:
            if self.path == "/prices":
                self._json(_state); return
            if self.path == "/healthz":
                self._json({"ok": True, "assets": sorted(_state)}); return
            if self.path.startswith("/price/"):
                t = self.path.split("/price/", 1)[1].strip("/").upper()
                if t in _state:
                    self._json(_state[t]); return
        self._json({"error": "not found"}, 404)

    def log_message(self, *_a):  # quiet
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8088
    tick = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    threading.Thread(target=_walk, args=(tick,), daemon=True).start()
    print("mock price API on 127.0.0.1:%d, tick=%.0fs, assets: %s"
          % (port, tick, ", ".join(sorted(BASE))), flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
