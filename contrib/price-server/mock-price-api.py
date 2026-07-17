#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers. MIT license.
"""Market-data API for the Sequentia open-fee-market testnet.

Serves REAL market prices (USD) for the tokenized real-world assets, polled from Yahoo Finance, in
the JSON shape the price server jsonapi source expects. USD is the reference unit: USDX 1:1 USD,
EURX=EUR/USD, GOLD/SILVR/OILX=gold/silver/WTI spot, tBTC=BTC/USD. Only tSEQ (SEQ) is mocked: its
planned day-1 price (0.375 USD) with a bounded +/-10% fluctuation. Issuer offering-reference prices
are seeded via POST /seed (static). Endpoints: GET /prices,/price/<T>,/healthz ; POST /seed.
"""
import json, random, sys, threading, time, urllib.request, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REAL = {"EURX":"EURUSD=X", "GOLD":"GC=F", "SILVR":"SI=F", "OILX":"CL=F", "tBTC":"BTC-USD"}
SEQ_DAY1 = 0.375
SEED0 = {"EURX":1.08,"GOLD":2350.0,"SILVR":29.0,"OILX":78.0,"tBTC":64000.0,"SEQ":SEQ_DAY1,"USDX":1.00}
_lock = threading.Lock()
_state = {t:{"price":p,"market_cap":random.uniform(5e8,5e9),"volume_24h":random.uniform(5e6,5e8)} for t,p in SEED0.items()}
_static = set()
YURL = "https://query1.finance.yahoo.com/v8/finance/chart/"

def _yahoo(sym, timeout=8):
    req = urllib.request.Request(YURL+urllib.parse.quote(sym), headers={"User-Agent":"Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r: d = json.load(r)
    return float(d["chart"]["result"][0]["meta"]["regularMarketPrice"])

MAXDEV = 5.0   # reject a live tick that jumps > MAXDEV x or < 1/MAXDEV x the last-good price

def _sane(t, v):
    """Guard a LIVE feed value before it reprices every maker: a bad-but-valid upstream tick
    (0, negative, NaN/Inf, or a wild spike) must NOT replace a good price. Keep-last-good then
    covers it. Returns True only for a finite, positive value within MAXDEV of the last good one."""
    try: v = float(v)
    except (TypeError, ValueError): return False
    if not (v > 0.0) or v != v or v in (float("inf"), float("-inf")): return False
    with _lock: last = _state.get(t, {}).get("price")
    if last and last > 0 and (v > last * MAXDEV or v < last / MAXDEV): return False
    return True

def _set(t, price):
    with _lock:
        s = _state.setdefault(t, {"market_cap":0.0,"volume_24h":0.0})
        s["price"]=max(1e-6,price); s["market_cap"]=random.uniform(5e8,5e9); s["volume_24h"]=random.uniform(5e6,5e8)

def _refresh(tick):
    while True:
        for t,sym in REAL.items():
            try:
                v = _yahoo(sym)
                if _sane(t, v): _set(t, v)
                else: print("feed: %s (%s) rejected out-of-band value %r (keeping last-good)"%(t,sym,v), flush=True)
            except Exception as e: print("feed: %s (%s) failed: %s"%(t,sym,e), flush=True)
        _set("USDX", 1.00)
        with _lock: cur = _state.get("SEQ",{}).get("price", SEQ_DAY1)
        cur = cur*(1+random.uniform(-0.02,0.02)); cur += (SEQ_DAY1-cur)*0.10
        _set("SEQ", min(SEQ_DAY1*1.10, max(SEQ_DAY1*0.90, cur)))
        time.sleep(tick)

class Handler(BaseHTTPRequestHandler):
    def _json(self,obj,code=200):
        b=json.dumps(obj).encode(); self.send_response(code)
        self.send_header("Content-Type","application/json"); self.send_header("Content-Length",str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        with _lock:
            if self.path=="/prices": self._json(_state); return
            if self.path=="/healthz": self._json({"ok":True,"assets":sorted(_state)}); return
            if self.path.startswith("/price/"):
                t=self.path.split("/price/",1)[1].strip("/").upper()
                if t in _state: self._json(_state[t]); return
        self._json({"error":"not found"},404)
    def do_POST(self):
        if self.path!="/seed": self._json({"error":"not found"},404); return
        try:
            n=int(self.headers.get("Content-Length",0)); body=json.loads(self.rfile.read(n) or b"{}")
            t=str(body["ticker"]).upper(); price=float(body["price"])
        except (ValueError,KeyError,TypeError): self._json({"error":"expected {ticker,price}"},400); return
        with _lock:
            _state[t]={"price":max(1e-6,price),"market_cap":0.0,"volume_24h":0.0,
                       "kind":body.get("kind","offering-reference"),"quote":body.get("quote","USD")}
            _static.add(t)
        self._json({"ok":True,"ticker":t,"price":price})
    def log_message(self,*_a): pass

def main():
    port=int(sys.argv[1]) if len(sys.argv)>1 else 8088
    tick=float(sys.argv[2]) if len(sys.argv)>2 else 60.0
    threading.Thread(target=_refresh,args=(tick,),daemon=True).start()
    print("real price API :%d tick=%.0fs real=%s + SEQ(mock)"%(port,tick,",".join(sorted(REAL))),flush=True)
    ThreadingHTTPServer(("127.0.0.1",port),Handler).serve_forever()

if __name__=="__main__": main()
