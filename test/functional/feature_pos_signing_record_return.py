#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The share-lock when Bitcoin replaces a signed block's anchor and returns to it
(doc/sequentia/04-proof-of-stake.md, "The share-lock" and "The signing record").

The network of feature_pos_signing_record_anchor.py: node0 is the parent chain,
nodes 1-4 a gossip committee of four single seats, quorum 3; node1 is M. Nodes 3
and 4 stop, so no round at height H certifies, and M signs block after block at
H as the rounds pass. Bitcoin then moves under it:

  A    the parent's tip is A: M signs X, anchored on A's branch;
  B    Bitcoin replaces A's branch: X binds nothing, and M signs Y without
       asking about X (Bitcoin anchoring is supreme);
  A'   Bitcoin returns to A: X can certify again, so M asks its peers for X's
       certificate and waits one round before it signs any Z, while Y (whose
       anchor is now gone) binds nothing;
  B''  Bitcoin goes back to B's branch: the same for Y before any W.

M signing X and Y on two Bitcoin branches is following Bitcoin, never a fault.
The whole sequence runs twice: with M running throughout, and with M killed
(SIGKILL) before each move of the parent and started after it, so that what
binds comes from the signing record. Each time, nodes 3 and 4 then return and
height H certifies: a height whose first blocks missed their quorum still
certifies while their anchors stand.
"""

import json
import os
import re
import time

from feature_pos_signing_record_anchor import M, PosSigningRecordAnchorTest

SIGNED = re.compile(r"PoS gossip: signed ([0-9a-f]{64}) at height (\d+)")


class PosSigningRecordReturnTest(PosSigningRecordAnchorTest):
    def set_test_params(self):
        super().set_test_params()

    def add_options(self, parser):
        parser.add_argument("--only", choices=("running", "restart"), help="run one sequence only")

    def run_test(self):
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= 3, timeout=120)
        if self.options.only != "restart":
            self.variant(restart=False)
        if self.options.only != "running":
            self.variant(restart=True)
        out = os.environ.get("CR1_RESULTS", "")
        if out:
            with open(out, "w", encoding="utf8") as f:
                json.dump(self.results, f, indent=1)

    def shares_at(self, h):
        return {e["hash"]: e for e in self.nodes[M].getsigningrecord()["entries"] if e["kind"] == "share" and e["height"] == h}

    def on_best_chain(self, e):
        p = self.nodes[0]
        return e["anchorheight"] <= p.getblockcount() and p.getblockhash(e["anchorheight"]) == e["anchorhash"]

    def first_new(self, pos, h, seen):
        """M's first signature at h, after `pos` in its log, on a block not in `seen` and anchored on the
        parent's best chain (a block signed before M's watcher saw the parent move is the old branch's).
        Returns the block and M's log before that signature."""
        end = time.time() + 120
        while time.time() < end:
            text = self.log_since(M, pos)
            shares = self.shares_at(h)
            for mm in SIGNED.finditer(text):
                b = mm.group(1)
                if int(mm.group(2)) != h or b in seen or b not in shares:
                    continue
                if self.on_best_chain(shares[b]):
                    return b, text[:mm.start()]
            time.sleep(0.1)
        raise AssertionError("M signed no new block at %d on the parent's present branch; log:\n%s" % (h, self.log_since(M, pos)[-4000:]))

    def bounce(self, restart):
        """Before the parent moves: with restarts, M is killed (and started by resume())."""
        if restart:
            self.sigkill(M)

    def resume(self, restart):
        pos = self.log_size(M)
        if restart:
            self.start_node(M, extra_args=self.args_for(M))
            self.connect_nodes(M, 2)
        return pos

    def must_have_asked(self, phase, before, blocks):
        for b in blocks:
            assert "share-lock at %s" % b in before, "%s: M signed a rival before asking for the certificate of %s" % (phase, b[:16])
            assert "no certificate for %s" % b in before, "%s: M signed a rival before %s's grace round passed" % (phase, b[:16])

    def must_not_have_waited(self, phase, before, blocks):
        # Until M's anchor watcher polls the parent (once a second here), M still
        # judges by its last verdict and may end a grace it began: that is the
        # poll's lag, not a hold. Judge from the moment M saw the move.
        seen = before.find("Anchor: parent chain reorganized")
        if seen >= 0:
            before = before[seen:]
        for b in blocks:
            assert "no certificate for %s" % b not in before, "%s: %s held M though Bitcoin replaced its anchor" % (phase, b[:16])

    def variant(self, restart):
        tag = "restart" if restart else "no restart"
        m, p = self.nodes[M], self.nodes[0]
        for i in (3, 4):
            self.stop_node(i)
        if restart:
            # A clean stop first, so that M's index on disk carries H-1.
            self.stop_node(M)
            self.start_node(M, extra_args=self.args_for(M))
            self.connect_nodes(M, 2)
        h = self.nodes[2].getblockcount() + 1
        self.parent_mine(2)
        top = p.getblockcount()
        a_fork, a_tip = p.getblockhash(top - 1), p.getblockhash(top)
        self.log.info("%s: H=%d, parent tip A=%s at %d" % (tag, h, a_tip[:12], top))

        # A: X, anchored on A's branch.
        pos = self.log_size(M)
        x, _ = self.first_new(pos, h, set())
        time.sleep(2)
        sa = {b for b, e in self.shares_at(h).items() if e["anchorheight"] >= top - 1}
        assert x in sa
        self.record("%s A" % tag, "M signed %d block(s) at %d on A's branch, first %s" % (len(sa), h, x[:16]))

        # B: Bitcoin replaces A's branch.
        self.bounce(restart)
        p.invalidateblock(a_fork)
        self.parent_mine(4)
        b_fork = p.getblockhash(top - 1)
        pos = self.resume(restart)
        y, before = self.first_new(pos, h, set(self.shares_at(h)))
        self.must_not_have_waited("%s B" % tag, before, sa)
        assert m.getblockcount() + 1 == h
        time.sleep(2)
        sb = {b for b, e in self.shares_at(h).items() if b not in sa and self.on_best_chain(e)}
        self.record("%s B" % tag, "A's branch replaced: M signed %s at %d without asking about the %d block(s) of A" % (y[:16], h, len(sa)))

        # A': Bitcoin returns to A.
        self.bounce(restart)
        p.reconsiderblock(a_fork)
        p.invalidateblock(b_fork)
        self.parent_mine(2)
        assert p.getblockhash(top) == a_tip, "the parent did not return to A"
        t0 = time.time()
        pos = self.resume(restart)
        z, before = self.first_new(pos, h, set(self.shares_at(h)))
        self.must_have_asked("%s A'" % tag, before, sa)
        # Only blocks anchored above the fork changed side: one anchored below
        # it (the producer backs its anchor off a contested tip) stands on both.
        self.must_not_have_waited("%s A'" % tag, before, {b for b in sb if self.shares_at(h)[b]["anchorheight"] >= top - 1})
        assert m.getblockcount() + 1 == h
        self.record("%s A'" % tag, "Bitcoin back on A: M asked for the certificates of A's %d block(s), waited a round, then signed %s at %d, %.1f s after the move" %
                    (len(sa), z[:16], h, time.time() - t0))
        time.sleep(2)
        # A' blocks: anchored on A's branch above the fork. A block M signed on B's
        # branch before its watcher saw the return to A is B's, not A''s.
        sz = {b for b, e in self.shares_at(h).items()
              if b not in sa and b not in sb and e["anchorheight"] >= top - 1 and self.on_best_chain(e)}

        # B'': Bitcoin goes back to B's branch.
        self.bounce(restart)
        p.reconsiderblock(b_fork)
        p.invalidateblock(a_fork)
        self.parent_mine(2)
        assert p.getblockhash(top - 1) == b_fork, "the parent did not return to B's branch"
        t0 = time.time()
        pos = self.resume(restart)
        w, before = self.first_new(pos, h, set(self.shares_at(h)))
        self.must_have_asked("%s B''" % tag, before, sb)
        self.must_not_have_waited("%s B''" % tag, before, sa | sz)
        self.record("%s B''" % tag, "Bitcoin back on B's branch: M asked for the certificates of B's %d block(s), waited a round, then signed %s at %d, %.1f s after the move" %
                    (len(sb), w[:16], h, time.time() - t0))

        # Everyone back: H certifies, though M and node2 signed blocks there
        # whose anchors stand (the stricter reading would never certify it).
        for i in (3, 4):
            self.start_node(i, extra_args=self.args_for(i))
        self.mesh()
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= h + 1, timeout=180)
        self.sync_blocks(self.nodes[1:], timeout=60)
        at_h = m.getblockhash(h)
        assert m.getblockheader(at_h)["poscertified"]
        rivals = [t for n in self.nodes[1:] for t in n.getchaintips()
                  if t["status"] != "active" and t["height"] == h and n.getblockheader(t["hash"]).get("poscertified")]
        assert not rivals, "a certified rival at %d: %s" % (h, rivals)
        self.record("%s certified" % tag, "with all four back, %s certified at %d; no certified rival" % (at_h[:16], h))


if __name__ == '__main__':
    PosSigningRecordReturnTest().main()
