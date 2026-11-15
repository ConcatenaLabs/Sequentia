#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Caching, request-deduplicating Bitcoin-RPC proxy for a many-node anchored
testnet on a single machine.

The Sequentia mainchain-RPC client speaks plain HTTP + Basic auth; a public
Bitcoin endpoint is HTTPS and rate-limited. A naive forwarding proxy (one curl
per request, no cache) collapses under a 100-node committee: every node validates
the same anchor against the same parent block, so the public endpoint sees a
flood of *identical* requests, throttles, and the nodes stall in ConnectTip
"waiting for parent chain daemon" — the chain never advances past the first block.

This proxy fixes that with two things:
  * a short-TTL response cache keyed by (method, params): 100 nodes asking the
    same getblockheader/getblockhash/getbestblockhash within the TTL get one
    upstream call, not 100;
  * per-key in-flight de-duplication: simultaneous identical misses (the block-1
    validation burst) wait on one upstream call instead of stampeding it.

The parent (Bitcoin testnet) only changes every ~10 minutes, so a 10 s TTL is
far fresher than the anchor needs. Auth headers are ignored (the public endpoint
needs none) — point the nodes' -mainchainrpcuser/-password at anything.

What it keeps about its callers: the time each client address last made a
call, in memory only, for a day (SEEN_TTL), for at most SEEN_MAX addresses
(SEQ_PROXY_SEEN_MAX, default 100000); an entry older than a day is dropped,
and a restart forgets them all. Nothing else is recorded, and no request is
logged. The record answers one question, whether a given address has been
using this endpoint lately, and only to the machine the proxy runs on, so it
says how far it can be trusted: "since" is when the record began (the
proxy's start), and "dropped_last" is the last time it was full and left a
new caller out (null when never). An address absent from the record has made
no call since "since", except possibly at "dropped_last" and before. The
answers, to loopback callers only (anyone else gets 404):
  GET /last-seen?addr=<address>
      {"addr": ..., "last": <seconds or null>, "since": <seconds>, "dropped_last": <seconds or null>}
  GET /last-seen
      {"since": ..., "dropped_last": ..., "seen": {<address>: <seconds>, ...}}

Run it and leave it open:  python3 testnet-rpc-cache-proxy.py
"""

import http.server
import json
import os
import socketserver
import subprocess
import threading
import time
import urllib.parse

# Upstream Bitcoin RPC endpoint and listen address are env-configurable so the
# same proxy serves testnet3, testnet4, or any gateway:
#   SEQ_PROXY_TARGET   upstream URL (default: a public testnet3 endpoint)
#   SEQ_PROXY_HEADER   an extra HTTP header to send upstream, e.g. an API key
#                      'x-api-key: <KEY>' for gateways like Tatum (optional)
#   SEQ_PROXY_PORT     local listen port (default 18332)
# testnet4 via Tatum:
#   SEQ_PROXY_TARGET=https://bitcoin-testnet4.gateway.tatum.io/ \
#   SEQ_PROXY_HEADER='x-api-key: <your-tatum-key>' python3 testnet-rpc-cache-proxy.py
TARGET = os.environ.get("SEQ_PROXY_TARGET", "https://bitcoin-testnet-rpc.publicnode.com/")
EXTRA_HEADER = os.environ.get("SEQ_PROXY_HEADER", "")     # e.g. "x-api-key: ..."
LISTEN = (os.environ.get("SEQ_PROXY_BIND", "127.0.0.1"), int(os.environ.get("SEQ_PROXY_PORT", "18332")))
UA = "curl/8.5.0"
TTL = 10.0                       # seconds a cached result stays fresh
UPSTREAM_TIMEOUT = 25            # curl -m

_cache = {}                      # key -> (expiry, result_obj)
_locks = {}                      # key -> Lock (in-flight dedup)
_guard = threading.Lock()
_stats = {"hits": 0, "upstream": 0}

SEEN_TTL = 86400.0               # seconds a caller's last-call time is kept
SEEN_MAX = int(os.environ.get("SEQ_PROXY_SEEN_MAX", "100000"))  # addresses kept at most
_seen = {}                       # client address -> unix time of its last call
_seen_guard = threading.Lock()
_seen_pruned = [0.0]
_seen_since = time.time()        # when the record began
_seen_dropped = [None]           # last time a new caller was left out: the record was full


def _norm_addr(addr):
    """An IPv4 caller on a dual-stack socket arrives as ::ffff:a.b.c.d."""
    return addr[7:] if addr.startswith("::ffff:") and "." in addr else addr


def _prune(now):
    _seen_pruned[0] = now
    for a in [a for a, t in _seen.items() if now - t > SEEN_TTL]:
        del _seen[a]


def _note_caller(addr, now=None):
    """Record a call. When the record is full, a new address is left out and
    the time noted, so that the record says it may have missed a caller:
    evicting an entry instead would let a flood of addresses erase one."""
    now = time.time() if now is None else now
    a = _norm_addr(addr)
    with _seen_guard:
        if now - _seen_pruned[0] > 60:
            _prune(now)
        if a not in _seen and len(_seen) >= SEEN_MAX:
            if now - _seen_pruned[0] > 1:
                _prune(now)
            if len(_seen) >= SEEN_MAX:
                _seen_dropped[0] = now
                return
        _seen[a] = now


def _last_seen(addr=None, now=None):
    """The record of one address, or of all, with how far it can be trusted."""
    now = time.time() if now is None else now
    with _seen_guard:
        dropped = None if _seen_dropped[0] is None else int(_seen_dropped[0])
        head = {"since": int(_seen_since), "dropped_last": dropped}
        if addr is not None:
            a = _norm_addr(addr)
            t = _seen.get(a)
            return dict({"addr": a, "last": int(t) if t is not None and now - t <= SEEN_TTL else None}, **head)
        return dict(head, seen={a: int(t) for a, t in _seen.items() if now - t <= SEEN_TTL})


def _is_loopback(addr):
    a = _norm_addr(addr)
    return a == "::1" or a.startswith("127.")

# When this proxy is exposed beyond localhost (e.g. over a tailnet to an external
# tester), block everything that isn't a read-only block/chain query: node
# control, mining, and all wallet/key RPCs. The Sequentia mainchain client only
# ever calls read-only block/header/chain methods, which pass through.
_DENY = {"stop", "setban", "clearbanned", "setnetworkactive", "addnode",
         "disconnectnode", "invalidateblock", "reconsiderblock", "submitblock",
         "submitheader", "generatetoaddress", "generateblock", "setmocktime",
         "pruneblockchain", "savemempool", "addconnection"}
_DENY_PREFIX = ("wallet", "import", "dump", "backup", "encrypt", "send", "sign",
                "sethdseed", "createwallet", "loadwallet", "unloadwallet",
                "getnewaddress", "getrawchangeaddress", "listunspent",
                "listtransactions", "abandontransaction", "rescan", "keypool")


def _denied(method):
    if not isinstance(method, str):
        return True
    m = method.lower()
    return m in _DENY or any(m.startswith(p) for p in _DENY_PREFIX)


def _key_lock(key):
    with _guard:
        lk = _locks.get(key)
        if lk is None:
            lk = _locks[key] = threading.Lock()
        return lk


def _upstream(body):
    # application/json (not text/plain): stricter gateways (e.g. Tatum) reject
    # text/plain; standard endpoints accept JSON either way.
    cmd = ["curl", "-s", "-m", str(UPSTREAM_TIMEOUT), "-A", UA,
           "--data-binary", "@-", "-H", "content-type: application/json"]
    if EXTRA_HEADER:
        cmd += ["-H", EXTRA_HEADER]
    cmd.append(TARGET)
    p = subprocess.run(cmd, input=body, capture_output=True)
    return p.stdout


def _with_id(obj, rid):
    try:
        o = dict(obj)
        o["id"] = rid
        return json.dumps(o).encode()
    except Exception:
        return json.dumps(obj).encode()


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        if url.path != "/last-seen" or not _is_loopback(self.client_address[0]):
            self._reply(404, b'{"error":"not found"}')
            return
        q = urllib.parse.parse_qs(url.query)
        out = _last_seen(q["addr"][0]) if "addr" in q else _last_seen()
        self._reply(200, json.dumps(out).encode())

    def _reply(self, code, out):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def do_POST(self):
        _note_caller(self.client_address[0])
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        out = self._serve(body)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def _serve(self, body):
        try:
            req = json.loads(body)
        except Exception:
            return _upstream(body) or b'{"result":null,"error":{"code":-1,"message":"proxy: bad request"},"id":null}'
        if isinstance(req, list):                      # batch bypasses the deny-check; refuse it
            return b'{"result":null,"error":{"code":-32600,"message":"proxy: batch not permitted"},"id":null}'

        method, params, rid = req.get("method"), req.get("params", []), req.get("id")
        if _denied(method):
            return json.dumps({"result": None, "error": {"code": -32601, "message": "proxy: method not permitted"}, "id": rid}).encode()
        key = json.dumps([method, params], sort_keys=True)
        now = time.time()

        ent = _cache.get(key)
        if ent and ent[0] > now:
            _stats["hits"] += 1
            return _with_id(ent[1], rid)

        lk = _key_lock(key)
        with lk:                                        # dedup the stampede
            ent = _cache.get(key)
            if ent and ent[0] > time.time():
                _stats["hits"] += 1
                return _with_id(ent[1], rid)
            raw = _upstream(body)
            _stats["upstream"] += 1
            if _stats["upstream"] % 50 == 0:
                print("upstream=%d  cache_hits=%d  cached_keys=%d"
                      % (_stats["upstream"], _stats["hits"], len(_cache)), flush=True)
            try:
                obj = json.loads(raw)
            except Exception:
                return raw or b'{"result":null,"error":{"code":-1,"message":"proxy: empty upstream"},"id":null}'
            if isinstance(obj, dict) and obj.get("error") is None:   # cache successes only
                _cache[key] = (time.time() + TTL, obj)
            return _with_id(obj, rid)

    def log_message(self, *a):
        pass


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    # Default socketserver backlog is 5 — far too small for ~100 nodes opening
    # connections at once (the rest get reset, and those nodes stall). Queue many.
    request_queue_size = 256


if __name__ == "__main__":
    print("Caching proxy on http://%s:%d -> %s  (TTL %ss). Leave open."
          % (LISTEN[0], LISTEN[1], TARGET, TTL), flush=True)
    S(LISTEN, H).serve_forever()
