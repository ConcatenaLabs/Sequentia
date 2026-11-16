#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers. MIT license.
"""Market-data API for the Sequentia open-fee-market testnet.

Serves REAL market prices (USD) for the tokenized real-world assets, polled from Yahoo Finance, in
the JSON shape the price server jsonapi source expects. USD is the reference unit: USDX and USDC
1:1 USD, EURX and EURC=EUR/USD, GOLD/SILVR/OILX=gold/silver/WTI spot, tBTC=BTC/USD. Only tSEQ (SEQ)
is mocked: its planned launch price (0.175 USD), walking at most about 0.05% a tick within +/-10%,
the pace of a quiet market, so the books priced from it rest rather than reprice every tick.
Issuer offering-reference prices are seeded via POST /seed (static).
Fiat rates (USD, EUR) are entries marked `"fiat": true` with no market cap or volume.
Endpoints: GET /prices,/price/<T>,/healthz ; POST /seed.

Every ticker carries `as_of`, the unix time of the reading it serves: a source that stops answering
leaves its last good price in place with its age showing, and a consumer decides how old is too old
(the leaf makers skip a price older than ten minutes). A seeded entry also carries `"static": true`.
A Yahoo ticker is served only once it has been read live: after a restart it is absent from /prices
until its first reading, never a placeholder. A reading more than 25% from the price served is held
until three readings in a row agree on it; one more than 5x away, or not a positive number, is
rejected outright.
"""
import json
import random
import sys
import threading
import time
import urllib.request
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# The fiat rates, as a real feed's exchange-rate endpoint carries them: one entry per
# currency, keyed by its code, in the quote currency (USD), marked "fiat": true and
# with no market cap or volume, which a currency does not have. USD is 1 in a
# USD-quoted feed; EUR is the live EUR/USD rate. The bridged stablecoins are valued
# at these (the price server maps USDC.e to USD and EURC.e to EUR). EURC is kept as
# a market entry under the same EUR/USD reading, and USDC pinned to 1.00 below,
# for the readers that look them up by market symbol.
FIAT = {"USD", "EUR"}
REAL = {"EURX":"EURUSD=X", "EURC":"EURUSD=X", "EUR":"EURUSD=X", "GOLD":"GC=F", "SILVR":"SI=F", "OILX":"CL=F", "tBTC":"BTC-USD"}
SEQ_DAY1 = 0.175
SEQ_STEP = 0.0005    # the tSEQ walk's largest step a tick, before its pull to SEQ_DAY1
SEQ_PULL = 0.01      # the share of its distance to SEQ_DAY1 it closes a tick
_lock = threading.Lock()
# Only what is not read from a source starts in the table: the pinned dollars and the mock tSEQ.
# A Yahoo ticker enters with its first live reading.
_state = {}
_static = set()
_held = {}           # ticker -> the readings held back in a row, each > HOLD from the price served
YURL = "https://query1.finance.yahoo.com/v8/finance/chart/"

def _yahoo(sym, timeout=8):
    req = urllib.request.Request(YURL+urllib.parse.quote(sym), headers={"User-Agent":"Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.load(r)
    return float(d["chart"]["result"][0]["meta"]["regularMarketPrice"])

MAXDEV = 5.0     # a reading more than MAXDEV x, or less than 1/MAXDEV x, the price served is rejected
HOLD = 1.25      # one more than HOLD x, or less than 1/HOLD x, it is held ...
HOLD_READS = 3   # ... until HOLD_READS readings in a row agree on it (within HOLD of each other)

def _judge(t, v):
    """What a LIVE reading does to the price served: 'take', 'hold' or 'reject'.
    A bad-but-valid upstream tick (0, negative, NaN/Inf, a unit slip, a one-off spike) must not
    reprice every maker. The price served stays, with its `as_of` ageing, until the source agrees
    with itself again. The first reading of a ticker has nothing to compare with and is taken."""
    try:
        v = float(v)
    except (TypeError, ValueError):
        return "reject"
    if not (v > 0.0) or v != v or v in (float("inf"), float("-inf")):
        return "reject"
    with _lock:
        last = _state.get(t, {}).get("price")
        if not last or last <= 0:
            _held.pop(t, None)
            return "take"
        if v > last * MAXDEV or v < last / MAXDEV:
            _held.pop(t, None)
            return "reject"
        if v <= last * HOLD and v >= last / HOLD:
            _held.pop(t, None)
            return "take"
        h = _held.setdefault(t, [])
        h.append(v)
        del h[:-HOLD_READS]
        if len(h) == HOLD_READS and max(h) <= min(h) * HOLD:
            _held.pop(t, None)
            return "take"
        return "hold"

def _set(t, price, at=None):
    with _lock:
        if t in FIAT:
            _state[t] = {"price": max(1e-6, price), "fiat": True,
                         "as_of": int(at if at is not None else time.time())}
            return
        s = _state.setdefault(t, {"market_cap":0.0,"volume_24h":0.0})
        s["price"]=max(1e-6,price)
        s["market_cap"]=random.uniform(5e8,5e9)
        s["volume_24h"]=random.uniform(5e6,5e8)
        s["as_of"]=int(at if at is not None else time.time())

def _walk_seq():
    """One tick of the mock tSEQ price: a small random step and a slight pull to its day-1 price,
    within +/-10% of it."""
    with _lock:
        cur = _state.get("SEQ",{}).get("price", SEQ_DAY1)
    cur = cur*(1+random.uniform(-SEQ_STEP,SEQ_STEP))
    cur += (SEQ_DAY1-cur)*SEQ_PULL
    _set("SEQ", min(SEQ_DAY1*1.10, max(SEQ_DAY1*0.90, cur)))

def _fixed():
    _set("USDX", 1.00)
    _set("USDC", 1.00)
    _set("USD", 1.00)

def _refresh(tick):
    while True:
        for t,sym in REAL.items():
            try:
                v = _yahoo(sym)
                j = _judge(t, v)
                if j == "take":
                    _set(t, float(v))
                elif j == "hold":
                    print("feed: %s (%s) held a reading %r far from the price served (%d of %d to agree)"%(t,sym,v,len(_held.get(t,[])),HOLD_READS), flush=True)
                else:
                    print("feed: %s (%s) rejected out-of-band value %r (keeping last-good)"%(t,sym,v), flush=True)
            except Exception as e:
                print("feed: %s (%s) failed: %s"%(t,sym,e), flush=True)
        _fixed()
        _walk_seq()
        time.sleep(tick)

class Handler(BaseHTTPRequestHandler):
    def _json(self,obj,code=200):
        b=json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type","application/json")
        self.send_header("Content-Length",str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def do_GET(self):
        with _lock:
            if self.path=="/prices":
                self._json(_state)
                return
            if self.path=="/healthz":
                self._json({"ok":True,"assets":sorted(_state)})
                return
            if self.path.startswith("/price/"):
                # Match case-INSENSITIVELY against the real keys rather than
                # upper-casing the request. Some tickers are not upper-case --
                # tBTC is the obvious one -- and upper-casing made every such
                # ticker unreachable through /price/<T> while still listing it
                # in /prices, which is a confusing way to be missing.
                # NB do_GET already holds _lock, and threading.Lock is NOT
                # reentrant: acquiring it again here deadlocks this handler
                # while it still holds the outer lock, which wedges every
                # subsequent request too. Do not add a `with _lock` in here.
                want=self.path.split("/price/",1)[1].strip("/")
                key=next((k for k in _state if k.lower()==want.lower()), None)
                if key is not None:
                    self._json(_state[key])
                    return
        self._json({"error":"not found"},404)
    def do_POST(self):
        if self.path!="/seed":
            self._json({"error":"not found"},404)
            return
        try:
            n=int(self.headers.get("Content-Length",0))
            body=json.loads(self.rfile.read(n) or b"{}")
            t=str(body["ticker"]).upper()
            price=float(body["price"])
        except (ValueError,KeyError,TypeError):
            self._json({"error":"expected {ticker,price}"},400)
            return
        with _lock:
            _state[t]={"price":max(1e-6,price),"market_cap":0.0,"volume_24h":0.0,
                       "kind":body.get("kind","offering-reference"),"quote":body.get("quote","USD"),
                       "as_of":int(time.time()),"static":True}
            _static.add(t)
        self._json({"ok":True,"ticker":t,"price":price})
    def log_message(self,*_a): pass

def main():
    port=int(sys.argv[1]) if len(sys.argv)>1 else 8088
    tick=float(sys.argv[2]) if len(sys.argv)>2 else 60.0
    _fixed()
    _walk_seq()
    threading.Thread(target=_refresh,args=(tick,),daemon=True).start()
    print("real price API :%d tick=%.0fs real=%s + SEQ(mock)"%(port,tick,",".join(sorted(REAL))),flush=True)
    ThreadingHTTPServer(("127.0.0.1",port),Handler).serve_forever()

if __name__=="__main__":
    main()
