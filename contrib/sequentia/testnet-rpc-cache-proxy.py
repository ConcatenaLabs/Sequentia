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

Run it and leave it open:  python3 testnet-rpc-cache-proxy.py
"""

import http.server
import json
import os
import socketserver
import subprocess
import threading
import time

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
LISTEN = ("127.0.0.1", int(os.environ.get("SEQ_PROXY_PORT", "18332")))
UA = "curl/8.5.0"
TTL = 10.0                       # seconds a cached result stays fresh
UPSTREAM_TIMEOUT = 25            # curl -m

_cache = {}                      # key -> (expiry, result_obj)
_locks = {}                      # key -> Lock (in-flight dedup)
_guard = threading.Lock()
_stats = {"hits": 0, "upstream": 0}


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

    def do_POST(self):
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
        if isinstance(req, list):                      # batch: forward uncached
            return _upstream(body) or b'{"result":null,"error":{"code":-1,"message":"proxy: empty upstream"},"id":null}'

        method, params, rid = req.get("method"), req.get("params", []), req.get("id")
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
