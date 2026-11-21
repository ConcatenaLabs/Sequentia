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
  r3  two lost branches whose transactions conflict: the one lost first stands;
  r4  a carried transaction paying its fee in an asset the producer does not
      list: claimed by default, left unclaimed with -poslostleaveunlisted;
  r5  more due transactions than one block holds, bound by signature
      operations: the longest prefix that fits, refused when empty or short,
      nothing else while it is cut, ordinary transactions once it is done;
  r6  the abuse: the seller fills its own certified block with transactions
      paying fees in an asset only it lists; the block is lost; Bitcoin goes
      there, back and there again: each is mined once, and ordinary
      transactions follow as soon as the due set is done.
"""

import os
import shutil
import time
from decimal import Decimal

from feature_pos_lost_blocks import WAIVER, LostBlocksTest, SELLER, HONEST, LATE, ALL_KEYS, N, W
from test_framework.authproxy import JSONRPCException
from test_framework.messages import tx_from_hex, CTxInWitness
from test_framework.script import CScript, OP_0, OP_1, OP_IF, OP_ENDIF, OP_DROP, OP_CHECKMULTISIG
from test_framework.util import assert_equal

CASES = os.environ.get("LOST_CASES", "r1,r2,r3,r4,r5,r6,b2").split(",")
ABUSE_TXS = int(os.environ.get("LOST_ABUSE_TXS", "340"))


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
        r = self.nodes[2].submitlostblock(ev["block"], ev["btc"])
        self.wait_until(lambda: x["hash"] in self.lost(2), timeout=60)
        self.record("r3", "two lost branches on node 2", "L (T, lost first) and X (R, lost second, shown by submitlostblock: %r)" % r)
        h = self.produce(2, ALL_KEYS, leaders=[SELLER])
        txs = self.tx_ids(self.nodes[2], h["hash"])
        assert_equal(txs[1], t_id)
        assert r_id not in txs
        self.record("r3", "outcome", "block %s at height %d assembled by node 2: tx[1] = T; R not carried (its input is T's)" % (h["hash"][:12], h["height"]))
        self.sync_tips(HONEST + [LATE])
        self.realign()


    # ---------------------------------------------------------------- r4
    def coinbase_assets(self, node, h):
        cb = node.getblock(h, 2)["tx"][0]
        out = {}
        for o in cb["vout"]:
            if o.get("value", 0) > 0:
                out[o["asset"]] = out.get(o["asset"], Decimal(0)) + Decimal(str(o["value"]))
        return out

    def lose_one(self, case, txid, maker=SELLER, leader=1):
        """`txid` (in `maker`'s mempool) certified in L with a certified child, then L lost."""
        self.btc_mine(1)
        L = self.produce(maker, ALL_KEYS, leaders=[leader], txs_must=[txid])
        self.produce(maker, ALL_KEYS, leaders=[leader])
        self.sync_tips(range(N))
        assert self.nodes[1].getblockheader(L["hash"])["poscertified"]
        since = self.lose(case, [L["hash"]])
        return L, since

    def case_r4(self):
        self.log.info("(r4) a carried fee in an asset the producer does not list: claimed by default; left unclaimed with the switch")
        a = self.nodes[SELLER]
        n1 = self.nodes[1]
        iss = a.issueasset(assetamount=Decimal("1000"), tokenamount=0, blind=False, fee_asset="bitcoin")
        self.produce(SELLER, ALL_KEYS, txs_must=[iss["txid"]])
        self.sync_tips(range(N))
        X = iss["asset"]
        rates = a.getfeeexchangerates()
        rates[X] = 100000000
        a.setfeeexchangerates(rates)
        assert X not in n1.getfeeexchangerates()
        self.record("r4", "asset", "X = %s, issued and listed by the seller only" % X[:16])
        results = {}
        for mode, node_i in (("default", 1), ("switch", 2)):
            if mode == "switch":
                self.extra = {2: ["-poslostleaveunlisted=1"]}
                self.restart_node(2, extra_args=self.node_args(2))
                for j in range(N):
                    if j != 2:
                        self.connect_nodes(2, j)
                self.sync_tips(range(N))
            t = a.sendtoaddress(address=n1.getnewaddress(), amount=Decimal("1"), assetlabel=X, fee_asset_label=X)
            fee = -Decimal(str(a.gettransaction(t)["fee"][X]))
            try:
                n1.sendrawtransaction(a.getrawtransaction(t))
                raise AssertionError("an honest mempool took a fee in an unlisted asset")
            except JSONRPCException as e:
                if mode == "default":
                    self.record("r4", "honest mempool refuses T (fee in X)", e.error["message"])
            L, since = self.lose_one("r4", t)
            h = self.produce(node_i, ALL_KEYS, leaders=[SELLER])
            node = self.nodes[node_i]
            txs = self.tx_ids(node, h["hash"])
            assert_equal(txs[1], t)
            claimed = self.coinbase_assets(node, h["hash"])
            self.sync_tips(HONEST + [LATE])
            for i in HONEST + [LATE]:
                assert self.in_chain(self.nodes[i], h["hash"])
            results[mode] = claimed.get(X)
            self.record("r4", mode, "block %s assembled by node %d: tx[1] = T (fee %s X); coinbase claims of X: %s; accepted by nodes %s" %
                        (h["hash"][:12], node_i, fee, claimed.get(X, "none"), HONEST + [LATE]))
            if mode == "default":
                assert_equal(claimed.get(X), fee)
            else:
                assert X not in claimed
                assert self.log_has(2, "left unclaimed (-poslostleaveunlisted)")
            self.realign()
        self.extra = {}
        self.restart_node(2, extra_args=self.node_args(2))
        self.mesh(range(N))
        self.sync_tips(range(N))

    # ---------------------------------------------------------------- r5
    def heavy_scripts(self, n):
        """n distinct witness scripts, each counted as 3,940 signature operations (197 CHECKMULTISIG of 20)."""
        return [CScript([i.to_bytes(4, "little"), OP_DROP, OP_0, OP_IF] + [OP_CHECKMULTISIG] * 197 + [OP_ENDIF, OP_1]) for i in range(n)]

    def sats(self, v):
        return int(round(Decimal(str(v)) * 10**8))

    def order_first(self, node, txids):
        """Make `node`'s template take `txids` first, in this order (absolute modified fees)."""
        for j, t in enumerate(txids):
            e = node.getmempoolentry(t)
            target = self.sats(e["fees"]["base"]) + (len(txids) - j + 1) * 10**9
            node.prioritisetransaction(txid=t, fee_delta=target - self.sats(e["fees"]["modified"]))

    def clear_priority(self, node, txids):
        for t in txids:
            if t in node.getrawmempool():
                e = node.getmempoolentry(t)
                d = self.sats(e["fees"]["base"]) - self.sats(e["fees"]["modified"])
                if d:
                    node.prioritisetransaction(txid=t, fee_delta=d)

    def kill_check(self, case, when, lost_hashes, i=3):
        """kill -9 of holder `i`; it comes back holding the lost blocks, on the honest tip."""
        self.kill9(i)
        held = self.lost(i)
        assert all(h in held for h in lost_hashes), held.keys()
        self.sync_tips(HONEST + [LATE])
        self.record(case, "kill -9 of node %d %s" % (i, when), "restarted holding %d lost block(s), back on the honest tip %s" %
                    (len(held), self.nodes[i].getbestblockhash()[:12]))

    def case_zflake(self):
        """PV22's r2 set-up failure: does a wallet send pick a coin fund() handed out before?"""
        a = self.nodes[SELLER]
        hits = 0
        for _ in range(8):
            c1 = self.fund(lock=False)
            c2 = self.fund(lock=False)
            vin = [(i["txid"], i["vout"]) for i in a.getrawtransaction(c2["txid"], 1)["vin"]]
            hits += (c1["txid"], c1["vout"]) in vin
        self.record("zflake", "unlocked", "in %d of 8 pairs, the second fund() spent the first's coin" % hits)
        hits = 0
        for _ in range(8):
            c1 = self.fund()
            c2 = self.fund()
            vin = [(i["txid"], i["vout"]) for i in a.getrawtransaction(c2["txid"], 1)["vin"]]
            hits += (c1["txid"], c1["vout"]) in vin
        self.record("zflake", "locked", "in %d of 8 pairs, the second fund() spent the first's coin" % hits)
        assert_equal(hits, 0)

    def case_r5(self):
        self.log.info("(r5) more due transactions than one block holds (signature operations): the longest prefix, and nothing else")
        a = self.nodes[SELLER]
        n1 = self.nodes[1]
        n = 30
        scripts = self.heavy_scripts(n)
        addrs = [a.decodescript(s.hex())["segwit"]["address"] for s in scripts]
        coin = self.fund(Decimal("2"))
        outs = [{ad: Decimal("0.01")} for ad in addrs]
        outs.append({a.getnewaddress(): Decimal("2") - Decimal("0.3") - Decimal("0.001")})
        outs.append({"fee": Decimal("0.001")})
        raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}], outs)
        ftx = a.sendrawtransaction(a.signrawtransactionwithwallet(raw)["hex"])
        self.produce(SELLER, ALL_KEYS, txs_must=[ftx])
        self.sync_tips(range(N))
        fdec = a.getrawtransaction(ftx, 1)
        heavy = []
        for i, s in enumerate(scripts):
            vout = [o["n"] for o in fdec["vout"] if o["scriptPubKey"].get("address") == addrs[i]][0]
            r = a.createrawtransaction([{"txid": ftx, "vout": vout}], [{n1.getnewaddress(): Decimal("0.009")}, {"fee": Decimal("0.001")}])
            tx = tx_from_hex(r)
            while len(tx.wit.vtxinwit) < len(tx.vin):
                tx.wit.vtxinwit.append(CTxInWitness())
            tx.wit.vtxinwit[0].scriptWitness.stack = [bytes(s)]
            hx = tx.serialize().hex()
            heavy.append(n1.sendrawtransaction(hx))
        # An ordinary transaction, made now, sent once the replay starts.
        coin2 = self.fund()
        o_hex = self.spend(coin2, n1.getnewaddress(), Decimal("0.0001"))
        o_id = tx_from_hex(o_hex).rehash()
        self.btc_mine(1)
        Ls = []
        while any(t in n1.getrawmempool() for t in heavy):
            Ls.append(self.produce(1, ALL_KEYS))
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        order = []
        for L in Ls:
            order += [t for t in self.tx_ids(n1, L["hash"])[1:] if t in heavy]
        assert_equal(len(order), n)
        sig = [n1.getrawtransaction(t, 1) for t in order[:1]]
        self.record("r5", "lost branch", "%d blocks %s holding %d transactions of 3,940 signature operations each (%s per block); one block holds 80,000" %
                    (len(Ls), [L["height"] for L in Ls], n, [len(self.tx_ids(n1, L["hash"])) - 1 for L in Ls]))
        since = self.lose("r5", [L["hash"] for L in Ls])
        # kill -9 of a holder between the disconnect and the first block.
        self.kill_check("r5", "between the disconnect and the first block", [L["hash"] for L in Ls])
        since = [self.log_size(i) for i in range(N)]
        # A leader that carries none of them.
        x = self.seller_block(ALL_KEYS, exclude=order)
        self.refused_by("r5", HONEST, x["hash"], since, "bad-lost-replay")
        line = self.log_has(1, "Lost blocks: refusing block %s" % x["hash"], since[1])[0]
        k = int(line.split(" in the prefix")[0].rsplit("(", 1)[1])
        self.record("r5", "the floor", "%d of %d due fit the signature-operation limit (80,000 less 4,000 held back): k = %d" % (k, n, k))
        assert k < n
        # A leader that carries a shorter prefix, in lost order.
        self.realign()
        since = [self.log_size(i) for i in range(N)]
        self.order_first(a, order[:k - 1])
        try:
            y = self.seller_block(ALL_KEYS, exclude=order[k - 1:])
        finally:
            self.clear_priority(a, order)
        assert_equal(self.tx_ids(a, y["hash"])[1:k], order[:k - 1])
        self.refused_by("r5", HONEST, y["hash"], since, "(%d of %d in place" % (k - 1, k))
        # The full prefix and an ordinary transaction while it is cut.
        self.realign()
        since = [self.log_size(i) for i in range(N)]
        a.sendrawtransaction(o_hex)
        self.order_first(a, order[:k])
        try:
            z = self.seller_block(ALL_KEYS, exclude=order[k:], include=[o_id])
        finally:
            self.clear_priority(a, order)
        assert_equal(self.tx_ids(a, z["hash"])[1:k + 1], order[:k])
        self.refused_by("r5", HONEST, z["hash"], since, "carries other transactions while due lost transactions did not fit")
        self.realign()
        # The honest assembler: the prefix alone, then the rest, then ordinary transactions.
        self.kill_check("r5", "after three refused blocks", [L["hash"] for L in Ls])
        n1.sendrawtransaction(o_hex)
        h1 = self.produce(1, ALL_KEYS, leaders=[SELLER])
        t1 = self.tx_ids(n1, h1["hash"])
        assert_equal(t1[1:], order[:k])
        self.sync_tips(HONEST + [LATE])
        # kill -9 between the two blocks of the replay: the holder still refuses a block without the rest.
        self.kill_check("r5", "between F+1 and F+2", [L["hash"] for L in Ls])
        since = [self.log_size(i) for i in range(N)]
        w = self.seller_block(ALL_KEYS, exclude=order[k:])
        self.refused_by("r5", HONEST, w["hash"], since, "(0 of %d in place" % (n - k))
        self.realign()
        h2 = self.produce(1, ALL_KEYS, leaders=[SELLER])
        t2 = self.tx_ids(n1, h2["hash"])
        assert_equal(t2[1:1 + n - k], order[k:])
        self.sync_tips(HONEST + [LATE])
        self.record("r5", "outcome", "F+1 (%s): the %d of the prefix and nothing else; F+2 (%s): the other %d, then %d ordinary transaction(s)%s" %
                    (h1["hash"][:12], k, h2["hash"][:12], n - k, len(t2) - 1 - (n - k), " (the waiting one among them)" if o_id in t2 else ""))
        assert o_id in t2
        self.realign()

    # ---------------------------------------------------------------- r6
    def chain_txs(self, node, from_height):
        """txid -> [heights] over the active chain from `from_height`."""
        seen = {}
        for hh in range(from_height, node.getblockcount() + 1):
            for t in node.getblock(node.getblockhash(hh))["tx"][1:]:
                seen.setdefault(t, []).append(hh)
        return seen

    def case_r6(self):
        self.log.info("(r6) the seller fills its own certified block with fees in its own asset; there, back and there again")
        a = self.nodes[SELLER]
        n1 = self.nodes[1]
        m = ABUSE_TXS
        iss = a.issueasset(assetamount=Decimal("100000"), tokenamount=0, blind=False, fee_asset="bitcoin")
        self.produce(SELLER, ALL_KEYS, txs_must=[iss["txid"]])
        self.sync_tips(range(N))
        X = iss["asset"]
        rates = a.getfeeexchangerates()
        rates[X] = 100000000
        a.setfeeexchangerates(rates)
        # An ordinary transaction, made now, sent to the honest nodes once the replay starts.
        coin = self.fund()
        o_hex = self.spend(coin, n1.getnewaddress(), Decimal("0.0001"))
        o_id = tx_from_hex(o_hex).rehash()
        # m coins of X, one per abusive transaction.
        utxo = [u for u in a.listunspent() if u["asset"] == X][0]
        addrs = [a.getnewaddress() for _ in range(m)]
        outs = [{ad: Decimal("1"), "asset": X} for ad in addrs]
        chg = a.getnewaddress()
        outs.append({chg: Decimal(str(utxo["amount"])) - m - Decimal("0.01"), "asset": X})
        outs.append({"fee": Decimal("0.01"), "fee_asset": X})
        assets = set(addrs)
        raw = a.createrawtransaction([{"txid": utxo["txid"], "vout": utxo["vout"]}], outs)
        fan = a.sendrawtransaction(a.signrawtransactionwithwallet(raw)["hex"])
        self.produce(SELLER, ALL_KEYS, txs_must=[fan])
        self.sync_tips(range(N))
        fdec = a.getrawtransaction(fan, 1)
        dest = n1.getnewaddress()
        abuse = []
        for o in fdec["vout"]:
            if o["scriptPubKey"].get("address") not in assets:
                continue
            r = a.createrawtransaction([{"txid": fan, "vout": o["n"]}], [{dest: Decimal("0.99999"), "asset": X}, {"fee": Decimal("0.00001"), "fee_asset": X}])
            abuse.append(a.sendrawtransaction(a.signrawtransactionwithwallet(r)["hex"]))
        assert_equal(len(abuse), m)
        self.btc_mine(1)
        f_height = n1.getblockcount()
        L = self.produce(SELLER, ALL_KEYS, leaders=[SELLER])
        in_l = [t for t in L["block"]["tx"][1:] if t in abuse]
        Lw = L["block"]["weight"]
        assert_equal(len(in_l), m)
        self.produce(SELLER, ALL_KEYS, leaders=[SELLER])
        self.sync_tips(range(N))
        assert n1.getblockheader(L["hash"])["poscertified"]
        self.record("r6", "L", "the seller's certified block at height %d: %d of its own transactions paying fees in X, weight %d of %d" %
                    (L["height"], len(in_l), Lw, 400000))
        since = self.lose("r6", [L["hash"]])
        n1.sendrawtransaction(o_hex)
        hs = []
        while True:
            h = self.produce(1, ALL_KEYS, leaders=[1, 2, 3, 4])
            txs = self.tx_ids(n1, h["hash"])
            carried = [t for t in txs[1:] if t in abuse]
            hs.append((h["height"], len(carried), o_id in txs, n1.getblock(h["hash"])["weight"]))
            if o_id in txs or len(hs) > 6:
                break
        self.sync_tips(HONEST + [LATE])
        seen = self.chain_txs(n1, f_height + 1)
        assert all(len(seen.get(t, [])) == 1 for t in in_l)
        self.record("r6", "there", "honest blocks (height, carried, ordinary tx in it, weight): %s; each of the %d mined once" % (hs, len(in_l)))
        assert o_id in self.tx_ids(n1, self.nodes[1].getblockhash(hs[-1][0]))
        # Back: Bitcoin returns to L's branch.
        b = self.btc()
        orig, new = self.btc_orig_first, self.btc_new
        b.invalidateblock(new[0])
        b.reconsiderblock(orig)
        self.btc_mine(3)
        self.wait_until(lambda: all(self.in_chain(self.nodes[i], L["hash"]) for i in HONEST), timeout=120)
        h_back = self.produce(1, ALL_KEYS, leaders=[1, 2, 3, 4])
        self.sync_tips(HONEST)
        seen = self.chain_txs(n1, f_height + 1)
        twice = [t for t in in_l + [o_id] if len(seen.get(t, [])) != 1]
        self.record("r6", "back", "L's branch active again on every honest node; next block %d carries %d abusive and the ordinary tx: %s; each of the %d and the ordinary tx in the chain once: %s" %
                    (h_back["height"], len([t for t in self.tx_ids(n1, h_back["hash"]) if t in abuse]), o_id in seen, len(in_l), not twice))
        assert not twice
        # There again: a third Bitcoin branch; both earlier branches are lost.
        b.invalidateblock(orig)
        self.btc_mine(4)
        self.wait_until(lambda: all(not self.in_chain(self.nodes[i], L["hash"]) for i in HONEST), timeout=120)
        self.wait_lost(L["hash"], HONEST)
        hs2 = []
        while True:
            h = self.produce(1, ALL_KEYS, leaders=[1, 2, 3, 4])
            txs = self.tx_ids(n1, h["hash"])
            hs2.append((h["height"], len([t for t in txs[1:] if t in abuse]), o_id in txs, n1.getblock(h["hash"])["weight"]))
            if len(hs2) >= 3:
                break
        self.sync_tips(HONEST + [LATE])
        seen = self.chain_txs(n1, f_height + 1)
        counts = {}
        for t in in_l + [o_id]:
            counts[len(seen.get(t, []))] = counts.get(len(seen.get(t, [])), 0) + 1
        self.record("r6", "there again", "honest blocks (height, carried, ordinary tx, weight): %s; occurrences in the chain of the %d abusive and the ordinary tx: %s" %
                    (hs2, len(in_l), counts))
        assert_equal(counts, {1: len(in_l) + 1})
        self.realign()

    # ----------------------------------------------------------- fab
    def case_fab(self):
        self.log.info("(fab) a fabricated lost block on a deep fork point: at the tip and at a syncing node")
        a = self.nodes[SELLER]
        n1, n2 = self.nodes[1], self.nodes[2]
        c = self.fund()
        p_hash = n1.getbestblockhash()
        a_p = self.tip(n1)["anchorheight"]
        # The honest chain moves on, W + 2 Bitcoin blocks later.
        self.btc_mine(W + 2)
        for _ in range(2):
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        top = n1.getblockhash(n1.getblockheader(p_hash)["height"] + 1)
        # The fabricator, isolated, builds L' on P, anchored on a fresh Bitcoin block B, with Tf spending c.
        b_hash = self.btc_mine(1)[0]
        self.disconnect_all(SELLER)
        a.setnetworkactive(False)
        a.invalidateblock(top)
        self.wait_until(lambda: a.getbestblockhash() == p_hash, timeout=30)
        tf_hex = self.spend(c, a.getnewaddress(), Decimal("0.0001"))
        tf = a.sendrawtransaction(tf_hex)
        fab = self.produce(SELLER, ALL_KEYS, leaders=[SELLER], txs_must=[tf])
        self.record("fab", "L'", "%s on P (height %d, anchor %d), anchor %d, certified by the current keys, carries Tf spending c" %
                    (fab["hash"][:12], fab["height"] - 1, a_p, fab["block"]["anchorheight"]))
        # Bitcoin replaces B; the fabricator goes back to the honest chain; L' is in its store, lost.
        self.btc().invalidateblock(b_hash)
        self.btc_mine(2)
        a.reconsiderblock(top)
        self.wait_until(lambda: a.getbestblockhash() == n1.getbestblockhash(), timeout=60)
        self.wait_until(lambda: fab["hash"] in [b["hash"] for b in a.getlostblocks()["blocks"]], timeout=30)
        ev = a.getlostblock(fab["hash"])
        # A syncing node: node 5 wiped and started with no peer, shown L' first.
        self.stop_node(LATE)
        shutil.rmtree(os.path.join(self.nodes[LATE].datadir, "elementsregtest", "blocks"))
        shutil.rmtree(os.path.join(self.nodes[LATE].datadir, "elementsregtest", "chainstate"))
        lp = os.path.join(self.nodes[LATE].datadir, "elementsregtest", "lostblocks.dat")
        if os.path.exists(lp):
            os.remove(lp)
        self.start_node(LATE, extra_args=self.node_args(LATE))
        late = self.nodes[LATE]
        self.record("fab", "syncing node (at genesis, IBD %s)" % late.getblockchaininfo()["initialblockdownload"],
                    self.sub(late, ev["block"], ev["btc"]))
        # At the tip.
        r = self.sub(n2, ev["block"], ev["btc"])
        time.sleep(3)
        self.record("fab", "node 2 at the tip", "%s; L' held lost there: %s" % (r, fab["hash"] in self.lost(2)))
        a.setnetworkactive(True)
        self.mesh(range(N))
        self.sync_tips(range(N))
        self.record("fab", "node 5 synced", "tip %s; holds L': %s" % (late.getbestblockhash()[:12], fab["hash"] in self.lost(LATE)))
        # Does L' freeze c? The owner spends c otherwise.
        r_hex = self.spend(c, n1.getnewaddress(), Decimal("0.0002"))
        r_id = tx_from_hex(r_hex).rehash()
        try:
            n2.sendrawtransaction(r_hex)
            self.record("fab", "node 2's mempool takes c's other spend", "yes")
        except JSONRPCException as e:
            self.record("fab", "node 2's mempool takes c's other spend", "no: " + e.error["message"])
        a.sendrawtransaction(r_hex)
        since = [self.log_size(i) for i in range(N)]
        h = self.produce(SELLER, ALL_KEYS, leaders=[1], txs_must=[r_id])
        time.sleep(3)
        on = [i for i in HONEST if self.in_chain(self.nodes[i], h["hash"])]
        self.record("fab", "a block spending c otherwise", "taken by honest nodes %s" % on)
        refusal = self.log_has(2, "Lost blocks: refusing block %s" % h["hash"], since[2])
        if refusal:
            self.record("fab", "node 2 refuses it", refusal[0].split("[LostCheckBlock] ", 1)[-1][:300])
        assert "fork-too-deep" in r, r
        assert_equal(on, HONEST)
        self.realign()

    # ---------------------------------------------------------- deep
    def rss_kb(self, i):
        with open("/proc/%d/status" % self.nodes[i].process.pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        return -1

    def deep_build(self, case, depth, per, per_btc):
        """Full lost blocks: `depth` Bitcoin blocks, `per_btc` Sequentia blocks of `per` transactions on each,
        made by generateposblock. Returns them, their transactions, an ordinary transaction O (unsent)
        and the fork point's height."""
        a = self.nodes[SELLER]
        n1 = self.nodes[1]
        nblocks = depth * per_btc
        total = nblocks * per
        # Coins: fan-out transactions of `per` outputs each, mined first.
        fans = []
        coins = [self.fund(Decimal("10")) for _ in range(nblocks)]
        o_coin = self.fund()
        for coin in coins:
            addrs = [a.getnewaddress() for _ in range(per)]
            outs = [{ad: Decimal("0.01")} for ad in addrs]
            outs.append({a.getnewaddress(): Decimal("10") - Decimal("0.01") * per - Decimal("0.001")})
            outs.append({"fee": Decimal("0.001")})
            raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}], outs)
            fans.append((a.sendrawtransaction(a.signrawtransactionwithwallet(raw)["hex"]), set(addrs)))
        while any(f in a.getrawmempool() for f, _ in fans):
            self.produce(SELLER, ALL_KEYS)
        self.sync_tips(range(N))
        o_hex = self.spend(o_coin, n1.getnewaddress(), Decimal("0.0001"))
        o_id = tx_from_hex(o_hex).rehash()
        dest = n1.getnewaddress()
        t0 = time.time()
        batches = []
        for f, addrs in fans:
            dec = a.getrawtransaction(f, 1)
            batch = []
            for o in dec["vout"]:
                if o["scriptPubKey"].get("address") in addrs:
                    r = a.createrawtransaction([{"txid": f, "vout": o["n"]}], [{dest: Decimal("0.0099")}, {"fee": Decimal("0.0001")}])
                    batch.append(a.signrawtransactionwithwallet(r)["hex"])
            batches.append(batch)
        self.log.info("  %d transactions signed in %.0f s", total, time.time() - t0)
        f_height = n1.getblockcount()
        Ls = []
        for d in range(depth):
            self.btc_mine(1)
            for k in range(per_btc):
                ids = [n1.sendrawtransaction(hx) for hx in batches[d * per_btc + k]]
                Ls.append(self.produce(1, ALL_KEYS, leaders=[1, 2, 3, 4], txs_must=ids[:1]))
        self.sync_tips(range(N), timeout=300)
        lost_txids = []
        weights = []
        for L in Ls:
            blk = n1.getblock(L["hash"])
            lost_txids += blk["tx"][1:]
            weights.append(blk["weight"])
        self.record(case, "lost branch", "%d blocks at heights %d..%d, %d transactions, weights %d..%d (limit 400,000)" %
                    (len(Ls), Ls[0]["height"], Ls[-1]["height"], len(lost_txids), min(weights), max(weights)))
        return Ls, lost_txids, o_hex, o_id, f_height

    def case_deep(self):
        depth = int(os.environ.get("LOST_DEPTH", "1"))
        per = int(os.environ.get("LOST_DEEP_TXS", "480"))
        per_btc = 10
        case = "deep%d" % depth
        self.log.info("(%s) %d stale Bitcoin block(s), %d full Sequentia blocks each" % (case, depth, per_btc))
        n1 = self.nodes[1]
        Ls, lost_txids, o_hex, o_id, f_height = self.deep_build(case, depth, per, per_btc)
        nblocks = depth * per_btc
        rss0 = self.rss_kb(1)
        since = [self.log_size(i) for i in range(N)]
        t0 = time.time()
        self.bitcoin_reorg(depth)
        self.wait_until(lambda: all(self.nodes[i].getblockcount() <= f_height for i in HONEST), timeout=300)
        t_leave = time.time() - t0
        self.wait_lost(Ls[-1]["hash"], HONEST, timeout=300)
        t_hold = time.time() - t0
        lb = os.path.getsize(os.path.join(n1.datadir, "elementsregtest", "lostblocks.dat"))
        self.record(case, "reorg", "Bitcoin %d deep: honest nodes left the branch in %.1f s and held it lost in %.1f s; lostblocks.dat %d bytes; node 1 RSS %d -> %d kB" %
                    (depth, t_leave, t_hold, lb, rss0, self.rss_kb(1)))
        n1.sendrawtransaction(o_hex)
        hs = []
        left = set(lost_txids)
        t0 = time.time()
        while True:
            tp = time.time()
            h = self.produce(1, ALL_KEYS, leaders=[1, 2, 3, 4])
            tp = time.time() - tp
            txs = self.tx_ids(n1, h["hash"])
            carried = [t for t in txs[1:] if t in left]
            left -= set(carried)
            hs.append((h["height"], len(carried), o_id in txs, round(tp, 1)))
            if o_id in txs or len(hs) > nblocks + 5:
                break
        self.sync_tips(HONEST + [LATE], timeout=300)
        lines = self.log_has(1, "plan computed on", since[1])
        micros = [int(l.split("classified in ")[1].split(" us")[0]) for l in lines]
        recs = self.log_has(1, "records", since[1])
        self.record(case, "replay", "%d blocks (height, carried, ordinary tx in it, seconds to produce): %s; left uncarried: %d; ordinary tx waited %d block(s), %.0f s" %
                    (len(hs), hs, len(left), len(hs) - 1, time.time() - t0))
        self.record(case, "classifier", "%d plans computed on node 1, %d..%d us (median %d); node 1 RSS %d kB; record lines: %d" %
                    (len(micros), min(micros), max(micros), sorted(micros)[len(micros) // 2], self.rss_kb(1), len(recs)))
        assert not left
        self.realign()

    # ------------------------------------------------------------ b1, b2
    def b_setup(self, case, honest_apart=False):
        """T certified in L while node 5 is offline; Bitcoin replaces L's anchor; a dishonest quorum
        certifies X (R) on the fork point. Node 5 then starts with the seller as its only peer."""
        a = self.nodes[SELLER]
        self.stop_node(LATE)
        online = [0, 1, 2, 3, 4]
        st = self.gap(case, online=online)
        if honest_apart:
            # The honest nodes see nothing of X's chain until the case delivers it.
            self.disconnect_all(SELLER)
        x = self.respend_block(case, st, ALL_KEYS)
        since = [self.log_size(i) for i in online]
        self.start_node(LATE, extra_args=self.node_args(LATE))
        self.connect_nodes(LATE, SELLER)
        self.wait_until(lambda: self.nodes[LATE].getbestblockhash() == x["hash"], timeout=60)
        self.record(case, "node 5 (never saw L) took X from the seller", "tip %s, certified" % x["hash"][:16])
        return st, x

    def case_b1(self):
        self.log.info("(b1) a quorum certified X once; the honest chain goes on certified; node 5 on X moves")
        st, x = self.b_setup("b1")
        late = self.nodes[LATE]
        for j in HONEST:
            self.connect_nodes(LATE, j)
        self.wait_until(lambda: st["L"] in self.lost(LATE), timeout=60)
        time.sleep(3)
        assert_equal(late.getbestblockhash(), x["hash"])
        self.record("b1", "node 5 shown L", "holds L lost, stays on X (forward-only): %s" % (late.getbestblockhash() == x["hash"]))
        keys = [1, 2, 3, 4, 5]
        hs = []
        for _ in range(4):
            hs.append(self.produce(1, keys, leaders=[1, 2, 3, 4]))
        assert_equal(self.nodes[1].getblock(hs[0]["hash"])["tx"][1], st["t_id"])
        self.sync_tips(HONEST)
        t0 = time.time()
        self.record("b1", "honest chain", "4 certified blocks on the fork point (T first), tip height %d" % hs[-1]["height"])
        self.wait_until(lambda: late.getbestblockhash() == self.nodes[1].getbestblockhash(), timeout=900)
        line = self.log_has(LATE, "releasing local finality")
        self.record("b1", "outcome", "node 5 moved to the honest chain %.0f s after it was 4 certified blocks ahead; %s" %
                    (time.time() - t0, (line[0].split("] ", 3)[-1][:200] if line else "no release line")))
        self.mesh(range(N))
        self.realign()

    def case_b2(self):
        self.log.info("(b2) the quorum stays dishonest: holders follow X's chain at the waiver depth, not before")
        st, x = self.b_setup("b2")
        a = self.nodes[SELLER]
        for j in HONEST:
            self.connect_nodes(LATE, j)
        time.sleep(3)
        assert all(not self.in_chain(self.nodes[i], x["hash"]) for i in HONEST)
        # Relayed in chain order: X is refused first, then what is built on it.
        for k in range(1, WAIVER):
            self.produce(SELLER, ALL_KEYS, leaders=[SELLER])
            time.sleep(3)
            assert all(not self.in_chain(self.nodes[i], x["hash"]) for i in HONEST), "followed at %d of %d" % (k, WAIVER)
        self.record("b2", "below the depth", "%d certified block(s) above X: every honest node still refuses it" % (WAIVER - 1))
        t0 = time.time()
        top = self.produce(SELLER, ALL_KEYS, leaders=[SELLER])
        self.wait_until(lambda: all(self.nodes[i].getbestblockhash() == a.getbestblockhash() for i in HONEST), timeout=90)
        self.record("b2", "outcome", "%d certified block(s) above X: every honest node took X's chain in %.1f s, tip height %d; R mined: %s" %
                    (WAIVER, time.time() - t0, top["height"], self.block_of(st["r_id"], self.nodes[1]) is not None))
        self.mesh(range(N))

    def case_b2o(self):
        self.log.info("(b2o) the waiver does not depend on delivery order: refused block first, and child headers first")
        st, x = self.b_setup("b2o", honest_apart=True)
        a = self.nodes[SELLER]
        chain = [x] + [self.produce(SELLER, ALL_KEYS, leaders=[SELLER]) for _ in range(WAIVER)]
        raws = [a.getblock(b["hash"], 0) for b in chain]
        hdrs = [a.getblockheader(b["hash"], False) for b in chain]
        top = chain[-1]["hash"]
        n1, n2 = self.nodes[1], self.nodes[2]
        for i in HONEST:
            self.disconnect_all(i)
        def finish(n):
            # What was refused before the waiver is offered again, as a peer would.
            def done():
                for raw in raws:
                    n.submitblock(raw)
                return n.getbestblockhash() == top
            self.wait_until(done, timeout=60)
        # The refused block first, then its children in chain order.
        n1.submitblock(raws[0])
        assert not self.in_chain(n1, x["hash"])
        for raw in raws[1:-1]:
            n1.submitblock(raw)
        time.sleep(3)
        assert not self.in_chain(n1, x["hash"]), "followed below the depth (refused block first)"
        n1.submitblock(raws[-1])
        finish(n1)
        self.record("b2o", "refused block first", "node 1 refuses X below the depth and follows at %d certified block(s)" % WAIVER)
        # The children's headers first.
        def offer_header(h):
            try:
                n2.submitheader(h)
            except JSONRPCException as e:
                assert "lost-respend" in e.error["message"], e.error
        for h in hdrs[:-1]:
            offer_header(h)
        n2.submitblock(raws[0])
        time.sleep(3)
        assert not self.in_chain(n2, x["hash"]), "followed below the depth (headers first)"
        offer_header(hdrs[-1])
        finish(n2)
        self.record("b2o", "child headers first", "node 2 refuses X below the depth and follows at %d certified block(s)" % WAIVER)
        self.mesh(range(N))

    # ---------------------------------------------------------------- hc
    def wallet_view(self, w, txid):
        t = w.gettransaction(txid)
        return {k: t.get(k) for k in ("confirmations", "trusted", "walletconflicts", "bip125-replaceable") if k in t}

    def case_hc(self):
        self.log.info("(hc) honest conflicts: a holder's own wallet bumps or respends its lost transaction")
        a, n1 = self.nodes[SELLER], self.nodes[1]
        w = n1.get_wallet_rpc("w")
        got = a.sendtoaddress(address=w.getnewaddress(), amount=Decimal("2"), fee_asset_label="bitcoin")
        self.produce(SELLER, ALL_KEYS, txs_must=[got])
        self.sync_tips(range(N))
        t_id = w.sendtoaddress(address=self.nodes[2].getnewaddress(), amount=Decimal("1"), replaceable=True, fee_asset_label="bitcoin")
        self.btc_mine(1)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        self.record("hc", "T", "node 1's wallet tx %s in certified L at height %d: %s" % (t_id[:16], L["height"], self.wallet_view(w, t_id)))
        t0 = time.time()
        self.lose("hc", [L["hash"]])
        self.record("hc", "wallet after the loss", "%s; in node 1's mempool: %s" % (self.wallet_view(w, t_id), t_id in n1.getrawmempool()))
        time.sleep(float(os.environ.get("LOST_HC_WAIT", "0")))
        self.record("hc", "node 1's bound outpoints when the wallet bumps", n1.getlostblocks()["boundoutpoints"])
        try:
            r = w.bumpfee(t_id)
            self.record("hc", "bumpfee", "returned %s; the bump in node 1's mempool: %s; T in it: %s; the wallet shows the bump: %s" %
                        (r["txid"][:16], r["txid"] in n1.getrawmempool(), t_id in n1.getrawmempool(), self.wallet_view(w, r["txid"])))
        except JSONRPCException as e:
            self.record("hc", "bumpfee", "refused: %s" % e.error["message"])
        tx = w.gettransaction(t_id, True, True)["decoded"]
        vin = [{"txid": i["txid"], "vout": i["vout"]} for i in tx["vin"]]
        total = sum(Decimal(str(w.gettxout(i["txid"], i["vout"], False)["value"])) if w.gettxout(i["txid"], i["vout"], False) else Decimal(0) for i in vin)
        self.record("hc", "T's inputs unspent on the chain now", "%d of %d, %s" % (sum(1 for i in vin if w.gettxout(i["txid"], i["vout"], False)), len(vin), total))
        raw = w.createrawtransaction(vin, [{w.getnewaddress(): total - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        rs = w.signrawtransactionwithwallet(raw)["hex"]
        try:
            w.sendrawtransaction(rs)
            self.record("hc", "respend", "accepted")
        except JSONRPCException as e:
            self.record("hc", "respend", "refused: %s" % e.error["message"])
        h = self.produce(1, ALL_KEYS)
        self.sync_tips(HONEST)
        self.record("hc", "first block after the loss", "height %d, tx[1] is T: %s; %.1f s after the reorg; wallet: %s" %
                    (h["height"], h["block"]["tx"][1:2] == [t_id], time.time() - t0, self.wallet_view(w, t_id)))
        try:
            w.bumpfee(t_id)
        except JSONRPCException as e:
            self.record("hc", "bumpfee once T is back", "refused: %s" % e.error["message"])
        assert h["block"]["tx"][1] == t_id
        self.realign()

    # ---------------------------------------------------------------- run
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
