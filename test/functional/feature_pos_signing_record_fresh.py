#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A block that binds a height is released at once only on what the parent
chain's daemon says in the check itself; a revival restarts the escape of the
block it revives and of no other; and one check waits for the daemon once,
for at most half a round (doc/sequentia/04-proof-of-stake.md, "The signing
record").

The MuSig2 committee chain of feature_pos_signing_record_rpc.py: node0 is the
parent chain, node1 (M) holds every member's key and signs by RPC with no
producer thread, node2 is a peer, kept apart.

return-partial, return-generate: M certifies X, anchored on the newest Bitcoin
  block, and its anchor watcher (polling every 20 s) drops X once Bitcoin
  replaces that block. Bitcoin then returns to X's anchor. Before the
  watcher's next tick, while it still remembers that anchor as replaced and X
  is still marked invalid, M is asked for a partial signature of a rival
  (musigpartialsign, with the block) / to make and certify a rival
  (generateposblock). Refused: X's anchor stands, as the daemon says now.
  The watcher then revives X.
unrelated: M certifies W and X above it; the operator invalidates W. M makes Z
  in W's place after one round, and signs Y, a rival of X, after one more.
  Reconsidering Z, a valid block on another branch, clears nothing: Y is
  signed again at once, three times. Reconsidering X (W and X come back) and
  invalidating X again costs Y a fresh round.
once: X dropped as above; M restarts with the daemon refusing header queries,
  then holding them with no reply. One check asks about X's anchor once
  (it is also the anchor the watcher's mark names) and gives up within half a
  round (at least 500 ms): undecided, refused. A second check at once does not
  wait again. Once the daemon answers that the anchor is replaced, the rival
  is signed at once.
--testnet-settings runs at the testnet's round length and window and its
anchor watcher's poll (-posroundms=9450 -poswindowms=6750
-anchorpollinterval=15), and once at the default -mainchainrpctimeout (10 s).
"""

import time

from feature_pos_signing_record_paths import ROUND_S
from feature_pos_signing_record_rpc import PosSigningRecordRpcTest
from feature_pos_signing_record_state import (
    TESTNET_SETTINGS,
    WATCHER_POLL_S,
    ParentGate,
    made_at_once,
    released_after_one_round,
)
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, pos_sign_after_share_lock, rpc_port

CASES = ("return-partial", "return-generate", "once", "unrelated")


class PosSigningRecordFreshTest(PosSigningRecordRpcTest):
    def set_test_params(self):
        super().set_test_params()
        self.rpc_timeout = 120

    def add_options(self, parser):
        parser.add_argument("--only", choices=CASES, help="run one case only")
        parser.add_argument("--testnet-settings", dest="testnet_settings", default=False, action="store_true",
                            help="the testnet's round length, window and anchor watcher poll")

    def args(self, i):
        a = [x for x in super().args(i) if not x.startswith("-anchorpollinterval")] + ["-posfinalitydelayms=60000"]
        return a + (TESTNET_SETTINGS if self.options.testnet_settings else ["-anchorpollinterval=%d" % WATCHER_POLL_S])

    @property
    def round_ms(self):
        return 9450 if self.options.testnet_settings else int(round((ROUND_S - 0.3) * 1000))

    @property
    def round_s(self):
        return self.round_ms / 1000.0 + 0.3

    @property
    def budget_s(self):
        # PosSigningDaemonBudgetMs: half a round, at least 500 ms.
        return max(0.5, self.round_ms / 2000.0)

    @property
    def poll_s(self):
        return 15 if self.options.testnet_settings else WATCHER_POLL_S

    def run_test(self):
        self.disconnect_nodes(1, 2)
        self.gen()
        only = self.options.only
        for case in CASES:
            if only in (None, case):
                getattr(self, "case_" + case.replace("-", "_"))()

    # --- helpers ---

    def keys(self):
        return self.stakers[0][0], [w for w, _ in self.stakers[1:]]

    def gen(self):
        return pos_sign_after_share_lock(self.nodes[1].generateposblock, *self.keys())["hash"]

    def status(self, x):
        return [t["status"] for t in self.nodes[1].getchaintips() if t["hash"] == x]

    def partial(self, tmpl, tag):
        m = self.nodes[1]
        wif_for = {pub: wif for wif, pub in self.stakers}
        pk = tmpl["members"][0]
        nonces = [m.musignonce("%s%d" % (tag, k), wif_for[p], tmpl["members"], tmpl["signhash"])["pubnonce"]
                  for k, p in enumerate(tmpl["members"])]
        return m.musigpartialsign("%s0" % tag, wif_for[pk], tmpl["members"], nonces, tmpl["signhash"], tmpl["block"])

    def x_on_newest(self):
        """X, certified by M, anchored on the newest Bitcoin block."""
        m, parent = self.nodes[1], self.nodes[0]
        self.parent_mine(1)
        for _ in range(8):
            x = self.gen()
            if m.getblockheader(x)["anchorheight"] == parent.getblockcount():
                X = m.getblockheader(x)
                assert X["poscertified"]
                assert_equal(X["anchorhash"], parent.getblockhash(X["anchorheight"]))
                return X
        raise AssertionError("no block anchored on the newest Bitcoin block")

    def drop(self, X):
        """Bitcoin replaces X's anchor; M's anchor watcher drops X."""
        m, parent = self.nodes[1], self.nodes[0]
        parent.invalidateblock(X["anchorhash"])
        self.parent_mine(2)
        self.wait_until(lambda: m.getblockcount() < X["height"], timeout=self.poll_s * 4)
        assert_equal(self.status(X["hash"]), ["invalid"])

    def bitcoin_returns(self, X):
        parent = self.nodes[0]
        a = X["anchorheight"]
        replaced = parent.getblockhash(a)
        parent.reconsiderblock(X["anchorhash"])
        parent.invalidateblock(replaced)
        self.parent_mine(3)
        assert_equal(parent.getblockhash(a), X["anchorhash"])

    # --- cases ---

    def case_return(self, kind):
        m = self.nodes[1]
        case = "return-" + kind
        for attempt in range(4):
            X = self.x_on_newest()
            x, h = X["hash"], X["height"]
            self.drop(X)
            tmpl = self.template(m)
            assert_equal(tmpl["height"], h)
            self.bitcoin_returns(X)
            assert_equal(self.status(x), ["invalid"])
            self.log.info("%s: X=%s at %d dropped by the watcher; Bitcoin is back on its anchor %s at %d, and X is "
                          "still marked invalid here" % (case, x[:16], h, X["anchorhash"][:16], X["anchorheight"]))
            t0 = time.time()
            try:
                if kind == "partial":
                    r = self.partial(tmpl, "ret%d" % attempt)
                    what = "a partial signature of the rival %s" % tmpl["signhash"][:16]
                else:
                    r = m.generateposblock(*self.keys())
                    what = "the certified rival %s at %d" % (r["hash"][:16], m.getblockheader(r["hash"])["height"])
                made, msg = True, what
            except JSONRPCException as e:
                made, msg = False, e.error["message"]
            took = time.time() - t0
            before_tick = self.status(x) == ["invalid"]
            if not before_tick:
                self.log.info("%s: the watcher ticked during the call; again" % case)
                self.wait_until(lambda: m.getblockcount() >= h and m.getblockhash(h) == x, timeout=self.poll_s * 4)
                continue
            if made:
                raise AssertionError("%s: made %s while X's anchor stands, before the anchor watcher's tick" % (case, msg))
            assert x in msg and "anchor stands" in msg, "%s: refused for another reason: %s" % (case, msg)
            self.log.info("%s: refused before the watcher's tick, in %.3f s: %s" % (case, took, msg))
            if kind == "partial":
                # The daemon answered: the check did not wait out its bound.
                assert took < self.budget_s, "%s: the check took %.3f s with the daemon answering" % (case, took)
            self.wait_until(lambda: m.getblockcount() >= h and m.getblockhash(h) == x, timeout=self.poll_s * 4)
            self.log.info("%s: the watcher's tick revived X at %d" % (case, h))
            return
        raise AssertionError("%s: the watcher ticked during every attempt" % case)

    def case_return_partial(self):
        self.case_return("partial")

    def case_return_generate(self):
        self.case_return("generate")

    def case_unrelated(self):
        m = self.nodes[1]
        w = self.gen()
        x = self.gen()
        assert_equal(m.getblockheader(x)["previousblockhash"], w)
        hx = m.getblockheader(x)["height"]
        m.invalidateblock(w)
        assert_equal(m.getblockcount(), hx - 2)
        # A new Bitcoin block to anchor on, so that the block made in W's
        # place is not W again (the same block is always free to sign, and W
        # is invalid here).
        self.parent_mine(1)
        z = released_after_one_round(self.log, "unrelated", "Z in W's place (generateposblock)",
                                     lambda k: m.generateposblock(*self.keys()), w, self.round_s)["hash"]
        tmpl = self.template(m)
        assert_equal(tmpl["height"], hx)
        released_after_one_round(self.log, "unrelated", "Y, a rival of X (musigpartialsign)",
                                 lambda k: self.partial(tmpl, "uy%d" % k), x, self.round_s)
        for k in range(3):
            m.reconsiderblock(z)
            assert_equal(m.getbestblockhash(), z)
            assert_equal(self.status(x), ["invalid"])
            made_at_once(self.log, "unrelated", "Y again after reconsidering Z, a valid block on another branch (%d)" % k,
                         lambda: self.partial(tmpl, "uz%d" % k), self.round_s)
        # The original case: X revived and invalidated again, with no check
        # between, owes a fresh round.
        m.reconsiderblock(x)
        self.wait_until(lambda: m.getbestblockhash() == x)
        m.invalidateblock(x)
        assert_equal(m.getblockcount(), hx - 1)
        released_after_one_round(self.log, "unrelated", "Y after X was revived and invalidated again",
                                 lambda k: self.partial(tmpl, "ur%d" % k), x, self.round_s)
        m.reconsiderblock(x)
        self.wait_until(lambda: m.getbestblockhash() == x)

    def case_once(self):
        m, parent = self.nodes[1], self.nodes[0]
        X = self.x_on_newest()
        x, h, a_hash = X["hash"], X["height"], X["anchorhash"]
        self.drop(X)
        tmpl = self.template(m)
        assert_equal(tmpl["height"], h)
        self.stop_node(1)
        timeout_s = 10 if self.options.testnet_settings else 5
        budget_s = min(timeout_s, self.budget_s)
        gate = ParentGate(rpc_port(9), rpc_port(0))
        try:
            gate.blocked = {"getblockheader", "getblock"}
            args = [arg for arg in self.args(1) if not arg.startswith(("-mainchainrpcport=", "-anchorpollinterval"))]
            args += ["-mainchainrpcport=%d" % rpc_port(9), "-anchorpollinterval=600"]
            if not self.options.testnet_settings:
                args.append("-mainchainrpctimeout=%d" % timeout_s)
            self.start_node(1, extra_args=args)
            # The watcher's first tick is over once the daemon hears nothing.
            self.wait_until(lambda: time.time() - max([0] + [t for t, _ in gate.log]) > 3, timeout=120)
            assert_equal(m.getblockcount(), h - 1)
            gate.blocked = set()
            gate.hung = {"getblockheader"}
            t0 = time.time()
            try:
                r = self.partial(tmpl, "once0")
                raise AssertionError("once: made %s with the daemon silent" % r)
            except JSONRPCException as e:
                msg = e.error["message"]
            took = time.time() - t0
            asked = [p for mth, p in gate.calls_since(t0) if mth == "getblockheader" and p and p[0] == a_hash]
            self.log.info("once: one check took %.3f s and asked the daemon about X's anchor %d time(s): %s" %
                          (took, len(asked), msg))
            assert x in msg and "has not judged" in msg, msg
            assert_equal(len(asked), 1)
            assert took < budget_s + 1.0, "once: one check waited %.3f s for the daemon (bound %.3f s)" % (took, budget_s)
            assert took < self.round_ms / 1000.0, "once: one check waited %.3f s, a round or more" % took
            # A second check at once does not wait again.
            t1 = time.time()
            try:
                self.partial(tmpl, "once1")
                raise AssertionError("once: made a rival right after the daemon was silent")
            except JSONRPCException as e:
                assert x in e.error["message"], e.error["message"]
            took1 = time.time() - t1
            asked1 = [p for mth, p in gate.calls_since(t1) if mth == "getblockheader"]
            self.log.info("once: the next check at once took %.3f s, %d header question(s)" % (took1, len(asked1)))
            assert_equal(len(asked1), 0)
            assert took1 < 0.5 * budget_s + 0.2, took1
            # The daemon answers: X's anchor is replaced, so the rival is made at once.
            gate.hung = set()
            time.sleep(budget_s + 0.2)
            made_at_once(self.log, "once", "the rival once the daemon says X's anchor is replaced",
                         lambda: self.partial(tmpl, "once2"), self.round_s)
            assert parent.getblockheader(a_hash)["confirmations"] < 0
        finally:
            self.stop_node(1)
            gate.close()
        self.start_node(1, extra_args=self.args(1))


if __name__ == "__main__":
    PosSigningRecordFreshTest().main()
