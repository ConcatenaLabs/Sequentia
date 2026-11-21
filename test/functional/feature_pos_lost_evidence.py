#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Evidence never weakens what a holder demands (doc 03, "Transactions of a lost
block").

Live producers at the testnet's round lengths (LOST_ROUND_MS, LOST_WINDOW_MS) on
the network of feature_pos_lost_blocks.py. Cases (LOST_CASES):
  evstall  every honest node holds T in a lost block L. A coalition certifies a
           sibling of L that spends T's coin differently, after the fact, and
           then withholds three of the six seats, so no collection reaches the
           quorum. Bitcoin moves on until the escaping-stall valve opens at each
           height. T must be mined, or the chain must wait; the seller's respend
           R must not be mined while T is still bound or after T was left out.
  shorter  Bitcoin moves to a branch whose tip is below L's anchor, as a branch
           with fewer blocks and more work does. L is lost, T stays protected
           and a block carrying R is refused, before and after Bitcoin grows
           back to L's anchor height; the honest chain then mines T.
  valve    a sub-quorum block of the escaping-stall valve that omits T, with the
           valve open: every holder refuses it (bad-lost-replay).
  twolost  two lost blocks of a node's own chain at one height. The holders
           follow a block X carrying R once a quorum has certified past it at
           the waiver's depth, and Bitcoin then takes X too. Both are their own:
           T comes first by lost order, R's block is refused, and T is mined.
  afterwaiver  the holders have followed a quorum past one lost block. A later,
           unrelated block L2 is lost above it: the waiver of the first loss says
           nothing about L2, so a sub-quorum block carrying L2's respend is refused.
"""

import time
from decimal import Decimal

from feature_pos_lost_autonomous import LostAutonomousTest
from feature_pos_lost_blocks import ALL_KEYS, HONEST, SELLER, W, WAIVER
from feature_pos_lost_record_attacks import LostRecordAttacksTest
from test_framework.authproxy import JSONRPCException
from test_framework.messages import tx_from_hex
from test_framework.util import assert_equal

COALITION = [0, 4, 5]
HOLDERS = [1, 2, 3]
#: The quorum of the sibling at the lost height: the coalition and one more seat.
SIBLING_SIGNERS = [0, 4, 5, 1]


class LostEvidenceTest(LostAutonomousTest, LostRecordAttacksTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        LostAutonomousTest.run_test(self)

    def restarted(self, since, height):
        """Whether a holder's collection at `height` ended without a certificate."""
        text = "at height %d" % height
        return any(text in line for i in HOLDERS for line in self.log_has(i, "restarting collection", since[i]))

    def chain_has(self, node, txid, from_height):
        for h in range(from_height, node.getblockcount() + 1):
            if txid in node.getblock(node.getblockhash(h))["tx"]:
                return h
        return None

    def case_shorter(self):
        case = "shorter"
        self.log.info("(shorter) Bitcoin's new tip is below the lost block's anchor")
        a, n1 = self.nodes[SELLER], self.nodes[1]
        coin = self.fund()
        t_hex = self.spend(coin, n1.getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()
        self.btc_mine(2)
        anchor_h = self.btc().getblockcount()
        first = self.btc().getblockhash(anchor_h - 1)
        n1.sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        assert_equal(L["block"]["anchorheight"], anchor_h)
        assert n1.getblock(L["block"]["previousblockhash"])["anchorheight"] <= anchor_h - 2
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(len(self.nodes)))
        assert n1.getblockheader(L["hash"])["poscertified"]
        since = [self.log_size(i) for i in range(len(self.nodes))]
        self.btc().invalidateblock(first)
        self.btc_mine(1)
        assert_equal(self.btc().getblockcount(), anchor_h - 1)
        self.wait_until(lambda: all(self.nodes[i].getblockcount() < L["height"] for i in range(len(self.nodes))), timeout=60)
        self.wait_lost(L["hash"], HONEST)
        marks = {i: (self.lost(i)[L["hash"]]["own"], self.lost(i)[L["hash"]]["anchor"]) for i in HONEST}
        self.record(case, "L on each holder (own, anchor), Bitcoin's tip one below its anchor", marks)
        assert all(m == (True, "stale") for m in marks.values()), marks
        try:
            self.nodes[2].sendrawtransaction(r_hex)
            raise AssertionError("ATTACK SUCCEEDED (shorter): an honest mempool took R")
        except JSONRPCException as e:
            assert "txn-lost-conflict" in e.error["message"], e.error
            self.record(case, "honest mempool refuses R", e.error["message"])
        if r_id not in a.getrawmempool():
            a.sendrawtransaction(r_hex)
        x = self.produce(SELLER, ALL_KEYS, leaders=[SELLER], txs_must=[r_id])
        self.assert_refused(case, HONEST, x["hash"], since)
        raw_x = a.getblock(x["hash"], 0)
        self.btc_mine(1)
        assert_equal(self.btc().getblockcount(), anchor_h)
        time.sleep(4)
        answers = {i: self.nodes[i].submitblock(raw_x) for i in HONEST}
        self.record(case, "the block with R offered again, Bitcoin back at the anchor's height", answers)
        assert all(self.nodes[i].getbestblockhash() != x["hash"] for i in HONEST)
        assert all(self.lost(i)[L["hash"]]["anchor"] == "stale" for i in HONEST)
        h = self.produce(1, [1, 2, 3, 4], leaders=[1, 2, 3, 4])
        self.sync_tips(HONEST)
        t_at, r_at = self.block_of(t_id, n1), self.block_of(r_id, n1)
        self.record(case, "outcome", "honest block at height %d; T at %s, R at %s" % (h["height"], t_at, r_at))
        if r_at is not None or t_at is None:
            raise AssertionError("ATTACK SUCCEEDED (shorter): T at %s, R at %s" % (t_at, r_at))
        assert_equal(n1.getblock(n1.getblockhash(t_at))["tx"][1], t_id)
        self.record(case, "blocked", "T is mined, and R is not")

    def case_valve(self):
        case = "valve"
        self.log.info("(valve) a sub-quorum block that omits a due transaction")
        st = self.gap(case)
        a, n1 = self.nodes[SELLER], self.nodes[1]
        for t in (st["t_id"], st["r_id"]):   # the block carries neither T nor R
            a.prioritisetransaction(txid=t, fee_delta=-10**12)
        since = [self.log_size(i) for i in range(len(self.nodes))]
        x = self.produce(SELLER, COALITION, leaders=[SELLER])
        assert st["t_id"] not in x["block"]["tx"] and st["r_id"] not in x["block"]["tx"], (
            "the seller's block carries T: %s, R: %s" % (st["t_id"] in x["block"]["tx"], st["r_id"] in x["block"]["tx"]))
        head = a.getblockheader(x["hash"])
        parent = a.getblockheader(head["previousblockhash"])
        self.record(case, "block", "%s at height %d, %d countersignature(s), certified %s, anchor %d over the parent's %d" % (
            x["hash"][:16], x["height"], x["countersignatures"], head["poscertified"], head["anchorheight"], parent["anchorheight"]))
        assert not head["poscertified"] and head["anchorheight"] - parent["anchorheight"] >= 3
        self.assert_refused(case, HONEST, x["hash"], since)
        lines = self.log_has(1, "Lost blocks: refusing block %s" % x["hash"], since[1])
        assert "bad-lost-replay" in lines[0], lines[0]
        assert n1.getbestblockhash() != x["hash"]
        self.record(case, "blocked", "every holder refuses it: bad-lost-replay")

    def case_twolost(self):
        case = "twolost"
        self.log.info("(twolost) two lost blocks of one node's own chain at one height, waiver depth %d" % WAIVER)
        st = self.gap(case)
        a, n1 = self.nodes[SELLER], self.nodes[1]
        self.btc_mine(3)   # past the contested heights, so that X anchors at Bitcoin's tip
        time.sleep(2)
        x = self.produce(SELLER, ALL_KEYS, leaders=[SELLER], txs_must=[st["r_id"]])
        assert_equal(x["height"], st["L_h"])
        top = x
        for _ in range(WAIVER):
            top = self.produce(SELLER, ALL_KEYS, leaders=[SELLER])
        self.wait_until(lambda: all(self.nodes[i].getbestblockhash() == top["hash"] for i in HONEST), timeout=120)
        self.record(case, "followed", "the holders follow X (carries R) at height %d under %d certified block(s)" % (x["height"], WAIVER))
        x_anchor = a.getblockheader(x["hash"])["anchorheight"]
        assert_equal(x_anchor, self.btc().getblockcount())
        since = [self.log_size(i) for i in range(len(self.nodes))]
        self.bitcoin_reorg(1)
        self.wait_until(lambda: all(self.nodes[i].getblockcount() < x["height"] for i in HONEST), timeout=60)
        self.wait_lost(x["hash"], HONEST)
        marks = {i: (self.lost(i)[st["L"]]["own"], self.lost(i)[x["hash"]]["own"]) for i in HONEST}
        self.record(case, "(L own, X own) on each holder", marks)
        assert all(m == (True, True) for m in marks.values()), marks
        if st["r_id"] not in a.getrawmempool():
            a.sendrawtransaction(st["r_hex"])
        y = self.produce(SELLER, ALL_KEYS, leaders=[SELLER], txs_must=[st["r_id"]])
        self.assert_refused(case, HONEST, y["hash"], since)
        lines = self.log_has(1, "Lost blocks: refusing block %s" % y["hash"], since[1])
        self.record(case, "a block that carries R first", lines[0].split("refusing block ", 1)[1][:190])
        assert "bad-lost-replay" in lines[0], lines[0]
        h = self.produce(1, [1, 2, 3, 4], leaders=[1, 2, 3, 4])
        self.sync_tips(HONEST)
        txs = n1.getblock(n1.getblockhash(st["L_h"]))["tx"]
        t_at, r_at = self.block_of(st["t_id"], n1), self.block_of(st["r_id"], n1)
        self.record(case, "outcome", "honest block at height %d, first transaction T: %s; T at %s, R at %s" % (h["height"], txs[1] == st["t_id"], t_at, r_at))
        if r_at is not None or t_at is None or txs[1] != st["t_id"]:
            raise AssertionError("ATTACK SUCCEEDED (twolost): T at %s, R at %s" % (t_at, r_at))
        self.record(case, "blocked", "T comes first by lost order and is mined; R is not")

    def case_afterwaiver(self):
        case = "afterwaiver"
        self.log.info("(afterwaiver) a later loss above a block the holders followed, waiver depth %d" % WAIVER)
        st = self.gap(case)
        a, n1 = self.nodes[SELLER], self.nodes[1]
        self.btc_mine(3)
        time.sleep(2)
        self.produce(SELLER, ALL_KEYS, leaders=[SELLER], txs_must=[st["r_id"]])
        for _ in range(WAIVER):
            top = self.produce(SELLER, ALL_KEYS, leaders=[SELLER])
        self.wait_until(lambda: all(self.nodes[i].getbestblockhash() == top["hash"] for i in HONEST), timeout=120)
        self.record(case, "first loss", "the holders followed the quorum past L at depth %d" % WAIVER)
        coin = self.fund()
        t_hex = self.spend(coin, n1.getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()
        self.btc_mine(1)
        anchor = self.btc().getblockcount()
        n1.sendrawtransaction(t_hex)
        l2 = self.produce(1, ALL_KEYS, txs_must=[t_id])
        assert_equal(l2["block"]["anchorheight"], anchor)
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(len(self.nodes)))
        since = [self.log_size(i) for i in range(len(self.nodes))]
        self.bitcoin_reorg(1)
        self.wait_until(lambda: all(self.nodes[i].getblockcount() < l2["height"] for i in HONEST), timeout=60)
        self.wait_lost(l2["hash"], HONEST)
        if r_id not in a.getrawmempool():
            a.sendrawtransaction(r_hex)
        y = self.produce(SELLER, COALITION, leaders=[SELLER], txs_must=[r_id])
        self.wait_until(lambda: all(self.nodes[i].getbestblockhash() == y["hash"] or
                                    self.log_has(i, "Lost blocks: refusing block %s" % y["hash"], since[i]) for i in HONEST), timeout=30)
        took = [i for i in HONEST if self.nodes[i].getbestblockhash() == y["hash"]]
        self.record(case, "a sub-quorum block with L2's respend, 3 seats, no certified child", "taken by holders %s" % took)
        if took:
            raise AssertionError("ATTACK SUCCEEDED (afterwaiver): holders %s took the respend of a later lost block" % took)
        self.assert_refused(case, HONEST, y["hash"], since)
        self.record(case, "blocked", "the waiver of the first loss does not bear on the second")

    def case_evstall(self):
        case = "evstall"
        self.log.info("(evstall) a sibling certified after the fact, then a blocking share, waiver depth %d" % WAIVER)
        st = self.gap(case)
        a, n1 = self.nodes[SELLER], self.nodes[1]
        hdr = n1.getblockheader(st["L"])
        fork = hdr["height"] - 1
        self.isolate()
        sibling = self.evidence_block([tx_from_hex(st["r_hex"])], hdr["anchorheight"], int(hdr["anchorhash"], 16), SIBLING_SIGNERS)
        sibling.rehash()
        self.show_evidence(sibling, to=2, btc=self.stale_chain(hdr["anchorhash"]))
        self.wait_until(lambda: all(sibling.hash in self.lost(i) for i in HOLDERS), timeout=60)
        self.record(case, "sibling", "%s at height %d carries R, certified by seats %s after the loss; every holder has it as evidence: %s" % (
            sibling.hash[:16], hdr["height"], SIBLING_SIGNERS, all(not self.lost(i)[sibling.hash]["own"] for i in HOLDERS)))

        since = [self.log_size(i) for i in range(len(self.nodes))]
        t0 = time.time()
        self.start_producers(HOLDERS)
        rows = []
        lapse = None
        for _ in range(8):
            height = n1.getblockcount() + 1
            # The first collection at this height fails on the withheld seats.
            self.wait_until(lambda: n1.getblockcount() >= height or self.restarted(since, height), timeout=420)
            if n1.getblockcount() < height:
                self.btc_mine(3)
            self.wait_until(lambda: n1.getblockcount() >= height, timeout=600)
            b = n1.getblock(n1.getblockhash(height))
            head = n1.getblockheader(b["hash"])
            rows.append(dict(height=height, at_s=round(time.time() - t0), anchor=head["anchorheight"], certified=head["poscertified"],
                             has_T=st["t_id"] in b["tx"], txs=len(b["tx"]) - 1))
            self.record(case, "block at height %d" % height, rows[-1])
            if st["t_id"] in b["tx"]:
                break
            if height >= hdr["height"]:
                at_lost = n1.getblockheader(n1.getblockhash(hdr["height"]))["anchorheight"]
                lapse = max(hdr["anchorheight"], at_lost) + W
                if self.btc().getblockcount() >= lapse and head["anchorheight"] >= lapse:
                    break

        t_at = self.chain_has(n1, st["t_id"], fork + 1)
        r_at = self.chain_has(n1, st["r_id"], fork + 1)
        if t_at is None:
            # T was left out up to the lapse. The producers stop, the seller comes back and proposes R.
            self.record(case, "lapse", "T left out of every block to Bitcoin height %s; the seller now mines R" % lapse)
            for i in HOLDERS:
                self.restart_node(i, extra_args=self.node_args(i))
            self.mesh(range(1, len(self.nodes)))
            self.rejoin()
            if st["r_id"] not in a.getrawmempool():
                a.sendrawtransaction(st["r_hex"])
            x = self.produce(SELLER, COALITION, leaders=[SELLER], txs_must=[st["r_id"]], allow_fail=True)
            self.record(case, "seller's block", {k: x[k] for k in ("hash", "height", "error") if k in x})
            if "hash" in x:
                self.wait_until(lambda: n1.getbestblockhash() == x["hash"] or
                                self.log_has(1, "Lost blocks: refusing block %s" % x["hash"], since[1]), timeout=60)
            t_at = self.chain_has(n1, st["t_id"], fork + 1)
            r_at = self.chain_has(n1, st["r_id"], fork + 1)
        self.record(case, "outcome", "T at height %s, the seller's R at height %s, %d block(s) after the fork in %d s" % (
            t_at, r_at, n1.getblockcount() - fork, time.time() - t0))
        if r_at is not None:
            raise AssertionError("ATTACK SUCCEEDED (evstall): the seller's respend is mined at height %d and T is not" % r_at)
        assert t_at is not None or n1.getblockcount() == fork, "T left out of a block without the seller's spend"
        self.record(case, "blocked", "T is mined, and R is not")


if __name__ == '__main__':
    LostEvidenceTest().main()
