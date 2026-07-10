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
     USD) into the node's scaled rate (rate = round(price_in_quote * 1e8)) and
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

# Published rates must satisfy 0 < rate <= MAX_RATE. The node parses each rate
# with get_int64; a value above INT64_MAX (~9.22e18) makes that parse throw and
# the node drops the WHOLE setfeeexchangerates batch. We clamp well below that so
# one absurd quote can never poison every other asset's rate; offenders are
# dropped per-asset and logged instead.
MAX_RATE = 1_000_000_000_000_000  # 1e15, comfortably below INT64_MAX

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
    domain)}. The index is { id: [domain, ticker, name, precision, verified] }.
    A registry tells us the asset universe and the id we publish rates against,
    so the operator never maintains an asset list by hand."""
    data = http_get_json(url, timeout)
    out = {}
    for asset_id, meta in data.items():
        if not isinstance(meta, list) or len(meta) < 2:
            continue
        domain, ticker = meta[0], meta[1]
        if not ticker:
            continue
        out[str(ticker).upper()] = (asset_id, domain)
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
                     reference=None):
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
            if self.config_path:
                tmp = self.config_path + ".tmp"
                with open(tmp, "w") as f:
                    json.dump(self.cfg, f, indent=2)
                os.replace(tmp, self.config_path)
        log.info("config updated via UI and persisted")

    # ---- admission ----
    def _admit(self, ticker, asset_id, domain, m, state):
        """Decide whether an asset qualifies for the whitelist this round and, if
        so, its scaled rate. m = {price, market_cap, volume_24h}. Returns
        (rate|None, status_string). Rules: always_reject wins over always_admit;
        else every configured threshold is combined by `require` (all|any)."""
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
        new_rate = round(price * COIN)
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
        raw, report, ticker_of_id = {}, [], {}
        with self._lock:
            for ticker in sorted(reg_tickers):
                asset_id, domain = registry[ticker]
                ticker_of_id[asset_id] = ticker
                m = prices.get(aliases.get(ticker.upper(), ticker.upper()))
                if not m:
                    report.append({"ticker": ticker, "id": asset_id, "domain": domain,
                                   "price": None, "rate": None, "status": "skipped: no price from API"})
                    continue
                state = self._states.setdefault(ticker, AssetState(vol_window))
                rate, status = self._admit(ticker, asset_id, domain, m, state)
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
        admitted = sum(1 for r in report if r["rate"] is not None)
        if self.dry_run:
            log.info("dry-run: %d/%d discovered assets admitted; would publish %s",
                     admitted, len(report), json.dumps(rates))
            return rates
        ok = 0
        for rpc in self.rpcs:
            try:
                rpc.call("setfeeexchangerates", rates, False)  # persist=False: re-pushed each poll
                ok += 1
            except Exception as e:
                log.warning("publish to %s failed: %s", rpc.url, e)
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
# Config UI: a tiny self-served localhost web page to point the server at an API
# and tune the admission rules without hand-editing JSON. Pure stdlib.
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


def _ui_render(srv, csrf_token, saved=False):
    esc = html.escape
    t = srv.thresholds()
    exc = srv.exceptions()
    s = _src(srv)
    ck = lambda v: "checked" if v else ""
    is_custom = (s["format"] != "sequentia")

    crit = "".join(
        '<tr><td><input type=checkbox name="en_%s" %s></td>'
        '<td><div><b>%s</b></div><div class=hint>%s</div></td>'
        '<td><input name="%s" value="%s" placeholder="%s"></td></tr>'
        % (k, ck(k in t), esc(lbl), esc(hint), k, esc(str(t.get(k, ""))), esc(ph))
        for k, lbl, ph, hint in _UI_CRITERIA)

    def exc_list(key, label, hint):
        items = [str(x) for x in exc.get(key, [])]
        rows = "".join(
            '<div class=excrow><input name="%s" value="%s">'
            '<button type=button class=rm onclick="this.parentNode.remove()">&times;</button></div>'
            % (key, esc(v)) for v in items)
        return (
            '<div class=excblock><div><b>%s</b></div><div class=hint>%s</div>'
            '<div id="list_%s">%s</div>'
            '<button type=button class=add onclick="addExc(\'%s\')">+ Add</button></div>'
            % (esc(label), esc(hint), key, rows, key))

    discovered = srv.last_report
    if discovered:
        drows = "".join(
            '<tr><td>%s</td><td class=mono>%s</td><td>%s</td><td class=st-%s>%s</td></tr>'
            % (esc(r["ticker"]),
               esc((r["id"] or "")[:12] + ("…" if r["id"] and len(r["id"]) > 12 else "")),
               esc("%.6g" % r["price"]) if r.get("price") else "—",
               "ok" if r["rate"] is not None else "no",
               esc(r["status"]))
            for r in discovered)
        disc_html = ("<table class=disc><tr><th>Ticker</th><th>Asset id</th><th>Price (%s)</th><th>Decision</th></tr>%s</table>"
                     % (esc(s["quote_currency"]), drows))
    else:
        disc_html = "<p class=hint>No poll has completed yet — assets discovered from the registry + API will appear here, with the admission decision for each.</p>"

    banner = '<p class=saved><b>Saved &amp; applied.</b></p>' if saved else ""
    custom_disp = "" if is_custom else "display:none"
    css = ("body{font-family:system-ui,Arial,sans-serif;max-width:820px;margin:2rem auto;padding:0 1rem;color:#1a1c1f}"
           "h1{font-size:1.35rem}h2{font-size:1.05rem;margin:1.6rem 0 .4rem;border-bottom:1px solid #eee;padding-bottom:.3rem}"
           "table{border-collapse:collapse;width:100%}td,th{padding:.4rem .5rem;border-bottom:1px solid #eee;text-align:left;vertical-align:top}"
           "input:not([type=checkbox]),select{width:100%;box-sizing:border-box;padding:.4rem}"
           ".hint{color:#667;font-size:.85rem;line-height:1.35;margin-top:.15rem}"
           ".excrow{display:flex;gap:.4rem;margin:.3rem 0}.excrow input{flex:1}"
           ".rm{flex:0 0 auto;cursor:pointer}.add{margin-top:.3rem;cursor:pointer}"
           ".excblock{margin:.8rem 0}.mono{font-family:ui-monospace,Consolas,monospace;font-size:.85rem}"
           ".st-ok{color:#1e7e34}.st-no{color:#9a6f00}.saved{color:#1e7e34}"
           "button{padding:.5rem 1rem;font-size:.95rem;cursor:pointer}.primary{padding:.7rem 1.4rem;font-size:1rem}"
           "label.fmt{font-weight:normal;margin-right:1rem}")
    script = ("<script>"
              "function addExc(k){var l=document.getElementById('list_'+k);var d=document.createElement('div');d.className='excrow';"
              "var i=document.createElement('input');i.name=k;var b=document.createElement('button');b.type='button';b.className='rm';"
              "b.innerHTML='\\u00d7';b.onclick=function(){d.remove()};d.appendChild(i);d.appendChild(b);l.appendChild(d);i.focus();}"
              "var radios=document.querySelectorAll('input[name=format]');var cp=document.getElementById('custompaths');"
              "function syncFmt(){cp.style.display=document.querySelector('input[name=format]:checked').value==='custom'?'':'none';}"
              "radios.forEach(function(r){r.addEventListener('change',syncFmt)});"
              "</script>")
    return "".join([
        "<!doctype html><meta charset=utf-8><title>Sequentia price server</title>",
        "<style>", css, "</style>",
        "<h1>Sequentia price server</h1>", banner,
        "<p class=hint>This sidecar discovers your network's assets from the Asset Registry, prices them from a "
        "market-data API, and publishes the resulting fee-asset whitelist to your node. You don't list assets by "
        "hand — admission rules below decide which discovered assets get whitelisted.</p>",
        '<form method=post action=/save>',
        '<input type=hidden name=csrf_token value="', esc(csrf_token), '">',

        "<h2>Price source</h2><table>",
        '<tr><td style="width:36%"><b>API URL</b><div class=hint>The market-data API to poll. Default: the Sequentia testnet demo feed.</div></td>',
        '<td><input name=source_url value="', esc(s["url"]), '" placeholder="', DEFAULT_SOURCE_URL, '"></td></tr>',
        '<tr><td><b>Quote currency</b><div class=hint>The currency the API reports prices in. Fees are priced in this unit. Usually real USD.</div></td>',
        '<td><input name=quote_currency value="', esc(s["quote_currency"]), '" placeholder="USD"></td></tr>',
        '<tr><td><b>API format</b><div class=hint>A <b>Sequentia-format</b> API returns every asset keyed by ticker '
        '(<span class=mono>{"SEQ":{"price":…}}</span>) in one call — no paths needed. Choose <b>Custom</b> only for a '
        'third-party API, then map where each field lives in its JSON.</div></td>',
        '<td><label class=fmt><input type=radio name=format value=sequentia ', ck(not is_custom), '> Sequentia-format</label>',
        '<label class=fmt><input type=radio name=format value=custom ', ck(is_custom), '> Custom</label></td></tr></table>',
        '<table id=custompaths style="', custom_disp, '">',
        '<tr><td style="width:36%"><b>Price path</b><div class=hint>Dotted JSON path to the price in the API response. '
        'Use <span class=mono>{ticker}</span> for the asset ticker; put it in the URL too for per-asset endpoints.</div></td>',
        '<td><input name=price_path value="', esc(s["price_path"]), '" placeholder="data.price"></td></tr>',
        '<tr><td><b>Market-cap path</b> <span class=hint>(optional)</span></td>',
        '<td><input name=market_cap_path value="', esc(s["market_cap_path"]), '" placeholder="data.market_cap"></td></tr>',
        '<tr><td><b>24h-volume path</b> <span class=hint>(optional)</span></td>',
        '<td><input name=volume_24h_path value="', esc(s["volume_24h_path"]), '" placeholder="data.volume_24h"></td></tr></table>',

        "<h2>Admission rules</h2>",
        "<p class=hint>Tick a rule to enforce it; untick to ignore. Then choose whether an asset must pass "
        "<b>all</b> ticked rules or <b>any</b> one.</p>",
        '<p><label class=fmt><input type=radio name=require value=all ', ck(t.get("require", "all") != "any"), '> Require ALL ticked</label>',
        '<label class=fmt><input type=radio name=require value=any ', ck(t.get("require", "all") == "any"), '> Require ANY one</label></p>',
        "<table>", crit, "</table>",

        "<h2>Exceptions &amp; trusted issuers</h2>",
        "<p class=hint>Exceptions override the rules above. Each entry is an asset ticker or id; add as many as you like.</p>",
        exc_list("always_reject", "Always reject", "Never whitelist these, whatever the rules say (reject wins over admit)."),
        exc_list("always_admit", "Always admit", "Always whitelist these (if a valid price is available), skipping the rules."),
        exc_list("issuer_domains", "Trusted issuer domains",
                 "If the 'issuer domain' rule is ticked above, assets whose registry domain is in this list pass it."),

        "<h2>General</h2><table>",
        '<tr><td style="width:36%">Poll interval (seconds)</td><td><input name=poll_interval value="', esc(str(srv.cfg.get("poll_interval_secs", 60))), '"></td></tr>',
        '<tr><td>Registry URL<div class=hint>Where the asset universe (ticker &rarr; id) is read from.</div></td>',
        '<td><input name=registry_url value="', esc(str(srv.cfg.get("registry_url", DEFAULT_REGISTRY_URL))), '"></td></tr>',
        '<tr><td>Publisher name</td><td><input name=source_name value="', esc(str(srv.source_name)), '"></td></tr></table>',

        "<p><button type=submit class=primary>Save &amp; apply</button></p></form>",
        "<h2>Discovered assets &amp; decisions</h2>", disc_html,
        script,
    ])


def _ui_parse(srv, form):
    """Parse the posted form into kwargs for apply_config(). Preserves
    default_thresholds keys the form doesn't expose (volatility_window, etc.)."""
    g = lambda k, d="": form.get(k, [d])[0]
    # thresholds
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
    # exceptions: multiple inputs share a name -> a list of rows
    exc = dict(srv.exceptions())
    for key in ("always_reject", "always_admit", "issuer_domains"):
        items = [x.strip() for x in form.get(key, []) if x.strip()]
        if items:
            exc[key] = items
        else:
            exc.pop(key, None)
    # carry issuer_domains into thresholds too (the rule reads it there)
    if "issuer_domains" in exc:
        t["issuer_domains"] = exc["issuer_domains"]
    else:
        t.pop("issuer_domains", None)
    # source
    fmt = "custom" if g("format", "sequentia") == "custom" else "sequentia"
    source = {"url": g("source_url").strip() or DEFAULT_SOURCE_URL,
              "quote_currency": g("quote_currency").strip() or DEFAULT_QUOTE,
              "format": fmt}
    if fmt == "custom":
        source["price_path"] = g("price_path").strip() or "price"
        if g("market_cap_path").strip():
            source["market_cap_path"] = g("market_cap_path").strip()
        if g("volume_24h_path").strip():
            source["volume_24h_path"] = g("volume_24h_path").strip()
    try:
        poll = max(1, int(g("poll_interval", "60")))
    except ValueError:
        poll = None
    registry_url = g("registry_url").strip() or DEFAULT_REGISTRY_URL
    return {"source": source, "thresholds": t, "exceptions": exc,
            "poll_interval": poll, "registry_url": registry_url}


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
        def _send(self, code, body, ctype="text/html; charset=utf-8"):
            data = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path.startswith("/status"):
                self._send(200, json.dumps({"rates": srv.last_rates, "report": srv.last_report}, indent=2),
                           "application/json")
            elif self.path == "/" or self.path.startswith("/?"):
                self._send(200, _ui_render(srv, csrf_token, saved=self.path.endswith("saved=1")))
            else:
                self._send(404, "not found")

        def do_POST(self):
            if self.path != "/save":
                self._send(404, "not found"); return
            if not _same_origin(self):
                log.warning("rejected /save: cross-origin or missing Origin/Referer")
                self._send(403, "forbidden: cross-origin request"); return
            n = int(self.headers.get("Content-Length", 0) or 0)
            form = urllib.parse.parse_qs(self.rfile.read(n).decode())
            if not hmac.compare_digest(form.get("csrf_token", [""])[0], csrf_token):
                log.warning("rejected /save: bad or missing CSRF token")
                self._send(403, "forbidden: invalid CSRF token"); return
            try:
                srv.apply_config(**_ui_parse(srv, form))
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
                        help="serve the config UI on this port (0 = off)")
    parser.add_argument("--ui-host", default="127.0.0.1",
                        help="config UI bind address (default: localhost only)")
    parser.add_argument("--ui-allow-remote", action="store_true",
                        help="permit binding the config UI to a non-loopback address")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    with open(args.config) as f:
        config = json.load(f)

    server = PriceServer(config, dry_run=args.dry_run, config_path=args.config)
    if args.ui_port:
        if not _is_loopback(args.ui_host) and not args.ui_allow_remote:
            parser.error(
                "refusing to bind the config UI to non-loopback address %r: it has no "
                "authentication beyond same-origin/CSRF and writes the live config. Pass "
                "--ui-allow-remote to override (and put it behind your own auth/firewall)." % args.ui_host)
        start_config_ui(server, args.ui_host, args.ui_port)
    if args.once:
        print(json.dumps(server.poll_once(), indent=2))
    else:
        server.run()


if __name__ == "__main__":
    sys.exit(main())
