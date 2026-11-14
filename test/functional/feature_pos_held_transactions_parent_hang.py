#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Held transactions on a member whose Bitcoin Core stops answering (KR11), and its maintenance thread.

The chain of feature_pos_held_transactions_rpc_liveness.py: node 0 signs only
through the RPCs, so nothing but the maintenance thread applies its clocks.
Node 0 reaches Bitcoin Core through a relay in this test, which can hold back
the replies to chosen calls. In every case node 0 holds a certified
transaction T that a Bitcoin reorganization disconnected and declines the
attacker's respend R; nothing is certified afterwards, so the backstop,
counted from that decline, must release T on time:

  hung     Bitcoin Core is suspended (SIGSTOP): it accepts connections and
           never replies, so every parent call waits out -mainchainrpctimeout.
  partial  the relay answers the first parent call of each watcher tick
           (getbestblockhash) and holds back every later one, on a tick that
           has later ones to make (Bitcoin has just moved).
  window   a second Bitcoin reorganization arrives just before the backstop
           is due, with the record's file written only later
           (-posdebugheldfillms). The record holds T2, what it disconnected,
           from the disconnect itself: T2's respend is refused before the
           backstop and before the file is written. The backstop's clock runs
           on through a disconnect, so it releases T and T2 together, on
           time.
  stops    node 0 is stopped and started again and again while it holds: just
           after a decline, around the moment the backstop releases, and
           before its start has finished. It must exit cleanly every time,
           and the record must survive.
  stopped  Bitcoin Core is stopped for good after the decline.

A real bitcoind is the parent chain.
"""

import http.client
import http.server
import json
import os
import signal
import threading
import time
from decimal import Decimal

from feature_pos_held_transactions_rpc_liveness import HeldTransactionsRpcLivenessTest, ROUND_MS, WINDOW_MS
from test_framework.messages import tx_from_hex
from test_framework.util import assert_equal, rpc_port

BACKSTOP_S = 2 * (12 * ROUND_MS + WINDOW_MS) / 1000.0
# What a release may add to the backstop: the maintenance thread's period and this test's sampling.
SLACK_S = 1.5
RPC_TIMEOUT_S = int(os.environ.get("CR4_RPC_TIMEOUT", "10"))
FILL_MS = 8000
# How long before the backstop the window case's second reorganization lands.
LEAD_S = 5.0
CASES = os.environ.get("CR4_HANG", "hung,partial,window,stops,stopped").split(",")


class ParentRelay:
    """Relays parent-chain RPC calls to bitcoind. While `hold` is set, a call it selects gets no reply."""

    def __init__(self, port, target_port):
        self.port = port
        self.hold = None            # None, or a function of the list of methods in a request
        self.gate = threading.Event()
        self.calls = []             # (time, methods, answered)
        relay = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                try:
                    req = json.loads(body)
                    methods = [r.get("method") for r in req] if isinstance(req, list) else [req.get("method")]
                except Exception:
                    methods = ["?"]
                hold = relay.hold
                if hold is not None and hold(methods):
                    relay.calls.append((time.time(), methods, False))
                    relay.gate.wait(600)
                    self.close_connection = True
                    return
                relay.calls.append((time.time(), methods, True))
                conn = http.client.HTTPConnection("127.0.0.1", target_port, timeout=120)
                headers = {"Content-Type": "application/json"}
                if self.headers.get("Authorization"):
                    headers["Authorization"] = self.headers["Authorization"]
                try:
                    conn.request("POST", self.path, body, headers)
                    reply = conn.getresponse()
                    data = reply.read()
                except OSError:
                    # bitcoind is gone: the caller sees its connection closed.
                    self.close_connection = True
                    return
                finally:
                    conn.close()
                self.send_response(reply.status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.hold = None
        self.gate.set()
        self.server.shutdown()
        self.server.server_close()


class HeldTransactionsParentHangTest(HeldTransactionsRpcLivenessTest):
    def set_test_params(self):
        super().set_test_params()
        self.relay = None

    def node_args(self, i):
        args = [x for x in super().node_args(i) if not x.startswith("-posdebugheldfillms=")]
        if i == 0:
            if self.relay is None:
                self.relay = ParentRelay(rpc_port(6), self.btc_port)
            args = ["-mainchainrpcport=%d" % self.relay.port if x.startswith("-mainchainrpcport=") else x for x in args]
            args += ["-mainchainrpctimeout=%d" % RPC_TIMEOUT_S, "-posdebugheldfillms=%d" % FILL_MS]
        return args

    def run_test(self):
        try:
            self.body()
        finally:
            if getattr(self, "btc_proc", None) and self.btc_proc.poll() is None:
                try:
                    os.kill(self.btc_proc.pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            if self.relay is not None:
                self.relay.stop()
            out = os.environ.get("CR1_RESULTS", "")
            if out:
                with open(out, "w") as f:
                    json.dump(self.results, f, indent=1)
            self.stop_bitcoind()

    # ------------------------------------------------------------ helpers
    def spend(self, vout, to, fee, locktime=0):
        att = self.nodes[1]
        r = att.createrawtransaction([{"txid": self.fund_txid, "vout": vout, "sequence": 0xfffffffd}],
                                     [{to: Decimal("100") - fee}, {"fee": fee}], locktime)
        return att.signrawtransactionwithwallet(r)["hex"]

    def pair(self, vout, locktime=0):
        """T (to node 0) and the attacker's respend R of the same coin: their hex and ids."""
        m, att = self.nodes
        t_hex = self.spend(vout, m.getnewaddress(), Decimal("0.0001"), locktime)
        r_hex = self.spend(vout, att.getnewaddress(), Decimal("0.001"))
        return t_hex, r_hex, tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()

    def bitcoin_reorg(self, depth):
        b = self.btc()
        b.invalidateblock(b.getblockhash(b.getblockcount() - depth + 1))
        self.btc_mine(depth + 1)

    def hold(self, vout):
        """T certified, then disconnected by a 2-deep Bitcoin reorganization and held by node 0; R with the attacker."""
        m, att = self.nodes
        t_hex, r_hex, t_id, r_id = self.pair(vout)
        self.btc_mine(2)
        self.wait_until(lambda: self.certified_block(m) and self.anchored_at_tip(m), timeout=60)
        m.sendrawtransaction(t_hex)
        b_t = self.certified_block(m)
        self.certified_block(m)
        self.sync_blocks()
        assert t_id in m.getblock(b_t)["tx"]
        self.bitcoin_reorg(2)
        self.wait_until(lambda: t_id in m.getheldtxs()["txids"], timeout=60)
        self.sync_blocks()
        att.prioritisetransaction(txid=t_id, fee_delta=-100000000)
        att.sendrawtransaction(r_hex)
        return t_id, r_id

    def offer_r(self, tag, t_id, r_id):
        """The attacker's block carrying R, offered to node 0 for its signature: node 0 must decline."""
        m, att = self.nodes
        self.wait_until(lambda: m.getbestblockhash() == att.getbestblockhash(), timeout=60)
        d = self.template(att)
        self.refused(tag + ": decline", "held-conflict: transaction %s respends an input of held transaction %s" % (r_id, t_id),
                     lambda: self.musig(m, d, tag, d["block"]))
        return time.time()

    def lines(self, since, text):
        with open(self.nodes[0].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(since)
            return [line.strip() for line in f if text in line]

    def wait_release(self, tag, t_id, t0, size0, timeout):
        """Wait until node 0 no longer holds T; assert it was the backstop, on time, from the maintenance thread."""
        m = self.nodes[0]
        deadline = time.time() + timeout * self.options.timeout_factor
        while t_id in m.getheldtxs()["txids"]:
            assert time.time() < deadline, "node 0 still holds T %.0fs after the decline" % (time.time() - t0)
            time.sleep(0.1)
        elapsed = time.time() - t0
        rel = self.lines(size0, "(the backstop,")
        self.record(tag + ": release", "%.2fs after the decline (backstop %.2fs): %s" %
                    (elapsed, BACKSTOP_S, rel[0].split(" ", 1)[1] if rel else "no backstop line"))
        return elapsed, rel

    def check_release(self, elapsed, rel):
        assert elapsed <= BACKSTOP_S + SLACK_S, "node 0 held past its backstop (%.2fs)" % elapsed
        assert elapsed >= BACKSTOP_S - SLACK_S, "node 0 released before its backstop (%.2fs)" % elapsed
        assert rel and "[heldtxs]" in rel[0], rel

    def settle(self):
        """Nothing is held any more: the attacker's respends are certified, so no case leaves anything behind."""
        m, att = self.nodes
        self.wait_until(lambda: m.getbestblockhash() == att.getbestblockhash(), timeout=120)
        self.wait_until(lambda: self.certified_block(att) and self.anchored_at_tip(att), timeout=120)
        self.certified_block(att)
        self.sync_blocks()
        self.wait_until(lambda: not m.getheldtxs()["txids"], timeout=60)

    # ------------------------------------------------------------ cases
    def case_hung(self, vout):
        m = self.nodes[0]
        t_id, r_id = self.hold(vout)
        size0 = os.path.getsize(m.debug_log_path)
        t0 = self.offer_r("hung", t_id, r_id)
        time.sleep(max(0, BACKSTOP_S - 2.5 - (time.time() - t0)))
        pid = self.btc_proc.pid
        os.kill(pid, signal.SIGSTOP)
        try:
            self.record("hung: parent", "bitcoind suspended %.2fs after the decline; -mainchainrpctimeout=%d" % (time.time() - t0, RPC_TIMEOUT_S))
            elapsed, rel = self.wait_release("hung", t_id, t0, size0, timeout=5 * RPC_TIMEOUT_S + 30)
        finally:
            os.kill(pid, signal.SIGCONT)
        self.check_release(elapsed, rel)
        self.settle()

    def case_partial(self, vout):
        m = self.nodes[0]
        t_id, r_id = self.hold(vout)
        size0 = os.path.getsize(m.debug_log_path)
        t0 = self.offer_r("partial", t_id, r_id)
        time.sleep(max(0, BACKSTOP_S - 2.5 - (time.time() - t0)))
        n0 = len(self.relay.calls)
        self.relay.gate.clear()
        self.relay.hold = lambda methods: methods != ["getbestblockhash"]
        # Bitcoin moves, so the tick that sees it has more to ask after its first call.
        self.btc_mine(1)
        try:
            elapsed, rel = self.wait_release("partial", t_id, t0, size0, timeout=10 * RPC_TIMEOUT_S + 30)
            calls = self.relay.calls[n0:]
            answered = [c[1][0] for c in calls if c[2]]
            held = [c[1][0] for c in calls if not c[2]]
            self.record("partial: parent calls until the release", "answered %s; no reply to %s" % (answered, held))
        finally:
            self.relay.hold = None
            self.relay.gate.set()
        self.check_release(elapsed, rel)
        assert "getbestblockhash" in answered, "no tick began while the relay held calls back"
        assert held, "no later call of a tick was held back"
        self.settle()

    def case_window(self, vout1, vout2):
        m, att = self.nodes
        b = self.btc()
        # T1, with a lock time that keeps it out of the next blocks once its own is gone.
        self.btc_mine(2)
        self.wait_until(lambda: self.certified_block(m) and self.anchored_at_tip(m), timeout=60)
        lock = m.getblockcount() + 6
        t1_hex, r1_hex, t1_id, r1_id = self.pair(vout1, locktime=lock)
        while m.getblockcount() < lock:
            self.certified_block(m)
        m.sendrawtransaction(t1_hex)
        b_t1 = self.certified_block(m)
        self.certified_block(m)
        self.sync_blocks()
        assert t1_id in m.getblock(b_t1)["tx"]
        self.bitcoin_reorg(2)
        self.wait_until(lambda: t1_id in m.getheldtxs()["txids"], timeout=60)
        self.sync_blocks()
        # T2, certified on the new Bitcoin branch while T1 is held and not minable yet.
        q = self.btc_mine(1)[0]
        self.wait_until(lambda: self.certified_block(m) and self.anchored_at_tip(m), timeout=60)
        t2_hex, r2_hex, t2_id, r2_id = self.pair(vout2)
        m.sendrawtransaction(t2_hex)
        b_t2 = self.certified_block(m)
        self.certified_block(m)
        self.sync_blocks()
        assert t2_id in m.getblock(b_t2)["tx"]
        assert m.getblockcount() < lock, "the chain reached T1's lock time"
        assert_equal(m.getheldtxs()["txids"], [t1_id])
        att.prioritisetransaction(txid=t1_id, fee_delta=-100000000)
        att.sendrawtransaction(r1_hex)
        size0 = os.path.getsize(m.debug_log_path)
        t0 = self.offer_r("window", t1_id, r1_id)
        # The second reorganization, LEAD_S before the backstop is due.
        time.sleep(max(0, BACKSTOP_S - LEAD_S - (time.time() - t0)))
        b.invalidateblock(q)
        self.btc_mine(2)
        while t2_id not in m.getheldtxs()["txids"]:
            assert time.time() - t0 < BACKSTOP_S, "T2 not held before the backstop"
            time.sleep(0.05)
        t_held2 = time.time() - t0
        # Never signs over the departure: R2 refused at once, before the backstop and the file's write.
        self.wait_until(lambda: m.getbestblockhash() == att.getbestblockhash(), timeout=10)
        att.prioritisetransaction(txid=t2_id, fee_delta=-100000000)
        att.sendrawtransaction(r2_hex)
        # A block carrying R2 and not R1, so the refusal can only be T2's.
        att.prioritisetransaction(txid=r1_id, fee_delta=-100000000)
        d = self.template(att)
        att.prioritisetransaction(txid=r1_id, fee_delta=100000000)
        assert t1_id in m.getheldtxs()["txids"] and t2_id in m.getheldtxs()["txids"], "released before the probe"
        self.refused("window: R2 before the backstop", "held-conflict: transaction %s respends an input of held transaction %s" % (r2_id, t2_id),
                     lambda: self.musig(m, d, "window2", d["block"]))
        t_refused2 = time.time() - t0
        self.record("window: T2", "held %.2fs and R2 refused %.2fs after the first decline (backstop %.2fs)" % (t_held2, t_refused2, BACKSTOP_S))
        assert t_refused2 < BACKSTOP_S, "R2 was refused only after the backstop"
        elapsed, rel = self.wait_release("window", t1_id, t0, size0, timeout=60)
        detect = self.lines(size0, "Parent chain reorganization detected")
        # The first line that holds something: InvalidateBlock records each block it takes off in its own
        # iteration, so the empty certified block above T2's comes first, holding nothing.
        fill = [x for x in self.lines(size0, "Held transactions: holding ") if "holding 0 " not in x]
        self.record("window: order in node 0's log", [x.split(" ", 1)[0][11:] + " " + x.split("] ", 3)[-1][:70] for x in (detect[:1] + fill[:1] + rel[:1])])
        self.check_release(elapsed, rel)
        assert detect and fill and detect[0] < fill[0] < rel[0], "T2 was not recorded before the backstop"
        inf = m.getheldtxs()
        assert_equal(inf["txids"], [])
        self.record("window: after the backstop", "node 0 holds nothing: the backstop's clock ran on through the disconnect")
        self.settle()

    def case_stops(self, vout):
        m = self.nodes[0]
        t_id, r_id = self.hold(vout)
        args = self.node_args(0)

        def again():
            self.start_node(0, extra_args=args)
            m.loadwallet("w")
            self.connect_nodes(0, 1)
            return m.getheldtxs()

        # Stopped while it holds, just after a decline: the record survives, the clocks do not.
        for k in range(3):
            self.offer_r("stops h%d" % k, t_id, r_id)
            time.sleep(0.3 * k)
            self.stop_node(0)
            inf = again()
            assert t_id in inf["txids"], "a stop during a hold lost the held transaction"
            assert_equal(inf["held_for_ms"], -1)
        self.record("stops: during a hold", "3 stops just after a decline: clean exits, T still held, no clock after each start")
        # Stopped before its start has finished (the thread starts before the node answers RPCs).
        # An interrupted start may exit with status 1 (the start gave up), or die of the SIGTERM itself
        # when it comes before the node has installed its handler; a crash dies of another signal.
        self.stop_node(0)
        early = []
        for delay in (0.05, 0.3, 0.8, 1.5, 2.5):
            m.start(extra_args=args)
            time.sleep(delay)
            m.process.terminate()
            status = m.process.wait(timeout=120)
            m.stderr.seek(0)
            err = m.stderr.read().decode("utf-8", "replace")
            m.stdout.close()
            m.stderr.close()
            m.running = False
            m.process = None
            assert status in (0, 1, -signal.SIGTERM), "node 0 died with status %d when stopped %.2fs after launch: %s" % (status, delay, err[:300])
            assert not any(w in err for w in ("Assertion", "terminate called", "runtime error", "Sanitizer")), err[:600]
            early.append("%.2fs: %d" % (delay, status))
        inf = again()
        assert t_id in inf["txids"], "a stop during the start lost the held transaction"
        self.record("stops: during the start", "terminated after launch (delay: exit status) %s; T still held" % early)
        # Stopped around the moment the backstop releases.
        outcomes = []
        for off in ((-0.4, 0.0, 0.4) if not os.environ.get("CR1_TESTNET") else (0.0,)):
            if t_id not in m.getheldtxs()["txids"]:
                break
            t0 = self.offer_r("stops r%+.1f" % off, t_id, r_id)
            time.sleep(max(0, BACKSTOP_S + off - (time.time() - t0)))
            self.stop_node(0)
            inf = again()
            outcomes.append("%+.1fs: %s" % (off, "released" if t_id not in inf["txids"] else "still held"))
        self.record("stops: around the release", "stopped at the backstop %s; clean exit each time" % outcomes)
        if t_id in m.getheldtxs()["txids"]:
            size0 = os.path.getsize(m.debug_log_path)
            t0 = self.offer_r("stops last", t_id, r_id)
            elapsed, rel = self.wait_release("stops", t_id, t0, size0, timeout=BACKSTOP_S + 30)
            self.check_release(elapsed, rel)
        self.settle()

    def case_stopped(self, vout):
        m = self.nodes[0]
        t_id, r_id = self.hold(vout)
        size0 = os.path.getsize(m.debug_log_path)
        t0 = self.offer_r("stopped", t_id, r_id)
        self.stop_bitcoind()
        self.record("stopped: parent", "bitcoind stopped %.2fs after the decline" % (time.time() - t0))
        elapsed, rel = self.wait_release("stopped", t_id, t0, size0, timeout=BACKSTOP_S + 60)
        self.check_release(elapsed, rel)

    def body(self):
        m, att = self.nodes
        g = m.getblock(m.getblockhash(0), 2)
        coin = [(tx["txid"], vo["n"], Decimal(vo["value"])) for tx in g["tx"] for vo in tx["vout"]
                if vo["scriptPubKey"]["hex"] == "51" and vo.get("value", 0) > 0][0]
        n = 6
        outs = [{att.getnewaddress(): Decimal("100")} for _ in range(n)]
        raw = m.createrawtransaction([{"txid": coin[0], "vout": coin[1]}],
                                     outs + [{att.getnewaddress(): coin[2] - 100 * n - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        self.fund_txid = m.sendrawtransaction(raw)
        self.certified_block(m)
        self.sync_blocks()
        v = [o["n"] for o in m.getrawtransaction(self.fund_txid, 1)["vout"] if o.get("value") == Decimal("100")]
        self.log.info("backstop %.2fs, -mainchainrpctimeout=%d, cases %s", BACKSTOP_S, RPC_TIMEOUT_S, CASES)
        if "hung" in CASES:
            self.log.info("(hung) Bitcoin Core suspended before the backstop is due")
            self.case_hung(v[0])
        if "partial" in CASES:
            self.log.info("(partial) Bitcoin Core answers the first call of a tick and no later one")
            self.case_partial(v[1])
        if "window" in CASES:
            self.log.info("(window) a departure just before the backstop is due")
            self.case_window(v[2], v[3])
        if "stops" in CASES:
            self.log.info("(stops) node 0 stopped and started while it holds")
            self.case_stops(v[4])
        if "stopped" in CASES:
            self.log.info("(stopped) Bitcoin Core stopped for good after the decline")
            self.case_stopped(v[5])


if __name__ == '__main__':
    HeldTransactionsParentHangTest().main()
