#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""What a certified block binds is read from the node's state when the check
runs, whatever order the certificate, the body, an invalidation and a Bitcoin
reorganization reach the node in (doc/sequentia/04-proof-of-stake.md, "The
signing record"). The cases that need a certificate message, which only a BLS
committee has, are in feature_pos_signing_record_state_bls.py.

The MuSig2 committee chain of feature_pos_signing_record_rpc.py: node0 is the
parent chain, node1 (M) holds every member's key and signs by RPC with no
producer thread, node2 is a peer.

bare: M certifies X at height 1 and the operator invalidates X. A partial
  signature over the bare hash of a rival is refused, naming X, and is made
  once one grace round has passed: invalidating a certified block releases the
  member after one round on every path, a bare hash included.
ancestor: M leads with the stale-parent check off, so it builds X over a
  parent P just after Bitcoin replaced P's anchor; X's own anchor is on
  Bitcoin's new branch, and X is certified. The anchor watcher then drops P and
  X with it. The block that replaces X at its height is made at once: a block
  the watcher dropped, through its own anchor or an ancestor's, binds nothing.
"""

import time

from feature_pos_signing_record_paths import ROUND_S, block_hash
from feature_pos_signing_record_rpc import PosSigningRecordRpcTest
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, pos_sign_after_share_lock

WATCHER_POLL_S = 20


def released_after_one_round(log, case, what, call, x):
    """Refused at first, naming X; made once one grace round has passed."""
    try:
        r = call(0)
        raise AssertionError("%s: %s made at once (%s); X binds for one grace round" % (case, what, str(r)[:40]))
    except JSONRPCException as e:
        msg = e.error["message"]
        assert x in msg, "%s: %s refused for another reason: %s" % (case, what, msg)
        log.info("%s: %s refused: %s" % (case, what, msg))
    time.sleep(ROUND_S + 0.3)
    try:
        r = call(1)
    except JSONRPCException as e:
        raise AssertionError("%s: %s still refused after one grace round: %s" % (case, what, e.error["message"]))
    log.info("%s: %s made after one grace round" % (case, what))
    return r


def made_at_once(log, case, what, call):
    try:
        r = call()
    except JSONRPCException as e:
        msg = e.error["message"]
        time.sleep(ROUND_S + 0.3)
        try:
            call()
            again = "made after one grace round"
        except JSONRPCException as e2:
            again = "refused again after one grace round: %s" % e2.error["message"]
        raise AssertionError("%s: %s refused: %s (%s)" % (case, what, msg, again))
    log.info("%s: %s made at once" % (case, what))
    return r


class PosSigningRecordStateTest(PosSigningRecordRpcTest):
    def set_test_params(self):
        super().set_test_params()

    def add_options(self, parser):
        parser.add_argument("--only", choices=("bare", "ancestor"), help="run one case only")

    def args(self, i):
        # Keep the finalized point from moving the tip's height out of reach:
        # the case is the signing check, not finality.
        return super().args(i) + ["-posfinalitydelayms=60000"]

    def run_test(self):
        only = self.options.only
        # The bare case needs X at height 1, so that no certified block below
        # it stands at or above the tip once X is invalidated.
        if only in (None, "bare"):
            self.case_bare()
        else:
            self.certified_block(self.nodes[1])
            self.sync_blocks(self.nodes[1:])
        if only in (None, "ancestor"):
            self.case_ancestor()

    # --- helpers ---

    def all_keys(self):
        return self.stakers[0][0], [w for w, _ in self.stakers[1:]]

    def generate(self, node):
        leader, others = self.all_keys()
        return node.generateposblock(leader, others)

    def held(self, node, x):
        b = [b for b in node.getsigningrecord()["held"] if b["hash"] == x]
        return b[0] if b else None

    def bare_partial(self, m, tmpl, tag):
        wif_for = {pub: wif for wif, pub in self.stakers}
        pk = tmpl["members"][0]
        nonces = [m.musignonce("%s%d" % (tag, k), wif_for[p], tmpl["members"], tmpl["signhash"])["pubnonce"]
                  for k, p in enumerate(tmpl["members"])]
        return m.musigpartialsign("%s0" % tag, wif_for[pk], tmpl["members"], nonces, tmpl["signhash"])

    # --- cases ---

    def case_bare(self):
        m = self.nodes[1]
        # Alone, so the peer does not ask for X while it is invalid here.
        self.disconnect_nodes(1, 2)
        (rival,) = [self.template(m)]
        assert_equal(rival["height"], 1)
        time.sleep(1.1)
        x = self.certified_block(m)
        assert_equal(m.getblockheader(x)["height"], 1)
        assert block_hash(rival) != x
        m.invalidateblock(x)
        assert_equal(m.getblockcount(), 0)
        h = self.held(m, x)
        self.log.info("bare: M certified X=%s at 1 and invalidated it; the check holds it: %s" % (x[:16], h))
        assert h and h["certified"] and h["invalidated"], h
        r = released_after_one_round(self.log, "bare", "a partial signature over the bare hash of a rival",
                                          lambda k: self.bare_partial(m, rival, "bare%d" % k), x)
        assert "partialsig" in r
        m.reconsiderblock(x)
        assert_equal(m.getbestblockhash(), x)
        self.connect_nodes(1, 2)
        self.sync_blocks(self.nodes[1:])

    def case_ancestor(self):
        m, parent = self.nodes[1], self.nodes[0]
        self.stop_node(1)
        args = [a for a in self.args(1) if not a.startswith("-anchorpollinterval")]
        self.start_node(1, extra_args=args + ["-anchorpollinterval=%d" % WATCHER_POLL_S, "-anchorstaleparentcheck=0"])
        gen = lambda: pos_sign_after_share_lock(m.generateposblock, *self.all_keys())
        for _ in range(5):
            # P, anchored on the newest Bitcoin block.
            self.parent_mine(1)
            for _ in range(8):
                ph = gen()["hash"]
                if m.getblockheader(ph)["anchorheight"] == parent.getblockcount():
                    break
            P = m.getblockheader(m.getbestblockhash())
            a = P["anchorheight"]
            assert_equal(a, parent.getblockcount())
            # Bitcoin replaces P's anchor and extends its new branch; M builds
            # X over P before its watcher (polling every 20 s) drops P.
            parent.invalidateblock(parent.getblockhash(a))
            self.parent_mine(2)
            X = m.getblockheader(gen()["hash"])
            if X["previousblockhash"] == P["hash"] and X["anchorhash"] == parent.getblockhash(X["anchorheight"]):
                break
            self.log.info("ancestor: the watcher was first; again")
        else:
            raise AssertionError("ancestor: could not build X over P")
        x = X["hash"]
        assert X["poscertified"]
        assert P["anchorhash"] != parent.getblockhash(a)
        self.log.info("ancestor: X=%s certified at %d over P=%s, whose anchor Bitcoin replaced; X's own anchor stands" %
                      (x[:16], X["height"], P["hash"][:16]))
        self.wait_until(lambda: m.getblockcount() < P["height"] or m.getblockhash(P["height"]) != P["hash"],
                        timeout=WATCHER_POLL_S * 4)
        status = [t["status"] for t in m.getchaintips() if t["hash"] == x]
        self.log.info("ancestor: the watcher dropped P and X (X: %s); the check holds X: %s" % (status, self.held(m, x)))
        assert_equal(status, ["invalid"])
        while m.getblockcount() < X["height"] - 1:
            gen()
        r = made_at_once(self.log, "ancestor", "the block replacing X at its height (generateposblock)", lambda: self.generate(m))
        assert_equal(m.getblockheader(r["hash"])["height"], X["height"])


if __name__ == "__main__":
    PosSigningRecordStateTest().main()
