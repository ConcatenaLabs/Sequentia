#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The rule for the transactions of a lost block is enforced from its height,
and not before (doc 03, "Activation").

The network of feature_pos_lost_blocks.py, with -con_lost_blocks_height=H on
every node. Twice, a seller's transaction T is certified in a block L that a
Bitcoin reorganization then replaces, and a block is forced at L's height on an
honest holder (a block it produced, rewritten, certified again with
recertifyposblock and submitted; every node runs -par=1):

  below  L's height is H - 1: the rule does not apply there, so the holder takes
         a block that respends T's inputs instead of carrying T and that carries a
         malformed record (an ordinary OP_RETURN output below H); its mempool
         judges the respend by the ordinary rules;
  at     L's height is H: the holder refuses a malformed record (bad-lost-record)
         and the respend (bad-txns-lost-respend), its mempool refuses the respend
         (txn-lost-conflict), and the block it builds carries T first.

getdeploymentinfo reports lost_blocks not active below H, active from the block
before it, and getlostblocks reports the same.
"""

import time
from decimal import Decimal

import test_framework.util
from feature_pos_lost_blocks import ALL_KEYS, HONEST, N, SELLER
from feature_pos_lost_swap import LostSwapTest
from pos_lost_attack_util import block_from_hex
from test_framework.messages import CTxOut, CTxOutWitness, hash256, tx_from_hex
from test_framework.script import CScript
from test_framework.util import assert_equal

H = 30
COMMITMENT = bytes.fromhex("6a24aa21a9ed")
EVIDENCE_MSG = "poslost"


class LostActivationTest(LostSwapTest):
    def set_test_params(self):
        super().set_test_params()

    def node_args(self, i):
        return super().node_args(i) + ["-con_lost_blocks_height=%d" % H]

    def produce_by(self):
        """A block on node 1 from its mempool, certified by every staker; returns it and its leader."""
        n1 = self.nodes[1]
        last = None
        for _ in range(80):
            for k in ALL_KEYS:
                try:
                    return n1.generateposblock(self.stakers[k][0], [self.stakers[j][0] for j in ALL_KEYS if j != k]), k
                except Exception as e:  # noqa: B902 -- the slot of this leader is not open yet
                    last = e
            time.sleep(0.25)
        raise AssertionError("no block: %s" % last)

    def forced_alone(self, height, **kw):
        """A block forced in at `height` by node 1, rewritten from one node 1 makes
        while cut off from its peers, so that no other node ever holds the block it
        replaces. A certified block is final against a sibling: a node that took both
        would keep the first, and a node rebuilt from disk could take it again."""
        n1 = self.nodes[1]
        self.disconnect_all(1)
        base, leader = self.produce_by()
        assert_equal(base["height"], height)
        self.template_txs = n1.getblock(base["hash"])["tx"]
        raw = n1.getblock(base["hash"], 0)
        n1.invalidateblock(base["hash"])
        rc = self.forced(raw, leader, **kw)
        res = n1.submitblock(rc["hex"])
        assert res is None, res
        assert_equal(n1.getbestblockhash(), rc["hash"])
        others = [j for j in range(N) if j != 1]
        for j in others:
            self.connect_nodes(1, j)
        self.sync_tips(range(N))
        assert_equal(self.nodes[0].getbestblockhash(), rc["hash"])
        for j in others:
            assert base["hash"] not in [t["hash"] for t in self.nodes[j].getchaintips()], j
        return rc

    def forced(self, base, leader, drop=(), add=(), record=None):
        """`base` (a block node 1 produced, now invalidated) without the transactions spending what
        `drop` spends, with `add` and with a coinbase record output `record`, certified again."""
        n1 = self.nodes[1]
        blk = block_from_hex(base)
        cb = blk.vtx[0]
        test_framework.util.node_fastmerkle = n1
        nonce = cb.wit.vtxinwit[0].scriptWitness.stack[0]

        def commitment(txs):
            wroot = bytes.fromhex(test_framework.util.calcfastmerkleroot([t.calc_witness_hash() for t in txs]))[::-1]
            return COMMITMENT + hash256(wroot + nonce)
        k = [i for i, o in enumerate(cb.vout) if bytes(o.scriptPubKey).startswith(COMMITMENT)][-1]
        assert_equal(commitment(blk.vtx), bytes(cb.vout[k].scriptPubKey))
        added = [tx_from_hex(h) for h in add]
        gone = {(i.prevout.hash, i.prevout.n) for t in [tx_from_hex(h) for h in drop] + added for i in t.vin}
        blk.vtx = [cb] + [t for t in blk.vtx[1:] if not any((i.prevout.hash, i.prevout.n) in gone for i in t.vin)] + added
        if record is not None:
            cb.vout.insert(k, CTxOut(0, b"\x6a\x04SQLR" + bytes(CScript([record])), cb.vout[0].nAsset))
            if cb.wit.vtxoutwit:
                cb.wit.vtxoutwit.insert(k, CTxOutWitness())
            k += 1
        cb.vout[k].scriptPubKey = commitment(blk.vtx)
        for t in blk.vtx:
            t.sha256 = None
            t.rehash()
        blk.hashMerkleRoot = blk.calc_merkle_root()
        rc = n1.recertifyposblock(blk.serialize().hex(), self.stakers[leader][0],
                                  [self.stakers[j][0] for j in ALL_KEYS if j != leader])
        return rc

    @staticmethod
    def signed(node, raw):
        res = node.signrawtransactionwithwallet(raw)
        assert res["complete"], res
        return res["hex"]

    def lost_T(self, case, coin, replaceable=False):
        """T, spending `coin`, certified in L at the next height, then lost. It signals replacement only if asked."""
        n1, b, a = self.nodes[1], self.btc(), self.nodes[SELLER]
        raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"], "sequence": 0xfffffffd if replaceable else 0xffffffff}],
                                     [{a.getnewaddress(): coin["amount"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        t_hex = self.signed(a, raw)
        t_id = tx_from_hex(t_hex).rehash()
        b.generatetoaddress(1, self.bbuyer.getnewaddress())
        time.sleep(2)  # the anchor follows Bitcoin's new tip (-anchorpollinterval=1)
        n1.sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        self.btc_mine(1)
        self.bitcoin_reorg(2)
        self.wait_lost(L["hash"], HONEST)
        assert_equal(n1.getblockcount(), L["height"] - 1)
        # The seller's wallet signs against its own node's chain and mempool: until
        # that node has left L too, T's coin is spent there and the respend would
        # come back unsigned.
        self.wait_until(lambda: a.getblockheader(L["hash"])["confirmations"] < 0, timeout=60)
        raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],
                                     [{a.getnewaddress(): coin["amount"] - Decimal("0.002")}, {"fee": Decimal("0.002")}])
        r_hex = self.signed(a, raw)
        self.record(case, "lost", "T %s certified in L at height %d (anchor %d), lost on %s" %
                    (t_id[:16], L["height"], L["block"]["anchorheight"], HONEST))
        return t_hex, t_id, r_hex, L["height"]

    def dep(self):
        return self.nodes[1].getdeploymentinfo()["deployments"]["lost_blocks"]

    def active(self, expect):
        """getdeploymentinfo and getlostblocks agree on whether the rule applies to the next block."""
        n1 = self.nodes[1]
        assert_equal(self.dep()["active"], expect)
        assert_equal(n1.getlostblocks()["active"], expect)
        self.record("active", "tip %d" % n1.getblockcount(), "lost_blocks.active and getlostblocks.active: %s" % expect)

    def run_test(self):
        try:
            self.body()
        finally:
            self.stop_bitcoind()

    def body(self):
        self.wallets()
        n1 = self.nodes[1]
        coins = [self.fund(Decimal("2")), self.fund(Decimal("2")), self.fund(Decimal("2"))]
        garbage = b"\x02not a record"

        self.log.info("below: the lost block's height is H - 1")
        while n1.getblockcount() < H - 2:
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        self.active(False)
        assert_equal(self.dep()["height"], H)
        t_hex, t_id, r_hex, h = self.lost_T("below", coins[0], replaceable=True)
        assert_equal(h, H - 1)
        why = n1.testmempoolaccept([r_hex])[0].get("reject-reason")
        self.record("below", "the respend in the mempool", why)
        assert why != "txn-lost-conflict"
        # Below H the node is one without the rule: the respend replaces T by the
        # ordinary rules and stays there, a better paying transaction goes first,
        # and no evidence crosses the network.
        r_id = n1.sendrawtransaction(r_hex)
        u_hex = self.spend(coins[2], n1.getnewaddress(), Decimal("0.005"))
        u_id = tx_from_hex(u_hex).rehash()
        n1.sendrawtransaction(u_hex)
        time.sleep(4)  # several passes of the rule's upkeep
        pool = n1.getrawmempool()
        assert r_id in pool and t_id not in pool, (r_id in pool, t_id in pool)
        self.record("below", "the mempool after the upkeep", "holds the respend, not T")
        relayed = sum(p["bytesrecv_per_msg"].get(EVIDENCE_MSG, 0) for j in range(N) for p in self.nodes[j].getpeerinfo())
        assert_equal(relayed, 0)
        self.record("below", "evidence bytes received, all nodes", relayed)
        self.forced_alone(H - 1, drop=[t_hex], add=[r_hex], record=garbage)
        assert t_id not in self.template_txs and self.template_txs.index(u_id) < self.template_txs.index(r_id), self.template_txs
        self.record("below", "template", "the better paying transaction first, then the respend; T is not in it")
        self.record("below", "forced", "block %d with the respend and a malformed record instead of T: accepted "
                    "by every node" % (H - 1))

        self.log.info("at: the lost block's height is H")
        self.active(True)
        t_hex, t_id, r_hex, h = self.lost_T("at", coins[1])
        assert_equal(h, H)
        why = n1.testmempoolaccept([r_hex])[0].get("reject-reason")
        self.record("at", "the respend in the mempool", why)
        assert_equal(why, "txn-lost-conflict")
        base, leader = self.produce_by()
        assert_equal(base["height"], H)
        blk = n1.getblock(base["hash"])
        assert_equal(blk["tx"][1], t_id)
        self.record("at", "template", "node 1's block at %d carries T first" % H)
        raw = n1.getblock(base["hash"], 0)
        n1.invalidateblock(base["hash"])
        for what, kw, expect in [("a malformed record", dict(record=garbage), "bad-lost-record"),
                                 ("the respend instead of T", dict(drop=[t_hex], add=[r_hex]), "bad-txns-lost-respend")]:
            rc = self.forced(raw, leader, **kw)
            res = n1.submitblock(rc["hex"])
            assert res and expect in res, "expected %r, got %r" % (expect, res)
            assert_equal(n1.getblockcount(), H - 1)
            self.record("at", "forced: " + what, res)
        n1.reconsiderblock(base["hash"])
        self.wait_until(lambda: n1.getbestblockhash() == base["hash"], timeout=30)
        self.sync_tips(HONEST)


if __name__ == '__main__':
    LostActivationTest().main()
