#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Transactions of a lost block carried into the chain that replaces it
(doc 03, "Transactions of a lost block"): the replay, its categories and order,
the holders' duty, and the node's own store across an unclean stop.

The network, the seller and the Bitcoin daemon are those of
feature_pos_lost_blocks.py. Cases (LOST_CASES):
  r1  the replay carries a transaction this producer's own floor refuses, and a
      block that omits it is refused even when a quorum certified it;
  r2  two lost blocks: a transaction due at once, one held by its locktime, a
      child held by its relative lock; replayed at the heights they fall due,
      with a holder killed (-9) between the steps;
  r3  two lost branches whose transactions conflict: the one lost first stands.
"""

import os
import time
from decimal import Decimal

from feature_pos_lost_blocks import LostBlocksTest, SELLER, HONEST, LATE, ALL_KEYS, N
from test_framework.authproxy import JSONRPCException
from test_framework.messages import tx_from_hex
from test_framework.util import assert_equal

CASES = os.environ.get("LOST_CASES", "r1,r2,r3").split(",")


class LostReplayTest(LostBlocksTest):
    def set_test_params(self):
        super().set_test_params()

    # ------------------------------------------------------------- helpers
    def tx_ids(self, node, h):
        return node.getblock(h)["tx"]

    def lose(self, case, Ls):
        """Bitcoin replaces the anchor of the blocks Ls (1 deep); wait until every honest node holds them lost."""
        sizes = [self.log_size(i) for i in range(N)]
        fork = self.bitcoin_reorg(1)
        for L in Ls:
            self.wait_lost(L, HONEST)
        self.record(case, "reorg", "Bitcoin 1 deep (fork %d); %d block(s) lost on %s" % (fork, len(Ls), HONEST))
        return sizes

    def refused_by(self, case, idx, h, since, reason, timeout=30):
        def ok():
            return all(self.log_has(i, "Lost blocks: refusing block %s" % h, since[i]) for i in idx)
        self.wait_until(ok, timeout=timeout)
        line = self.log_has(idx[0], "Lost blocks: refusing block %s" % h, since[idx[0]])[0]
        assert reason in line, line
        for i in idx:
            assert self.nodes[i].getbestblockhash() != h
        self.record(case, "refusal (node %d)" % idx[0], line.split("[LostCheckBlock] ", 1)[-1][:300])

    def kill9(self, i):
        n = self.nodes[i]
        n.process.kill()
        n.process.wait(timeout=30)
        n.running = False
        n.process = None
        n.rpc_connected = False
        n.rpc = None
        # The framework expects a clean stop; the datadir is what matters.
        self.start_node(i, extra_args=self.node_args(i))
        for j in range(N):
            if j != i:
                self.connect_nodes(i, j)

    def seller_block(self, signers, exclude=(), include=()):
        """The seller builds on its own tip, leaving `exclude` out of its template."""
        a = self.nodes[SELLER]
        for t in exclude:
            a.prioritisetransaction(txid=t, fee_delta=-10**12)
        try:
            x = self.produce(SELLER, signers, leaders=[SELLER], txs_must=list(include))
        finally:
            for t in exclude:
                a.prioritisetransaction(txid=t, fee_delta=10**12)
        for t in exclude:
            assert t not in x["block"]["tx"]
        return x

    # ---------------------------------------------------------------- r1
    def case_r1(self):
        self.log.info("(r1) the replay carries what this producer's floor refuses; omitting it is refused")
        a = self.nodes[SELLER]
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0000001"))
        t_id = tx_from_hex(t_hex).rehash()
        try:
            self.nodes[1].sendrawtransaction(t_hex)
            raise AssertionError("an honest mempool took a transaction with a fee below its floor")
        except JSONRPCException as e:
            self.record("r1", "honest node's own floor refuses T (a fee below its floor)", e.error["message"])
        a.sendrawtransaction(t_hex)
        self.btc_mine(1)
        L = self.produce(SELLER, ALL_KEYS, leaders=[1], txs_must=[t_id])
        self.produce(SELLER, ALL_KEYS, leaders=[1])
        self.sync_tips(range(N))
        assert self.nodes[1].getblockheader(L["hash"])["poscertified"]
        self.record("r1", "T", "%s, a fee below its floor, in certified L %s at height %d" % (t_id[:16], L["hash"][:16], L["height"]))
        since = self.lose("r1", [L["hash"]])
        # A quorum certifies a block on the fork point that leaves T out.
        x = self.seller_block(ALL_KEYS, exclude=[t_id])
        self.record("r1", "X", "%s at height %d, certified by %d signers, without T" % (x["hash"][:16], x["height"], x["countersignatures"] + 1))
        self.refused_by("r1", HONEST, x["hash"], since, "bad-lost-replay")
        # The seller leads; an honest node assembles: T comes first, whatever the floor.
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        txs = self.tx_ids(self.nodes[1], h["hash"])
        assert_equal(txs[1], t_id)
        self.sync_tips(HONEST + [LATE])
        self.record("r1", "outcome", "block %s led by the seller's key, assembled by node 1: tx[1] = T (a fee below its floor) at height %d on every honest node" %
                    (h["hash"][:16], h["height"]))
        self.realign()

    # ---------------------------------------------------------------- r2
    def case_r2(self):
        self.log.info("(r2) two lost blocks: due now, held by a locktime, held by a relative lock")
        a = self.nodes[SELLER]
        n1 = self.nodes[1]
        coin1 = self.fund()
        coin2 = self.fund()
        self.btc_mine(1)
        f_height = n1.getblockcount()
        # T1 now; T2 locked to L2's height; T3 spends T1 with a relative lock of 1 block.
        mid = a.getnewaddress()
        t1_hex = self.spend(coin1, mid, Decimal("0.0001"))
        t1 = tx_from_hex(t1_hex)
        t1_id = t1.rehash()
        raw2 = a.createrawtransaction([{"txid": coin2["txid"], "vout": coin2["vout"], "sequence": 0xfffffffe}],
                                      [{n1.getnewaddress(): coin2["amount"] - Decimal("0.0001")}, {"fee": Decimal("0.0001")}],
                                      f_height + 1)
        t2_hex = a.signrawtransactionwithwallet(raw2)["hex"]
        t2_id = tx_from_hex(t2_hex).rehash()
        n1.sendrawtransaction(t1_hex)
        L1 = self.produce(1, ALL_KEYS, txs_must=[t1_id])
        self.sync_tips(range(N))
        vout = [o for o in a.getrawtransaction(t1_id, 1)["vout"] if o["scriptPubKey"].get("address") == mid][0]
        raw3 = a.createrawtransaction([{"txid": t1_id, "vout": vout["n"], "sequence": 1}],
                                      [{n1.getnewaddress(): Decimal(str(vout["value"])) - Decimal("0.0001")}, {"fee": Decimal("0.0001")}])
        t3_hex = a.signrawtransactionwithwallet(raw3)["hex"]
        t3_id = tx_from_hex(t3_hex).rehash()
        n1.sendrawtransaction(t2_hex)
        n1.sendrawtransaction(t3_hex)
        L2 = self.produce(1, ALL_KEYS, txs_must=[t2_id, t3_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        assert_equal(L1["block"]["anchorheight"], L2["block"]["anchorheight"])
        self.record("r2", "lost branch", "L1 %s (h %d: T1 %s), L2 %s (h %d: T2 %s nLockTime %d, T3 %s nSequence 1 on T1)" %
                    (L1["hash"][:12], L1["height"], t1_id[:12], L2["hash"][:12], L2["height"], t2_id[:12], f_height + 1, t3_id[:12]))
        since = self.lose("r2", [L1["hash"], L2["hash"]])
        # Height F+1: only T1 is due. A block with nothing is refused.
        x = self.seller_block(ALL_KEYS, exclude=[t1_id, t2_id, t3_id])
        self.refused_by("r2", HONEST, x["hash"], since, "bad-lost-replay")
        h1 = self.produce(1, ALL_KEYS, leaders=[SELLER])
        txs = self.tx_ids(n1, h1["hash"])
        assert_equal(txs[1], t1_id)
        assert t2_id not in txs and t3_id not in txs
        self.sync_tips(HONEST + [LATE])
        self.record("r2", "F+1", "block %s at height %d: tx[1] = T1; T2 (locktime) and T3 (relative lock) held back" % (h1["hash"][:12], h1["height"]))
        # A holder stopped with kill -9 between the steps keeps its store and its duty.
        self.kill9(3)
        held = self.lost(3)
        assert L1["hash"] in held and L2["hash"] in held
        self.sync_tips(HONEST + [LATE])
        self.record("r2", "kill -9 of node 3 after F+1", "restarted holding %d lost block(s) at once (L1, L2 among them), back on the honest tip %s; X stays refused there: %s" %
                    (len(held), self.nodes[3].getbestblockhash()[:12], self.status_of(self.nodes[3], x["hash"])))
        self.realign()
        since = [self.log_size(i) for i in range(N)]
        y = self.seller_block(ALL_KEYS, exclude=[t2_id, t3_id])
        self.refused_by("r2", [1, 2, 3, 4], y["hash"], since, "bad-lost-replay")
        h2 = self.produce(1, ALL_KEYS, leaders=[SELLER])
        txs = self.tx_ids(n1, h2["hash"])
        assert_equal(set(txs[1:3]), {t2_id, t3_id})
        self.sync_tips(HONEST + [LATE])
        self.record("r2", "F+2", "block %s at height %d: tx[1..2] = T2, T3 (in lost order)" % (h2["hash"][:12], h2["height"]))
        self.realign()

    # ---------------------------------------------------------------- r3
    def case_r3(self):
        self.log.info("(r3) two lost branches that conflict: the one lost first stands")
        a = self.nodes[SELLER]
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()
        self.btc_mine(1)
        self.nodes[1].sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        self.sync_tips(range(N))
        self.bitcoin_reorg(1)
        self.wait_lost(L["hash"], HONEST)
        # The seller's branch: X with R, certified by a dishonest quorum, refused by holders.
        a.sendrawtransaction(r_hex)
        x = self.produce(SELLER, ALL_KEYS, leaders=[SELLER], txs_must=[r_id])
        # Bitcoin moves to a third branch: X is lost too (on the seller's node, which shows it).
        self.bitcoin_reorg(1)
        self.wait_until(lambda: x["hash"] in [b["hash"] for b in a.getlostblocks()["blocks"]], timeout=60)
        ev = a.getlostblock(x["hash"])
        r = self.nodes[2].submitlostblock(ev["block"], ev["bitcoinheaders"])
        self.wait_until(lambda: x["hash"] in self.lost(2), timeout=60)
        self.record("r3", "two lost branches on node 2", "L (T, lost first) and X (R, lost second, shown by submitlostblock: %r)" % r)
        h = self.produce(2, ALL_KEYS, leaders=[SELLER])
        txs = self.tx_ids(self.nodes[2], h["hash"])
        assert_equal(txs[1], t_id)
        assert r_id not in txs
        self.record("r3", "outcome", "block %s at height %d assembled by node 2: tx[1] = T; R not carried (its input is T's)" % (h["hash"][:12], h["height"]))
        self.sync_tips(HONEST + [LATE])
        self.realign()

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
    LostReplayTest().main()
