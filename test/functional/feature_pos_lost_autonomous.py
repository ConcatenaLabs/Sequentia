#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Lost blocks under the autonomous committee (doc 03, "Transactions of a lost
block"), at the round lengths the testnet runs.

The network, the seller and the Bitcoin daemon are those of
feature_pos_lost_blocks.py, but blocks are made by every member's own producer
(-posproducer through startposproducer) and its gossip rounds, not by
generateposblock. Round and window lengths come from LOST_ROUND_MS and
LOST_WINDOW_MS (the testnet's 9450 and 6750 by default). Cases (LOST_CASES):
  aT     the gap, repeated LOST_TRIALS times: T certified in L, Bitcoin replaces
         L's anchor, the seller's own template leaves T out and carries the
         respend R. Each trial records whether the seller's proposal was the one
         the committee backed first at the fork point's child, in which round
         that height was certified, how long it took, and that T is in it and
         R nowhere.
  deepT  full lost blocks (LOST_DEPTH stale Bitcoin blocks, ten Sequentia
         blocks each, made by generateposblock), then the producers start and
         replay them: the round and the members of each replay height, and the
         holders' plan time.
  c2     -poslostbindsigned: LOST_C2_SIGNERS members sign a candidate L2 that
         never certifies; its anchor goes stale; a respend of its transaction is
         in every other mempool. The pause until a block certifies.
  mute   every member proposes at a height on a Bitcoin block that is then
         replaced (1 deep): whether the height certifies before Bitcoin's tip
         moves again (timing-dependent: retried until it lands).
  mixT   one member on master's binary (LOST_BASE_BIN, node LOST_BASE_NODES)
         in a committee on the rule: the gap's rounds, as in aT.
  split  a real split: three seats hold each of two certified siblings as their
         own. Bitcoin moves three blocks at a time (LOST_SPLIT_STEPS steps) until
         all six nodes are on one tip.
"""

import os
import time
from decimal import Decimal

from feature_pos_lost_blocks import ALL_KEYS, SELLER, HONEST, LATE, N
from feature_pos_lost_replay import LostReplayTest
from test_framework.authproxy import JSONRPCException
from test_framework.messages import tx_from_hex
from test_framework.util import assert_equal

CASES = os.environ.get("LOST_CASES", "aT").split(",")
ROUND_MS = int(os.environ.get("LOST_ROUND_MS", "9450"))
WINDOW_MS = int(os.environ.get("LOST_WINDOW_MS", "6750"))
TRIALS = int(os.environ.get("LOST_TRIALS", "6"))
C2_SIGNERS = int(os.environ.get("LOST_C2_SIGNERS", "3"))
C2_ON = os.environ.get("LOST_C2", "1") == "1"
SPLIT_STEPS = int(os.environ.get("LOST_SPLIT_STEPS", "7"))


class LostAutonomousTest(LostReplayTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra = {i: ["-posroundms=%d" % ROUND_MS, "-poswindowms=%d" % WINDOW_MS, "-debug=net"] for i in range(N)}
        if C2_ON and "c2" in CASES:
            for i in range(N):
                self.extra[i].append("-poslostbindsigned=1")
        for i, a in enumerate(os.environ.get("LOST_EXTRA_ARGS_NODE", "").split(";")):
            if a:
                self.extra[i] += a.split(",")

    # ------------------------------------------------------------- helpers
    def start_producers(self, idx):
        for i in idx:
            r = self.nodes[i].startposproducer([self.stakers[i][0]])
            assert r["producing"], r

    def cert_line(self, i, h, since):
        """The node's own log of how block h was certified or adopted."""
        for text in ("certified block %s" % h, "adopted certified block %s" % h):
            ls = self.log_has(i, text, since)
            if ls:
                return ls[0]
        return None

    def round_of(self, h, since):
        for i in range(N):
            line = self.cert_line(i, h, since[i])
            if line and "(round " in line:
                return int(line.split("(round ")[1].split(",")[0]), int(line.split(", ")[-1].split(" ")[0])
        return None, None

    def proposals_at(self, i, height, since):
        out = []
        for line in self.log_has(i, "PoS gossip: proposing block ", since):
            if line.endswith("at height %d" % height):
                out.append(line.split("proposing block ")[1].split(" ")[0])
        return out

    def wait_height(self, idx, height, timeout):
        self.wait_until(lambda: all(self.nodes[i].getblockcount() >= height for i in idx), timeout=timeout)

    def wait_tx_certified(self, node, txid, timeout=120):
        """The block holding txid, once it is certified with a certified child."""
        def ok():
            h = self.block_of(txid, node) if txid not in node.getrawmempool() else None
            if h is None or node.getblockcount() < h + 1:
                return False
            hdr = node.getblockheader(node.getblockhash(h))
            child = node.getblockheader(node.getblockhash(h + 1))
            return hdr["poscertified"] and child["poscertified"]
        self.wait_until(ok, timeout=timeout)
        return node.getblockheader(node.getblockhash(self.block_of(txid, node)))

    # ---------------------------------------------------------------- aT
    def gap_trial(self, case, k, coin, online):
        a = self.nodes[SELLER]
        n1 = self.nodes[1]
        t_hex = self.spend(coin, n1.getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()
        a.prioritisetransaction(txid=t_id, fee_delta=-10**12)   # the seller's own template leaves T out
        self.btc_mine(1)
        btc_tip = self.btc().getblockcount()
        n1.sendrawtransaction(t_hex)
        L = self.wait_tx_certified(n1, t_id)
        depth = self.btc().getblockcount() - L["anchorheight"] + 1
        # The fork point: the last block below L anchored under L's anchor.
        f = L["height"] - 1
        while n1.getblockheader(n1.getblockhash(f))["anchorheight"] >= L["anchorheight"]:
            f -= 1
        since = [self.log_size(i) for i in range(N)]
        t0 = time.time()
        self.bitcoin_reorg(depth)
        self.wait_until(lambda: all(self.nodes[i].getblockcount() < L["height"] or
                                    self.nodes[i].getblockheader(self.nodes[i].getblockhash(L["height"]))["hash"] != L["hash"]
                                    for i in online), timeout=60)
        t_leave = time.time() - t0
        try:
            a.sendrawtransaction(r_hex)
            r_sent = "in the seller's mempool %.1f s after the reorg" % (time.time() - t0)
        except JSONRPCException as e:
            r_sent = "seller's mempool refused R: %s" % e.error["message"]
        honest = [i for i in online if i != SELLER]
        self.wait_height(honest, f + 1, timeout=600)
        t_f1 = time.time() - t0
        f1 = n1.getblockhash(f + 1)
        txs = n1.getblock(f1)["tx"]
        rnd, members = self.round_of(f1, since)
        seller_props = self.proposals_at(SELLER, f + 1, since[SELLER])
        refused = {p: [i for i in honest if self.log_has(i, "Lost blocks: refusing block %s" % p, since[i])] for p in seller_props}
        refusal = None
        for p, idx in refused.items():
            if idx:
                refusal = self.log_has(idx[0], "Lost blocks: refusing block %s" % p, since[idx[0]])[0].split("[LostCheckBlock] ", 1)[-1][:200]
        led = any(refused.values())
        # A few more blocks: R never mined, T stays.
        self.wait_height(honest, f + 3, timeout=600)
        chain_txs = set()
        for h in range(f + 1, n1.getblockcount() + 1):
            chain_txs |= set(n1.getblock(n1.getblockhash(h))["tx"])
        res = dict(trial=k, L_height=L["height"], anchor=L["anchorheight"], btc_reorg_depth=depth, fork=f,
                   left_s=round(t_leave, 2), f1_s=round(t_f1, 2), f1_round=rnd, f1_members=members,
                   T_first=(txs[1] == t_id if len(txs) > 1 else False), T_in_chain=t_id in chain_txs,
                   R_in_chain=r_id in chain_txs, R=r_sent, seller_proposals=len(seller_props),
                   seller_backed_and_refused=led, refusal=refusal)
        self.record(case, "trial %d" % k, res)
        assert res["T_first"] and not res["R_in_chain"], res
        return res

    def case_split(self):
        """Two certified siblings that spend one coin differently, each held as its own by half the seats. Neither side
        has a quorum for its own transaction. Bitcoin moves on under live rounds: each step records the tips, what each
        side's chain carries and how many certified blocks stand above the fork, until all six nodes are on one tip."""
        case = "split"
        self.log.info("(split) a double-signed split under live rounds, %d/%d ms" % (ROUND_MS, WINDOW_MS))
        self.restart_node(SELLER, extra_args=self.node_args(SELLER) + ["-posdebugignorelost=0"])
        self.nodes[SELLER].loadwallet("w")
        self.mesh(range(N))
        self.sync_tips(range(N))
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, self.nodes[SELLER].getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()
        side_a, side_b = [1, 2, 3], [0, 4, 5]
        for i in range(N):
            self.disconnect_all(i)
        self.mesh(side_a)
        self.mesh(side_b)
        self.btc_mine(1)
        self.nodes[1].sendrawtransaction(t_hex)
        self.nodes[4].sendrawtransaction(r_hex)
        l = self.produce(1, ALL_KEYS, txs_must=[t_id])
        s = self.produce(4, ALL_KEYS, txs_must=[r_id])
        assert_equal(l["height"], s["height"])
        self.sync_tips(side_a)
        self.sync_tips(side_b)
        fork_h = l["height"] - 1
        self.record(case, "siblings", "L %s (T) on seats %s, S %s (R) on seats %s, both certified, height %d" % (l["hash"][:12], side_a, s["hash"][:12], side_b, l["height"]))
        self.bitcoin_reorg(1)
        self.wait_lost(l["hash"], side_a)
        self.wait_lost(s["hash"], side_b)
        self.mesh_across(side_a, side_b)
        self.wait_lost(s["hash"], side_a, timeout=120)
        self.wait_lost(l["hash"], side_b, timeout=120)
        marks = {i: self.lost(i)[l["hash"] if i in side_a else s["hash"]]["own"] for i in range(N)}
        other = {i: self.lost(i)[s["hash"] if i in side_a else l["hash"]]["own"] for i in range(N)}
        self.record(case, "each side holds its own block as its own and the other's as evidence", {"own": marks, "other": other})
        assert all(marks.values()) and not any(other.values())
        t0 = time.time()
        btc0 = self.btc().getblockcount()
        self.start_producers(range(N))
        since = [self.log_size(i) for i in range(N)]

        def chain_from(i):
            n = self.nodes[i]
            return [n.getblock(n.getblockhash(h)) for h in range(fork_h + 1, n.getblockcount() + 1)]

        def certified_above(i):
            n = self.nodes[i]
            return sum(1 for h in range(fork_h + 1, n.getblockcount() + 1) if n.getblockheader(n.getblockhash(h))["poscertified"])
        one_tip_at = None
        for step in range(SPLIT_STEPS):
            before = [self.nodes[i].getblockcount() for i in range(N)]
            self.wait_until(lambda: any(self.nodes[i].getblockcount() > before[i] or
                                        self.log_has(i, "restarting collection", since[i]) for i in (1, 4)), timeout=300)
            since = [self.log_size(i) for i in range(N)]
            self.btc_mine(3)
            end = time.time() + 150
            while time.time() < end and all(self.nodes[i].getblockcount() == before[i] for i in range(N)):
                time.sleep(1)
            time.sleep(3)
            tips = {i: self.nodes[i].getbestblockhash() for i in range(N)}
            has = {i: {"T": any(t_id in b["tx"] for b in chain_from(i)), "R": any(r_id in b["tx"] for b in chain_from(i))} for i in (1, 4)}
            self.record(case, "step %d" % step, dict(at_s=round(time.time() - t0), bitcoin_blocks=self.btc().getblockcount() - btc0,
                        heights={i: self.nodes[i].getblockcount() for i in range(N)}, tips=len(set(tips.values())),
                        certified_above_fork={i: certified_above(i) for i in (1, 4)}, side_a_has=has[1], side_b_has=has[4]))
            if len(set(tips.values())) == 1 and self.nodes[1].getblockcount() >= l["height"]:
                one_tip_at = round(time.time() - t0)
                break
        chain_a, chain_b = chain_from(1), chain_from(4)
        self.record(case, "outcome", "one tip on all %d nodes: %s (at %s s); side A's chain carries T: %s, R: %s; side B's carries T: %s, R: %s; %d Bitcoin blocks" % (
            N, one_tip_at is not None, one_tip_at, any(t_id in b["tx"] for b in chain_a), any(r_id in b["tx"] for b in chain_a),
            any(t_id in b["tx"] for b in chain_b), any(r_id in b["tx"] for b in chain_b), self.btc().getblockcount() - btc0))
        assert one_tip_at is not None, "the two sides did not reach one tip"

    def mesh_across(self, side_a, side_b):
        for i in side_a:
            for j in side_b:
                self.connect_nodes(i, j)

    def case_aT(self, case="aT"):
        self.log.info("(%s) autonomous committee, rounds %d ms, window %d ms, %d trials" % (case, ROUND_MS, WINDOW_MS, TRIALS))
        coins = [self.fund() for _ in range(TRIALS)]
        online = list(range(N))
        self.start_producers(online)
        n1 = self.nodes[1]
        h0 = n1.getblockcount()
        self.wait_height(online, h0 + 2, timeout=300)
        out = [self.gap_trial(case, k, coins[k], online) for k in range(TRIALS)]
        led = [r for r in out if r["seller_backed_and_refused"]]
        self.record(case, "summary", "%d trials; the seller's proposal backed first and refused in %d; F+1 certified in round(s) %s, %s s after the reorg; T first in every F+1; R in no chain" %
                    (len(out), len(led), [r["f1_round"] for r in out], [r["f1_s"] for r in out]))

    # ------------------------------------------------------------- deepT
    def case_deepT(self):
        depth = int(os.environ.get("LOST_DEPTH", "3"))
        per = int(os.environ.get("LOST_DEEP_TXS", "480"))
        case = "deepT%d" % depth
        self.log.info("(%s) %d stale Bitcoin block(s) of full blocks, replayed by the autonomous committee" % (case, depth))
        n1 = self.nodes[1]
        Ls, lost_txids, o_hex, o_id, f_height = self.deep_build(case, depth, per, 10)
        since = [self.log_size(i) for i in range(N)]
        t0 = time.time()
        self.bitcoin_reorg(depth)
        self.wait_until(lambda: all(self.nodes[i].getblockcount() <= f_height for i in HONEST), timeout=300)
        self.wait_lost(Ls[-1]["hash"], HONEST, timeout=300)
        self.record(case, "reorg", "honest nodes hold %d lost blocks %.1f s after the reorg" % (len(Ls), time.time() - t0))
        n1.sendrawtransaction(o_hex)
        producers = [1, 2, 3, 4, LATE]
        t0 = time.time()
        self.start_producers(producers)
        left = set(lost_txids)
        rows = []
        h = f_height
        last = t0
        while left or o_id in n1.getrawmempool():
            self.wait_height(producers, h + 1, timeout=900)
            now = time.time()
            bh = n1.getblockhash(h + 1)
            txs = n1.getblock(bh)["tx"]
            carried = [t for t in txs[1:] if t in left]
            left -= set(carried)
            rnd, members = self.round_of(bh, since)
            rows.append((h + 1, len(carried), o_id in txs, rnd, members, round(now - last, 1)))
            last = now
            h += 1
            if len(rows) > len(Ls) + 5:
                break
        lines = self.log_has(1, "plan computed on", since[1])
        micros = [int(l.split("classified in ")[1].split(" us")[0]) for l in lines]
        restarts = sum(len(self.log_has(i, "restarting collection", since[i])) for i in producers)
        self.record(case, "replay", "%d heights (height, carried, ordinary tx, round, members, seconds since previous): %s; left uncarried: %d; total %.0f s" %
                    (len(rows), rows, len(left), time.time() - t0))
        self.record(case, "rounds", "heights certified in round 0: %d of %d; collections restarted (all producers): %d" %
                    (sum(1 for r in rows if r[3] == 0), len(rows), restarts))
        if micros:
            self.record(case, "plans", "%d plans on node 1, %d..%d us (median %d)" %
                        (len(micros), min(micros), max(micros), sorted(micros)[len(micros) // 2]))
        assert not left

    # ---------------------------------------------------------------- c2
    def case_c2(self):
        m = C2_SIGNERS
        case = "c2-m%d-%s" % (m, "on" if C2_ON else "off")
        self.log.info("(%s) %d of 6 members sign a candidate that never certifies; its anchor goes stale" % (case, m))
        A = [1, 2, 3][:m]
        B = [i for i in range(N) if i not in A]
        a = self.nodes[SELLER]
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()
        # The partition: A among itself (one member alone gets node 5, not producing yet, as its peer).
        for i in range(N):
            self.disconnect_all(i)
        for i in range(N):
            self.nodes[i].setnetworkactive(False)
        for i in range(N):
            self.nodes[i].setnetworkactive(True)
        self.wait_until(lambda: all(self.nodes[i].getconnectioncount() == 0 for i in range(N)), timeout=30)
        self.mesh(A if m > 1 else A + [LATE])
        self.mesh(B)
        H = self.nodes[1].getblockcount() + 1
        self.btc_mine(1)
        h = self.btc().getblockcount()
        self.wait_until(lambda: all(self.nodes[i].getbestblockhash() and self.btc().getblockcount() == h for i in range(N)), timeout=10)
        self.nodes[A[0]].sendrawtransaction(t_hex)
        self.nodes[B[0]].sendrawtransaction(r_hex)
        self.sync_mempools_of(B, r_id)
        time.sleep(3)   # every node's anchor watcher has seen Bitcoin h before A proposes on it
        since = [self.log_size(i) for i in range(N)]
        self.start_producers(A)
        # Wait until every member of A has signed a candidate at H carrying T.
        def signed():
            if not all(self.proposals_at(i, H, since[i]) for i in A):
                return False
            if C2_ON:
                return all(self.nodes[i].getlostblocks()["signed"] >= 1 for i in A)
            return time.time() - t_start > 3 * ROUND_MS / 1000.0
        t_start = time.time()
        self.wait_until(signed, timeout=180)
        self.record(case, "signed", "members %s signed a candidate at height %d carrying T (anchor %d); the rest hold R in their mempools" % (A, H, h))
        assert all(self.nodes[i].getblockcount() < H for i in range(N))
        self.bitcoin_reorg(1)
        time.sleep(3)
        if C2_ON:
            self.wait_until(lambda: all(self.nodes[i].getlostblocks()["signedboundoutpoints"] >= 1 for i in A), timeout=30)
        self.record(case, "after the reorg, A's getlostblocks", [{k: self.nodes[i].getlostblocks()[k] for k in ("signed", "signedboundoutpoints")} for i in A])
        since = [self.log_size(i) for i in range(N)]
        t0 = time.time()
        self.mesh(range(N))
        self.start_producers(B)
        # Bitcoin goes on: one block every LOST_C2_BTC_EVERY seconds until the height is certified.
        every = int(os.environ.get("LOST_C2_BTC_EVERY", "60"))
        btc0 = self.btc().getblockcount()
        anchor_stale = h
        while self.nodes[1].getblockcount() < H:
            try:
                self.wait_height(range(N), H, timeout=every)
            except AssertionError:
                assert time.time() - t0 < 3600, "no block at height %d in an hour" % H
                self.btc_mine(1)
        dt = time.time() - t0
        self.record(case, "Bitcoin meanwhile", "%d block(s) mined (one per %d s), Bitcoin at %d; the stale candidate's anchor was %d, its window ends above %d" %
                    (self.btc().getblockcount() - btc0, every, self.btc().getblockcount(), anchor_stale, anchor_stale + 6))
        bh = self.nodes[1].getblockhash(H)
        txs = self.nodes[1].getblock(bh)["tx"]
        rnd, members = self.round_of(bh, since)
        restarts = max(len(self.log_has(i, "restarting collection", since[i])) for i in range(N))
        refusals = sum(len(self.log_has(i, "bad-txns-signed-respend", since[i])) for i in A)
        self.record(case, "pause", "height %d certified %.1f s after the reconnect (round %s of its collection, %s members); collections restarted: %d; "
                    "the block carries T: %s, R: %s; signed-respend refusals logged by A: %d" %
                    (H, dt, rnd, members, restarts, t_id in txs, r_id in txs, refusals))

    def has_block(self, i, h):
        try:
            self.nodes[i].getblock(h)
            return True
        except JSONRPCException:
            return False

    # ---------------------------------------------------------------- mute
    def case_mute(self):
        """A 1-deep Bitcoin reorganization lands while the proposals at the next height are being collected,
        and every one of them is anchored on the replaced Bitcoin block (the tip's own anchor is older)."""
        wait_s = int(os.environ.get("LOST_MUTE_WAIT", "300"))
        case = "mute"
        online = list(range(N))
        self.start_producers(online)
        n1 = self.nodes[1]
        self.wait_height(online, n1.getblockcount() + 2, timeout=300)
        for attempt in range(15):
            tip = n1.getblockcount()
            s0 = [self.log_size(i) for i in range(N)]
            # The proposals at tip+1 are out (anchored on the current Bitcoin tip): a new Bitcoin block now.
            self.wait_until(lambda: sum(1 for i in online if self.proposals_at(i, tip + 1, s0[i])) >= 4 or n1.getblockcount() > tip, timeout=60)
            if n1.getblockcount() > tip:
                continue
            self.btc_mine(1)
            h = self.btc().getblockcount()
            since = [self.log_size(i) for i in range(N)]
            self.wait_height(online, tip + 1, timeout=120)
            H = tip + 2
            # Every member proposes at H, on Bitcoin h; replace h before any of them certifies.
            self.wait_until(lambda: all(self.proposals_at(i, H, since[i]) for i in online) or n1.getblockcount() >= H, timeout=60)
            if n1.getblockcount() >= H or n1.getblockheader(n1.getblockhash(H - 1))["anchorheight"] >= h:
                self.log.info("  attempt %d: timing missed (tip %d, anchor of H-1 %d, Bitcoin %d), again", attempt,
                              n1.getblockcount(), n1.getblockheader(n1.getblockhash(H - 1))["anchorheight"], h)
                continue
            t0 = time.time()
            self.bitcoin_reorg(1)
            break
        else:
            raise AssertionError("no attempt landed inside a collection")
        self.record(case, "set-up", "tip %d anchored below Bitcoin %d; every member proposed at height %d on Bitcoin %d; Bitcoin then replaced %d (1 deep) at %.1f s" %
                    (H - 1, h, H, h, h, 0.0))
        try:
            self.wait_height(online, H, timeout=wait_s)
            self.record(case, "outcome", "height %d certified %.1f s after the reorganization, Bitcoin unchanged since" % (H, time.time() - t0))
        except AssertionError:
            restarts = [len(self.log_has(i, "restarting collection", since[i])) for i in online]
            aside = [len(self.log_has(i, "anchor-stale", since[i])) for i in online]
            self.record(case, "outcome", "no block at height %d %d s after the reorganization, Bitcoin unchanged since; collections restarted per node %s; 'anchor-stale' lines per node %s" %
                        (H, wait_s, restarts, aside))
            t1 = time.time()
            self.btc_mine(1)
            try:
                self.wait_height(online, H, timeout=wait_s)
                self.record(case, "after one more Bitcoin block", "height %d certified %.1f s later" % (H, time.time() - t1))
            except AssertionError:
                self.record(case, "after one more Bitcoin block", "still no block at height %d %d s later" % (H, wait_s))
                raise

    # ---------------------------------------------------------------- mixT
    def case_mixT(self):
        assert self.base_nodes, "set LOST_BASE_NODES and LOST_BASE_BIN"
        # The member on master's binary does not know the rule at all.
        for i in sorted(self.base_nodes):
            known = "unknown command" not in self.nodes[i].help("getlostblocks").lower()
            self.record("mixT", "node %d runs the rule" % i, known)
            assert not known, "node %d is not on master's binary" % i
        self.case_aT(case="mixT")

    def run_test(self):
        try:
            for c in CASES:
                getattr(self, "case_" + c)()
        finally:
            from feature_pos_lost_blocks import RESULTS
            if RESULTS:
                import json
                with open(RESULTS, "w") as f:
                    json.dump(self.results, f, indent=1)
            self.stop_bitcoind()


if __name__ == '__main__':
    LostAutonomousTest().main()
