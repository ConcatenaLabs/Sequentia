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

The return cases: Bitcoin returns to a branch it showed before, and the
Sequentia branch anchored on it outweighs the one node 0 stands on. In the
flood case the branch node 0 stands on is anchored at the Bitcoin fork point,
so the walk keeps it and it leaves by fork choice once the watcher clears the
returning branch's marks, while a peer sends node 0 getblocks without pause
(each one runs an activation on the network thread) and the recovery set holds
stale entries the watcher must ask Bitcoin about. Whichever thread moves node
0 first, T must be held and R declined, in every try (CR6_FLOOD_TRIES).

A real bitcoind is the parent chain.
"""

import os
import struct
import threading
import time
from decimal import Decimal
from io import BytesIO

from feature_pos_held_transactions_rpc_liveness import HeldTransactionsRpcLivenessTest, ROUND_MS, WINDOW_MS
from test_framework.authproxy import JSONRPCException
from test_framework.messages import msg_getblocks, tx_from_hex
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.util import assert_equal

BACKSTOP_S = 2 * (12 * ROUND_MS + WINDOW_MS) / 1000.0
# One flip every quarter of a backstop, for three backstops.
EVERY_S = BACKSTOP_S / 4
# What a release may add to the backstop: the maintenance thread's period and this test's sampling.
SLACK_S = 1.5
# The flood case: how many tries, and how many stale recovery-set entries each one adds.
FLOOD_TRIES = int(os.environ.get("CR6_FLOOD_TRIES", "3"))
FLOOD_DECOYS = int(os.environ.get("CR6_FLOOD_DECOYS", "2"))
# The flood case restarts node 0 with -posdebugreconsideractivatems at this value, so that the watcher waits
# between clearing the returning branch's marks and its own activation and the flood's activation moves node 0
# first; 0 leaves the race to chance (the watcher then mostly wins it).
FLOOD_DELAY_MS = int(os.environ.get("CR6_FLOOD_DELAY_MS", "300"))
# The peer-before-watcher case and the crash case: how many tries each.
PEER_TRIES = int(os.environ.get("CR6_PEER_TRIES", "3"))
CRASH_TRIES = int(os.environ.get("CR6_CRASH_TRIES", "3"))


class GetblocksFlooder(P2PInterface):
    """A peer that frames every message but decodes only the handshake's and pings: it never parses a
    Sequentia block or transaction, which the test framework cannot deserialize."""
    KEEP = {b"version", b"verack", b"ping", b"pong", b"sendheaders", b"wtxidrelay", b"sendaddrv2",
            b"feefilter", b"sendcmpct"}

    def _on_data(self):
        while len(self.recvbuf) >= 24:
            msgtype = self.recvbuf[4:16].split(b"\x00", 1)[0]
            msglen = struct.unpack("<i", self.recvbuf[16:20])[0]
            if len(self.recvbuf) < 24 + msglen:
                return
            msg = self.recvbuf[24:24 + msglen]
            self.recvbuf = self.recvbuf[24 + msglen:]
            if msgtype in self.KEEP and msgtype in MESSAGEMAP:
                t = MESSAGEMAP[msgtype]()
                t.deserialize(BytesIO(msg))
                self.on_message(t)


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
                     or "Held transactions: holding" in x or "left the active chain while the record was open" in x
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

    def returning_flood(self, case, vout):
        """CA, anchored on Bitcoin branch A, is certified; Bitcoin leaves A for its parent P, and the walk takes
        CA off. FLOOD_DECOYS stale entries join the recovery set (each a certified block anchored on a sibling of
        A that Bitcoin then drops). T is certified in CB, anchored at P, so the walk keeps it. Bitcoin returns
        to A, where CA outweighs CB, while a peer floods node 0 with getblocks. True when node 0 ended on CA
        holding T and declined R."""
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
        a_blk = self.btc_mine(1)[0]
        ca = self.anchored_blocks(m, 3, h0)
        fork = m.getblockheader(ca[0])["previousblockhash"]
        self.sync_blocks()
        b.invalidateblock(a_blk)
        self.both_on(fork)
        assert_equal(b.getblockcount(), h0)
        for _ in range(FLOOD_DECOYS):
            z = self.btc_mine(1)[0]
            time.sleep(1.5)  # the watcher must see z as Bitcoin's tip, or its leaving is no tip change
            d = self.certified_block(m)
            assert_equal(m.getblockheader(d)["anchorheight"], h0 + 1)
            self.sync_blocks()
            b.invalidateblock(z)
            self.both_on(fork)
        m.sendrawtransaction(t_hex)
        cb = [self.certified_block(m) for _ in range(2)]
        self.sync_blocks()
        assert t_id in m.getblock(cb[0])["tx"]
        assert all(m.getblockheader(x)["anchorheight"] == h0 for x in cb)
        stop = threading.Event()
        sent = [0]
        p2p = m.add_p2p_connection(GetblocksFlooder())

        def flood():
            while not stop.is_set():
                try:
                    p2p.send_message(msg_getblocks())
                except Exception:
                    return
                sent[0] += 1
                if sent[0] % 50 == 0:
                    time.sleep(0.001)
        th = threading.Thread(target=flood, daemon=True)
        th.start()
        time.sleep(0.5)
        size0 = os.path.getsize(m.debug_log_path)
        b.reconsiderblock(a_blk)
        t_back = time.time()
        self.wait_until(lambda: m.getbestblockhash() == ca[-1], timeout=60)
        follow_s = time.time() - t_back
        stop.set()
        th.join()
        m.disconnect_p2ps()
        time.sleep(1.5)
        with open(m.debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(size0)
            lines = [x.strip() for x in f if "is canonical again; reconsidering" in x or "Held transactions: holding" in x
                     or "left the active chain while the record was open" in x or "the reconsideration took" in x]
        for x in lines[-6:]:
            self.log.info("    node 0: %s", (x.split(" ", 1)[0][11:] + " " + x.split("] ", 3)[-1])[:150])
        # Which thread moved node 0 off CB first: the watcher's own activation, or another one (the flood's).
        with open(m.debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(size0)
            moves = [x for x in f if "UpdateTip: new best=" in x]
        mover = moves[0].split("[", 2)[1].split("]")[0] if moves and "[" in moves[0] else "?"
        held = t_id in m.getheldtxs()["txids"]
        self.both_on(ca[-1])
        att.prioritisetransaction(txid=t_id, fee_delta=-100000000)
        att.sendrawtransaction(r_hex)
        declined = self.offer_r(case, t_id, r_id)
        self.record(case + ": result", "getblocks flood %d msgs, %d stale entries: node 0 on CA's top %.2fs after Bitcoin's "
                    "return, first moved by [%s]; T %s; R %s" % (sent[0], FLOOD_DECOYS, follow_s, mover,
                                                                "held" if held else "NOT held",
                                                                "declined" if declined else "SIGNED"))
        return held and declined

    def spend_pair(self, vout):
        m, att = self.nodes

        def spend(to, fee):
            r = att.createrawtransaction([{"txid": self.fund_txid, "vout": vout, "sequence": 0xfffffffd}],
                                         [{to: Decimal("100") - fee}, {"fee": fee}])
            return att.signrawtransactionwithwallet(r)["hex"]
        t_hex, r_hex = spend(m.getnewaddress(), Decimal("0.0001")), spend(att.getnewaddress(), Decimal("0.001"))
        return t_hex, r_hex, tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()

    def restart_member(self, extra=(), drop=()):
        m = self.nodes[0]
        args = [a for a in self.node_args(0) if not any(a.startswith(d) for d in drop)] + list(extra)
        self.restart_node(0, extra_args=args)
        m.loadwallet("w")
        self.connect_nodes(0, 1)
        self.sync_blocks()

    def first_mover(self, since):
        with open(self.nodes[0].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(since)
            moves = [x for x in f if "UpdateTip: new best=" in x]
        return moves[0].split("[", 2)[1].split("]")[0] if moves and "[" in moves[0] else "?"

    def peer_first(self, case, vout):
        """KR17's departure: node 0 polls Bitcoin every 15 s; Bitcoin replaces B, on which CB carries the certified T;
        the attacker's watcher (1 s) leaves CB and it certifies a heavier CA' on the new Bitcoin branch, which reaches
        node 0 from the peer before node 0's watcher runs. True when node 0 held T and declined R."""
        m, att = self.nodes
        b = self.btc()
        t_hex, r_hex, t_id, r_id = self.spend_pair(vout)
        self.btc_mine(1)
        self.wait_until(lambda: self.certified_block(m) and self.anchored_at_tip(m), timeout=60)
        h0 = b.getblockcount()
        self.sync_blocks()
        fork = m.getbestblockhash()
        b_blk = self.btc_mine(1)[0]
        m.sendrawtransaction(t_hex)
        cb = self.anchored_blocks(m, 2, h0)
        self.sync_blocks()
        assert any(t_id in m.getblock(x)["tx"] for x in cb)
        size0 = os.path.getsize(m.debug_log_path)
        b.invalidateblock(b_blk)
        self.btc_mine(2)
        t_back = time.time()
        self.wait_until(lambda: att.getblockheader(att.getbestblockhash())["height"] <= m.getblockheader(fork)["height"], timeout=30)
        # The attacker's branch leaves T out (its disconnect put T back in its mempool).
        att.prioritisetransaction(txid=t_id, fee_delta=-100000000)
        ca = self.anchored_blocks(att, 3, h0)
        self.wait_until(lambda: m.getbestblockhash() == ca[-1], timeout=60)
        follow_s = time.time() - t_back
        mover = self.first_mover(size0)
        held = t_id in m.getheldtxs()["txids"]
        assert all(t_id not in att.getblock(x)["tx"] for x in ca)
        att.sendrawtransaction(r_hex)
        declined = self.offer_r(case, t_id, r_id)
        self.record(case + ": result", "node 0 (poll 15 s) on CA's top %.2fs after Bitcoin's move, first moved by [%s]; T %s; R %s" %
                    (follow_s, mover, "held" if held else "NOT held", "declined" if declined else "SIGNED"))
        return mover, held and declined

    def crash(self, case, vout):
        """Node 0 is killed after its walk disconnected T's certified block and a chainstate flush
        (gettxoutsetinfo) persisted the disconnect, with the record's own write held back
        (-posdebugheldfillms). After the restart either T's block is on the chain again, to leave
        again, or the record holds T; in the end T is held and R declined. True when both hold."""
        m, att = self.nodes
        b = self.btc()
        t_hex, r_hex, t_id, r_id = self.spend_pair(vout)
        self.btc_mine(1)
        self.wait_until(lambda: self.certified_block(m) and self.anchored_at_tip(m), timeout=60)
        h0 = b.getblockcount()
        self.sync_blocks()
        b_blk = self.btc_mine(1)[0]
        m.sendrawtransaction(t_hex)
        cb = self.anchored_blocks(m, 2, h0)
        b_t = [x for x in cb if t_id in m.getblock(x)["tx"]][0]
        self.sync_blocks()
        size0 = os.path.getsize(m.debug_log_path)
        b.invalidateblock(b_blk)
        self.btc_mine(2)

        def disconnected():
            with open(m.debug_log_path, encoding="utf-8", errors="replace") as f:
                f.seek(size0)
                return any("Parent chain reorganization detected" in x for x in f)
        self.wait_until(disconnected, timeout=30)
        self.wait_until(lambda: m.getblockheader(b_t)["confirmations"] == -1, timeout=30)
        m.gettxoutsetinfo()
        m.process.kill()
        m.process.wait(timeout=60)
        m.running = False
        m.process = None
        m.rpc_connected = False
        m.rpc = None
        self.start_node(0, extra_args=[a for a in self.node_args(0) if not a.startswith(("-posdebugheldfillms", "-anchorpollinterval"))] +
                        ["-posdebugheldfillms=600000", "-anchorpollinterval=600"])
        m.loadwallet("w")
        on_chain = m.getblockheader(b_t)["confirmations"] >= 0
        held_at_start = t_id in m.getheldtxs()["txids"]
        self.restart_member(extra=["-posdebugheldfillms=600000"], drop=["-posdebugheldfillms"])
        if not (on_chain or held_at_start):
            # The chainstate has the disconnect and the record lacks it: nothing will hold T again.
            self.both_on(m.getbestblockhash())
            att.prioritisetransaction(txid=t_id, fee_delta=-100000000)
            att.sendrawtransaction(r_hex)
            declined = self.offer_r(case, t_id, r_id)
            self.record(case + ": result", "at the restart, before the watcher: T's block off the chain and T NOT held; R %s" %
                        ("declined" if declined else "SIGNED"))
            return False
        self.wait_until(lambda: t_id in m.getheldtxs()["txids"] or m.getblockheader(b_t)["confirmations"] >= 0, timeout=60)
        self.wait_until(lambda: m.getblockheader(b_t)["confirmations"] == -1, timeout=60)
        held = t_id in m.getheldtxs()["txids"]
        self.both_on(m.getbestblockhash())
        att.prioritisetransaction(txid=t_id, fee_delta=-100000000)
        att.sendrawtransaction(r_hex)
        declined = self.offer_r(case, t_id, r_id)
        ok_start = on_chain or held_at_start
        self.record(case + ": result", "at the restart, before the watcher: T's block %s, T %s; then T %s, R %s" %
                    ("on the chain" if on_chain else "off the chain", "held" if held_at_start else "NOT held",
                     "held" if held else "NOT held", "declined" if declined else "SIGNED"))
        return ok_start and held and declined

    def body(self):
        m, att = self.nodes
        NVOUT = 4 + FLOOD_TRIES + PEER_TRIES + CRASH_TRIES
        g = m.getblock(m.getblockhash(0), 2)
        coin = [(tx["txid"], vo["n"], Decimal(vo["value"])) for tx in g["tx"] for vo in tx["vout"]
                if vo["scriptPubKey"]["hex"] == "51" and vo.get("value", 0) > 0][0]
        raw = m.createrawtransaction([{"txid": coin[0], "vout": coin[1]}],
                                     [{att.getnewaddress(): Decimal("100")} for _ in range(NVOUT)] +
                                     [{att.getnewaddress(): coin[2] - Decimal(100 * NVOUT) - Decimal("0.001")},
                                      {"fee": Decimal("0.001")}])
        self.fund_txid = m.sendrawtransaction(raw)
        self.certified_block(m)
        self.sync_blocks()
        vouts = [o["n"] for o in m.getrawtransaction(self.fund_txid, 1)["vout"] if o.get("value") == Decimal("100")]
        cases = os.environ.get("CR4_FLIPS", "flips,restart,return,return_equal,return_flood,peer_first,crash").split(",")
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
        if "return_flood" in cases:
            self.log.info("(return_flood) the return, the current branch anchored at the fork point, a peer flooding getblocks")
            if FLOOD_DELAY_MS > 0:
                self.restart_node(0, extra_args=self.node_args(0) + ["-posdebugreconsideractivatems=%d" % FLOOD_DELAY_MS])
                m.loadwallet("w")
                self.connect_nodes(0, 1)
                self.sync_blocks()
            bad = [k for k in range(FLOOD_TRIES) if not self.returning_flood("return_flood%d" % k, vouts[4 + k])]
            self.record("return_flood: summary", "%d of %d tries left CB without T held and R declined" % (len(bad), FLOOD_TRIES))
            assert not bad, "tries %s left CB without T held and R declined" % bad
        base = 4 + FLOOD_TRIES
        if "peer_first" in cases:
            self.log.info("(peer_first) a peer's heavier branch moves node 0 off the certified block before its watcher runs")
            self.restart_member(extra=["-anchorpollinterval=15"], drop=["-anchorpollinterval", "-posdebugreconsideractivatems"])
            runs = [self.peer_first("peer_first%d" % k, vouts[base + k]) for k in range(PEER_TRIES)]
            peer = [ok for mover, ok in runs if mover == "msghand"]
            self.record("peer_first: summary", "%d of %d tries moved by the peer first; %d of them left T unheld or R signed; %d tries failed in all" %
                        (len(peer), PEER_TRIES, peer.count(False), [ok for _, ok in runs].count(False)))
            assert all(ok for _, ok in runs), "a member moved by a peer did not hold T and decline R"
            self.restart_member()
        base += PEER_TRIES
        if "crash" in cases:
            self.log.info("(crash) node 0 killed after a flushed disconnect, before the record's own write")
            self.restart_member(extra=["-posdebugheldfillms=600000"], drop=["-posdebugheldfillms"])
            bad = [k for k in range(CRASH_TRIES) if not self.crash("crash%d" % k, vouts[base + k])]
            self.record("crash: summary", "%d of %d tries ended without T on the chain or in the record at the restart, or without T held and R declined" %
                        (len(bad), CRASH_TRIES))
            assert not bad, "crash tries %s" % bad
            self.restart_member()


if __name__ == '__main__':
    HeldTransactionsFlipsTest().main()
