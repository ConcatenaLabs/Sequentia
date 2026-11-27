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

The watcher reads the daemon's chain tips (getchaintips) at every poll: a branch
the daemon stood on since the last poll is a tip that is new, or whose status
changed, and every verdict above its fork point is dropped. This test makes
the daemon move onto a branch and back between two polls of the node under
test, with checks during the replacement, and asserts what the node holds
after the next poll:

  1. one block replaced and restored, then extended (the case of #467), with
     the mirror: a block anchored on the replacing branch is accepted during
     the replacement and is dropped after the poll, and another is refused;
  2. the same, restored without an extension: the watcher's tip comparison sees
     no move at all;
  3. two blocks replaced and restored: no wrong verdict at either height;
  4. one block replaced, the daemon returns, and the replacing branch grows by a
     block the daemon does not connect before the poll: the branch's tip then
     reads "valid-headers", which hides that the branch was active;
  5. a header-only fork announced from below the tip: the verdicts above its
     fork are asked again once, and not at the polls after;
  6. a daemon that answers its best hash but refuses getchaintips: nothing is
     kept from one poll to the next, and the node says so once in its log;
  7. the cost: a poll where Bitcoin stood still costs one getbestblockhash and
     one getchaintips, a plain extension adds one getblockheader, and a
     question answered from the cache costs nothing.

Topology:
  node0: the parent chain (stands in for Bitcoin).
  node1: the producer: a single genesis-configured staker, so every
         generateposblock block is certified on its own; polls every second.
  node2: the node under test: no stake, polls every POLL seconds, and gets
         blocks only by submitblock, so that each case decides what it sees.
  node3: a second parent-chain node, never connected, that mines the blocks the
         parent must learn as headers or as blocks it does not connect.
"""

import http.server
import json
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


class NoChainTipsProxy:
    """Forwards JSON-RPC to the parent and refuses getchaintips: a Bitcoin
    endpoint that answers for its tip but not for its branches."""

    def __init__(self, port, target_port):
        target = "http://127.0.0.1:%d/" % target_port

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                req = json.loads(body)
                if isinstance(req, dict) and req.get("method") == "getchaintips":
                    out = json.dumps({"result": None, "id": req.get("id"),
                                      "error": {"code": -32601, "message": "Method not found"}}).encode()
                else:
                    fwd = urllib.request.Request(target, data=body, headers={
                        "Authorization": self.headers.get("Authorization", ""),
                        "Content-Type": "application/json"})
                    try:
                        with urllib.request.urlopen(fwd, timeout=30) as r:
                            out = r.read()
                    except urllib.error.HTTPError as e:
                        out = e.read()
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
        return ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
                "-validatepegin=0", "-initialfreecoins=0", "-con_blocksubsidy=5000000000",
                "-anyonecanspendaremine=1", "-signblockscript=51", "-blindedaddresses=0"]

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

    def open_window(self):
        """Wait for a poll of the node under test to begin, give it a moment to
        finish, and return its tick count: what follows happens before the next."""
        start = self.ticks()
        self.wait_until(lambda: self.ticks() > start, timeout=self.options.poll * 3)
        time.sleep(1)
        return self.ticks()

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
            q.submitblock(p.getblock(p.getblockhash(h), 0))
        assert_equal(q.getbestblockhash(), p.getblockhash(height))

    def record(self, name, ok, detail):
        self.results[name] = (ok, detail)
        self.log.info("%s: %s (%s)" % (name, "as expected" if ok else "WRONG", detail))

    # --- the cases ----------------------------------------------------------

    def one_deep(self, extend):
        """Cases 1 and 2."""
        p, prod, v = self.nodes[0], self.nodes[1], self.nodes[2]
        name = "one deep, %s" % ("restored and extended" if extend else "restored, no extension")
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
        self.wait_ticks(1)
        s0 = self.stats()
        self.wait_ticks(ticks)
        s1 = self.stats()
        return {m: s1.get(m, 0) - s0.get(m, 0) for m in s1 if s1.get(m, 0) != s0.get(m, 0)}

    def cost(self):
        """Case 7."""
        self.build_anchors(12)
        self.settle()
        n = len(self.distinct_anchors())
        quiet = self.quiet_cost()
        ticks = quiet.get("getbestblockhash", 0)
        self.log.info("quiet polls, %d distinct anchors on the chain: %r" % (n, quiet))
        self.record("cost: a quiet poll costs getbestblockhash and getchaintips only",
                    set(quiet) == {"getbestblockhash", "getchaintips"} and quiet["getchaintips"] == ticks,
                    "%r over %d polls" % (quiet, ticks))
        s0 = self.stats()
        self.mine()
        self.wait_ticks(3)
        s1 = self.stats()
        ext = {m: s1.get(m, 0) - s0.get(m, 0) for m in s1 if s1.get(m, 0) != s0.get(m, 0)}
        self.log.info("one parent extension, 3 polls: %r" % ext)
        # Beyond the two calls of every poll: one header read that finds the
        # move was an extension, and the checkpoint scan's read of the new
        # block (a header and the block). Nothing scales with the chain.
        polls = ext.pop("getbestblockhash", 0)
        self.record("cost: a plain extension adds the extension check and the checkpoint scan only",
                    ext.pop("getchaintips", 0) == polls and ext == {"getblockheader": 2, "getblock": 1},
                    "%r beyond %d polls" % (ext, polls))
        # A question the cache answers: the tip's anchor, via getanchorstatus.
        s0 = self.stats()
        for _ in range(5):
            assert_equal(self.nodes[2].getanchorstatus()["anchorstatus"], "ok")
        s1 = self.stats()
        asked = {m: s1.get(m, 0) - s0.get(m, 0) for m in s1 if m != "getbestblockhash" and m != "getchaintips"
                 and s1.get(m, 0) != s0.get(m, 0)}
        self.record("cost: a question answered from the cache costs nothing", asked == {}, "%r" % asked)

    def header_fork(self):
        """Case 5."""
        p, q = self.nodes[0], self.nodes[3]
        self.build_anchors(4)
        self.settle()
        fork_height = p.getblockcount() - 3
        self.sync_node3(fork_height)
        h = self.mine(1, node=q)[0]
        above = len(self.distinct_anchors(fork_height))
        recovery = len(self.nodes[2].getanchorstatus()["recoveryset"])
        self.wait_ticks(1)
        s0 = self.stats()
        assert p.submitheader(q.getblockheader(h, False)) is None
        tips = {t["hash"]: t for t in p.getchaintips()}
        assert_equal((tips[h]["status"], tips[h]["height"] - tips[h]["branchlen"]), ("headers-only", fork_height))
        self.after_next_poll()
        s1 = self.stats()
        first = s1.get("getblockheader", 0) - s0.get("getblockheader", 0)
        later = self.quiet_cost(3).get("getblockheader", 0)
        self.log.info("header-only fork at %d: %d anchors above it (and %d in the recovery set); "
                      "re-asked %d at the poll after it, %d over the next 3" % (fork_height, above, recovery, first, later))
        self.record("header-only fork: the verdicts above its fork are asked again once",
                    1 <= first <= above + recovery, "%d re-asked, %d anchors above the fork" % (first, above))
        self.record("header-only fork: and nothing at the polls after", later == 0, "%d" % later)

    def refused_chaintips(self):
        """Case 6."""
        v = self.nodes[2]
        port = rpc_port(5)
        proxy = NoChainTipsProxy(port, rpc_port(0))
        try:
            args = self.seq_args(2, self.options.poll, parent_port=port)
            self.restart_node(2, extra_args=args)
            self.wait_ticks(2)
            n = len(self.distinct_anchors()) + len(v.getanchorstatus()["recoveryset"])
            cost = self.quiet_cost(3)
            ticks = cost.get("getbestblockhash", 0)
            per_poll = cost.get("getblockheader", 0) / max(ticks, 1)
            self.log.info("getchaintips refused: %r over %d polls, %d distinct anchors (with the recovery set)" % (cost, ticks, n))
            with open(v.debug_log_path, encoding="utf-8") as f:
                warnings = f.read().count("does not answer getchaintips")
            self.record("getchaintips refused: every verdict is asked again at every poll",
                        per_poll >= len(self.distinct_anchors()) - 1, "%.1f getblockheader per poll" % per_poll)
            self.record("getchaintips refused: said once in the log", warnings == 1, "%d warning(s)" % warnings)
        finally:
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

        wrong = [name for name, (ok, _) in self.results.items() if not ok]
        for name, (ok, detail) in self.results.items():
            self.log.info("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail))
        assert not wrong, "wrong: %s" % "; ".join(wrong)


if __name__ == '__main__':
    UnseenBranchTest().main()
