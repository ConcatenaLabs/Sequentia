#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Anchor verdicts taken while Bitcoin stood on a branch it then left.

A node checks a block's anchor against its Bitcoin daemon when the block
arrives, and caches the answer. The anchor watcher drops the cached answers a
Bitcoin change could have made wrong, once per poll. Comparing its last tip with
the current one cannot show a branch the daemon moved onto and back from
between two polls: the return reads as a plain extension, or as no move at all.
A verdict taken during the replacement then survives the poll. In one
direction the node refuses, for good, every block anchored on the restored
Bitcoin block; in the other it keeps a block anchored on a Bitcoin block that
is no longer on the best chain, and accepts more.

The watcher reads the daemon's chain tips (getchaintips) at every poll where
the tip moved, and compares them with the list read at the previous such poll.
A branch the daemon stood on in between is a connected tip that is new or
whose status changed, or a tip whose last block was never connected and that
has more work than the old tip; every verdict above its fork point is dropped.
A branch merely observed (a new header-only fork with less work) only has the
verdicts above its fork asked again. This test makes the daemon move onto a
branch and back between two polls of the node under test, with checks during
the replacement, and asserts what the node holds after the next poll:

  1. one block replaced and restored, then extended (the case of #467), with
     the mirror: a block anchored on the replacing branch is accepted during
     the replacement and is dropped after the poll, and another is refused;
  2. the same, restored without an extension, which only the daemon's
     operator can do: the tip does not move, the wrong verdicts stay until the
     next Bitcoin block, and that poll corrects them;
  3. two blocks replaced and restored: no wrong verdict at either height;
  4. one block replaced, the daemon returns, and the replacing branch grows by a
     block the daemon does not connect before the poll: the branch's tip then
     reads "valid-headers", which hides that the branch was active;
  5. a header-only fork with less work announced from below the tip: at the
     next poll where the tip moves, the verdicts above its fork are asked again
     once, and nothing at the moves after;
  6. a daemon that answers its best hash but refuses getchaintips: nothing is
     kept across a tip move, quiet polls cost nothing more, and the node says
     so once in its log;
  7. the cost: a poll where Bitcoin stood still costs one getbestblockhash, a
     plain extension adds the extension check, the checkpoint scan and one
     getchaintips, and a question answered from the cache costs nothing;
  8. a replacing branch whose last block the daemon held only as a header
     before the replacement: the list of tips reads the same before and after,
     and only the branch's work, more than the old tip's, shows it;
  9. a proxy that caches getchaintips for 10 s, as the testnet's shared
     endpoint does, answers the moving poll with a list from before the
     branch: the reading once more than 12 s after the move finds it, with no
     Bitcoin block;
 10. more tips than the node reads (-anchortipsmax), counted and by the length
     of the reply: nothing is kept across a move, and the node says so once;
 11. two moves within the settle time, then a header-only sibling of the
     middle tip: no comparison takes it for a branch the daemon stood on (it
     has nothing above the fork but itself, and each comparison pairs a list
     with the old tip of its own move);
 12. a cached flip onto a branch whose next block failed, back, and one more
     move before the first move's settle time: the first move's comparison,
     done again with a fresh list, still finds the failed branch.

Topology:
  node0: the parent chain (stands in for Bitcoin): proof of work, so that its
         headers carry their chainwork.
  node1: the producer: a single genesis-configured staker, so every
         generateposblock block is certified on its own; polls every second.
  node2: the node under test: no stake, polls every POLL seconds, and gets
         blocks only by submitblock, so that each case decides what it sees.
  node3: a second parent-chain node, never connected, that mines the blocks the
         parent must learn as headers or as blocks it does not connect.
"""

import base64
import hashlib
import http.server
import json
import os
import re
import threading
import time
import urllib.error
import urllib.request

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    get_auth_cookie,
    get_datadir_path,
    p2p_port,
    rpc_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

POLL = 8   # default -anchorpollinterval of the node under test, seconds (the testnet uses 15: --poll=15)
CHAIN = "elementsregtest"


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def bad_coinbase_height(block_hex, header_len):
    """A copy of a one-transaction proof-of-work parent block whose coinbase
    names another height, with its merkle root and proof of work made good
    again: a parent marks it failed when it arrives (bad-cb-height), before
    it could ever be connected."""
    raw = bytearray(bytes.fromhex(block_hex))
    hl = header_len

    def dsha(b):
        return hashlib.sha256(hashlib.sha256(b).digest()).digest()

    def varint(i):
        v = raw[i]
        if v < 0xfd:
            return v, i + 1
        n = {0xfd: 2, 0xfe: 4, 0xff: 8}[v]
        return int.from_bytes(raw[i + 1:i + 1 + n], "little"), i + 1 + n

    ntx, i = varint(hl)
    assert ntx == 1
    tx = i
    i += 5                                     # version, flags
    nin, i = varint(i)
    assert nin == 1
    i += 36                                    # the null prevout
    slen, i = varint(i)
    if 0x51 <= raw[i] <= 0x60:                 # a height of 1 to 16 is OP_1..OP_16
        raw[i] = raw[i] + 1 if raw[i] < 0x60 else 0x5f
    else:
        assert 1 <= raw[i] <= 0x4b             # otherwise pushed as data, first
        raw[i + 1] ^= 0x01                     # another height
    i += slen + 4
    nout, i = varint(i)
    for _ in range(nout):
        i += 33 if raw[i] in (0x01, 0x0a, 0x0b) else 1         # asset
        i += 9 if raw[i] == 0x01 else (33 if raw[i] in (0x08, 0x09) else 1)  # value
        i += 33 if raw[i] in (0x02, 0x03) else 1               # nonce
        sl, i = varint(i)
        i += sl
    i += 4                                     # lock time
    raw[36:68] = dsha(bytes(raw[tx:tx + 4]) + b"\x00" + bytes(raw[tx + 5:i]))   # one transaction: its txid
    bits = int.from_bytes(raw[hl - 8:hl - 4], "little")
    target = (bits & 0xffffff) << (8 * ((bits >> 24) - 3))
    for nonce in range(1 << 32):
        raw[hl - 4:hl] = nonce.to_bytes(4, "little")
        if int.from_bytes(dsha(bytes(raw[:hl])), "little") <= target:
            break
    return raw.hex()


class TipsProxy:
    """Forwards JSON-RPC to the parent, and does one thing to getchaintips:

      refuse: answers it with an error (an endpoint that answers for its tip
              but not for its branches);
      pad:    pads its reply with `pad` bytes of whitespace (a long list);
      cache:  serves it from a cache for `ttl` seconds after a reply, as the
              testnet's shared endpoint does; prime() loads the cache."""

    def __init__(self, port, target_port, mode, pad=0, ttl=10.0, auth=None):
        target = "http://127.0.0.1:%d/" % target_port
        self.port, self.mode, self.cache, self.hits, self.auth = port, mode, {}, 0, auth
        proxy = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                req = json.loads(body)
                tips = isinstance(req, dict) and req.get("method") == "getchaintips"
                if tips and proxy.mode == "refuse":
                    self.reply(json.dumps({"result": None, "id": req.get("id"),
                                           "error": {"code": -32601, "message": "Method not found"}}).encode())
                    return
                if tips and proxy.mode == "cache":
                    hit = proxy.cache.get("tips")
                    if hit and hit[0] > time.time():
                        proxy.hits += 1
                        obj = dict(hit[1])
                        obj["id"] = req.get("id")
                        self.reply(json.dumps(obj).encode())
                        return
                fwd = urllib.request.Request(target, data=body, headers={
                    "Authorization": self.headers.get("Authorization", ""),
                    "Content-Type": "application/json"})
                try:
                    with urllib.request.urlopen(fwd, timeout=30) as r:
                        out = r.read()
                except urllib.error.HTTPError as e:
                    out = e.read()
                if tips and proxy.mode == "cache":
                    obj = json.loads(out)
                    if obj.get("error") is None:
                        proxy.cache["tips"] = (time.time() + ttl, obj)
                if tips and proxy.mode == "pad":
                    out = out[:-1] + b" " * pad + out[-1:]
                self.reply(out)

            def reply(self, out):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def prime(self):
        """Someone else's getchaintips through the proxy: loads its cache
        afresh, for the next `ttl` seconds."""
        self.cache.clear()
        req = urllib.request.Request("http://127.0.0.1:%d/" % self.port,
                                     data=json.dumps({"method": "getchaintips", "params": [], "id": 1}).encode())
        req.add_header("Authorization", "Basic " + base64.b64encode(self.auth.encode()).decode())
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read())["result"]

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


class UnseenBranchTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--poll", type=int, default=POLL,
                            help="-anchorpollinterval of the node under test (default: %(default)s)")

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.staker_wif, self.staker_pub = make_staker()
        self.results = {}

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def parent_args(self, i):
        # Proof of work, as on Bitcoin: a signed-blocks parent reports no
        # chainwork, and the watcher then counts every unconnected tip as a
        # branch it may have stood on.
        return ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
                "-validatepegin=0", "-initialfreecoins=0", "-con_blocksubsidy=5000000000",
                "-anyonecanspendaremine=1", "-con_signed_blocks=0", "-blindedaddresses=0"]

    def seq_args(self, i, poll, parent_port=None):
        return ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
                "-validatepegin=0", "-initialfreecoins=0", "-con_blocksubsidy=5000000000",
                "-anyonecanspendaremine=1", "-signblockscript=51", "-blindedaddresses=0",
                "-con_pos=1", "-posvrf=1", "-posslotinterval=1", "-staker=%s:1" % self.staker_pub,
                "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorminconf=1",
                "-anchorpollinterval=%d" % poll, "-debug=validation",
                "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % (parent_port or rpc_port(0)),
                "-mainchainrpcuser=%s" % self.rpc_u, "-mainchainrpcpassword=%s" % self.rpc_p,
                "-parentgenesisblockhash=%s" % self.parentgenesis]

    def setup_network(self, split=False):
        self.nodes = []
        self.add_nodes(1, [self.parent_args(0)], chain=[CHAIN])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)
        self.rpc_u, self.rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), CHAIN)
        # The producer always anchors on the parent's tip: the forks this test
        # leaves behind must not pull its anchors back.
        self.add_nodes(1, [self.seq_args(1, 1) + ["-anchoravoidcontested=0"]], chain=[CHAIN])
        self.start_node(1)
        self.validator_args = self.seq_args(2, self.options.poll)
        self.add_nodes(1, [self.validator_args], chain=[CHAIN])
        self.start_node(2)
        self.add_nodes(1, [self.parent_args(3)], chain=[CHAIN])
        self.start_node(3)

    # --- helpers ------------------------------------------------------------

    def mine(self, n=1, node=None):
        node = node or self.nodes[0]
        return self.generatetoaddress(node, n, self.parent_addr, sync_fun=self.no_op)

    def produce(self, expect_anchor):
        prod = self.nodes[1]
        self.wait_until(lambda: self._try_produce(), timeout=60)
        h = prod.getbestblockhash()
        assert_equal(prod.getblockheader(h)["anchorhash"], expect_anchor)
        return h, prod.getblock(h, 0)

    def _try_produce(self):
        prod = self.nodes[1]
        before = prod.getbestblockhash()
        try:
            prod.generateposblock(self.staker_wif)
        except Exception:
            return False
        return prod.getbestblockhash() != before

    def stats(self, node=None):
        return dict((node or self.nodes[2]).getmainchainrpcstats()["bymethod"])

    def ticks(self):
        return self.stats().get("getbestblockhash", 0)

    def wait_ticks(self, n):
        start = self.ticks()
        self.wait_until(lambda: self.ticks() >= start + n, timeout=self.options.poll * (n + 4))

    def tick_done(self):
        """Wait for a poll of the node under test to begin, then give it a
        moment to finish (its work takes milliseconds); return its stats."""
        start = self.ticks()
        self.wait_until(lambda: self.ticks() > start, timeout=self.options.poll * 3)
        time.sleep(1)
        return self.stats()

    def open_window(self):
        """Return the tick count just after a poll finished: what follows
        happens before the next."""
        return self.tick_done()["getbestblockhash"]

    @staticmethod
    def delta(s0, s1):
        return {m: s1.get(m, 0) - s0.get(m, 0) for m in s1 if s1.get(m, 0) != s0.get(m, 0)}

    def measure(self, polls, between=None):
        """Calls per method over `polls` whole polls, sampled after each end
        poll has finished; `between` runs right after the first sample."""
        s0 = self.tick_done()
        if between:
            between()
        for _ in range(polls - 1):
            self.wait_ticks(1)
        s1 = self.tick_done()
        return self.delta(s0, s1)

    def close_window(self, opened):
        assert_equal(self.ticks(), opened)  # a poll fell inside the flip: rerun

    def after_next_poll(self):
        """Return once a poll that began after this call has finished."""
        self.wait_ticks(2)

    def on_active_chain(self, h):
        try:
            return self.nodes[2].getblockheader(h)["confirmations"] >= 1
        except Exception:
            return False

    def settle(self):
        """Fresh caches on the node under test, synced to the producer, and two
        polls with the parent standing still."""
        v, prod = self.nodes[2], self.nodes[1]
        self.restart_node(2, extra_args=self.validator_args)
        self.connect_nodes(1, 2)
        self.wait_until(lambda: v.getbestblockhash() == prod.getbestblockhash(), timeout=120)
        self.disconnect_nodes(1, 2)
        self.wait_ticks(2)
        return v.getbestblockhash()

    def sync_node3(self, height):
        """Give the never-connected parent node3 node0's blocks up to `height`."""
        p, q = self.nodes[0], self.nodes[3]
        for h in range(1, height + 1):
            hh = p.getblockhash(h)
            try:
                q.reconsiderblock(hh)              # an earlier case may have invalidated it there
            except Exception:
                pass                               # not known to node3 yet
            q.submitblock(p.getblock(hh, 0))
        self.node3_to(p.getblockhash(height))

    def node3_to(self, target):
        """Make `target` node3's tip: node3 may hold forks of earlier cases
        with as much work or more."""
        q = self.nodes[3]
        for _ in range(20):
            if q.getbestblockhash() == target:
                break
            q.preciousblock(target)
            if q.getbestblockhash() != target:
                q.invalidateblock(q.getbestblockhash())
        assert_equal(q.getbestblockhash(), target)

    def record(self, name, ok, detail):
        self.results[name] = (ok, detail)
        self.log.info("%s: %s (%s)" % (name, "as expected" if ok else "WRONG", detail))

    # --- the cases ----------------------------------------------------------

    def one_deep(self, extend):
        """Cases 1 and 2."""
        p, prod, v = self.nodes[0], self.nodes[1], self.nodes[2]
        name = "one deep, %s" % ("restored and extended" if extend else "restored, no extension, then a block")
        a = self.mine()[0]
        x, x_hex = self.produce(a)
        prod.invalidateblock(x)
        self.settle()

        opened = self.open_window()
        p.invalidateblock(a)
        b = self.mine()[0]
        y, y_hex = self.produce(b)
        prod.invalidateblock(y)
        y2, y2_hex = self.produce(b)
        during_x = v.submitblock(x_hex)
        during_y = v.submitblock(y_hex)
        y_was_tip = v.getbestblockhash() == y
        p.reconsiderblock(a)
        p.preciousblock(a)
        if extend:
            self.mine()
        self.close_window(opened)
        assert_equal(during_x, "anchor-stale")      # correct: A is off
        assert_equal(during_y, None)                # correct: B is on
        assert y_was_tip
        tips = {t["hash"]: t["status"] for t in p.getchaintips()}
        assert_equal(tips[b], "valid-fork")

        self.after_next_poll()
        if not extend:
            # The tip did not move, so the branches were not read: the stated
            # limit, which only the daemon's operator can reach. It lasts until
            # the next Bitcoin block.
            held = (v.submitblock(x_hex), self.on_active_chain(y))
            self.record(name + ": until the next Bitcoin block the wrong verdicts stay (operator-only limit)",
                        held == ("anchor-stale", True), "X %r, Y on the active chain %r" % held)
            self.mine()
            self.after_next_poll()
        y_dropped = not self.on_active_chain(y)
        after_x = v.submitblock(x_hex)
        after_y2 = v.submitblock(y2_hex)
        self.record(name + ": the block on the restored anchor is accepted",
                    after_x is None and v.getbestblockhash() == x, "submitblock %r" % after_x)
        self.record(name + ": the active block anchored on the replacing branch is dropped",
                    y_dropped, "still on the active chain" if not y_dropped else "dropped")
        self.record(name + ": a new block anchored on the replacing branch is refused",
                    after_y2 == "anchor-stale", "submitblock %r" % after_y2)
        prod.reconsiderblock(x)
        self.wait_until(lambda: prod.getbestblockhash() == x, timeout=60)

    def two_deep(self):
        """Case 3."""
        p, prod, v = self.nodes[0], self.nodes[1], self.nodes[2]
        name = "two deep"
        a1 = self.mine()[0]
        x1, x1_hex = self.produce(a1)
        prod.invalidateblock(x1)
        a2 = self.mine()[0]
        x2, x2_hex = self.produce(a2)
        prod.invalidateblock(x2)
        self.settle()

        opened = self.open_window()
        p.invalidateblock(a1)
        b1 = self.mine()[0]
        y1, y1_hex = self.produce(b1)
        b2 = self.mine()[0]
        y2, y2_hex = self.produce(b2)
        prod.invalidateblock(y1)
        z2, z2_hex = self.produce(b2)
        during = [v.submitblock(h) for h in (x1_hex, x2_hex, y1_hex, y2_hex)]
        p.reconsiderblock(a1)
        p.preciousblock(a2)
        self.mine()
        self.close_window(opened)
        assert_equal(during, ["anchor-stale", "anchor-stale", None, None])

        self.after_next_poll()
        y_dropped = not self.on_active_chain(y1) and not self.on_active_chain(y2)
        after_x1 = v.submitblock(x1_hex)
        after_x2 = v.submitblock(x2_hex)
        after_z2 = v.submitblock(z2_hex)
        self.record(name + ": the blocks on both restored anchors are accepted",
                    after_x1 is None and after_x2 in (None, "inconclusive"),
                    "submitblock %r, %r" % (after_x1, after_x2))
        self.record(name + ": the active blocks anchored on the replacing branch are dropped",
                    y_dropped, "dropped" if y_dropped else "still on the active chain")
        self.record(name + ": a new block anchored on the upper replacing block is refused",
                    after_z2 == "anchor-stale", "submitblock %r" % after_z2)
        prod.reconsiderblock(x1)
        self.wait_until(lambda: prod.getbestblockhash() == x1, timeout=60)
        # X1 and X2 are siblings of equal work: leave the node under test on
        # the producer's, whichever it took.
        if v.getbestblockhash() == x2:
            v.invalidateblock(x2)
        self.wait_until(lambda: v.getbestblockhash() == x1, timeout=60)

    def grown_branch(self):
        """Case 4: the replacing branch grows by a block the daemon never connects."""
        p, prod, v, q = self.nodes[0], self.nodes[1], self.nodes[2], self.nodes[3]
        name = "replacing branch grown before the poll"
        self.sync_node3(p.getblockcount())
        a1 = self.mine()[0]
        x1, x1_hex = self.produce(a1)
        prod.invalidateblock(x1)
        self.settle()

        opened = self.open_window()
        p.invalidateblock(a1)
        b1 = self.mine()[0]
        y1, y1_hex = self.produce(b1)
        during_x = v.submitblock(x1_hex)
        during_y = v.submitblock(y1_hex)
        p.reconsiderblock(a1)
        p.preciousblock(a1)
        self.mine()
        # B2 on B1, mined elsewhere: as much work as the parent's tip, so it
        # keeps the tip it saw first and stores B2 without connecting it.
        q.submitblock(p.getblock(b1, 0))
        b2 = self.mine(1, node=q)[0]
        p.submitblock(q.getblock(b2, 0))
        self.close_window(opened)
        assert_equal((during_x, during_y), ("anchor-stale", None))
        tips = {t["hash"]: t for t in p.getchaintips()}
        assert b1 not in tips
        assert_equal(tips[b2]["status"], "valid-headers")

        self.after_next_poll()
        y_dropped = not self.on_active_chain(y1)
        after_x = v.submitblock(x1_hex)
        self.record(name + ": the block on the restored anchor is accepted",
                    after_x is None, "submitblock %r" % after_x)
        self.record(name + ": the active block anchored on the replaced block is dropped",
                    y_dropped, "dropped" if y_dropped else "still on the active chain")
        prod.reconsiderblock(x1)
        self.wait_until(lambda: prod.getbestblockhash() == x1, timeout=60)

    def build_anchors(self, n):
        for _ in range(n):
            self.mine()
            self.nodes[1].generateposblock(self.staker_wif)

    def distinct_anchors(self, above=-1):
        v = self.nodes[2]
        out = set()
        for h in range(1, v.getblockcount() + 1):
            hdr = v.getblockheader(v.getblockhash(h))
            if hdr["anchorheight"] > above:
                out.add((hdr["anchorheight"], hdr["anchorhash"]))
        return out

    def quiet_cost(self, ticks=4):
        """Calls per method over `ticks` polls with the parent standing still."""
        return self.measure(ticks)

    def anchors_above(self, height):
        """Distinct anchors above `height` the node under test would ask about
        again: on its active chain and in its recovery set."""
        v = self.nodes[2]
        out = self.distinct_anchors(height)
        for h in v.getanchorstatus()["recoveryset"]:
            hdr = v.getblockheader(h)
            if hdr["anchorheight"] > height:
                out.add((hdr["anchorheight"], hdr["anchorhash"]))
        return len(out)

    def cost(self):
        """Case 7."""
        self.build_anchors(12)
        self.settle()
        n = len(self.distinct_anchors())
        quiet = self.quiet_cost()
        self.log.info("quiet polls, %d distinct anchors on the chain: %r" % (n, quiet))
        self.record("cost: a quiet poll costs one getbestblockhash only",
                    set(quiet) == {"getbestblockhash"}, "%r" % quiet)
        ext = self.measure(3, between=self.mine)
        self.log.info("one parent extension, 3 polls: %r" % ext)
        # Beyond the poll's getbestblockhash: one header read that finds the
        # move was an extension, the checkpoint scan's read of the new block (a
        # header and the block), and the branch tips, at the move and once
        # more at the first poll 12 s after it. Nothing scales with the chain.
        polls = ext.pop("getbestblockhash", 0)
        self.record("cost: a plain extension adds the extension check, the checkpoint scan and two readings of the tips",
                    ext == {"getblockheader": 2, "getblock": 1, "getchaintips": 2},
                    "%r beyond %d polls" % (ext, polls))
        # A question the cache answers: the tip's anchor, via getanchorstatus.
        s0 = self.stats()
        for _ in range(5):
            assert_equal(self.nodes[2].getanchorstatus()["anchorstatus"], "ok")
        s1 = self.stats()
        asked = {m: d for m, d in self.delta(s0, s1).items() if m != "getbestblockhash"}
        self.record("cost: a question answered from the cache costs nothing", asked == {}, "%r" % asked)

    def header_fork(self):
        """Case 5."""
        p, q = self.nodes[0], self.nodes[3]
        self.build_anchors(4)
        self.settle()
        fork_height = p.getblockcount() - 3
        self.sync_node3(fork_height)
        h = self.mine(1, node=q)[0]
        above = self.anchors_above(fork_height)

        def announce():
            assert p.submitheader(q.getblockheader(h, False)) is None
        quiet = self.measure(2, between=announce)
        tips = {t["hash"]: t for t in p.getchaintips()}
        assert_equal((tips[h]["status"], tips[h]["height"] - tips[h]["branchlen"]), ("headers-only", fork_height))
        first = self.measure(2, between=self.mine)
        later = self.measure(2, between=self.mine)
        self.log.info("header-only fork at %d, %d anchors above it: %r while the tip stood still, %r at the move after, "
                      "%r at the next move" % (fork_height, above, quiet, first, later))
        self.record("header-only fork: nothing while the tip stands still", set(quiet) == {"getbestblockhash"}, "%r" % quiet)
        # The move: its extension check and checkpoint scan (2 headers, 1
        # block), and the anchors above the fork. The fork is one header alone
        # above its fork point, never connected, so its chainwork is not even
        # asked: it cannot be evidence.
        self.record("header-only fork: at the next move, the verdicts above its fork are asked again once",
                    first.get("getblockheader", 0) == 2 + above,
                    "%d header reads, %d anchors above the fork" % (first.get("getblockheader", 0), above))
        self.record("header-only fork: and nothing at the moves after",
                    later.get("getblockheader", 0) == 2, "%d header reads" % later.get("getblockheader", 0))

    def refused_chaintips(self):
        """Case 6."""
        v = self.nodes[2]
        port = rpc_port(5)
        proxy = TipsProxy(port, rpc_port(0), "refuse")
        try:
            args = self.seq_args(2, self.options.poll, parent_port=port)
            self.restart_node(2, extra_args=args)
            self.wait_ticks(2)
            quiet = self.quiet_cost(3)
            moves = []
            for _ in range(2):
                n = self.anchors_above(-1)
                moves.append((self.measure(2, between=self.mine), n))
            self.log.info("getchaintips refused: %r over quiet polls; %r at two moves (distinct anchors)" % (quiet, moves))
            with open(v.debug_log_path, encoding="utf-8") as f:
                warnings = f.read().count("does not answer getchaintips")
            self.record("getchaintips refused: quiet polls cost nothing more",
                        set(quiet) == {"getbestblockhash"}, "%r" % quiet)
            # Per getchaintips call (at the move, and at the poll 12 s after
            # it): every distinct anchor asked again, beyond the extension
            # check and the checkpoint scan.
            per_call = [(m.get("getblockheader", 0) - 2) / max(m.get("getchaintips", 0), 1) for m, _ in moves]
            self.record("getchaintips refused: every verdict is asked again at every move",
                        all(m.get("getchaintips", 0) >= 1 for m, _ in moves) and all(pc >= n for pc, (_, n) in zip(per_call, moves)),
                        "%s re-asked per getchaintips call, %s distinct anchors" % (per_call, [n for _, n in moves]))
            self.record("getchaintips refused: said once in the log", warnings == 1, "%d warning(s)" % warnings)
        finally:
            self.restart_node(2, extra_args=self.validator_args)
            proxy.stop()

    def withheld_tip(self):
        """Case 8: the replacing branch's last block was held as a header."""
        p, prod, v, q = self.nodes[0], self.nodes[1], self.nodes[2], self.nodes[3]
        name = "replacing branch held as a header before"
        # The producer anchors one block below the parent's tip, so that the
        # blocks of this case sit at a height a plain extension keeps.
        self.restart_node(1, extra_args=self.seq_args(1, 1) + ["-anchoravoidcontested=0", "-anchorminconf=2"])
        self.settle()
        f = p.getblockcount()
        self.sync_node3(f)
        a1, a2 = self.mine(2)
        x, x_hex = self.produce(a1)
        prod.invalidateblock(x)
        # On node3: A3, A4 on A2, and B1..B4 on the fork point.
        for h in (a1, a2):
            q.submitblock(p.getblock(h, 0))
        a3, a4 = self.mine(2, node=q)
        a_raw = [q.getblock(h, 0) for h in (a3, a4)]
        q.invalidateblock(a1)
        self.node3_to(p.getblockhash(f))
        # To a fresh address: B1 must not be the A1 just invalidated.
        bs = self.generatetoaddress(q, 4, p.getnewaddress(), sync_fun=self.no_op)
        b_raw = [q.getblock(h, 0) for h in bs]
        # The parent learns B's headers only, before the window.
        for h in bs:
            assert p.submitheader(q.getblockheader(h, False)) is None
        assert_equal(p.getbestblockhash(), a2)
        self.settle()

        def side_tips():
            return sorted((t["hash"], t["status"]) for t in p.getchaintips() if t["status"] != "active")
        before = side_tips()
        opened = self.open_window()
        for raw in b_raw[:3]:
            p.submitblock(raw)
        assert_equal(p.getbestblockhash(), bs[2])
        y, y_hex = self.produce(bs[1])
        during = (v.submitblock(x_hex), v.submitblock(y_hex))
        for raw in a_raw:
            p.submitblock(raw)
        assert_equal(p.getbestblockhash(), a4)
        self.close_window(opened)
        assert_equal(during, ("anchor-stale", None))
        assert_equal(side_tips(), before)          # the list reads the same
        assert (bs[3], "headers-only") in before

        self.after_next_poll()
        y_dropped = not self.on_active_chain(y)
        after_x = v.submitblock(x_hex)
        self.record(name + ": the block on the restored anchor is accepted", after_x is None, "submitblock %r" % after_x)
        self.record(name + ": the active block anchored on the replacing branch is dropped",
                    y_dropped, "dropped" if y_dropped else "still on the active chain")
        prod.reconsiderblock(x)
        self.wait_until(lambda: prod.getbestblockhash() == x, timeout=60)

    def log_from(self, pos):
        with open(self.nodes[2].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(pos)
            return f.read()

    def cached_tips(self):
        """Case 9."""
        p, prod, v, q = self.nodes[0], self.nodes[1], self.nodes[2], self.nodes[3]
        name = "tips cached by a proxy at the move"
        poll = self.options.poll
        proxy = TipsProxy(rpc_port(5), rpc_port(0), "cache", ttl=10.0, auth="%s:%s" % (self.rpc_u, self.rpc_p))
        direct = self.validator_args
        try:
            self.validator_args = self.seq_args(2, poll, parent_port=rpc_port(5))
            self.settle()
            f = p.getblockcount()
            self.sync_node3(f)
            a1, a2 = self.mine(2)
            x, x_hex = self.produce(a1)
            prod.invalidateblock(x)
            for h in (a1, a2):
                q.submitblock(p.getblock(h, 0))
            a3, a4 = self.mine(2, node=q)
            a_raw = [q.getblock(h, 0) for h in (a3, a4)]
            q.invalidateblock(a1)
            self.node3_to(p.getblockhash(f))
            bs = self.generatetoaddress(q, 3, p.getnewaddress(), sync_fun=self.no_op)
            b_raw = [q.getblock(h, 0) for h in bs]
            assert_equal(p.getbestblockhash(), a2)
            self.settle()

            opened = self.open_window()
            # Someone else's reading loads the cache 7 s before the next poll,
            # which it then answers: from before the branch.
            time.sleep(max(0, poll - 8))
            cached = proxy.prime()
            for raw in b_raw:
                p.submitblock(raw)
            assert_equal(p.getbestblockhash(), bs[2])
            y, y_hex = self.produce(bs[1])
            during = (v.submitblock(x_hex), v.submitblock(y_hex))
            for raw in a_raw:
                p.submitblock(raw)
            assert_equal(p.getbestblockhash(), a4)
            self.close_window(opened)
            assert_equal(during, ("anchor-stale", None))
            assert bs[2] not in {t["hash"] for t in cached}
            hits = proxy.hits
            self.tick_done()                       # the moving poll
            hit = proxy.hits - hits
            # The first poll that begins more than 12 s after the move.
            polls = 12 // poll + 1
            for _ in range(polls):
                self.tick_done()
            after_x = v.submitblock(x_hex)
            y_dropped = not self.on_active_chain(y)
            self.record(name + ": the moving poll read the cached list", hit >= 1, "%d cache hit(s)" % hit)
            self.record(name + ": %d poll(s) later, with no Bitcoin block, the block on the restored anchor is accepted" % polls,
                        after_x is None, "submitblock %r" % after_x)
            self.record(name + ": and the active block anchored on the replacing branch is dropped",
                        y_dropped, "dropped" if y_dropped else "still on the active chain")
            prod.reconsiderblock(x)
            self.wait_until(lambda: prod.getbestblockhash() == x, timeout=60)
        finally:
            self.validator_args = direct
            self.restart_node(2, extra_args=self.validator_args)
            proxy.stop()

    def bounds(self):
        """Case 10."""
        p, v = self.nodes[0], self.nodes[2]
        tips = len(p.getchaintips())
        warning = "branch tips exceed what this node reads"
        proxy = None
        try:
            for label, args, make in (
                ("more tips than the bound", self.validator_args + ["-anchortipsmax=%d" % (tips - 1)], None),
                ("a reply longer than the bound", self.seq_args(2, self.options.poll, parent_port=rpc_port(5))
                 + ["-anchortipsmax=%d" % (tips + 10)], (tips + 10) * 256 + 4096 + 10000)):
                if make:
                    proxy = TipsProxy(rpc_port(5), rpc_port(0), "pad", pad=make)
                pos = os.path.getsize(v.debug_log_path)
                self.restart_node(2, extra_args=args)
                self.wait_ticks(2)
                moves = []
                for _ in range(2):
                    n = self.anchors_above(-1)
                    moves.append((self.measure(2, between=self.mine), n))
                said = self.log_from(pos).count(warning)
                self.log.info("%s (%d tips): %r at two moves (distinct anchors); %d warning(s)" % (label, tips, moves, said))
                per_call = [(m.get("getblockheader", 0) - 2) / max(m.get("getchaintips", 0), 1) for m, _ in moves]
                self.record("bounds, %s: every verdict is asked again at each move" % label,
                            all(m.get("getchaintips", 0) >= 1 for m, _ in moves) and all(pc >= n for pc, (_, n) in zip(per_call, moves)),
                            "%s re-asked per getchaintips call, %s distinct anchors" % (per_call, [n for _, n in moves]))
                self.record("bounds, %s: said once in the log" % label, said == 1, "%d warning(s)" % said)
        finally:
            self.restart_node(2, extra_args=self.validator_args)
            if proxy:
                proxy.stop()

    def stood_on(self, text, height):
        """Whether the node's log says the daemon may have stood on a branch
        forking at `height`."""
        return bool(re.search(r"may have stood on a branch forking at height %d\b|forking at height %d that it may have stood on" % (height, height), text))

    def two_moves_header(self):
        """Case 11."""
        p, v, q = self.nodes[0], self.nodes[2], self.nodes[3]
        name = "two moves, then a header-only sibling of the middle tip"
        self.settle()
        w0 = p.getbestblockhash()
        h0 = p.getblockcount()
        self.sync_node3(h0)
        pos = os.path.getsize(v.debug_log_path)
        self.tick_done()
        w1 = self.mine()[0]
        self.tick_done()                       # the first move
        self.mine()
        self.tick_done()                       # the second, within the first's settle time at poll 8
        s = self.generatetoaddress(q, 1, p.getnewaddress(), sync_fun=self.no_op)[0]
        assert_equal(q.getblockheader(s)["previousblockhash"], w0)
        assert p.submitheader(q.getblockheader(s, False)) is None
        tips = {t["hash"]: t for t in p.getchaintips()}
        assert_equal((tips[s]["status"], tips[s]["branchlen"]), ("headers-only", 1))
        assert_equal(p.getblockheader(s)["height"], p.getblockheader(w1)["height"])
        for _ in range(12 // self.options.poll + 2):
            self.tick_done()                   # both moves' settle readings
        said = self.stood_on(self.log_from(pos), h0)
        self.record(name + ": no reading takes it for a branch the daemon stood on",
                    not said, "said" if said else "not said")

    def hidden_failed_branch(self):
        """Case 12."""
        p, v, q = self.nodes[0], self.nodes[2], self.nodes[3]
        name = "a cached flip onto a branch that failed, then a move before settlement"
        poll = self.options.poll
        proxy = TipsProxy(rpc_port(5), rpc_port(0), "cache", ttl=10.0, auth="%s:%s" % (self.rpc_u, self.rpc_p))
        direct = self.validator_args
        try:
            self.validator_args = self.seq_args(2, poll, parent_port=rpc_port(5))
            f = p.getblockcount()
            self.sync_node3(f)
            a = self.mine()[0]
            # On node3: B1, B2 on the fork point, and B3 on them with a coinbase
            # naming another height; then A1, A2 on A, which outwork B2.
            bs = self.generatetoaddress(q, 3, p.getnewaddress(), sync_fun=self.no_op)
            b_raw = [q.getblock(h, 0) for h in bs[:2]]
            b3_bad = bad_coinbase_height(q.getblock(bs[2], 0), len(q.getblockheader(bs[2], False)) // 2)
            q.submitblock(p.getblock(a, 0))
            q.invalidateblock(bs[0])
            self.node3_to(a)
            a_new = self.generatetoaddress(q, 2, p.getnewaddress(), sync_fun=self.no_op)
            a_raw = [q.getblock(h, 0) for h in a_new]
            self.settle()

            pos = os.path.getsize(v.debug_log_path)
            opened = self.open_window()
            time.sleep(max(0, poll - 8))
            proxy.prime()                      # the move's reading will come from this cache
            for raw in b_raw:
                p.submitblock(raw)
            assert_equal(p.getbestblockhash(), bs[1])
            assert_equal(p.submitblock(b3_bad), "bad-cb-height")
            for raw in a_raw:
                p.submitblock(raw)
            assert_equal(p.getbestblockhash(), a_new[1])
            self.close_window(opened)
            tips = {t["hash"]: t for t in p.getchaintips()}
            failed = [t for t in tips.values() if t["status"] == "invalid" and t["height"] - t["branchlen"] == f]
            assert_equal(len(failed), 1)
            assert_equal(failed[0]["branchlen"], 3)
            self.tick_done()                   # the move, answered from the cache
            self.mine()                        # one more move before its settle time
            for _ in range(12 // poll + 3):
                self.tick_done()
            said = self.stood_on(self.log_from(pos), f)
            self.record(name + ": the signing record is told, at the first move's comparison done again",
                        said, "said" if said else "not said")
        finally:
            self.validator_args = direct
            self.restart_node(2, extra_args=self.validator_args)
            proxy.stop()

    def run_test(self):
        p = self.nodes[0]
        p.createwallet(wallet_name="w", descriptors=True)
        self.parent_addr = p.getnewaddress()
        self.mine(101)
        self.build_anchors(3)
        self.settle()

        self.cost()
        self.header_fork()
        self.refused_chaintips()
        self.one_deep(extend=True)
        self.one_deep(extend=False)
        self.two_deep()
        self.grown_branch()
        self.withheld_tip()
        self.cached_tips()
        self.bounds()
        self.two_moves_header()
        self.hidden_failed_branch()

        wrong = [name for name, (ok, _) in self.results.items() if not ok]
        for name, (ok, detail) in self.results.items():
            self.log.info("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail))
        assert not wrong, "wrong: %s" % "; ".join(wrong)


if __name__ == '__main__':
    UnseenBranchTest().main()
