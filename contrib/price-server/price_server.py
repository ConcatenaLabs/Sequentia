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
import collections
import html
import http.server
import json
import logging
import math
import os
import re
import signal
import statistics
import sys
import threading
import time
import urllib.parse
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
        # Issuer identity, operator-declared (or filled from the Asset Registry),
        # used by the issuer_domains / issuer_pubkeys admission criteria.
        self.issuer_domain = cfg.get("issuer_domain")
        self.issuer_pubkey = cfg.get("issuer_pubkey")
        self.last_rate = None
        # Rolling window of recent median prices, for the volatility criterion.
        window = int(self.thresholds.get("volatility_window", 30))
        self.price_history = collections.deque(maxlen=max(2, window))

    def _volatility_ok(self, t):
        """Rolling stddev of log-returns over the window <= max_volatility.
        Returns True until there are enough samples (don't gate on no data)."""
        min_samples = int(t.get("volatility_min_samples", 5))
        hist = list(self.price_history)
        if len(hist) < max(3, min_samples):
            return True
        rets = [math.log(hist[i] / hist[i - 1])
                for i in range(1, len(hist)) if hist[i - 1] > 0 and hist[i] > 0]
        if len(rets) < 2:
            return True
        vol = statistics.pstdev(rets)
        if vol > t["max_volatility"]:
            log.warning("[%s] volatility %.4f over %d samples above max_volatility %g",
                        self.label, vol, len(hist), t["max_volatility"])
            return False
        return True

    def poll(self):
        """Return the scaled rate if the asset qualifies, else None.

        Admission is a configurable rule engine: a set of OPTIONAL criteria
        combined by `require` ("all" = every configured criterion must pass,
        "any" = at least one), plus always_admit / always_reject exceptions and a
        few always-on sanity checks (quorum, positive price/rate). Adding a new
        criterion is a single entry in the checks list below."""
        t = self.thresholds
        aid, lbl = self.asset_id, self.label

        def _listed(key):
            vals = t.get(key, [])
            return aid in vals or lbl in vals

        # Exceptions: reject wins over admit.
        if _listed("always_reject"):
            log.info("[%s] rejected: in always_reject", lbl)
            return None

        # Mandatory: need a quorum of sources to compute a price at all.
        results = []
        for source in self.sources:
            try:
                results.append(source.fetch())
            except Exception as e:
                log.warning("[%s] source %s failed: %s", lbl, source.name, e)
        quorum = int(t.get("min_sources", 1))
        if len(results) < quorum:
            log.info("[%s] rejected: only %d/%d sources responded", lbl, len(results), quorum)
            return None

        price = statistics.median(r["price"] for r in results)
        if price <= 0:
            log.info("[%s] rejected: non-positive price", lbl)
            return None
        market_caps = [r["market_cap"] for r in results if r["market_cap"] is not None]
        volumes = [r["volume_24h"] for r in results if r["volume_24h"] is not None]
        market_cap = statistics.median(market_caps) if market_caps else None
        volume_24h = statistics.median(volumes) if volumes else None
        self.price_history.append(price)
        new_rate = round(price * COIN)

        forced = _listed("always_admit")
        change_ok = True  # tracked so a rejected jump resets the baseline
        if not forced:
            # Each configured criterion -> (name, passed). All optional.
            checks = []
            if "min_price" in t:
                checks.append(("min_price", price >= t["min_price"]))
            if "max_price" in t:
                checks.append(("max_price", price <= t["max_price"]))
            if "min_market_cap" in t:
                checks.append(("min_market_cap", market_cap is not None and market_cap >= t["min_market_cap"]))
            if "min_volume_24h" in t:
                checks.append(("min_volume_24h", volume_24h is not None and volume_24h >= t["min_volume_24h"]))
            if "max_source_spread" in t and len(results) > 1:
                spread = (max(r["price"] for r in results) - min(r["price"] for r in results)) / price
                checks.append(("max_source_spread", spread <= t["max_source_spread"]))
            if "max_volatility" in t:
                checks.append(("max_volatility", self._volatility_ok(t)))
            if "max_change_factor" in t and self.last_rate is not None:
                f = t["max_change_factor"]
                change_ok = (self.last_rate / f) <= new_rate <= (self.last_rate * f)
                checks.append(("max_change_factor", change_ok))
            if "issuer_domains" in t or "issuer_pubkeys" in t:
                issuer_ok = (self.issuer_domain in t.get("issuer_domains", [])) or \
                            (self.issuer_pubkey in t.get("issuer_pubkeys", []))
                checks.append(("issuer", issuer_ok))

            if checks:
                require = t.get("require", "all")
                admit = (any(ok for _, ok in checks) if require == "any"
                         else all(ok for _, ok in checks))
                if not admit:
                    failed = [n for n, ok in checks if not ok]
                    log.info("[%s] rejected (require=%s): failed %s", lbl, require, failed)
                    if not change_ok:
                        self.last_rate = None  # fresh baseline after a rejected jump
                    return None

        if new_rate <= 0:
            log.info("[%s] rejected: scaled rate rounds to zero", lbl)
            return None
        self.last_rate = new_rate
        log.info("[%s] admitted%s: price=%g rate=%d mcap=%s vol24h=%s",
                 lbl, " (forced)" if forced else "", price, new_rate, market_cap, volume_24h)
        return new_rate


class PriceServer:
    def __init__(self, config, dry_run=False, config_path=None):
        self.cfg = config
        self.dry_run = dry_run
        self.config_path = config_path
        self._lock = threading.Lock()  # guards cfg/feeds between the poll + UI threads
        self.last_rates = {}
        # Publish to one node ("node_rpc") or many ("node_rpcs"). A single box can
        # run a whole committee, so one price server feeds them all — then whichever
        # producer the VRF elects in a round already prices the fee assets.
        if dry_run:
            self.rpcs = []
        elif "node_rpcs" in config:
            self.rpcs = [NodeRPC(n) for n in config["node_rpcs"]]
        else:
            self.rpcs = [NodeRPC(config["node_rpc"])]
        self.source_name = config.get("source_name", "price-server")
        self.stopping = False
        self._build_feeds()

    def _build_feeds(self):
        timeout = self.cfg.get("source_timeout", 15)
        defaults = self.cfg.get("default_thresholds", {})
        self.feeds = [AssetFeed(a, defaults, timeout) for a in self.cfg["assets"]]

    def apply_admission(self, thresholds, poll_interval=None, reference=None):
        """Runtime update from the config UI: replace the admission rule set (and a
        couple of globals), rebuild the feeds, and persist to the config file."""
        with self._lock:
            self.cfg["default_thresholds"] = thresholds
            if poll_interval is not None:
                self.cfg["poll_interval_secs"] = poll_interval
            if reference is not None:
                self.cfg["reference_asset_label"] = reference
            self._build_feeds()
            if self.config_path:
                tmp = self.config_path + ".tmp"
                with open(tmp, "w") as f:
                    json.dump(self.cfg, f, indent=2)
                os.replace(tmp, self.config_path)
        log.info("admission rules updated via config UI and persisted")

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
        with self._lock:
            feeds = list(self.feeds)  # snapshot; the UI thread may rebuild them
        raw, labels = {}, {}
        for feed in feeds:
            rate = feed.poll()
            if rate is not None:
                raw[feed.asset_id] = rate
                labels[feed.asset_id] = feed.label
        rates = self._denominate(raw, labels)
        self.last_rates = rates
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
        signal.signal(signal.SIGTERM, self._stop)
        signal.signal(signal.SIGINT, self._stop)
        log.info("starting: %d asset(s), poll every %ds",
                 len(self.feeds), self.cfg.get("poll_interval_secs", 60))
        while not self.stopping:
            try:
                self.poll_once()
            except Exception as e:
                log.error("poll failed: %s", e)
            # sleep in small steps so shutdown stays responsive; re-read the
            # interval each round so a config-UI change takes effect promptly.
            deadline = time.time() + self.cfg.get("poll_interval_secs", 60)
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


# ---------------------------------------------------------------------------
# Config UI: a tiny self-served web page (localhost) to edit the admission rules
# without hand-editing JSON. Pure stdlib (http.server); enabled with --ui-port.
# ---------------------------------------------------------------------------

_UI_CRITERIA = [  # (key, label, example)
    ("min_market_cap", "Min market cap", "50000000"),
    ("min_volume_24h", "Min 24h volume", "1000000"),
    ("max_source_spread", "Max cross-source spread (0-1)", "0.05"),
    ("max_change_factor", "Max change factor per poll", "5"),
    ("max_volatility", "Max volatility (stddev of log-returns)", "0.15"),
]
_UI_LISTS = [
    ("issuer_domains", "Issuer domains to auto-admit"),
    ("issuer_pubkeys", "Issuer pubkeys to auto-admit"),
    ("always_admit", "Always admit (asset id or label)"),
    ("always_reject", "Always reject (asset id or label)"),
]


def _ui_render(srv, saved=False):
    t = srv.cfg.get("default_thresholds", {})
    req = t.get("require", "all")
    ck = lambda v: "checked" if v else ""
    crit = "".join(
        '<tr><td><input type=checkbox name="en_%s" %s></td><td>%s</td>'
        '<td><input name="%s" value="%s" placeholder="%s"></td></tr>'
        % (k, ck(k in t), html.escape(lbl), k, html.escape(str(t.get(k, ""))), ph)
        for k, lbl, ph in _UI_CRITERIA)
    lists = "".join(
        '<tr><td>%s</td><td><input name="%s" value="%s"></td></tr>'
        % (html.escape(lbl), k, html.escape(" ".join(t.get(k, [])) if isinstance(t.get(k), list) else ""))
        for k, lbl in _UI_LISTS)
    assets = ", ".join(a.get("label", a["id"][:8]) for a in srv.cfg.get("assets", []))
    banner = '<p style="color:#0a0"><b>Saved &amp; applied.</b></p>' if saved else ""
    return (
        "<!doctype html><meta charset=utf-8><title>Sequentia price server</title>"
        "<style>body{font-family:system-ui,Arial;max-width:780px;margin:2rem auto;padding:0 1rem;color:#222}"
        "table{border-collapse:collapse;width:100%}td{padding:.35rem .5rem;border-bottom:1px solid #eee}"
        "input:not([type=checkbox]){width:100%;box-sizing:border-box;padding:.3rem}h1{font-size:1.3rem}"
        ".muted{color:#777;font-size:.9rem}button{padding:.6rem 1.2rem;font-size:1rem;cursor:pointer}"
        "pre{background:#f6f6f6;padding:.8rem;overflow:auto;border-radius:6px}</style>"
        "<h1>Sequentia price server &mdash; admission rules</h1>" + banner +
        "<form method=post action=/save>"
        "<p><b>Combine criteria:</b> "
        "<label><input type=radio name=require value=all " + ck(req != "any") + "> ALL must pass</label> &nbsp; "
        "<label><input type=radio name=require value=any " + ck(req == "any") + "> ANY one passes</label></p>"
        "<p class=muted>Tick a criterion to enforce it; untick to ignore it. All optional.</p>"
        "<table>" + crit + "</table>"
        "<h3>Exceptions &amp; issuer</h3>"
        "<p class=muted>Space/comma separated. Exceptions override the criteria (reject wins); issuer lists admit assets from a trusted issuer.</p>"
        "<table>" + lists + "</table>"
        "<h3>General</h3><table>"
        '<tr><td>Poll interval (seconds)</td><td><input name=poll_interval value="' + html.escape(str(srv.cfg.get("poll_interval_secs", 60))) + '"></td></tr>'
        '<tr><td>Reference asset label</td><td><input name=reference value="' + html.escape(str(srv.cfg.get("reference_asset_label", ""))) + '"></td></tr>'
        "</table>"
        "<p><button type=submit>Save &amp; apply</button></p></form>"
        "<p class=muted>Assets (" + html.escape(assets or "none") + ") and their price sources are edited in the config file.</p>"
        "<h3>Last published rates</h3><pre>" + html.escape(json.dumps(srv.last_rates, indent=2)) + "</pre>")


def _ui_parse(srv, form):
    """New default_thresholds from the posted form, preserving keys the form does
    not expose (min_sources, volatility_window, ...)."""
    t = dict(srv.cfg.get("default_thresholds", {}))
    t["require"] = "any" if form.get("require", ["all"])[0] == "any" else "all"
    for key, _, _ in _UI_CRITERIA:
        if form.get("en_" + key, [""])[0]:
            raw = form.get(key, [""])[0].strip()
            try:
                t[key] = float(raw) if re.search(r"[.eE]", raw) else int(raw)
            except ValueError:
                t.pop(key, None)
        else:
            t.pop(key, None)
    for key, _ in _UI_LISTS:
        items = [x for x in re.split(r"[,\s]+", form.get(key, [""])[0].strip()) if x]
        if items:
            t[key] = items
        else:
            t.pop(key, None)
    try:
        poll = max(1, int(form.get("poll_interval", ["60"])[0]))
    except ValueError:
        poll = None
    ref = form.get("reference", [""])[0].strip() or None
    return t, poll, ref


def start_config_ui(price_server, host, port):
    srv = price_server

    class Handler(http.server.BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="text/html; charset=utf-8"):
            data = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path.startswith("/status"):
                self._send(200, json.dumps(srv.last_rates, indent=2), "application/json")
            elif self.path == "/" or self.path.startswith("/?"):
                self._send(200, _ui_render(srv, saved=self.path.endswith("saved=1")))
            else:
                self._send(404, "not found")

        def do_POST(self):
            if self.path != "/save":
                self._send(404, "not found"); return
            n = int(self.headers.get("Content-Length", 0) or 0)
            form = urllib.parse.parse_qs(self.rfile.read(n).decode())
            try:
                t, poll, ref = _ui_parse(srv, form)
                srv.apply_admission(t, poll, ref)
            except Exception as e:
                self._send(400, "error: " + html.escape(str(e))); return
            self.send_response(303); self.send_header("Location", "/?saved=1"); self.end_headers()

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer((host, port), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log.info("config UI on http://%s:%d", host, port)
    return httpd


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True, help="path to JSON config file")
    parser.add_argument("--once", action="store_true", help="poll once and exit")
    parser.add_argument("--dry-run", action="store_true",
                        help="don't publish to the node, just log decisions")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--ui-port", type=int, default=0,
                        help="serve a config UI to edit the admission rules on this port (0 = off)")
    parser.add_argument("--ui-host", default="127.0.0.1",
                        help="config UI bind address (default: localhost only)")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    with open(args.config) as f:
        config = json.load(f)

    server = PriceServer(config, dry_run=args.dry_run, config_path=args.config)
    if args.ui_port:
        start_config_ui(server, args.ui_host, args.ui_port)
    if args.once:
        rates = server.poll_once()
        print(json.dumps(rates, indent=2))
    else:
        server.run()


if __name__ == "__main__":
    sys.exit(main())
