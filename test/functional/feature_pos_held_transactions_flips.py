#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Held transactions while Bitcoin flips between two branches it has already shown (RV63, A1).

The chain of feature_pos_held_transactions_rpc_liveness.py: a MuSig2 committee
whose blocks are all made by RPC, node 0 with the rule and node 1 without it
(the attacker). Two Bitcoin branches, N1 and N1', each carry certified
Sequentia blocks of their own (C1 and C2), built while nothing was held. Then
a certified transaction T, anchored one Bitcoin block further, loses its
anchor: node 0 holds T and declines the attacker's respend R, so its hold
begins. From there Bitcoin flips between N1 and N1', and after each flip the
attacker offers R again. Every flip re-connects certified blocks, but none of
them was certified after the hold began, so none is progress: the backstop,
counted from the first decline, must release T whatever Bitcoin does.

In the second case node 0 is restarted in the middle of the flips. The record
is on disk and the clocks are not: T is still held, the hold begins again at
the first decline after the restart, and ends one backstop after it.

A real bitcoind is the parent chain.
"""

import os
import time
from decimal import Decimal

from feature_pos_held_transactions_rpc_liveness import HeldTransactionsRpcLivenessTest, ROUND_MS, WINDOW_MS
from test_framework.authproxy import JSONRPCException
from test_framework.messages import tx_from_hex
from test_framework.util import assert_equal

BACKSTOP_S = 2 * (12 * ROUND_MS + WINDOW_MS) / 1000.0
# One flip every quarter of a backstop, for three backstops.
EVERY_S = BACKSTOP_S / 4
# What a release may add to the backstop: the maintenance thread's period and this test's sampling.
SLACK_S = 1.5


class HeldTransactionsFlipsTest(HeldTransactionsRpcLivenessTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def node_args(self, i):
        return ["-posdebugheldfillms=0" if x.startswith("-posdebugheldfillms=") else x for x in super().node_args(i)]

    def anchored_blocks(self, node, n, above):
        """Certified blocks on `node` until `n` of them anchor above Bitcoin height `above`; their hashes."""
        out = []
        for _ in range(n + 8):
            h = self.certified_block(node)
            if node.getblockheader(h)["anchorheight"] > above:
                out.append(h)
            if len(out) == n:
                return out
        raise AssertionError("no block anchored above Bitcoin height %d" % above)

    def both_on(self, block_hash, timeout=60):
        self.wait_until(lambda: all(n.getbestblockhash() == block_hash for n in self.nodes), timeout=timeout)

    def offer_r(self, tag, t_id, r_id):
        """The attacker's block carrying R, offered to node 0 for its signature. True when node 0 declined."""
        m, att = self.nodes
        self.wait_until(lambda: m.getbestblockhash() == att.getbestblockhash(), timeout=60)
        assert r_id in att.getrawmempool(), "the attacker lost its respend"
        d = self.template(att)
        try:
            self.musig(m, d, tag, d["block"])
        except JSONRPCException as e:
            want = "held-conflict: transaction %s respends an input of held transaction %s" % (r_id, t_id)
            assert want in e.error["message"], e.error["message"]
            return True
        return False

    def backstop_lines(self, since):
        with open(self.nodes[0].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(since)
            return [line.strip().split(" ", 1)[1] for line in f if "(the backstop," in line]

    def flips(self, case, vout, restart):
        m, att = self.nodes
        b = self.btc()

        def spend(to, fee):
            r = att.createrawtransaction([{"txid": self.fund_txid, "vout": vout, "sequence": 0xfffffffd}],
                                         [{to: Decimal("100") - fee}, {"fee": fee}])
            return att.signrawtransactionwithwallet(r)["hex"]
        t_hex = spend(m.getnewaddress(), Decimal("0.0001"))
        r_hex = spend(att.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()

        # The base: a certified tip anchored at Bitcoin's tip.
        self.btc_mine(1)
        self.wait_until(lambda: self.certified_block(m) and self.anchored_at_tip(m), timeout=60)
        h0 = b.getblockcount()
        base = m.getbestblockhash()
        # Three Bitcoin branches from here, and a Sequentia branch anchored on each, all built on the base.
        # N1 and C1: two certified blocks. Nothing is held.
        n1 = self.btc_mine(1)[0]
        c1 = self.anchored_blocks(m, 2, h0)
        assert_equal(m.getblockheader(c1[0])["previousblockhash"], base)
        self.sync_blocks()
        # N1' replaces N1; C1 leaves the chain (it carried no transaction: nothing held).
        # C2: three certified blocks, so it tops C1.
        b.invalidateblock(n1)
        n1p = self.btc_mine(2)[0]
        self.both_on(base)
        assert_equal(m.getheldtxs()["txids"], [])
        c2 = self.anchored_blocks(m, 3, h0)
        assert_equal(m.getblockheader(c2[0])["previousblockhash"], base)
        self.sync_blocks()
        # A third branch replaces N1'; C2 leaves the chain like C1. T is certified on it, in a branch
        # that tops C2: when Bitcoin returns to N1' the node must still stand on T's branch, so that its
        # watcher disconnects it and T is held. (Had it left by fork choice for a returning branch as
        # high, it would hold nothing.)
        b.invalidateblock(n1p)
        p = self.btc_mine(3)[0]
        self.both_on(base)
        assert_equal(m.getheldtxs()["txids"], [])
        self.anchored_blocks(m, 2, h0)
        m.sendrawtransaction(t_hex)
        b_t = self.certified_block(m)
        self.certified_block(m)
        self.sync_blocks()
        assert t_id in m.getblock(b_t)["tx"]
        assert m.getblockcount() > m.getblockheader(c2[-1])["height"]
        # Bitcoin returns to N1': T's branch loses its anchor for good, T is held, and the chain is C2
        # again. C1 and C2 were certified before anything was held, and neither carries T.
        b.invalidateblock(p)
        b.reconsiderblock(n1p)
        self.wait_until(lambda: t_id in m.getheldtxs()["txids"], timeout=60)
        self.both_on(c2[-1])
        self.record(case + ": setup", "C1 %s.. (height %d) on N1, C2 %s.. (height %d) on N1', both certified with nothing held; T %s held" %
                    (c1[-1][:12], m.getblockheader(c1[-1])["height"], c2[-1][:12], m.getblockheader(c2[-1])["height"], t_id))
        att.prioritisetransaction(txid=t_id, fee_delta=-100000000)
        att.sendrawtransaction(r_hex)

        size0 = os.path.getsize(m.debug_log_path)
        assert self.offer_r(case + "d", t_id, r_id), "node 0 signed the respend of a held transaction"
        t0 = time.time()
        assert m.getheldtxs()["held_for_ms"] >= 0
        on_n1 = False
        flips = 0
        declines = 1
        released = None
        restarted_at = None
        samples = []
        watch = 3 * BACKSTOP_S
        k = 0
        while time.time() - t0 < watch and released is None:
            if restart and restarted_at is None and time.time() - t0 >= BACKSTOP_S / 2:
                # The record survives the restart; the clocks and the order of certification do not.
                self.restart_node(0, extra_args=self.node_args(0))
                m.loadwallet("w")
                self.connect_nodes(0, 1)
                inf = m.getheldtxs()
                assert t_id in inf["txids"], "the restart lost the held transaction"
                assert_equal(inf["held_for_ms"], -1)
                size0 = os.path.getsize(m.debug_log_path)
                assert self.offer_r(case + "r", t_id, r_id), "node 0 signed the respend after its restart"
                restarted_at = time.time() - t0
                t0 = time.time()
                declines = 1
                self.record(case + ": restart", "node 0 restarted %.1fs after its first decline: T still held, no clock; "
                            "it declines R again and the hold begins afresh" % restarted_at)
            if on_n1:
                b.reconsiderblock(n1p)
                b.invalidateblock(n1)
            else:
                b.reconsiderblock(n1)
                b.invalidateblock(n1p)
            on_n1 = not on_n1
            flips += 1
            k += 1
            self.wait_until(lambda: m.getbestblockhash() == (c1[-1] if on_n1 else c2[-1]), timeout=60)
            self.wait_until(lambda: att.getbestblockhash() == (c1[-1] if on_n1 else c2[-1]), timeout=60)
            if t_id in m.getheldtxs()["txids"] and self.offer_r("%sf%d" % (case, k), t_id, r_id):
                declines += 1
            end = time.time() + EVERY_S
            while time.time() < end and released is None:
                inf = m.getheldtxs()
                if t_id not in inf["txids"]:
                    released = time.time() - t0
                samples.append((round(time.time() - t0, 1), "N1" if on_n1 else "N1'", m.getblockcount(), inf["held_for_ms"]))
                time.sleep(0.2)
        for s in samples[::max(1, len(samples) // 12)] + samples[-1:]:
            self.log.info("    %5.1fs after the decline, Bitcoin on %s: node 0 height %d, backstop clock %d ms" % s)
        lines = self.backstop_lines(size0)
        self.record(case + ": flips", "%d flips between N1 and N1', one every %.1fs, %d declines of R; backstop %.2fs" %
                    (flips, EVERY_S, declines, BACKSTOP_S))
        self.record(case + ": T held for", ("%.2fs from the %s, then released" % (released, "decline after the restart" if restart else "first decline"))
                    if released is not None else "the whole %.0fs" % (time.time() - t0))
        self.record(case + ": release", lines[0] if lines else "none")
        # Bitcoin ends on N1', with no other live branch.
        b.reconsiderblock(n1p)
        b.invalidateblock(n1)
        assert released is not None and released <= BACKSTOP_S + SLACK_S, \
            "T stayed held past the backstop while Bitcoin flipped (%s)" % released
        assert released >= BACKSTOP_S - SLACK_S, "T was released before the backstop (%.2fs)" % released
        assert lines, "T was not released by the backstop"
        assert flips >= 3
        # The respend is certified now that nothing is held; nothing of this case is left.
        self.both_on(c2[-1])
        assert r_id in att.getrawmempool()
        b_r = self.certified_block(att)
        self.certified_block(att)
        self.sync_blocks()
        assert r_id in m.getblock(b_r)["tx"]
        assert_equal(m.getheldtxs()["txids"], [])

    def returning(self, case, vout, higher):
        """Bitcoin branch A carries the certified Sequentia branch CA (no transaction). Bitcoin replaces A by
        B; on B the committee certifies T in CB. Bitcoin returns to A, where CA is one block higher than CB
        (higher) or as high (fork choice's tie-break decides). Node 0 must hold T and decline the respend R,
        whichever way fork choice leaves CB."""
        m, att = self.nodes
        b = self.btc()

        def spend(to, fee):
            r = att.createrawtransaction([{"txid": self.fund_txid, "vout": vout, "sequence": 0xfffffffd}],
                                         [{to: Decimal("100") - fee}, {"fee": fee}])
            return att.signrawtransactionwithwallet(r)["hex"]
        t_hex, r_hex = spend(m.getnewaddress(), Decimal("0.0001")), spend(att.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()
        self.btc_mine(1)
        self.wait_until(lambda: self.certified_block(m) and self.anchored_at_tip(m), timeout=60)
        h0 = b.getblockcount()
        base = m.getbestblockhash()
        a_blk = self.btc_mine(1)[0]
        ca = self.anchored_blocks(m, 4 if higher else 3, h0)
        self.sync_blocks()
        b.invalidateblock(a_blk)
        b_blk = self.btc_mine(3)[0]
        self.both_on(base)
        assert_equal(m.getheldtxs()["txids"], [])
        self.anchored_blocks(m, 1, h0)
        m.sendrawtransaction(t_hex)
        b_t = self.certified_block(m)
        cb_top = self.certified_block(m)
        self.sync_blocks()
        assert t_id in m.getblock(b_t)["tx"]
        self.record(case + ": branches", "CA tops at height %d (on Bitcoin branch A), CB at %d (on B) with T certified at %d" %
                    (m.getblockheader(ca[-1])["height"], m.getblockheader(cb_top)["height"], m.getblockheader(b_t)["height"]))
        size0 = os.path.getsize(m.debug_log_path)
        b.invalidateblock(b_blk)
        b.reconsiderblock(a_blk)
        t_back = time.time()
        # Following time: from Bitcoin's return to node 0 on CA's top, read by polling, and inside the
        # watcher's tick from its log: its first line after the return to the UpdateTip that reaches CA's top.
        while m.getbestblockhash() != ca[-1]:
            assert time.time() - t_back < 30, "node 0 did not reach CA's top in 30s"
            time.sleep(0.02)
        follow_s = time.time() - t_back
        stamp = lambda x: float(x[17:26]) + 60 * int(x[14:16])
        with open(m.debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(size0)
            lines = [x.strip() for x in f if "is canonical again; reconsidering" in x or "reorganization detected" in x
                     or "Held transactions: holding" in x or "the reconsideration took" in x
                     or ("UpdateTip: new best=" + ca[-1]) in x]
        first = [x for x in lines if "[anchorwatch]" in x and "UpdateTip" not in x]
        top = [x for x in lines if ("UpdateTip: new best=" + ca[-1]) in x]
        in_tick = "%.1f ms (%s)" % ((stamp(top[0]) - stamp(first[0])) * 1000, top[0].split(" ")[1]) if first and top else "n/a"
        self.record(case + ": followed", "node 0 on CA's top %.2fs after Bitcoin's return; in the watcher's tick %s" % (follow_s, in_tick))
        self.wait_until(lambda: t_id in m.getheldtxs()["txids"], timeout=30)
        self.both_on(ca[-1])
        for x in lines:
            self.log.info("    node 0: %s", (x.split(" ", 1)[0][11:] + " " + x.split("] ", 3)[-1])[:150])
        assert any("Held transactions: holding" in x for x in lines)
        att.prioritisetransaction(txid=t_id, fee_delta=-100000000)
        att.sendrawtransaction(r_hex)
        assert self.offer_r(case, t_id, r_id), "node 0 signed the attacker's block carrying R"
        self.record(case + ": after Bitcoin's return to A", "on CA's top, T held, R declined")
        # The hold ends by the backstop at the latest; the next case starts with nothing held.
        self.wait_until(lambda: m.getheldtxs()["txids"] == [], timeout=BACKSTOP_S + 30)

    def body(self):
        m, att = self.nodes
        g = m.getblock(m.getblockhash(0), 2)
        coin = [(tx["txid"], vo["n"], Decimal(vo["value"])) for tx in g["tx"] for vo in tx["vout"]
                if vo["scriptPubKey"]["hex"] == "51" and vo.get("value", 0) > 0][0]
        raw = m.createrawtransaction([{"txid": coin[0], "vout": coin[1]}],
                                     [{att.getnewaddress(): Decimal("100")} for _ in range(4)] +
                                     [{att.getnewaddress(): coin[2] - Decimal("400.001")}, {"fee": Decimal("0.001")}])
        self.fund_txid = m.sendrawtransaction(raw)
        self.certified_block(m)
        self.sync_blocks()
        vouts = [o["n"] for o in m.getrawtransaction(self.fund_txid, 1)["vout"] if o.get("value") == Decimal("100")]
        cases = os.environ.get("CR4_FLIPS", "flips,restart,return,return_equal").split(",")
        if "flips" in cases:
            self.log.info("(flips) the backstop while Bitcoin flips between two branches already seen: %.2fs", BACKSTOP_S)
            self.flips("flips", vouts[0], restart=False)
        if "restart" in cases:
            self.log.info("(restart) the same, with node 0 restarted in the middle of the flips")
            self.flips("restart", vouts[1], restart=True)
        if "return" in cases:
            self.log.info("(return) Bitcoin returns to a branch it showed before, whose Sequentia branch is higher")
            self.returning("return", vouts[2], higher=True)
        if "return_equal" in cases:
            self.log.info("(return_equal) the same, with the returning Sequentia branch as high as the current one")
            self.returning("return_equal", vouts[3], higher=False)


if __name__ == '__main__':
    HeldTransactionsFlipsTest().main()
