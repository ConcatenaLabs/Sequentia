#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A member that holds a full certificate for a block X at a height signs no
other block there while X's anchor stands, whatever reached its disk and
however the certificate arrived (doc/sequentia/04-proof-of-stake.md, "The
signing record").

The MuSig2 committee chain of feature_pos_signing_record_rpc.py: node0 is the
parent chain, node1 (M) holds every member's key and signs by RPC, node2 is a
peer. In each case M first holds signatures on rivals of X made before X was
certified (their own escape has run out), then X is certified, then M is asked,
several rounds later, to sign a rival at X's height by every path that can
reach that height: a partial signature with the block, a partial signature
over a bare hash, and the leader's signature through submitposblock. Each must
be refused, naming X.

flush: M certifies X itself (generateposblock); a block index write then
  drops X's entry from the record. X still binds, with no restart, because the
  check holds X as certified from the block index; and after M is killed and
  started again, the scan of the block index at the start holds X again.
submit: X is certified through submitposblock (the MuSig2 route); its
  certificate binds for good, not only for the one round of an uncertified
  signature.
relay: X is certified on the peer and reaches M as an ordinary block; M never
  signed X, and still signs no rival at X's height. Then M starts again
  without watching Bitcoin (-validateanchor=0): it cannot judge X's anchor,
  but X is on its active chain, so X still binds.
bare: M certifies X, the new tip, and is asked for a partial signature over the
  bare hash of a rival at X's height. A hash the node does not know cannot be
  told from the next block's, so it is refused while the certified tip binds
  its height: the block argument is what places a hash at a height.
"""

import time

from feature_pos_signing_record_paths import ROUND_S, block_hash
from feature_pos_signing_record_rpc import PosSigningRecordRpcTest
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, pos_sign_after_share_lock

WAIT_ROUNDS = 3


class PosSigningRecordCertifiedTest(PosSigningRecordRpcTest):
    def set_test_params(self):
        super().set_test_params()

    def add_options(self, parser):
        parser.add_argument("--only", choices=("flush", "submit", "relay", "bare"), help="run one case only")

    def args(self, i):
        # Keep the finalized point from moving the tip's height out of reach:
        # the case is the signing check, not finality.
        return super().args(i) + ["-posfinalitydelayms=60000"]

    def run_test(self):
        m = self.nodes[1]
        self.certified_block(m)
        self.sync_blocks(self.nodes[1:])
        only = self.options.only
        if only in (None, "flush"):
            self.case_flush()
        if only in (None, "submit"):
            self.case_submit()
        if only in (None, "relay"):
            self.case_relay()
        if only in (None, "bare"):
            self.case_bare()

    # --- helpers ---

    def templates(self, node, n):
        out = [self.template(node)]
        while len(out) < n:
            time.sleep(1.1)
            t = self.template(node)
            if t["signhash"] not in [o["signhash"] for o in out]:
                out.append(t)
        for t in out:
            assert_equal(t["height"], out[0]["height"])
        return out

    def refused(self, case, what, call, x):
        try:
            r = call()
        except JSONRPCException as e:
            msg = e.error["message"]
            assert x in msg, "%s: %s refused for another reason: %s" % (case, what, msg)
            self.log.info("%s: %s refused: %s" % (case, what, msg))
            return msg
        raise AssertionError("%s: %s signed (%s) while this node holds a full certificate for %s at that height" %
                             (case, what, str(r)[:40], x[:16]))

    def held(self, rec, x):
        """What the check holds of X (logged; the refusals are the proof)."""
        b = [b for b in rec["held"] if b["hash"] == x]
        if not b:
            return "nothing"
        return ", ".join(k for k in ("signed", "certified", "active") if b[0].get(k)) + " (%s)" % b[0]["state"]

    def bare_partial(self, m, tmpl, tag):
        wif_for = {pub: wif for wif, pub in self.stakers}
        pk = tmpl["members"][0]
        nonces = [m.musignonce("%s%d" % (tag, k), wif_for[p], tmpl["members"], tmpl["signhash"])["pubnonce"]
                  for k, p in enumerate(tmpl["members"])]
        return m.musigpartialsign("%s0" % tag, wif_for[pk], tmpl["members"], nonces, tmpl["signhash"])

    def every_path(self, case, m, x, signed_rival, signed_sig, rival):
        """Every RPC path that can sign at X's height, several rounds after X
        was certified, and again once the first refusal's grace round has run
        out: a certified block binds for good, not for one round."""
        time.sleep(WAIT_ROUNDS * ROUND_S)
        for attempt in (1, 2):
            what = "" if attempt == 1 else " (again, after the grace round)"
            self.refused(case, "the leader's signature on a rival (submitposblock)" + what,
                         lambda: m.submitposblock(signed_rival["block"], self.stakers[0][0], signed_sig), x)
            self.refused(case, "a partial signature on a rival, with the block" + what,
                         lambda: self.musig(m, rival, "%s%dr" % (case, attempt)), x)
            if attempt == 1:
                time.sleep(ROUND_S)
        msg = self.refused(case, "a partial signature over a bare hash",
                           lambda: self.bare_partial(m, rival, case + "b"), x)
        assert "give the block" in msg, msg
        assert_equal(m.getblockhash(rival["height"]), x)

    def prepare(self, case, m):
        """Two rivals at the next height: Y signed whole (its aggregate in hand),
        Z only templated. Y's partial signatures bind by the escape: X, signed
        next, waits one grace round for them (pos_sign_after_share_lock)."""
        y, z, x_tmpl = self.templates(m, 3)
        y_sig = self.musig(m, y, case + "y")
        return y, y_sig, z, x_tmpl

    def certify(self, node):
        r = pos_sign_after_share_lock(node.generateposblock, self.stakers[0][0], [w for w, _ in self.stakers[1:]])
        assert r["countersignatures"] >= 2
        return r["hash"]

    # --- cases ---

    def case_flush(self):
        m = self.nodes[1]
        y, y_sig, z, _ = self.prepare("flush", m)
        x = self.certify(m)
        h = y["height"]
        assert_equal(m.getblockheader(x)["height"], h)
        assert m.getblockheader(x)["poscertified"]
        m.gettxoutsetinfo()  # a block index write
        rec = m.getsigningrecord()
        assert not [e for e in rec["entries"] if e["hash"] == x], "flush: X's entry stays after the index write"
        self.log.info("flush: M certified X=%s at %d; after the index write the record has no entry for it; the check holds: %s" %
                      (x[:16], h, self.held(rec, x)))
        self.every_path("flush", m, x, y, y_sig, z)
        # Killed and started again: the record may still name X or not; the
        # block index does, and the scan at the start reads it.
        self.sigkill(1)
        self.start_node(1, extra_args=self.args(1))
        self.connect_nodes(1, 2)
        with open(m.debug_log_path, encoding="utf-8", errors="replace") as f:
            scan = [line for line in f if "with a verified full certificate in the window" in line]
        assert scan, "flush: no scan of the block index at the start"
        self.log.info("flush: restarted: %s" % scan[-1].split("Signing record: ")[-1].strip())
        self.every_path("flush (restarted)", m, x, y, y_sig, z)
        self.certified_block(m)  # move on past H
        self.sync_blocks(self.nodes[1:])

    def case_submit(self):
        m = self.nodes[1]
        y, y_sig, z, x_tmpl = self.prepare("submit", m)
        x = block_hash(x_tmpl)
        tries = iter(range(2))
        x_sig = pos_sign_after_share_lock(lambda: self.musig(m, x_tmpl, "submitx%d" % next(tries)))
        assert_equal(m.submitposblock(x_tmpl["block"], self.stakers[0][0], x_sig)["hash"], x)
        assert m.getblockheader(x)["poscertified"]
        self.log.info("submit: X=%s certified at %d through submitposblock; the check holds: %s" %
                      (x[:16], y["height"], self.held(m.getsigningrecord(), x)))
        self.every_path("submit", m, x, y, y_sig, z)
        self.certified_block(m)
        self.sync_blocks(self.nodes[1:])

    def case_relay(self):
        m, peer = self.nodes[1], self.nodes[2]
        y, y_sig, z, _ = self.prepare("relay", m)
        h = y["height"]
        # The peer certifies X with the members' keys; M receives it as a block.
        x = self.certify(peer)
        self.sync_blocks(self.nodes[1:])
        assert_equal(m.getblockhash(h), x)
        assert m.getblockheader(x)["poscertified"]
        rec = m.getsigningrecord()
        assert not [e for e in rec["entries"] if e["hash"] == x], "relay: M has an entry for a block it never signed"
        self.log.info("relay: X=%s certified on the peer at %d reached M as a block; M never signed it; the check holds: %s" %
                      (x[:16], h, self.held(rec, x)))
        self.every_path("relay", m, x, y, y_sig, z)
        self.stop_node(1)
        self.start_node(1, extra_args=[a for a in self.args(1) if a != "-validateanchor=1"] + ["-validateanchor=0"])
        self.connect_nodes(1, 2)
        self.log.info("relay: M restarted without the anchor watcher; the check holds: %s" % self.held(m.getsigningrecord(), x))
        self.every_path("relay (no watcher)", m, x, y, y_sig, z)

    def case_bare(self):
        m = self.nodes[1]
        (rival,) = self.templates(m, 1)
        time.sleep(1.1)
        x = self.certify(m)
        assert_equal(m.getblockheader(x)["height"], rival["height"])
        assert block_hash(rival) != x
        self.log.info("bare: M certified X=%s, the tip at %d" % (x[:16], rival["height"]))
        msg = self.refused("bare", "a partial signature over the bare hash of a rival at the tip's height",
                           lambda: self.bare_partial(m, rival, "bare"), x)
        assert "give the block" in msg, msg
        # The next block, given whole, is signed at once.
        (nxt,) = self.templates(m, 1)
        self.musig(m, nxt, "barenext")
        self.log.info("bare: the next block's partial signatures, with the block, were made at %d" % nxt["height"])


if __name__ == "__main__":
    PosSigningRecordCertifiedTest().main()
