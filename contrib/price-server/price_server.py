#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sequentia price server.

A locally run sidecar that maintains the *dynamic* layer of a Sequentia node's
fee-asset whitelist (see doc/sequentia/02-open-fee-market.md):

  1. polls operator-designated market data APIs for each configured asset
     (price, market cap, 24h volume),
  2. applies operator-defined admission thresholds,
  3. converts prices into the node's scaled exchange-rate format
     (atoms of the asset equal in value to 1 unit = 1e8 atoms of the
     reference; i.e. rate = round(price_in_reference * 1e8)),
  4. publishes the result via the node's `setdynamicfeerates` RPC.

Statically pinned rates on the node (setfeeexchangerates) always take
precedence over anything published here. On clean shutdown the dynamic layer
is cleared (fail safe: assets leave the whitelist rather than ride a dead
price). Stdlib only; no external dependencies.

Usage:
    price_server.py --config config.json [--once] [--dry-run]
"""

import argparse
import base64
import json
import logging
import signal
import statistics
import sys
import time
import urllib.request

log = logging.getLogger("price-server")

COIN = 100_000_000


class NodeRPC:
    """Minimal JSON-RPC client for the Sequentia node."""

    def __init__(self, cfg):
        self.url = "http://%s:%d/" % (cfg.get("host", "127.0.0.1"), cfg["port"])
        if "cookie" in cfg:
            with open(cfg["cookie"]) as f:
                auth = f.read().strip()
        else:
            auth = "%s:%s" % (cfg["user"], cfg["password"])
        self.auth_header = "Basic " + base64.b64encode(auth.encode()).decode()
        self.timeout = cfg.get("timeout", 30)

    def call(self, method, *params):
        payload = json.dumps({
            "jsonrpc": "1.0", "id": "price-server",
            "method": method, "params": list(params),
        }).encode()
        req = urllib.request.Request(self.url, data=payload, headers={
            "Authorization": self.auth_header,
            "Content-Type": "application/json",
        })
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            reply = json.loads(resp.read())
        if reply.get("error") is not None:
            raise RuntimeError("node RPC error from %s: %s" % (method, reply["error"]))
        return reply["result"]


def http_get_json(url, timeout):
    req = urllib.request.Request(url, headers={"User-Agent": "sequentia-price-server/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def jsonpath(obj, path):
    """Resolve a dotted path ("a.b.0.c") into a nested JSON object."""
    cur = obj
    for part in path.split("."):
        if isinstance(cur, list):
            cur = cur[int(part)]
        else:
            cur = cur[part]
    return cur


class Source:
    """A market data source for one asset.

    Two kinds are supported:
      - "coingecko": {"kind": "coingecko", "id": "tether", "vs": "usd",
                      "base_url": "https://api.coingecko.com"}
      - "jsonapi":   {"kind": "jsonapi", "url": "...",
                      "price_path": "data.price",
                      "market_cap_path": "...", "volume_24h_path": "..."}
    Each source returns a dict with keys: price (required), market_cap,
    volume_24h (optional, None when the API does not provide them).
    """

    def __init__(self, cfg, timeout):
        self.cfg = cfg
        self.timeout = timeout
        self.name = cfg.get("name") or cfg.get("id") or cfg.get("url", "?")

    def fetch(self):
        kind = self.cfg["kind"]
        if kind == "coingecko":
            base = self.cfg.get("base_url", "https://api.coingecko.com")
            vs = self.cfg.get("vs", "usd")
            url = ("%s/api/v3/simple/price?ids=%s&vs_currencies=%s"
                   "&include_market_cap=true&include_24hr_vol=true"
                   % (base, self.cfg["id"], vs))
            data = http_get_json(url, self.timeout)[self.cfg["id"]]
            return {
                "price": float(data[vs]),
                "market_cap": float(data.get("%s_market_cap" % vs)) if data.get("%s_market_cap" % vs) is not None else None,
                "volume_24h": float(data.get("%s_24h_vol" % vs)) if data.get("%s_24h_vol" % vs) is not None else None,
            }
        elif kind == "jsonapi":
            data = http_get_json(self.cfg["url"], self.timeout)
            out = {"price": float(jsonpath(data, self.cfg["price_path"]))}
            for key, path_key in (("market_cap", "market_cap_path"),
                                  ("volume_24h", "volume_24h_path")):
                out[key] = float(jsonpath(data, self.cfg[path_key])) if path_key in self.cfg else None
            return out
        raise ValueError("unknown source kind: %s" % kind)


class AssetFeed:
    """Collects metrics for one asset across its sources and decides
    admission according to the configured thresholds."""

    def __init__(self, cfg, defaults, timeout):
        self.asset_id = cfg["id"]
        self.label = cfg.get("label", self.asset_id[:8])
        self.sources = [Source(s, timeout) for s in cfg["sources"]]
        self.thresholds = {**defaults, **cfg.get("thresholds", {})}
        self.last_rate = None

    def poll(self):
        """Returns the scaled rate if the asset qualifies, else None."""
        results = []
        for source in self.sources:
            try:
                results.append(source.fetch())
            except Exception as e:
                log.warning("[%s] source %s failed: %s", self.label, source.name, e)

        quorum = int(self.thresholds.get("min_sources", 1))
        if len(results) < quorum:
            log.info("[%s] rejected: only %d/%d sources responded",
                     self.label, len(results), quorum)
            return None

        price = statistics.median(r["price"] for r in results)
        market_caps = [r["market_cap"] for r in results if r["market_cap"] is not None]
        volumes = [r["volume_24h"] for r in results if r["volume_24h"] is not None]
        market_cap = statistics.median(market_caps) if market_caps else None
        volume_24h = statistics.median(volumes) if volumes else None

        # Admission thresholds
        t = self.thresholds
        if price <= 0:
            log.info("[%s] rejected: non-positive price", self.label)
            return None
        if "min_price" in t and price < t["min_price"]:
            log.info("[%s] rejected: price %g below min_price %g", self.label, price, t["min_price"])
            return None
        if "max_price" in t and price > t["max_price"]:
            log.info("[%s] rejected: price %g above max_price %g", self.label, price, t["max_price"])
            return None
        if "min_market_cap" in t:
            if market_cap is None or market_cap < t["min_market_cap"]:
                log.info("[%s] rejected: market cap %s below min_market_cap %g",
                         self.label, market_cap, t["min_market_cap"])
                return None
        if "min_volume_24h" in t:
            if volume_24h is None or volume_24h < t["min_volume_24h"]:
                log.info("[%s] rejected: 24h volume %s below min_volume_24h %g",
                         self.label, volume_24h, t["min_volume_24h"])
                return None
        # Source agreement: max relative spread between sources
        if len(results) > 1 and "max_source_spread" in t:
            prices = [r["price"] for r in results]
            spread = (max(prices) - min(prices)) / price
            if spread > t["max_source_spread"]:
                log.info("[%s] rejected: source price spread %.2f%% above limit",
                         self.label, 100 * spread)
                return None
        # Sanity clamp: reject implausible jumps between polls
        if self.last_rate is not None and "max_change_factor" in t:
            f = t["max_change_factor"]
            new_rate = round(price * COIN)
            if new_rate > self.last_rate * f or new_rate < self.last_rate / f:
                log.warning("[%s] rejected: rate jumped more than %gx between polls "
                            "(old %d, new %d); keeping asset out until it stabilizes",
                            self.label, f, self.last_rate, new_rate)
                self.last_rate = None  # require a fresh baseline
                return None

        rate = round(price * COIN)
        if rate <= 0:
            log.info("[%s] rejected: scaled rate rounds to zero", self.label)
            return None
        self.last_rate = rate
        log.info("[%s] admitted: price=%g rate=%d mcap=%s vol24h=%s",
                 self.label, price, rate, market_cap, volume_24h)
        return rate


class PriceServer:
    def __init__(self, config, dry_run=False):
        self.cfg = config
        self.dry_run = dry_run
        # Publish to one node ("node_rpc") or many ("node_rpcs"). A single box can
        # run a whole committee, so one price server feeds them all — then whichever
        # producer the VRF elects in a round already prices the fee assets.
        if dry_run:
            self.rpcs = []
        elif "node_rpcs" in config:
            self.rpcs = [NodeRPC(n) for n in config["node_rpcs"]]
        else:
            self.rpcs = [NodeRPC(config["node_rpc"])]
        timeout = config.get("source_timeout", 15)
        defaults = config.get("default_thresholds", {})
        self.feeds = [AssetFeed(a, defaults, timeout) for a in config["assets"]]
        self.source_name = config.get("source_name", "price-server")
        self.stopping = False

    def _denominate(self, raw, labels):
        """Optionally re-express every rate relative to a reference asset.

        Sources quote prices in some common currency (e.g. USD). A Sequentia
        node, however, prices fees in its native gas/policy asset, whose rate is
        definitionally 1e8 (you cannot price the reference against itself). When
        `reference_asset_label` is configured we divide every raw rate by the
        reference asset's raw rate, so the reference lands at exactly 1e8 and
        every other asset is expressed in units of it (e.g. "how many SEQ equal
        one unit of GOLD"). Without it, rates are published as the sources quote
        them."""
        ref_label = self.cfg.get("reference_asset_label")
        if not ref_label:
            return dict(raw)
        ref_id = next((aid for aid, lbl in labels.items() if lbl == ref_label), None)
        if ref_id is None or not raw.get(ref_id):
            log.warning("reference asset %r unavailable this round; skipping "
                        "publish to avoid a denomination discontinuity", ref_label)
            return {}
        ref_rate = raw[ref_id]
        return {aid: max(1, round(r * COIN / ref_rate)) for aid, r in raw.items()}

    def poll_once(self):
        raw, labels = {}, {}
        for feed in self.feeds:
            rate = feed.poll()
            if rate is not None:
                raw[feed.asset_id] = rate
                labels[feed.asset_id] = feed.label
        rates = self._denominate(raw, labels)
        if self.dry_run:
            log.info("dry-run: would publish %s", json.dumps(rates))
            return rates
        ok = 0
        for rpc in self.rpcs:
            try:
                rpc.call("setdynamicfeerates", rates, self.source_name)
                ok += 1
            except Exception as e:
                log.warning("publish to %s failed: %s", rpc.url, e)
        log.info("published %d rate(s) to %d/%d node(s)", len(rates), ok, len(self.rpcs))
        return rates

    def run(self):
        interval = self.cfg.get("poll_interval_secs", 60)
        signal.signal(signal.SIGTERM, self._stop)
        signal.signal(signal.SIGINT, self._stop)
        log.info("starting: %d asset(s), poll every %ds", len(self.feeds), interval)
        while not self.stopping:
            try:
                self.poll_once()
            except Exception as e:
                log.error("poll failed: %s", e)
            # sleep in small steps so shutdown stays responsive
            deadline = time.time() + interval
            while not self.stopping and time.time() < deadline:
                time.sleep(0.5)
        # Fail safe: leave no dynamic rates behind on clean shutdown.
        if not self.dry_run:
            for rpc in self.rpcs:
                try:
                    rpc.call("cleardynamicfeerates")
                except Exception as e:
                    log.warning("could not clear dynamic rates on %s: %s", rpc.url, e)
            log.info("cleared dynamic rates on shutdown")

    def _stop(self, _sig, _frame):
        self.stopping = True


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True, help="path to JSON config file")
    parser.add_argument("--once", action="store_true", help="poll once and exit")
    parser.add_argument("--dry-run", action="store_true",
                        help="don't publish to the node, just log decisions")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    with open(args.config) as f:
        config = json.load(f)

    server = PriceServer(config, dry_run=args.dry_run)
    if args.once:
        rates = server.poll_once()
        print(json.dumps(rates, indent=2))
    else:
        server.run()


if __name__ == "__main__":
    sys.exit(main())
