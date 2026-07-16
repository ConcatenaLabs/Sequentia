#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sequentia price server.

A locally run sidecar that maintains a Sequentia node's *dynamic* fee-asset
whitelist (see doc/sequentia/02-open-fee-market.md). Each round it:

  1. DISCOVERS the asset universe from the node's Asset Registry (the same
     registry the node and explorer use): ticker -> (asset id, issuer domain).
  2. fetches current prices for those assets from ONE market-data API. A
     Sequentia-format API returns every asset keyed by ticker in one call
     ({"<TICKER>": {"price", "market_cap", "volume_24h"}}), so no per-asset
     URLs or JSON paths are needed; a "custom" API is fetched per ticker with
     operator-supplied JSON paths.
  3. applies operator-defined ADMISSION RULES (thresholds + always-admit /
     always-reject exceptions) to decide which discovered assets to whitelist.
  4. converts each admitted asset's price (in the API's quote currency, e.g.
     USD) into the node's scaled rate. The node values a fee paid in an asset as
     `atoms * rate / 1e8` and is precision-blind (it works purely in atoms), so
     the rate MUST carry the asset's denomination: for an asset with `precision`
     decimals the rate is `round(price * 1e8 * 10**(8 - precision))`. That is
     `round(price * 1e8)` for the common 8-decimal case (a no-op) and corrects
     the valuation for any asset with a different denomination — otherwise a fee
     paid in, say, a 2-decimal asset would be mis-valued by 1e6. Then it
  5. publishes the result via the node's `setfeeexchangerates` RPC with
     persist=false.

The node keeps a SINGLE fee-asset whitelist; this sidecar owns it in full.
`setfeeexchangerates` replaces the published set wholesale, so whatever this
server emits each round IS the whitelist. persist=false keeps these automated
pushes out of the node's exchangerates.json, so a restart falls back to the
operator's hand-configured (static) whitelist rather than a dead price server's
last rates. On clean shutdown the rates are cleared (fail safe: assets leave the
whitelist rather than ride a dead price).
Stdlib only; no external dependencies.

Usage:
    price_server.py --config config.json [--once] [--dry-run] [--ui-port N]
"""

import argparse
import base64
import collections
import hashlib
import hmac
import html
import http.server
import json
import logging
import math
import os
import re
import secrets
import signal
import statistics
import sys
import threading
import time
import urllib.parse
import urllib.request

log = logging.getLogger("price-server")

COIN = 100_000_000
DEFAULT_PRECISION = 8  # asset decimals when the registry omits them; also the reference scale


def scaled_rate(price, precision):
    """The node's fee rate for `price` (in the quote currency, per whole unit) of
    an asset with `precision` decimals: atoms of the asset worth one reference
    unit. The node values fees precision-blind (atoms * rate / 1e8), so the
    denomination is carried here — 10**(8 - precision) is 1 for 8-decimal assets
    (unchanged) and rescales the rest so a fee in a non-8-decimal asset is valued
    correctly rather than off by 10**(8 - precision)."""
    return round(price * COIN * (10 ** (DEFAULT_PRECISION - precision)))


# Published rates must satisfy 0 < rate <= MAX_RATE. The node parses each rate
# with get_int64; a value above INT64_MAX (~9.22e18) makes that parse throw and
# the node drops the WHOLE setfeeexchangerates batch. We clamp well below that so
# one absurd quote can never poison every other asset's rate; offenders are
# dropped per-asset and logged instead. The ceiling leaves headroom above 1e8x
# precision scaling (a 0-decimal asset multiplies the rate by 1e8) and the
# change-gate / re-denomination that run on top of it.
MAX_RATE = 1_000_000_000_000_000_000  # 1e18, comfortably below INT64_MAX (~9.22e18)

# Defaults for a fresh install pointed at the public Sequentia testnet demo. The
# config UI pre-fills these, so a freshly downloaded node's price server works
# against the testnet out of the box (and shows the REAL testnet assets, not a
# placeholder). Override in config.json / the UI for your own deployment.
DEFAULT_SOURCE_URL = "http://159.195.15.140/prices"
DEFAULT_REGISTRY_URL = "http://159.195.15.140/registry/index.minimal.json"
DEFAULT_QUOTE = "USD"


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
    req = urllib.request.Request(url, headers={"User-Agent": "sequentia-price-server/0.2"})
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


def fetch_registry(url, timeout):
    """Fetch the Asset Registry minimal index and return {TICKER: (asset_id,
    domain, precision)}. The index is { id: [domain, ticker, name, precision,
    verified] }. A registry tells us the asset universe, the id we publish rates
    against, and each asset's denomination (precision) — which the rate must
    carry (see scaled_rate) — so the operator never maintains an asset list, or a
    per-asset precision, by hand."""
    data = http_get_json(url, timeout)
    out = {}
    for asset_id, meta in data.items():
        if not isinstance(meta, list) or len(meta) < 2:
            continue
        domain, ticker = meta[0], meta[1]
        if not ticker:
            continue
        # precision (meta[3]) is the asset's on-chain denomination; default to 8
        # and ignore out-of-range/non-integer values (registry validates 0..8).
        precision = DEFAULT_PRECISION
        if len(meta) >= 4 and isinstance(meta[3], int) and 0 <= meta[3] <= DEFAULT_PRECISION:
            precision = meta[3]
        out[str(ticker).upper()] = (asset_id, domain, precision)
    return out


def fetch_prices(source, timeout):
    """Fetch current market data keyed by upper-case ticker:
        { TICKER: {"price": float, "market_cap": float|None, "volume_24h": float|None} }

    A "sequentia" source is one combined GET returning every asset in exactly
    that shape (the format the bundled mock + a real Sequentia price API use).
    A "custom" source needs JSON paths; it is fetched ONCE if the URL has no
    "{ticker}" placeholder (a combined custom feed), else per ticker — handled
    by the caller via fetch_prices_for()."""
    fmt = source.get("format", "sequentia")
    if fmt != "sequentia":
        raise ValueError("fetch_prices is for the sequentia (combined) format only")
    raw = http_get_json(source.get("url", DEFAULT_SOURCE_URL), timeout)
    out = {}
    for k, v in raw.items():
        if isinstance(v, dict):
            p = v.get("price")
            if p is None:
                continue
            out[str(k).upper()] = {
                "price": float(p),
                "market_cap": float(v["market_cap"]) if v.get("market_cap") is not None else None,
                "volume_24h": float(v["volume_24h"]) if v.get("volume_24h") is not None else None,
            }
        elif isinstance(v, (int, float)):  # tolerate {ticker: price} feeds
            out[str(k).upper()] = {"price": float(v), "market_cap": None, "volume_24h": None}
    return out


def fetch_prices_custom(source, tickers, timeout):
    """Custom (non-Sequentia) API: fetch each ticker with operator JSON paths.
    "{ticker}" in the URL/paths is replaced by the ticker. If the URL has no
    "{ticker}", it is treated as a combined feed and fetched once."""
    price_t = source.get("price_path") or "price"
    mcap_t = source.get("market_cap_path") or ""
    vol_t = source.get("volume_24h_path") or ""
    out = {}
    combined = None
    if "{ticker}" not in source.get("url", ""):
        combined = http_get_json(source["url"], timeout)  # fetch once
    for t in tickers:
        try:
            data = combined if combined is not None else http_get_json(
                source["url"].replace("{ticker}", t), timeout)
            rec = {"price": float(jsonpath(data, price_t.replace("{ticker}", t)))}
            rec["market_cap"] = float(jsonpath(data, mcap_t.replace("{ticker}", t))) if mcap_t else None
            rec["volume_24h"] = float(jsonpath(data, vol_t.replace("{ticker}", t))) if vol_t else None
            out[t] = rec
        except Exception as e:
            log.warning("custom source: %s failed: %s", t, e)
    return out


class AssetState:
    """Per-ticker rolling state (price history for volatility, last accepted /
    pending rate for the max_change_factor gate). Admission itself is decided by
    PriceServer._admit() so the rules live in one place."""

    def __init__(self, volatility_window):
        self.price_history = collections.deque(maxlen=max(2, int(volatility_window)))
        self.last_rate = None
        self.pending_rate = None  # a rate held back for jumping too far, re-checked next round

    def volatility_ok(self, t):
        min_samples = int(t.get("volatility_min_samples", 5))
        hist = list(self.price_history)
        if len(hist) < max(3, min_samples):
            return True  # not enough data: don't gate
        rets = [math.log(hist[i] / hist[i - 1])
                for i in range(1, len(hist)) if hist[i - 1] > 0 and hist[i] > 0]
        if len(rets) < 2:
            return True
        return statistics.pstdev(rets) <= t["max_volatility"]


class PriceServer:
    def __init__(self, config, dry_run=False, config_path=None):
        self.cfg = config
        self.dry_run = dry_run
        self.config_path = config_path
        self._lock = threading.Lock()
        self._states = {}            # TICKER -> AssetState
        self.last_rates = {}         # asset_id -> rate (last published)
        self.last_report = []        # [{ticker, id, domain, price, rate, status}] for the UI
        self.last_prices = {}        # TICKER -> {price, market_cap, volume_24h} (for the public API)
        self.last_poll_ts = None     # unix time of the last completed poll
        self.node_status = []        # [{url, ok, error, ts}] result of the last publish per node
        if dry_run:
            self.rpcs = []
        elif "node_rpcs" in config:
            self.rpcs = [NodeRPC(n) for n in config["node_rpcs"]]
        else:
            self.rpcs = [NodeRPC(config["node_rpc"])]
        self.source_name = config.get("source_name", "price-server")
        self.stopping = False

    # ---- config (UI / file) ----
    def source(self):
        return self.cfg.get("source", {})

    def thresholds(self):
        return self.cfg.get("default_thresholds", {})

    def exceptions(self):
        return self.cfg.get("exceptions", {})

    def apply_config(self, *, source=None, thresholds=None, exceptions=None,
                     poll_interval=None, source_name=None, registry_url=None,
                     reference=None, nodes=None, ui=None, manual_prices=None):
        """Replace config sections from the UI and persist atomically."""
        with self._lock:
            if source is not None:
                self.cfg["source"] = source
            if thresholds is not None:
                self.cfg["default_thresholds"] = thresholds
            if exceptions is not None:
                self.cfg["exceptions"] = exceptions
            if poll_interval is not None:
                self.cfg["poll_interval_secs"] = poll_interval
            if source_name is not None:
                self.source_name = source_name
                self.cfg["source_name"] = source_name
            if registry_url is not None:
                self.cfg["registry_url"] = registry_url
            if reference is not None:
                self.cfg["reference_asset_label"] = reference
            if nodes is not None:
                self.cfg["node_rpcs"] = nodes
                self.cfg.pop("node_rpc", None)
                if not self.dry_run:
                    self.rpcs = [NodeRPC(n) for n in nodes]
            if manual_prices is not None:
                self.cfg["manual_prices"] = manual_prices
            if ui is not None:
                merged = dict(self.cfg.get("ui", {}))
                merged.update(ui)
                self.cfg["ui"] = merged
            if self.config_path:
                tmp = self.config_path + ".tmp"
                with open(tmp, "w") as f:
                    json.dump(self.cfg, f, indent=2)
                os.replace(tmp, self.config_path)
        log.info("config updated via UI and persisted")

    # ---- admission ----
    def _admit(self, ticker, asset_id, domain, m, state, precision=DEFAULT_PRECISION):
        """Decide whether an asset qualifies for the whitelist this round and, if
        so, its scaled rate (denominated for `precision`; see scaled_rate). m =
        {price, market_cap, volume_24h}. Returns (rate|None, status_string).
        Rules: always_reject wins over always_admit; else every configured
        threshold is combined by `require` (all|any)."""
        t = self.thresholds()
        exc = self.exceptions()
        aid = asset_id

        def listed(key):
            vals = [str(x).upper() for x in exc.get(key, [])]
            return ticker.upper() in vals or (aid and aid.lower() in [v.lower() for v in exc.get(key, [])])

        if listed("always_reject"):
            return None, "rejected: always_reject"

        price = m.get("price")
        if not price or price <= 0:
            return None, "skipped: no price"
        state.price_history.append(price)
        new_rate = scaled_rate(price, precision)
        mcap, vol = m.get("market_cap"), m.get("volume_24h")

        forced = listed("always_admit")
        change_ok, change_eval = True, False
        if not forced:
            checks = []  # (name, passed, evaluated)
            if "min_price" in t:
                checks.append(("min_price", price >= t["min_price"], True))
            if "max_price" in t:
                checks.append(("max_price", price <= t["max_price"], True))
            if "min_market_cap" in t:
                checks.append(("min_market_cap", mcap is not None and mcap >= t["min_market_cap"], mcap is not None))
            if "min_volume_24h" in t:
                checks.append(("min_volume_24h", vol is not None and vol >= t["min_volume_24h"], vol is not None))
            if "max_volatility" in t:
                checks.append(("max_volatility", state.volatility_ok(t), True))
            if "max_change_factor" in t:
                baseline = state.last_rate if state.last_rate is not None else state.pending_rate
                if baseline is not None:
                    f = t["max_change_factor"]
                    change_ok = (baseline / f) <= new_rate <= (baseline * f)
                    change_eval = True
                    checks.append(("max_change_factor", change_ok, True))
                else:
                    state.pending_rate = new_rate  # seed a baseline for next round
                    checks.append(("max_change_factor", False, False))
            if "issuer_domains" in t:
                checks.append(("issuer_domain", domain in t.get("issuer_domains", []), True))

            if checks:
                require = t.get("require", "all")
                if require == "any":
                    admit = any(ok for _, ok, ev in checks if ev)
                else:
                    admit = all(ok and ev for _, ok, ev in checks)
                if not admit:
                    failed = [n for n, ok, ev in checks if not (ok and ev)]
                    if change_eval and not change_ok:
                        state.pending_rate = new_rate
                    return None, "rejected: " + ",".join(failed)

        if new_rate <= 0:
            return None, "skipped: rate rounds to 0"
        if new_rate > MAX_RATE:
            return None, "skipped: rate exceeds MAX_RATE"
        state.last_rate = new_rate
        state.pending_rate = None
        return new_rate, ("admitted (forced)" if forced else "admitted")

    def _denominate(self, raw, ticker_of_id):
        """Optionally re-express rates relative to a reference Sequentia asset
        (advanced; default off). With no reference, rates are published in the
        API's quote currency directly (the simple model)."""
        ref_label = self.cfg.get("reference_asset_label")
        if not ref_label:
            return dict(raw)
        ref_id = next((aid for aid, tk in ticker_of_id.items() if tk.upper() == str(ref_label).upper()), None)
        if ref_id is None or not raw.get(ref_id):
            log.warning("reference asset %r unavailable this round; retaining last-good", ref_label)
            return None
        ref_rate = raw[ref_id]
        return {aid: max(1, round(r * COIN / ref_rate)) for aid, r in raw.items()}

    @staticmethod
    def _clamp(rates):
        out = {}
        for aid, rate in rates.items():
            if 0 < rate <= MAX_RATE:
                out[aid] = rate
            else:
                log.warning("dropping %s: rate %s outside (0, %d]", aid, rate, MAX_RATE)
        return out

    def poll_once(self):
        src = self.source()
        timeout = self.cfg.get("source_timeout", 15)
        reg_url = self.cfg.get("registry_url", DEFAULT_REGISTRY_URL)
        vol_window = int(self.thresholds().get("volatility_window", 30))

        # 1) discover the asset universe (ticker -> id, domain)
        try:
            registry = fetch_registry(reg_url, timeout)
        except Exception as e:
            log.error("registry fetch failed (%s); cannot map tickers to ids this round", e)
            return self.last_rates

        # 2) fetch prices. feed_aliases remaps a registry ticker to the key the price
        #    feed uses for that asset — e.g. the native coin is the SAME asset whether
        #    the registry calls it "TSEQ" (its display ticker) or the feed prices it as
        #    "SEQ" (the chain-independent pricing key the node GUI + explorer also use).
        #    The alias merges them into ONE asset so the native is not double-counted.
        aliases = {str(k).upper(): str(v).upper()
                   for k, v in self.cfg.get("feed_aliases", {"TSEQ": "SEQ"}).items()
                   if not str(k).startswith("_")}
        reg_tickers = list(registry.keys())
        feed_keys = sorted({aliases.get(t.upper(), t.upper()) for t in reg_tickers})
        try:
            if src.get("format", "sequentia") == "sequentia":
                prices = fetch_prices(src, timeout)                    # combined feed: every key
            else:
                prices = fetch_prices_custom(src, feed_keys, timeout)  # per feed-key
        except Exception as e:
            log.error("price fetch failed: %s", e)
            return self.last_rates

        # 3) admit each REGISTERED asset (the asset universe) and build id -> rate. The
        #    native/policy asset is NOT special-cased: it goes through the same admission
        #    rules and can be admitted OR rejected, exactly like any other asset (the
        #    only thing special about SEQ is staking, never the fee market).
        #
        #    The market source can be scoped (source.mode = all | except | only, with
        #    source.assets). An asset with no market price — out of scope, or simply
        #    missing from the feed — can carry an operator-set MANUAL price (in the
        #    quote currency): it is admitted at that fixed rate, bypassing the market
        #    criteria (the operator setting a price by hand IS the decision), but
        #    always_reject still wins.
        smode = src.get("mode", "all")
        sassets = {str(x).upper() for x in src.get("assets", [])}
        def source_covers(tk, feed_key, asset_id):
            # An entry matches by registry ticker, feed key (alias) or asset id,
            # so "SEQ" covers the native whether the registry calls it TSEQ or SEQ.
            hit = tk in sassets or feed_key in sassets or asset_id.upper() in sassets
            if smode == "only": return hit
            if smode == "except": return not hit
            return True
        manual = {}
        for k, v in self.cfg.get("manual_prices", {}).items():
            if str(k).startswith("_"):
                continue
            try:
                manual[str(k).upper()] = float(v)
            except (TypeError, ValueError):
                pass
        raw, report, ticker_of_id = {}, [], {}
        with self._lock:
            rejects = [str(x).upper() for x in self.exceptions().get("always_reject", [])]
            for ticker in sorted(reg_tickers):
                asset_id, domain, precision = registry[ticker]
                ticker_of_id[asset_id] = ticker
                tk_u = ticker.upper()
                feed_key = aliases.get(tk_u, tk_u)
                covered = source_covers(tk_u, feed_key, asset_id)
                m = prices.get(feed_key) if covered else None
                if not m:
                    mp = manual.get(tk_u, manual.get(feed_key))
                    if mp and mp > 0:
                        if tk_u in rejects or asset_id.upper() in rejects:
                            status, rate = "rejected: always_reject", None
                        else:
                            rate = scaled_rate(mp, precision)
                            if not (0 < rate <= MAX_RATE):
                                status, rate = "skipped: manual rate out of range", None
                            else:
                                status = "admitted (manual price)"
                                raw[asset_id] = rate
                        report.append({"ticker": ticker, "id": asset_id, "domain": domain,
                                       "price": mp, "rate": rate, "status": status})
                        continue
                    why = ("skipped: outside the market-source scope, no manual price"
                           if not covered else "skipped: no price from API")
                    report.append({"ticker": ticker, "id": asset_id, "domain": domain,
                                   "price": None, "rate": None, "status": why})
                    continue
                state = self._states.setdefault(ticker, AssetState(vol_window))
                rate, status = self._admit(ticker, asset_id, domain, m, state, precision)
                if rate is not None:
                    raw[asset_id] = rate
                report.append({"ticker": ticker, "id": asset_id, "domain": domain,
                               "price": m.get("price"), "rate": rate, "status": status})

        # 4) (optional) re-denominate, clamp, publish
        rates = self._denominate(raw, ticker_of_id)
        if rates is None:
            self.last_report = report
            return self.last_rates
        rates = self._clamp(rates)
        self.last_rates = rates
        self.last_report = report
        self.last_prices = prices
        self.last_poll_ts = time.time()
        admitted = sum(1 for r in report if r["rate"] is not None)
        if self.dry_run:
            log.info("dry-run: %d/%d discovered assets admitted; would publish %s",
                     admitted, len(report), json.dumps(rates))
            return rates
        ok = 0
        node_status = []
        for rpc in self.rpcs:
            try:
                rpc.call("setfeeexchangerates", rates, False)  # persist=False: re-pushed each poll
                ok += 1
                node_status.append({"url": rpc.url, "ok": True, "error": "", "ts": time.time()})
            except Exception as e:
                log.warning("publish to %s failed: %s", rpc.url, e)
                node_status.append({"url": rpc.url, "ok": False, "error": str(e), "ts": time.time()})
        self.node_status = node_status
        log.info("published %d rate(s) to %d/%d node(s) (%d/%d discovered admitted)",
                 len(rates), ok, len(self.rpcs), admitted, len(report))
        return rates

    def run(self):
        signal.signal(signal.SIGTERM, self._stop)
        signal.signal(signal.SIGINT, self._stop)
        log.info("starting: registry=%s source=%s poll every %ds",
                 self.cfg.get("registry_url", DEFAULT_REGISTRY_URL),
                 self.source().get("url", DEFAULT_SOURCE_URL),
                 self.cfg.get("poll_interval_secs", 60))
        while not self.stopping:
            try:
                self.poll_once()
            except Exception as e:
                log.error("poll failed: %s", e)
            deadline = time.time() + self.cfg.get("poll_interval_secs", 60)
            while not self.stopping and time.time() < deadline:
                time.sleep(0.5)
        if not self.dry_run:
            for rpc in self.rpcs:
                try:
                    rpc.call("setfeeexchangerates", {}, False)  # persist=False: clear without touching the static file
                except Exception as e:
                    log.warning("could not clear the fee-asset whitelist on %s: %s", rpc.url, e)
            log.info("cleared the fee-asset whitelist on shutdown")

    def _stop(self, _sig, _frame):
        self.stopping = True


# ---------------------------------------------------------------------------
# Web UI & API. Two audiences share one port:
#
#   PUBLIC (optional, off by default): a read-only price page at / and a JSON
#   API — /api/prices in the Sequentia combined format (so another operator can
#   point THEIR price server at this one as its market source) and
#   /api/whitelist (rates + admission decisions). Both are rate-limited per
#   client IP: the typical host is a small VPS, not an exchange.
#
#   ADMIN (login): every setting — market source, admission rules, nodes,
#   access. Without a password the admin area answers only to 127.0.0.1
#   requests (the desktop-GUI flow: launch + open browser, no setup); binding
#   beyond loopback requires a password so the config is never writable, nor
#   node RPC details visible, to the open internet.
#
# Pure stdlib.
# ---------------------------------------------------------------------------

_UI_CRITERIA = [  # (key, label, placeholder, hint)
    ("min_market_cap", "Minimum market cap",
     "50000000", "Reject an asset whose market cap (in the quote currency) is below this. Leave unticked to ignore."),
    ("min_volume_24h", "Minimum 24h volume",
     "1000000", "Reject an asset whose 24h trading volume is below this — a liquidity floor."),
    ("max_change_factor", "Max price change per poll",
     "10", "Reject a quote that jumped more than this multiple (or fraction) from the last accepted one — guards against a bad print. 10 = allow up to 10x up or down."),
    ("max_volatility", "Max volatility",
     "0.15", "Reject an asset whose recent price volatility (stddev of log-returns) exceeds this. Needs a few polls of history first."),
    ("min_price", "Minimum price", "0", "Sanity floor on the unit price in the quote currency."),
    ("max_price", "Maximum price", "0", "Sanity cap on the unit price in the quote currency."),
]


def _src(srv):
    s = dict(srv.source())
    return {
        "url": s.get("url", DEFAULT_SOURCE_URL),
        "quote_currency": s.get("quote_currency", DEFAULT_QUOTE),
        "format": s.get("format", "sequentia"),
        "price_path": s.get("price_path", "price"),
        "market_cap_path": s.get("market_cap_path", "market_cap"),
        "volume_24h_path": s.get("volume_24h_path", "volume_24h"),
    }


def _hash_password(pw, iterations=200_000):
    salt = secrets.token_bytes(16)
    dk = hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, iterations)
    return "pbkdf2$%d$%s$%s" % (iterations, salt.hex(), dk.hex())


def _check_password(stored, pw):
    try:
        _scheme, it, salt, want = stored.split("$")
        dk = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), int(it))
        return hmac.compare_digest(dk.hex(), want)
    except Exception:
        return False


class _RateLimiter:
    """Per-IP sliding-window limiter for the public endpoints. In-memory and
    deliberately simple: the goal is to keep a hobby VPS responsive, not to
    survive a determined flood (put a real proxy in front for that)."""

    def __init__(self):
        self._hits = {}
        self._lock = threading.Lock()

    def allow(self, ip, per_min):
        now = time.time()
        with self._lock:
            q = self._hits.setdefault(ip, collections.deque())
            while q and q[0] < now - 60:
                q.popleft()
            if len(q) >= per_min:
                return False
            q.append(now)
            if len(self._hits) > 10_000:  # shed idle IPs so memory stays bounded
                self._hits = {k: v for k, v in self._hits.items() if v}
            return True


# Shared look for every served page (public, login, admin): Sequentia yellow on
# near-black, matching the node dashboard. Plain constant so no brace-escaping.
_CSS = """
:root{--bg:#0b0b0d;--panel:#141417;--panel2:#191920;--line:#26262c;--text:#f2f0ea;
--muted:#9b988e;--faint:#6d6a62;--accent:#f5b301;--accent-ink:#1a1400;
--good:#3ecf7a;--good-bg:rgba(62,207,122,.12);--warn:#ffb84d;--warn-bg:rgba(255,160,50,.12);
--bad:#ff6b6b;--bad-bg:rgba(255,90,90,.12);--mono:ui-monospace,'Cascadia Mono',Consolas,Menlo,monospace}
html,body{background:var(--bg);color:var(--text);margin:0}
body{font-family:'Segoe UI',system-ui,-apple-system,sans-serif;font-size:15px;line-height:1.45}
.wrap{max-width:1040px;margin:0 auto;padding:20px 18px 48px}
header{display:flex;align-items:center;gap:14px;padding:6px 0 14px;flex-wrap:wrap}
.mark{width:42px;height:42px;border-radius:8px;background:var(--accent);color:var(--accent-ink);
display:flex;align-items:center;justify-content:center;font-weight:800;font-size:1.25rem;font-family:var(--mono)}
.htitle h1{font-size:1.2rem;margin:0;font-weight:650}
.htitle .sub{color:var(--muted);font-size:.8rem;margin-top:2px}
.hchips{margin-left:auto;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.chip{font-size:.68rem;font-weight:700;letter-spacing:.09em;text-transform:uppercase;
padding:4px 10px;border-radius:3px;border:1px solid var(--line);color:var(--muted)}
.chip.live{border-color:var(--good);color:var(--good)}
.chip a{color:inherit;text-decoration:none}
nav{display:flex;gap:2px;border-bottom:1px solid var(--line);margin-bottom:16px;flex-wrap:wrap}
nav button{background:none;border:none;color:var(--muted);font:inherit;font-size:.88rem;font-weight:600;
padding:10px 16px;cursor:pointer;border-bottom:2px solid transparent}
nav button:hover{color:var(--text)}
nav button.on{color:var(--accent);border-bottom-color:var(--accent)}
.tab{display:none}.tab.on{display:block}
.card{background:var(--panel);border:1px solid var(--line);border-radius:6px;overflow:hidden;margin-bottom:14px}
.card h2{font-size:.72rem;font-weight:700;letter-spacing:.1em;text-transform:uppercase;color:var(--accent);
margin:0;padding:12px 16px 10px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:10px}
.card h2 .right{margin-left:auto;text-transform:none;letter-spacing:0;color:var(--faint);font-weight:400}
table{width:100%;border-collapse:collapse;font-size:.88rem}
th{font-size:.68rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--faint);
text-align:left;padding:8px 14px;border-bottom:1px solid var(--line);white-space:nowrap}
td{padding:8px 14px;border-bottom:1px solid rgba(255,255,255,.04);vertical-align:middle}
tr:last-child td{border-bottom:none}
.num{font-family:var(--mono);font-variant-numeric:tabular-nums}
.r{text-align:right}th.r{text-align:right}
.mut{color:var(--muted);font-weight:400}
.pill{display:inline-block;font-size:.7rem;font-weight:700;padding:2px 8px;border-radius:10px}
.pill.good{background:var(--good-bg);color:var(--good)}
.pill.warn{background:var(--warn-bg);color:var(--warn)}
.pill.bad{background:var(--bad-bg);color:var(--bad)}
.why{color:var(--faint);font-size:.76rem;white-space:normal}
.kpis{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin-bottom:14px}
@media(max-width:820px){.kpis{grid-template-columns:repeat(2,1fr)}}
.kpi{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:12px 14px}
.kpi .l{color:var(--faint);font-size:.7rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase}
.kpi .v{font-family:var(--mono);font-size:1.25rem;font-weight:700;margin-top:4px}
.kpi .s{color:var(--muted);font-size:.74rem;margin-top:2px}
.frm{padding:14px 16px;display:grid;gap:12px}
.frow{display:grid;grid-template-columns:250px 1fr;gap:12px;align-items:center}
@media(max-width:640px){.frow{grid-template-columns:1fr}}
.frow label{color:var(--muted);font-size:.88rem}
.hint{color:var(--faint);font-size:.75rem;line-height:1.4}
input:not([type=checkbox]):not([type=radio]),select{
background:var(--panel2);border:1px solid var(--line);border-radius:4px;color:var(--text);
font:inherit;font-size:.88rem;padding:8px 10px;width:100%;box-sizing:border-box}
input:focus,select:focus{outline:none;border-color:var(--accent)}
input[type=checkbox],input[type=radio]{accent-color:var(--accent);width:16px;height:16px}
.check{display:flex;gap:10px;align-items:center;color:var(--text);font-size:.88rem}
.mono{font-family:var(--mono);font-size:.8rem}
button.btn{background:var(--accent);color:var(--accent-ink);border:none;border-radius:4px;
font-weight:700;font-size:.82rem;padding:9px 16px;cursor:pointer}
button.btn:hover{filter:brightness(1.08)}
button.ghost{background:none;border:1px solid var(--line);color:var(--muted);border-radius:4px;
font-size:.8rem;padding:7px 12px;cursor:pointer}
button.ghost:hover{border-color:var(--accent);color:var(--accent)}
button.rm{background:none;border:1px solid rgba(255,90,90,.4);color:var(--bad);border-radius:4px;cursor:pointer;padding:6px 10px}
.savebar{background:var(--panel2);border-top:1px solid var(--line);padding:12px 16px;
display:flex;gap:12px;align-items:center}
.savebar .mut{font-size:.78rem}
.excrow{display:flex;gap:8px;margin:4px 0}.excrow input{flex:1}
.noterow{padding:10px 16px;color:var(--muted);font-size:.82rem}
.tbar{display:flex;gap:10px;align-items:center;padding:10px 14px;border-bottom:1px solid var(--line);flex-wrap:wrap}
.tbar input{max-width:300px}.tbar select{width:auto}
.tbar .mut{font-size:.76rem;margin-left:auto}
th.sortcol{cursor:pointer}th.sortcol:hover{color:var(--accent)}
.saved{background:var(--good-bg);color:var(--good);font-weight:600;font-size:.85rem;
padding:9px 14px;border-radius:4px;margin-bottom:14px}
.err{background:var(--bad-bg);color:var(--bad);font-weight:600;font-size:.85rem;
padding:9px 14px;border-radius:4px;margin-bottom:14px}
footer{color:var(--faint);font-size:.76rem;text-align:center;margin-top:20px}
code{font-family:var(--mono);font-size:.82em;background:var(--panel2);padding:1px 5px;border-radius:3px}
a{color:var(--accent)}
.login{max-width:360px;margin:10vh auto 0}
"""

_TAB_SCRIPT = """<script>
function tab(ev,id){
  document.querySelectorAll('nav button').forEach(function(b){b.classList.remove('on')});
  ev.target.classList.add('on');
  document.querySelectorAll('.tab').forEach(function(t){t.classList.toggle('on',t.id===id)});
}
function addExc(k){var l=document.getElementById('list_'+k);var d=document.createElement('div');d.className='excrow';
var i=document.createElement('input');i.name=k;var b=document.createElement('button');b.type='button';b.className='rm';
b.innerHTML='\\u00d7';b.onclick=function(){d.remove()};d.appendChild(i);d.appendChild(b);l.appendChild(d);i.focus();}
function addNode(){var t=document.getElementById('nodetpl');var c=t.content.cloneNode(true);
document.getElementById('nodelist').appendChild(c);}
function rmNode(b){b.closest('tr').remove();}
function addManual(){var l=document.getElementById('list_manual');var d=document.createElement('div');d.className='excrow';
d.innerHTML='<input name=manual_ticker placeholder="TICKER or 64-hex id" style="flex:2">'+
'<input name=manual_price placeholder="price in the quote currency" style="flex:1">'+
'<button type=button class=rm onclick="this.parentNode.remove()">\\u00d7</button>';
l.appendChild(d);d.querySelector('input').focus();}
function cellVal(tr,i){var td=tr.cells[i];if(!td)return'';var v=td.getAttribute('data-v');
if(v!==null){var n=parseFloat(v);return isNaN(n)?v:n;}return td.textContent.trim();}
function applyView(tb){
 var q=((document.getElementById('flt_'+tb.id)||{}).value||'').toLowerCase();
 var max=parseInt((document.getElementById('pgs_'+tb.id)||{}).value||'0')||0;
 var shown=0,total=0;
 tb.querySelectorAll('tbody tr').forEach(function(r){
  var ok=!q||r.textContent.toLowerCase().indexOf(q)>=0;
  if(ok)total++;
  var vis=ok&&(!max||shown<max);
  if(vis)shown++;
  r.style.display=vis?'':'none';
 });
 var n=document.getElementById('cnt_'+tb.id);if(n)n.textContent='showing '+shown+' of '+total;
}
document.querySelectorAll('table.sortable').forEach(function(tb){
 var dir={};
 tb.querySelectorAll('thead th').forEach(function(th,i){
  th.classList.add('sortcol');
  th.addEventListener('click',function(){
   dir[i]=!dir[i];
   var rows=Array.from(tb.querySelectorAll('tbody tr'));
   rows.sort(function(a,b){
    var x=cellVal(a,i),y=cellVal(b,i);
    if(typeof x=='number'&&typeof y=='number')return dir[i]?x-y:y-x;
    return dir[i]?String(x).localeCompare(String(y)):String(y).localeCompare(String(x));
   });
   var tbody=tb.querySelector('tbody');
   rows.forEach(function(r){tbody.appendChild(r)});
   applyView(tb);
  });
 });
 applyView(tb);
});
var radios=document.querySelectorAll('input[name=format]');var cp=document.getElementById('custompaths');
function syncFmt(){if(cp)cp.style.display=document.querySelector('input[name=format]:checked').value==='custom'?'':'none';}
radios.forEach(function(r){r.addEventListener('change',syncFmt)});
</script>"""


def _page(title, body, chips=""):
    esc = html.escape
    return "".join([
        "<!doctype html><html lang=en><head><meta charset=utf-8>",
        '<meta name=viewport content="width=device-width, initial-scale=1">',
        "<title>", esc(title), "</title><style>", _CSS, "</style></head><body><div class=wrap>",
        "<header><div class=mark>$</div><div class=htitle><h1>Sequentia Price Server</h1>",
        '<div class=sub>prices &amp; fee-asset whitelist</div></div>',
        '<div class=hchips>', chips, "</div></header>",
        body,
        "<footer>Sequentia price server \xb7 nothing here touches consensus — it only feeds the fee-asset whitelist</footer>",
        "</div>", _TAB_SCRIPT, "</body></html>",
    ])


def _fmt_qty(v):
    if v is None:
        return "—"
    a = abs(v)
    for cut, suf in ((1e9, " B"), (1e6, " M"), (1e3, " k")):
        if a >= cut:
            return "%.1f%s" % (v / cut, suf)
    return "%.6g" % v


def _fmt_age(ts):
    if not ts:
        return "never"
    d = int(time.time() - ts)
    if d < 90: return "%d s ago" % d
    if d < 5400: return "%d min ago" % (d // 60)
    return "%d h ago" % (d // 3600)


def _decisions_table(srv):
    """The per-asset decisions table, shared by the public page and the admin
    Status tab: sortable columns (click a header), a text filter and a
    rows-per-page selector, all client-side. Everything in it is already public
    information (prices and the resulting whitelist); node details never
    appear here."""
    esc = html.escape
    quote = _src(srv)["quote_currency"]
    if not srv.last_report:
        return ('<div class=noterow>No poll has completed yet — assets discovered from the registry '
                "will appear here with the admission decision for each.</div>")
    aliases = {str(k).upper(): str(v).upper()
               for k, v in srv.cfg.get("feed_aliases", {"TSEQ": "SEQ"}).items()
               if not str(k).startswith("_")}
    rows = []
    for r in srv.last_report:
        tk = r["ticker"].upper()
        m = srv.last_prices.get(aliases.get(tk, tk)) or {}
        ok = r["rate"] is not None
        pill = '<span class="pill good">ADMITTED</span>' if ok else (
            '<span class="pill bad">REJECTED</span>' if str(r["status"]).startswith("rejected")
            else '<span class="pill warn">SKIPPED</span>')
        price = r.get("price")
        mcap, vol = m.get("market_cap"), m.get("volume_24h")
        rows.append("".join([
            '<tr><td data-v="', esc(r["ticker"]), '"><b>', esc(r["ticker"]), "</b><div class='mut mono'>",
            esc((r["id"] or "")[:8]), "…</div></td>",
            '<td class="num r" data-v="', "%.10g" % price if price else "-1", '">',
            esc("%.6g" % price) if price else "—", "</td>",
            '<td class="num r" data-v="', "%.10g" % mcap if mcap is not None else "-1", '">', _fmt_qty(mcap), "</td>",
            '<td class="num r" data-v="', "%.10g" % vol if vol is not None else "-1", '">', _fmt_qty(vol), "</td>",
            '<td data-v="', ("0" if ok else "1"), '">', pill, "</td>",
            "<td class=why>", esc(r["status"]), "</td></tr>",
        ]))
    page = int(srv.cfg.get("ui", {}).get("page_size", 50) or 50)
    opts = "".join('<option value="%d"%s>%d</option>' % (n, " selected" if n == page else "", n)
                   for n in (10, 25, 50, 100, 250))
    return "".join([
        '<div class=tbar>',
        '<input id=flt_dec placeholder="filter: ticker, id, decision, reason…" oninput=applyView(dec)>',
        '<select id=pgs_dec onchange=applyView(dec)>', opts,
        '<option value=0', " selected" if not page else "", ">all</option></select>",
        '<span id=cnt_dec class=mut></span></div>',
        '<table class=sortable id=dec><thead><tr><th>Asset</th><th class=r>Price (', esc(quote), ")</th>",
        "<th class=r>Market cap</th><th class=r>24h volume</th><th>Decision</th><th>Why</th></tr></thead>",
        "<tbody>", "".join(rows), "</tbody></table>",
    ])


def _render_public(srv):
    esc = html.escape
    quote = _src(srv)["quote_currency"]
    admitted = sum(1 for r in srv.last_report if r["rate"] is not None)
    api_on = bool(srv.cfg.get("ui", {}).get("public_api"))
    chips = '<span class="chip live">Live</span><span class=chip>quote: ' + esc(quote) + "</span>"
    body = "".join([
        "<div class=kpis>",
        '<div class=kpi><div class=l>Whitelisted assets</div><div class=v>', str(admitted), " / ",
        str(len(srv.last_report)), "</div><div class=s>re-evaluated every poll</div></div>",
        '<div class=kpi><div class=l>Last update</div><div class=v>', _fmt_age(srv.last_poll_ts),
        "</div><div class=s>polls every ", esc(str(srv.cfg.get("poll_interval_secs", 60))), " s</div></div>",
        '<div class=kpi><div class=l>Quote currency</div><div class=v>', esc(quote),
        "</div><div class=s>all prices and caps in this unit</div></div>",
        '<div class=kpi><div class=l>Publisher</div><div class=v style="font-size:.95rem">', esc(srv.source_name),
        "</div><div class=s>read-only public view</div></div>",
        "</div>",
        "<div class=card><h2>Assets &amp; admission decisions</h2>", _decisions_table(srv), "</div>",
        "<div class=card><h2>API</h2><div class=noterow>",
        ("Machine-readable endpoints (rate-limited): <code>GET /api/prices</code> — every asset keyed by ticker "
         "in the Sequentia combined format, so you can point your own price server at this URL as its market source; "
         "<code>GET /api/whitelist</code> — published rates and per-asset decisions."
         if api_on else "The public JSON API is disabled by the operator."),
        "</div></div>",
    ])
    return _page("Sequentia Price Server", body, chips)


def _render_login(msg=""):
    err = '<div class=err>' + html.escape(msg) + "</div>" if msg else ""
    body = "".join([
        '<div class=login><div class=card><h2>Admin login</h2><div class=frm>', err,
        '<form method=post action=/login>',
        '<input type=password name=password placeholder="admin password" autofocus>',
        '<div style="margin-top:10px"><button class=btn type=submit>Log in</button></div>',
        "</form></div></div></div>",
    ])
    return _page("Price server — login", body)


def _scope_and_manual_cards(srv):
    """Market-source scope (all / all-except / only) and the manual fixed
    prices for assets the source doesn't cover. Part of the main /save form."""
    esc = html.escape
    s = dict(srv.source())
    quote = _src(srv)["quote_currency"]
    smode = s.get("mode", "all")
    ck = lambda v: "checked" if v else ""
    assets_rows = "".join(
        "<div class=excrow><input name=source_assets value=\"" + esc(str(v)) + "\">"
        "<button type=button class=rm onclick='this.parentNode.remove()'>&times;</button></div>"
        for v in s.get("assets", []))
    manual_rows = "".join(
        "<div class=excrow><input name=manual_ticker value=\"" + esc(str(k)) + "\" style=\"flex:2\">"
        "<input name=manual_price value=\"" + esc("%g" % v if isinstance(v, (int, float)) else str(v)) + "\" style=\"flex:1\">"
        "<button type=button class=rm onclick='this.parentNode.remove()'>&times;</button></div>"
        for k, v in srv.cfg.get("manual_prices", {}).items() if not str(k).startswith("_"))
    return "".join([
        "<div class=card><h2>Which assets use the market source</h2><div class=frm>",
        "<div>",
        '<label class=check style="display:inline-flex;margin-right:16px"><input type=radio name=source_mode value=all ',
        ck(smode not in ("except", "only")), "> All assets (standard)</label>",
        '<label class=check style="display:inline-flex;margin-right:16px"><input type=radio name=source_mode value=except ',
        ck(smode == "except"), "> All except the listed ones</label>",
        '<label class=check style="display:inline-flex"><input type=radio name=source_mode value=only ',
        ck(smode == "only"), "> Only the listed ones</label></div>",
        "<div class=hint>Tickers or 64-hex asset ids. Assets left without a market price fall back to a manual price "
        "below, or are skipped.</div>",
        "<div id=list_source_assets>", assets_rows, "</div>",
        "<div><button type=button class=ghost onclick=\"addExc('source_assets')\">+ Add asset</button></div>",
        "</div></div>",
        "<div class=card><h2>Manual prices <span class=right>in ", esc(quote), ", for assets without a market source</span></h2><div class=frm>",
        "<div class=hint>A fixed price per unit, in the quote currency — e.g. 0.5 means 1 unit = 0.5 ", esc(quote),
        ". Used only when the asset has no market price (out of scope above, or missing from the feed). A manually "
        "priced asset is admitted at that rate — setting it by hand IS the decision — but an always-reject exception still wins.</div>",
        "<div id=list_manual>", manual_rows, "</div>",
        "<div><button type=button class=ghost onclick=addManual()>+ Add manual price</button></div>",
        "</div></div>",
    ])


def _nodes_current(srv):
    if "node_rpcs" in srv.cfg:
        return list(srv.cfg["node_rpcs"])
    if "node_rpc" in srv.cfg:
        return [srv.cfg["node_rpc"]]
    return []


def _render_admin(srv, csrf_token, saved=False, error=""):
    esc = html.escape
    t = srv.thresholds()
    exc = srv.exceptions()
    s = _src(srv)
    ui = srv.cfg.get("ui", {})
    ck = lambda v: "checked" if v else ""
    is_custom = (s["format"] != "sequentia")
    quote = s["quote_currency"]

    # -- status tab --
    admitted = sum(1 for r in srv.last_report if r["rate"] is not None)
    nodes_ok = sum(1 for n in srv.node_status if n["ok"])
    node_rows = "".join(
        "".join(["<tr><td class='num mono'>", esc(n["url"]), "</td>",
                 "<td class=num>", _fmt_age(n["ts"]), "</td>",
                 "<td>", ('<span class="pill good">OK</span>' if n["ok"]
                          else '<span class="pill bad">' + esc(n["error"][:80] or "failed") + "</span>"), "</td></tr>"])
        for n in srv.node_status) or '<tr><td colspan=3 class=noterow>No publish attempted yet.</td></tr>'
    status_tab = "".join([
        "<div class=kpis>",
        '<div class=kpi><div class=l>Whitelisted</div><div class=v>', str(admitted), " / ", str(len(srv.last_report)), "</div>",
        '<div class=s>rule set: require ', esc(t.get("require", "all").upper()), "</div></div>",
        '<div class=kpi><div class=l>Last poll</div><div class=v>', _fmt_age(srv.last_poll_ts),
        "</div><div class=s>every ", esc(str(srv.cfg.get("poll_interval_secs", 60))), " s</div></div>",
        '<div class=kpi><div class=l>Quote currency</div><div class=v>', esc(quote), "</div>",
        '<div class=s>set in Market source</div></div>',
        '<div class=kpi><div class=l>Nodes updated</div><div class=v>', str(nodes_ok), " / ", str(len(srv.rpcs)),
        "</div><div class=s>last push per node below</div></div>",
        "</div>",
        "<div class=card><h2>Assets &amp; admission decisions</h2>", _decisions_table(srv), "</div>",
        "<div class=card><h2>Node pushes <span class=right>setfeeexchangerates \xb7 persist=false</span></h2>",
        "<table><tr><th>Node RPC</th><th>Last push</th><th>Status</th></tr>", node_rows, "</table></div>",
    ])

    # -- market source tab (part of the main /save form) --
    custom_disp = "" if is_custom else "display:none"
    source_tab = "".join([
        "<div class=card><h2>Market data source</h2><div class=frm>",
        "<div class=frow><label>API URL</label><input class=mono name=source_url value=\"", esc(s["url"]), "\"></div>",
        "<div class=frow><label>Quote currency</label><div><input name=quote_currency value=\"", esc(quote),
        "\" style=\"max-width:120px\"><div class=hint>The currency the API reports prices in — usually plain USD "
        "(it does not need to exist as an on-chain asset). Every price, market cap and volume on these pages, and the "
        "rates pushed to your nodes, are expressed in it.</div></div></div>",
        "<div class=frow><label>API format</label><div>",
        '<label class=check style="display:inline-flex;margin-right:16px"><input type=radio name=format value=sequentia ',
        ck(not is_custom), "> Sequentia-format</label>",
        '<label class=check style="display:inline-flex"><input type=radio name=format value=custom ', ck(is_custom), "> Custom</label>",
        '<div class=hint>A Sequentia-format API returns every asset keyed by ticker in one call — no paths needed. '
        "Another operator's price server exposes exactly this at <code>/api/prices</code>.</div></div></div>",
        '<div id=custompaths style="', custom_disp, ';display:grid;gap:12px">',
        "<div class=frow><label>Price JSON path</label><input class=mono name=price_path value=\"", esc(s["price_path"]), "\"></div>",
        "<div class=frow><label>Market-cap JSON path</label><input class=mono name=market_cap_path value=\"", esc(s["market_cap_path"]), "\"></div>",
        "<div class=frow><label>24h-volume JSON path</label><input class=mono name=volume_24h_path value=\"", esc(s["volume_24h_path"]), "\"></div>",
        "</div>",
        "<div class=frow><label>Asset registry URL</label><div><input class=mono name=registry_url value=\"",
        esc(str(srv.cfg.get("registry_url", DEFAULT_REGISTRY_URL))),
        "\"><div class=hint>The asset universe (ticker → id, issuer) is discovered here — you never list assets by hand.</div></div></div>",
        "</div></div>",
        _scope_and_manual_cards(srv),
        "<div class=card><h2>Polling &amp; identity</h2><div class=frm>",
        "<div class=frow><label>Poll interval (seconds)</label><input name=poll_interval value=\"",
        esc(str(srv.cfg.get("poll_interval_secs", 60))), "\" style=\"max-width:120px\"></div>",
        "<div class=frow><label>Publisher name</label><input name=source_name value=\"", esc(str(srv.source_name)), "\"></div>",
        "</div></div>",
    ])

    # -- admission rules tab (same form) --
    crit = "".join(
        "".join(["<tr><td style='width:30px'><input type=checkbox name=en_", k, " ", ck(k in t), "></td>",
                 "<td><b>", esc(lbl), "</b><div class=hint>", esc(hint), "</div></td>",
                 "<td style='width:180px'><input class=num name=", k, " value=\"", esc(str(t.get(k, ""))),
                 "\" placeholder=\"", esc(ph), "\"></td></tr>"])
        for k, lbl, ph, hint in _UI_CRITERIA)

    def exc_list(key, label, hint):
        items = [str(x) for x in exc.get(key, [])]
        rows = "".join(
            "<div class=excrow><input name=" + key + " value=\"" + esc(v) + "\">"
            "<button type=button class=rm onclick='this.parentNode.remove()'>&times;</button></div>"
            for v in items)
        return "".join(["<div class=card><h2>", esc(label), "</h2><div class=frm><div class=hint>", esc(hint), "</div>",
                        "<div id=list_", key, ">", rows, "</div>",
                        "<div><button type=button class=ghost onclick=\"addExc('", key, "')\">+ Add</button></div></div></div>"])

    rules_tab = "".join([
        "<div class=card><h2>Admission criteria</h2><div class=frm>",
        "<div class=frow><label>Combine criteria with</label><div>",
        '<label class=check style="display:inline-flex;margin-right:16px"><input type=radio name=require value=all ',
        ck(t.get("require", "all") != "any"), "> ALL must pass</label>",
        '<label class=check style="display:inline-flex"><input type=radio name=require value=any ',
        ck(t.get("require", "all") == "any"), "> ANY one passes</label></div></div>",
        "</div><table>", crit, "</table></div>",
        exc_list("always_admit", "Always admit", "Always whitelist these (if a valid price is available), skipping the rules. Ticker or 64-hex asset id."),
        exc_list("always_reject", "Always reject", "Never whitelist these, whatever the rules say — reject wins over admit."),
        exc_list("issuer_domains", "Trusted issuer domains", "Assets whose registry-verified issuer domain is in this list pass the issuer rule."),
    ])

    # -- nodes tab (its own form) --
    nodes = _nodes_current(srv)
    def node_row(n):
        return "".join([
            "<tr><td><input name=node_name value=\"", esc(str(n.get("name", ""))), "\" placeholder=\"my node\"></td>",
            "<td><input class=mono name=node_host value=\"", esc(str(n.get("host", "127.0.0.1"))), "\"></td>",
            "<td style='width:90px'><input class=num name=node_port value=\"", esc(str(n.get("port", ""))), "\"></td>",
            "<td><input name=node_user value=\"", esc(str(n.get("user", ""))), "\" autocomplete=off></td>",
            "<td><input type=password name=node_password value=\"", esc(str(n.get("password", ""))), "\" autocomplete=new-password></td>",
            "<td><input class=mono name=node_cookie value=\"", esc(str(n.get("cookie", ""))), "\" placeholder=\"(or cookie file)\"></td>",
            "<td><button type=button class=rm onclick=rmNode(this)>&times;</button></td></tr>",
        ])
    nodes_tab = "".join([
        '<form method=post action=/nodes>',
        '<input type=hidden name=csrf_token value="', esc(csrf_token), '">',
        "<div class=card><h2>Nodes receiving the whitelist <span class=right>setfeeexchangerates \xb7 persist=false</span></h2>",
        "<table><tr><th>Name</th><th>Host</th><th>RPC port</th><th>RPC user</th><th>RPC password</th><th>Cookie file</th><th></th></tr>",
        "<tbody id=nodelist>", "".join(node_row(n) for n in nodes), "</tbody></table>",
        "<template id=nodetpl>", node_row({}), "</template>",
        '<div class=frm><div><button type=button class=ghost onclick=addNode()>+ Add node</button></div>',
        "<div class=hint>Each node must run with <code>-con_any_asset_fees=1</code>. Give either user+password or a "
        "cookie file path. If the price server stops, every node simply keeps the last whitelist it received.</div></div>",
        '<div class=savebar><button class=btn type=submit>Save nodes</button>',
        "<span class=mut>applies live — the next poll publishes to the new list</span></div></div></form>",
    ])

    # -- access tab (its own form) --
    has_pw = bool(ui.get("password_hash"))
    access_tab = "".join([
        '<form method=post action=/access>',
        '<input type=hidden name=csrf_token value="', esc(csrf_token), '">',
        "<div class=card><h2>Public access</h2><div class=frm>",
        '<label class=check><input type=checkbox name=public_status ', ck(ui.get("public_status")),
        "> Public read-only price page <span class=mut>anyone reaching this URL can view prices and decisions — never the config or your nodes</span></label>",
        '<label class=check><input type=checkbox name=public_api ', ck(ui.get("public_api")),
        "> Public JSON API <span class=mut><code>/api/prices</code> \xb7 <code>/api/whitelist</code> — lets others use this server as their market source</span></label>",
        "<div class=frow><label>Rate limit (requests / min / IP)</label><input class=num name=api_rate_limit value=\"",
        esc(str(ui.get("api_rate_limit_per_min", 30))), "\" style=\"max-width:120px\"></div>",
        "<div class=hint>Applies to unauthenticated visitors only. Keep it low — this protects your VPS, not the data (it is public anyway once enabled).</div>",
        "<div class=frow><label>Table rows per page (default)</label><input class=num name=page_size value=\"",
        esc(str(ui.get("page_size", 50))), "\" style=\"max-width:120px\"></div>",
        "<div class=hint>Default page size of the asset tables (0 = show all); viewers can still change it per view.</div>",
        "</div></div>",
        "<div class=card><h2>Admin password</h2><div class=frm>",
        ("<div class=frow><label>Current password</label><input type=password name=cur_password autocomplete=off></div>" if has_pw else
         "<div class=hint>No password set: the admin area only answers to this machine (127.0.0.1). Set one to manage the server remotely.</div>"),
        "<div class=frow><label>New password</label><input type=password name=new_password autocomplete=new-password></div>",
        "<div class=frow><label>Repeat new password</label><input type=password name=new_password2 autocomplete=new-password></div>",
        "<div class=hint>With a password set, the admin area asks for login wherever it is reached from; "
        "to expose it beyond this machine restart with <code>--ui-host 0.0.0.0</code>.</div>",
        "</div></div>",
        "<div class=card><h2>Configuration file</h2><div class=noterow>",
        "The UI reads and writes the same file passed to <code>--config</code>. ",
        '<a href="/admin/config.json">Download the current config</a> (includes node RPC credentials — keep it private).',
        "</div></div>",
        '<div class=card><div class=savebar><button class=btn type=submit>Save access settings</button></div></div></form>',
    ])

    chips = ('<span class="chip live">Running</span><span class=chip><a href="/public">public view</a></span>'
             + ('<span class=chip><a href="/logout">log out</a></span>' if has_pw else ""))
    banner = ('<div class=saved>Saved &amp; applied.</div>' if saved else "") + \
             ('<div class=err>' + html.escape(error) + "</div>" if error else "")
    body = "".join([
        banner,
        "<nav>",
        '<button class=on onclick="tab(event,\'t-status\')">Status</button>',
        '<button onclick="tab(event,\'t-source\')">Market source</button>',
        '<button onclick="tab(event,\'t-rules\')">Admission rules</button>',
        '<button onclick="tab(event,\'t-nodes\')">Nodes</button>',
        '<button onclick="tab(event,\'t-access\')">Access</button>',
        "</nav>",
        '<div class="tab on" id=t-status>', status_tab, "</div>",
        '<form method=post action=/save>',
        '<input type=hidden name=csrf_token value="', esc(csrf_token), '">',
        '<div class=tab id=t-source>', source_tab,
        '<div class=card><div class=savebar><button class=btn type=submit>Save &amp; apply</button>',
        "<span class=mut>applies live, no restart</span></div></div></div>",
        '<div class=tab id=t-rules>', rules_tab,
        '<div class=card><div class=savebar><button class=btn type=submit>Save &amp; apply</button>',
        "<span class=mut>applies live, no restart</span></div></div></div>",
        "</form>",
        '<div class=tab id=t-nodes>', nodes_tab, "</div>",
        '<div class=tab id=t-access>', access_tab, "</div>",
    ])
    return _page("Price server — admin", body, chips)


def _ui_parse(srv, form):
    """Parse the main config form (/save) into kwargs for apply_config().
    Preserves default_thresholds keys the form doesn't expose."""
    g = lambda k, d="": form.get(k, [d])[0]
    t = dict(srv.thresholds())
    t["require"] = "any" if g("require", "all") == "any" else "all"
    for key, _, _, _ in _UI_CRITERIA:
        if g("en_" + key):
            raw = g(key).strip()
            try:
                t[key] = float(raw) if re.search(r"[.eE]", raw) else int(raw)
            except ValueError:
                t.pop(key, None)
        else:
            t.pop(key, None)
    exc = dict(srv.exceptions())
    for key in ("always_reject", "always_admit", "issuer_domains"):
        items = [x.strip() for x in form.get(key, []) if x.strip()]
        if items:
            exc[key] = items
        else:
            exc.pop(key, None)
    if "issuer_domains" in exc:
        t["issuer_domains"] = exc["issuer_domains"]
    else:
        t.pop("issuer_domains", None)
    fmt = "custom" if g("format", "sequentia") == "custom" else "sequentia"
    source = {"url": g("source_url").strip() or DEFAULT_SOURCE_URL,
              "quote_currency": g("quote_currency").strip().upper() or DEFAULT_QUOTE,
              "format": fmt}
    if fmt == "custom":
        source["price_path"] = g("price_path").strip() or "price"
        if g("market_cap_path").strip():
            source["market_cap_path"] = g("market_cap_path").strip()
        if g("volume_24h_path").strip():
            source["volume_24h_path"] = g("volume_24h_path").strip()
    smode = g("source_mode", "all")
    sassets = [x.strip().upper() for x in form.get("source_assets", []) if x.strip()]
    if smode in ("except", "only"):
        source["mode"] = smode
        source["assets"] = sassets
    manual = {}
    for tk, pr in zip(form.get("manual_ticker", []), form.get("manual_price", [])):
        tk = tk.strip().upper()
        try:
            pr = float(pr.strip().replace(",", "."))
        except ValueError:
            continue
        if tk and pr > 0:
            manual[tk] = pr
    try:
        poll = max(1, int(g("poll_interval", "60")))
    except ValueError:
        poll = None
    registry_url = g("registry_url").strip() or DEFAULT_REGISTRY_URL
    return {"source": source, "thresholds": t, "exceptions": exc,
            "poll_interval": poll, "registry_url": registry_url,
            "source_name": g("source_name").strip() or srv.source_name,
            "manual_prices": manual}


def _nodes_parse(form):
    """Parse the Nodes form: parallel input lists, one entry per row."""
    names = form.get("node_name", [])
    hosts = form.get("node_host", [])
    ports = form.get("node_port", [])
    users = form.get("node_user", [])
    pws = form.get("node_password", [])
    cookies = form.get("node_cookie", [])
    nodes = []
    for i in range(len(hosts)):
        host = hosts[i].strip()
        try:
            port = int(ports[i].strip()) if i < len(ports) else 0
        except ValueError:
            port = 0
        if not host or not port:
            continue  # an all-empty template row, or one without the essentials
        n = {"host": host, "port": port}
        if i < len(names) and names[i].strip():
            n["name"] = names[i].strip()
        if i < len(cookies) and cookies[i].strip():
            n["cookie"] = cookies[i].strip()
        else:
            n["user"] = users[i].strip() if i < len(users) else ""
            n["password"] = pws[i] if i < len(pws) else ""
        nodes.append(n)
    return nodes


def _is_loopback(host):
    if host in ("127.0.0.1", "::1", "localhost"):
        return True
    try:
        import ipaddress
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def start_config_ui(price_server, host, port):
    srv = price_server
    csrf_token = secrets.token_urlsafe(32)
    sessions = {}          # token -> expiry (unix)
    SESSION_TTL = 12 * 3600
    limiter = _RateLimiter()

    def ui_cfg():
        return srv.cfg.get("ui", {})

    def _same_origin(handler):
        host_hdr = handler.headers.get("Host", "")
        src = handler.headers.get("Origin") or handler.headers.get("Referer")
        if src is None:
            return False
        try:
            return urllib.parse.urlsplit(src).netloc == host_hdr
        except Exception:
            return False

    class Handler(http.server.BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="text/html; charset=utf-8", headers=None):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)

        def _redirect(self, where, headers=None):
            self.send_response(303)
            self.send_header("Location", where)
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()

        def _client_local(self):
            return self.client_address[0] in ("127.0.0.1", "::1")

        def _authed(self):
            """Admin? With no password set, only loopback clients qualify (the
            desktop-GUI flow). With one set, a valid session cookie is required
            from everyone, loopback included."""
            pw = ui_cfg().get("password_hash")
            if not pw:
                return self._client_local()
            cookie = self.headers.get("Cookie", "")
            m = re.search(r"(?:^|;\s*)psid=([A-Za-z0-9_-]+)", cookie)
            if not m:
                return False
            tok = m.group(1)
            exp = sessions.get(tok)
            if not exp or exp < time.time():
                sessions.pop(tok, None)
                return False
            sessions[tok] = time.time() + SESSION_TTL  # sliding renewal
            return True

        def _public_gate(self, enabled):
            """Returns None if the request may proceed, else sends the refusal."""
            if self._authed():
                return None  # the admin is never rate-limited or blocked
            if not enabled:
                self._send(403, json.dumps({"error": "disabled by the operator"}), "application/json")
                return True
            per_min = int(ui_cfg().get("api_rate_limit_per_min", 30) or 30)
            if not limiter.allow(self.client_address[0], per_min):
                self._send(429, json.dumps({"error": "rate limit exceeded, slow down"}), "application/json",
                           headers={"Retry-After": "30"})
                return True
            return None

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path == "/api/prices":
                if self._public_gate(ui_cfg().get("public_api")): return
                data = dict(srv.last_prices)
                data["_meta"] = {"quote_currency": _src(srv)["quote_currency"],
                                 "updated": srv.last_poll_ts, "publisher": srv.source_name,
                                 "format": "sequentia"}
                self._send(200, json.dumps(data, indent=2), "application/json")
            elif path in ("/api/whitelist", "/status"):  # /status: legacy alias
                if self._public_gate(ui_cfg().get("public_api")): return
                self._send(200, json.dumps({
                    "updated": srv.last_poll_ts,
                    "quote_currency": _src(srv)["quote_currency"],
                    "rates": srv.last_rates, "decisions": srv.last_report}, indent=2),
                    "application/json")
            elif path == "/":
                if self._authed():
                    self._redirect("/admin")  # the desktop GUI opens the root URL
                    return
                if self._public_gate(ui_cfg().get("public_status")): return
                self._send(200, _render_public(srv))
            elif path == "/public":
                if self._public_gate(ui_cfg().get("public_status")): return
                self._send(200, _render_public(srv))
            elif path == "/admin":
                if self._authed():
                    self._send(200, _render_admin(srv, csrf_token,
                                                  saved=self.path.endswith("saved=1")))
                else:
                    self._send(200, _render_login())
            elif path == "/admin/config.json":
                if not self._authed():
                    self._redirect("/admin"); return
                self._send(200, json.dumps(srv.cfg, indent=2), "application/json",
                           headers={"Content-Disposition": "attachment; filename=price-server-config.json"})
            elif path == "/logout":
                cookie = self.headers.get("Cookie", "")
                m = re.search(r"(?:^|;\s*)psid=([A-Za-z0-9_-]+)", cookie)
                if m:
                    sessions.pop(m.group(1), None)
                self._redirect("/admin", headers={"Set-Cookie": "psid=; Max-Age=0; Path=/"})
            else:
                self._send(404, "not found")

        def do_POST(self):
            path = urllib.parse.urlsplit(self.path).path
            n = int(self.headers.get("Content-Length", 0) or 0)
            if n > 1_000_000:
                self._send(413, "request too large"); return
            form = urllib.parse.parse_qs(self.rfile.read(n).decode())
            g = lambda k, d="": form.get(k, [d])[0]

            if path == "/login":
                pw_hash = ui_cfg().get("password_hash")
                if not pw_hash:
                    self._redirect("/admin"); return
                time.sleep(0.3)  # flat cost per attempt; blunts online guessing
                if _check_password(pw_hash, g("password")):
                    tok = secrets.token_urlsafe(32)
                    sessions[tok] = time.time() + SESSION_TTL
                    self._redirect("/admin", headers={
                        "Set-Cookie": "psid=%s; HttpOnly; SameSite=Strict; Path=/" % tok})
                else:
                    log.warning("admin login failed from %s", self.client_address[0])
                    self._send(200, _render_login("Wrong password."))
                return

            # Everything below changes the config: admin + same-origin + CSRF.
            if not self._authed():
                self._send(403, "forbidden: not logged in"); return
            if not _same_origin(self):
                log.warning("rejected %s: cross-origin or missing Origin/Referer", path)
                self._send(403, "forbidden: cross-origin request"); return
            if not hmac.compare_digest(g("csrf_token"), csrf_token):
                log.warning("rejected %s: bad or missing CSRF token", path)
                self._send(403, "forbidden: invalid CSRF token"); return

            try:
                if path == "/save":
                    srv.apply_config(**_ui_parse(srv, form))
                elif path == "/nodes":
                    nodes = _nodes_parse(form)
                    if not nodes:
                        self._send(200, _render_admin(srv, csrf_token,
                                                      error="No valid node rows (each needs host + port).")); return
                    srv.apply_config(nodes=nodes)
                elif path == "/access":
                    ui = {"public_status": bool(g("public_status")),
                          "public_api": bool(g("public_api"))}
                    try:
                        ui["api_rate_limit_per_min"] = max(1, int(g("api_rate_limit", "30")))
                    except ValueError:
                        pass
                    try:
                        ui["page_size"] = max(0, int(g("page_size", "50")))
                    except ValueError:
                        pass
                    new_pw = g("new_password")
                    if new_pw:
                        cur_hash = ui_cfg().get("password_hash")
                        if cur_hash and not _check_password(cur_hash, g("cur_password")):
                            self._send(200, _render_admin(srv, csrf_token,
                                                          error="Current password is wrong.")); return
                        if new_pw != g("new_password2"):
                            self._send(200, _render_admin(srv, csrf_token,
                                                          error="The new passwords don't match.")); return
                        if len(new_pw) < 8:
                            self._send(200, _render_admin(srv, csrf_token,
                                                          error="Password too short — use at least 8 characters.")); return
                        ui["password_hash"] = _hash_password(new_pw)
                    srv.apply_config(ui=ui)
                else:
                    self._send(404, "not found"); return
            except Exception as e:
                self._send(400, "error: " + html.escape(str(e))); return
            self._redirect("/admin?saved=1")

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer((host, port), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log.info("web UI on http://%s:%d (admin at /admin)", host, port)
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
                        help="serve the web UI/API on this port (0 = off)")
    parser.add_argument("--ui-host", default="127.0.0.1",
                        help="web UI bind address (default: localhost only)")
    parser.add_argument("--ui-allow-remote", action="store_true",
                        help="permit a non-loopback bind even without an admin password (NOT recommended)")
    parser.add_argument("--set-password", action="store_true",
                        help="prompt for an admin password, store its hash in the config, and exit")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    with open(args.config) as f:
        config = json.load(f)

    if args.set_password:
        import getpass
        pw = getpass.getpass("New admin password: ")
        if len(pw) < 8:
            parser.error("password too short — use at least 8 characters")
        if pw != getpass.getpass("Repeat it: "):
            parser.error("the passwords don't match")
        config.setdefault("ui", {})["password_hash"] = _hash_password(pw)
        tmp = args.config + ".tmp"
        with open(tmp, "w") as f:
            json.dump(config, f, indent=2)
        os.replace(tmp, args.config)
        print("Admin password set. Restart the price server to pick it up.")
        return 0

    server = PriceServer(config, dry_run=args.dry_run, config_path=args.config)
    if args.ui_port:
        if (not _is_loopback(args.ui_host)
                and not config.get("ui", {}).get("password_hash")
                and not args.ui_allow_remote):
            parser.error(
                "refusing to bind the web UI to non-loopback address %r without an admin "
                "password: the config (and your node RPC details) would be writable by "
                "anyone. Set one first with --set-password (or from the Access tab on "
                "localhost). --ui-allow-remote overrides, at your own risk." % args.ui_host)
        start_config_ui(server, args.ui_host, args.ui_port)
    if args.once:
        print(json.dumps(server.poll_once(), indent=2))
    else:
        server.run()


if __name__ == "__main__":
    sys.exit(main())
