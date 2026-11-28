#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A Bitcoin fork nobody's best chain was on does not void a member's escape.

The anchor watcher tells the signing record when Bitcoin moved off a block, and
the record then voids the escapes, grace periods and certificate queries of the
blocks it holds above that point (doc/sequentia/04-proof-of-stake.md, "The
signing record"). A branch the watcher merely observes in the daemon's list of
tips, such as a header-only fork with less work than the tip, is a reason to
ask about anchors again and nothing more: if it voided escapes, anyone able to
announce one valid header per poll could keep a held member from ever signing
a rival, and stall the chain.

nodes 1-4 are a committee of four, quorum 3; two are stopped, so the member
under test (M) is held at a height that cannot certify and signs rivals there
only by escaping. Bitcoin produces a block at every poll of M's watcher, so its
tip moves and the branches are read each time. The test counts the rivals M
signs at the held height in five equal phases at the mainnet defaults (rounds of
9450 ms, a poll every 5 seconds):

  - Bitcoin blocks only;
  - Bitcoin blocks, and after each one a new header-only fork from below the
    tip;
  - Bitcoin blocks, and after each one, once the member's watcher has seen it,
    a sibling of it with as much work, valid proof of work and a coinbase
    naming another height, which the parent marks failed when it arrives
    without ever connecting it;
  - the same with siblings of the block the held block's anchor rests on;
  - two Bitcoin blocks at consecutive ticks, within the settle time, then a
    header-only sibling of the first: more work than the tip before the
    first move, so a comparison that paired the newest list with that older
    tip would take it for a branch the daemon stood on.

No escape may be voided in any phase, and M signs as many rivals with the
forks as without (one fewer at most, for the phase boundary).
"""

import re
import time

from feature_bitcoin_anchoring_unseen_branch import bad_coinbase_height
from feature_pos_signing_record_anchor import PosSigningRecordAnchorTest, M
from test_framework.util import assert_equal, p2p_port, rpc_port

ROUND_MS = 9450   # 700 + 35 x the mainnet committee cap of 250
POLL_S = 5        # the default -anchorpollinterval
PHASE_S = 60


class SigningRecordBranchForksTest(PosSigningRecordAnchorTest):
    def set_test_params(self):
        super().set_test_params()
        self.num_nodes = 6

    def add_nodes(self, num_nodes, extra_args=None, **kwargs):
        # The parent chain (the first node added) runs on proof of work, as
        # Bitcoin does: a signed-blocks parent reports no chainwork, and the
        # watcher then counts every unconnected tip as a branch it may have
        # stood on.
        if not self.nodes and extra_args:
            extra_args = [["-con_signed_blocks=0" if a == "-signblockscript=51" else a for a in args]
                          for args in extra_args]
        super().add_nodes(num_nodes, extra_args, **kwargs)

    def setup_network(self):
        super().setup_network()
        # node5: a parent-chain node, never connected to node0, that mines the
        # fork headers node0 learns by submitheader.
        self.add_nodes(1, [["-port=%d" % p2p_port(5), "-rpcport=%d" % rpc_port(5), "-validatepegin=0",
                            "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                            "-con_signed_blocks=0"]], chain=[self.chain_name])
        self.start_node(5)

    def args_for(self, i):
        return super().args_for(i) + ["-anchorpollinterval=%d" % POLL_S, "-posroundms=%d" % ROUND_MS]

    def ticks(self):
        return self.nodes[M].getmainchainrpcstats()["bymethod"].get("getbestblockhash", 0)

    def signed_at(self, text, h):
        out = []
        for ln in text.splitlines():
            mm = re.search(r"PoS gossip: signed ([0-9a-f]{64}) at height (\d+)", ln)
            if mm and int(mm.group(2)) == h and mm.group(1) not in out:
                out.append(mm.group(1))
        return out

    def bad_sibling(self, of, header_only=False):
        """A block of as much work as the parent's block `of`, on its parent,
        with valid proof of work and a coinbase that names another height: the
        parent marks it failed when it arrives (bad-cb-height) and never
        connects it. With `header_only`, a valid sibling the parent learns as
        a header only."""
        p, q = self.nodes[0], self.nodes[5]
        for hh in range(q.getblockcount() + 1, p.getblockcount() + 1):
            q.submitblock(p.getblock(p.getblockhash(hh), 0))
        assert q.getbestblockhash() == p.getbestblockhash()
        parent = p.getblockheader(of)["previousblockhash"]
        q.invalidateblock(of)
        assert q.getbestblockhash() == parent
        blk = self.generatetoaddress(q, 1, p.getnewaddress(), sync_fun=self.no_op)[0]
        raw = bytearray(bytes.fromhex(q.getblock(blk, 0)))
        hl = len(q.getblockheader(blk, False)) // 2
        header = q.getblockheader(blk, False)
        q.invalidateblock(blk)
        q.reconsiderblock(of)
        if header_only:
            assert p.submitheader(header) is None
            return

        assert_equal(p.submitblock(bad_coinbase_height(raw.hex(), hl)), "bad-cb-height")

    def phase(self, h, forks, deep=None):
        """Run PHASE_S seconds with one Bitcoin block after each tick of M,
        and after it, with `forks`: "header", a header-only fork from below;
        "invalid", once M's watcher has seen that block, a sibling of it the
        parent marks failed (as much work as the tip M's next tick moves
        from); "deep-invalid", the same of the parent's block `deep`, the one
        the held block's anchor rests on. Return (rivals M signed at h, escape
        voids in M's log, forks made)."""
        p, q = self.nodes[0], self.nodes[5]
        pos = self.log_size(M)
        end = time.time() + PHASE_S
        made = 0
        while time.time() < end:
            t0 = self.ticks()
            while self.ticks() == t0 and time.time() < end:
                time.sleep(0.02)
            if time.time() >= end:
                break
            tip = self.parent_mine(1)[0]
            if forks == "header":
                # A sibling of the block below the tip: less work than the tip.
                blk = self.generatetoaddress(q, 1, p.getnewaddress(), sync_fun=self.no_op)[0]
                assert p.submitheader(q.getblockheader(blk, False)) is None
                q.invalidateblock(blk)
                made += 1
            elif forks == "two-then-header":
                # A second Bitcoin block at the next tick, and after M's watcher
                # has seen it, a header-only sibling of the first: as much
                # work as the tip of the first move, more than the one before.
                for _ in range(2):
                    t1 = self.ticks()
                    while self.ticks() == t1 and time.time() < end:
                        time.sleep(0.02)
                    if time.time() >= end:
                        break
                    if _ == 0:
                        self.parent_mine(1)
                else:
                    time.sleep(0.5)
                    self.bad_sibling(tip, header_only=True)
                    made += 1
            elif forks in ("invalid", "deep-invalid"):
                t1 = self.ticks()
                while self.ticks() == t1 and time.time() < end:
                    time.sleep(0.02)
                time.sleep(0.5)
                self.bad_sibling(tip if forks == "invalid" else deep)
                made += 1
        text = self.log_since(M, pos)
        return self.signed_at(text, h), text.count("any escape it had is void"), made

    def run_test(self):
        p, q = self.nodes[0], self.nodes[5]
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= 3, timeout=120)
        for i in (3, 4):
            self.stop_node(i)
        h = self.nodes[2].getblockcount() + 1
        pos = self.log_size(M)
        self.wait_until(lambda: self.signed_at(self.log_since(M, pos), h), timeout=120)
        self.log.info("held at height %d; rounds of %d ms; a poll every %d s" % (h, ROUND_MS, POLL_S))

        plain, plain_voids, _ = self.phase(h, forks=None)
        # node5 holds the parent's chain up to the block below its tip, so each
        # fork it mines is a sibling of a block the parent already has.
        top = p.getblockcount()
        for hh in range(1, top):
            q.submitblock(p.getblock(p.getblockhash(hh), 0))
        assert q.getblockcount() == top - 1
        forked, forked_voids, made = self.phase(h, forks="header")
        bad, bad_voids, made_bad = self.phase(h, forks="invalid")
        # The parent's block under the anchor of the block below the held
        # height: every block held at h is anchored at or above it.
        anchor_height = self.nodes[2].getblockheader(self.nodes[2].getblockhash(h - 1))["anchorheight"]
        deep, deep_voids, made_deep = self.phase(h, forks="deep-invalid", deep=p.getblockhash(anchor_height))
        two, two_voids, made_two = self.phase(h, forks="two-then-header")
        invalid_tips = sum(1 for t in p.getchaintips() if t["status"] == "invalid")
        self.log.info("Bitcoin blocks only: %d rivals signed, %d escape voids; with %d header-only forks: "
                      "%d rivals signed, %d escape voids; with %d equal-work invalid siblings: %d rivals signed, "
                      "%d escape voids; with %d invalid siblings of the block at the anchor (height %d): %d rivals "
                      "signed, %d escape voids (%d invalid tips on the parent)"
                      % (len(plain), plain_voids, made, len(forked), forked_voids, made_bad, len(bad), bad_voids,
                         made_deep, anchor_height, len(deep), deep_voids, invalid_tips))
        assert made >= PHASE_S // POLL_S - 2, "too few forks were announced (%d)" % made
        assert made_bad >= PHASE_S // POLL_S // 2 - 2 and made_deep >= PHASE_S // POLL_S // 2 - 2, \
            "too few invalid siblings (%d, %d)" % (made_bad, made_deep)
        assert invalid_tips >= made_bad + made_deep, "the parent did not mark every sibling failed"
        self.log.info("two Bitcoin blocks, then a header-only sibling of the first, %d times: %d rivals signed, "
                      "%d escape voids" % (made_two, len(two), two_voids))
        assert made_two >= PHASE_S // POLL_S // 3 - 2, "too few header siblings (%d)" % made_two
        assert plain_voids == 0 and forked_voids == 0 and bad_voids == 0 and deep_voids == 0 and two_voids == 0, \
            "an escape was voided"
        assert len(forked) >= 1 and len(forked) >= len(plain) - 1, "M signed fewer rivals with the forks"
        assert len(bad) >= 1 and len(bad) >= len(plain) - 1, "M signed fewer rivals with the invalid siblings"
        assert len(deep) >= 1 and len(deep) >= len(plain) - 1, "M signed fewer rivals with the deep invalid siblings"
        for i in (3, 4):
            self.start_node(i, extra_args=self.args_for(i))


if __name__ == '__main__':
    SigningRecordBranchForksTest().main()
