#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A swap of a Sequentia coin for on-chain bitcoin across Bitcoin
reorganizations, with the lost-block rule (doc 03) and the payment proof
(doc bitcoin-payment-proof.md) together. Needs both on one branch.

Cases (LOST_CASES):
  sA  the lock is lost; the seller leads; the replay brings it back first; the
      buyer's payment is mined again on Bitcoin and its claim is mined;
  sB  the claim is lost; the payment is mined again; the claim, recorded, comes
      back with a fresh proof and the same id; the seller's refund is refused;
  sC  the claim is lost and the payment is gone for good; the record holds the
      lock against the refund until the window ends (a node synced from
      nothing judges it from the chain alone); then the seller takes it back.
"""

import os
import shutil
import time
from decimal import Decimal

try:
    import feature_btc_payment_proof as bp
    from feature_btc_payment_proof import BtcPaymentProofTest
except ImportError:  # the payment proof is on another branch
    bp = None

    class BtcPaymentProofTest:
        proof_for = link_headers = spend_lock = claim = refund = recorded_anchor_at_or_above = None
from feature_pos_lost_blocks import LostBlocksTest, SELLER, HONEST, LATE, ALL_KEYS, N, W, CHAIN
from test_framework.address import program_to_witness
from test_framework.authproxy import JSONRPCException
from test_framework.key import compute_xonly_pubkey, generate_privkey
from test_framework.messages import tx_from_hex
from test_framework.script import CScript
from test_framework.util import assert_equal

CASES = os.environ.get("LOST_CASES", "sA,sB,sC").split(",")


class LostSwapTest(LostBlocksTest):
    def set_test_params(self):
        super().set_test_params()

    def skip_test_if_missing_module(self):
        super().skip_test_if_missing_module()
        if bp is None:
            from test_framework.test_framework import SkipTest
            raise SkipTest("needs the payment proof (feature/bitcoin-payment-proof) on the same branch")

    # Borrowed from the payment-proof suite.
    proof_for = BtcPaymentProofTest.proof_for
    link_headers = BtcPaymentProofTest.link_headers
    spend_lock = BtcPaymentProofTest.spend_lock
    claim = BtcPaymentProofTest.claim
    refund = BtcPaymentProofTest.refund

    def btc_rpc(self, wallet=None):
        return self.btc(wallet)

    def recorded_anchor_at_or_above(self, h, node=None):
        return BtcPaymentProofTest.recorded_anchor_at_or_above(self, h, node or self.nodes[1])

    # ------------------------------------------------------------ set-up
    def wallets(self):
        if getattr(self, "bbuyer", None):
            return
        b = self.btc()
        for w in ("buyer", "seller"):
            b.createwallet(w)
        self.bbuyer, self.bseller = self.btc("buyer"), self.btc("seller")
        b.generatetoaddress(101, self.bbuyer.getnewaddress())
        # Every coin of the buyer's mature well before any reorganization below.
        b.generatetoaddress(20, b.getnewaddress() if False else self.bseller.getnewaddress())
        self.genesis = int(self.nodes[1].getblockhash(0), 16)
        self.asset = self.nodes[1].dumpassetlabels()["bitcoin"]
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))

    def make_lock(self, sat, floor, deadline, refund_at):
        """The seller's lock T (not yet broadcast): claim by a proven payment of `sat` to the seller's
        Bitcoin address, refund once the anchor floor reaches refund_at."""
        sk_b, sk_s = generate_privkey(), generate_privkey()
        pk_b, pk_s = compute_xonly_pubkey(sk_b)[0], compute_xonly_pubkey(sk_s)[0]
        desc = self.bseller.getdescriptorinfo("wsh(and_v(v:pk(%s),older(1)))" % ("02" + pk_s.hex()))["descriptor"]
        addr = self.bseller.deriveaddresses(desc)[0]
        info = self.bseller.getaddressinfo(addr)
        program, version = bytes.fromhex(info["witness_program"]), info["witness_version"]
        claim = CScript([pk_b, bp.OP_CHECKSIGVERIFY, bp.OP_INSPECTBTCPAYMENT, bp.OP_DROP, bp.OP_DUP, floor, bp.OP_GREATERTHAN,
                         bp.OP_VERIFY, deadline, bp.OP_LESSTHANOREQUAL, bp.OP_VERIFY, bp.FIELD_OUTPUT, bp.OP_INSPECTBTCTX, version,
                         bp.OP_EQUALVERIFY, program, bp.OP_EQUALVERIFY, bp.le8(sat), bp.OP_GREATERTHANOREQUAL64])
        refund = CScript([refund_at, bp.OP_CHECKANCHORHEIGHTVERIFY, bp.OP_DROP, pk_s, bp.OP_CHECKSIG])
        tap = bp.taproot_construct(bp.NUMS, [("claim", claim), ("refund", refund)])
        lock_addr = program_to_witness(1, tap.scriptPubKey[2:])
        a = self.nodes[SELLER]
        raw = a.createrawtransaction([], [{lock_addr: Decimal("10")}])
        funded = a.fundrawtransaction(raw, {"fee_asset": "bitcoin", "replaceable": True})["hex"]
        t_hex = a.signrawtransactionwithwallet(funded)["hex"]
        t = tx_from_hex(t_hex)
        txid = t.rehash()
        vout = [i for i, o in enumerate(t.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        return dict(sk_b=sk_b, sk_s=sk_s, tap=tap, claim=claim, refund=refund, txid=txid, vout=vout, hex=t_hex,
                    txout=t.vout[vout], addr=addr, amount=sat, floor=floor, deadline=deadline, refund_at=refund_at,
                    asset_amount=10 * 100000000)

    def coin_anchor(self, txid, node=None):
        node = node or self.nodes[1]
        bh = node.getrawtransaction(txid, True)["blockhash"]
        return node.getblockheader(bh)["anchorheight"]

    def pay(self, lock):
        txid = self.bbuyer.sendtoaddress(lock["addr"], Decimal(lock["amount"]) / Decimal(100000000))
        return txid

    def buyer_claim(self, lock, pay_txid):
        pr = self.proof_for(pay_txid, self.coin_anchor(lock["txid"]))
        vout = self.vout_paying(pay_txid, lock["addr"], pr["blockhash"])
        if "buyer_spk" not in lock:
            lock["buyer_spk"] = bytes.fromhex(self.nodes[1].getaddressinfo(self.nodes[1].getnewaddress())["scriptPubKey"])
        return self.claim(lock, pr["record"], vout, out_spk=lock["buyer_spk"]), pr

    def vout_paying(self, txid, addr, blockhash):
        t = self.btc().getrawtransaction(txid, True, blockhash)
        return [o["n"] for o in t["vout"] if o["scriptPubKey"].get("address") == addr][0]

    def anchored_block(self, h, node_i=1):
        """Honest certified blocks until the tip anchors at or above Bitcoin height h."""
        for _ in range(20):
            if self.tip(self.nodes[node_i])["anchorheight"] >= h:
                return
            self.produce(node_i, ALL_KEYS)
            time.sleep(0.3)
        raise AssertionError("no block anchored at %d" % h)

    def records_in(self, node, h):
        cb = node.getblock(h, 2)["tx"][0]
        return [o for o in cb["vout"] if o["scriptPubKey"]["hex"].startswith("6a04" + b"SQLR".hex())]

    def force_on_seller(self, case, txs_hex, signers=ALL_KEYS):
        """The seller (rule off) builds a block carrying these transactions on its tip, certified by `signers`."""
        a = self.nodes[SELLER]
        ids = []
        for hx in txs_hex:
            ids.append(tx_from_hex(hx).rehash())
            try:
                a.sendrawtransaction(hx)
            except JSONRPCException as e:
                self.record(case, "seller's mempool on %s" % ids[-1][:12], e.error["message"])
        return self.produce(SELLER, signers, leaders=[SELLER], txs_must=ids)

    def lost_claim(self, case, refund_at_now=True, gone=False):
        """A lock buried 61 blocks (its refund path open), the payment, the claim certified in LC,
        then Bitcoin replaces the payment's block; `gone`: the payment is double-spent there."""
        self.wallets()
        b = self.btc()
        n1 = self.nodes[1]
        h0 = b.getblockcount()
        lock = self.make_lock(50_000_000, h0, h0 + 50, h0)
        n1.sendrawtransaction(lock["hex"])
        self.produce(1, ALL_KEYS, txs_must=[lock["txid"]])
        for _ in range(bp.FLOOR_DEPTH + 1):
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        pay = self.pay(lock)
        p_hash = b.generatetoaddress(1, self.bbuyer.getnewaddress())[0]
        p = b.getblockcount()
        self.anchored_block(p)
        c, pr = self.buyer_claim(lock, pay)
        c_id = c.rehash()
        n1.sendrawtransaction(c.serialize().hex())
        LC = self.produce(1, ALL_KEYS, txs_must=[c_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        self.record(case, "claim", "lock %s (refund open: floor %d), payment %s in Bitcoin block %d, claim %s certified in LC %s at height %d" %
                    (lock["txid"][:12], lock["refund_at"], pay[:12], p, c_id[:12], LC["hash"][:12], LC["height"]))
        # Bitcoin replaces the payment's block.
        buyer_in = b.getrawtransaction(pay, True, p_hash)["vin"][0]
        b.invalidateblock(p_hash)
        if gone:
            prev_value = Decimal(str(self.bbuyer.gettransaction(buyer_in["txid"], True, True)["decoded"]["vout"][buyer_in["vout"]]["value"]))
            raw = self.bbuyer.createrawtransaction([{"txid": buyer_in["txid"], "vout": buyer_in["vout"]}],
                                                   [{self.bbuyer.getnewaddress(): prev_value - Decimal("0.001")}])
            dbl = self.bbuyer.signrawtransactionwithwallet(raw)["hex"]
            b.sendrawtransaction(dbl, 0)
        b.generatetoaddress(2, self.bbuyer.getnewaddress())
        self.wait_lost(LC["hash"], HONEST)
        wtx = self.bbuyer.gettransaction(pay)
        on_btc = wtx.get("blockhash") if wtx.get("confirmations", 0) > 0 else None
        self.record(case, "reorg", "Bitcoin block %d replaced; LC lost on %s; the payment %s" %
                    (p, HONEST, ("mined again in %s" % on_btc[:12]) if on_btc else "gone (its input double-spent)"))
        return dict(lock=lock, pay=pay, p=p, c=c, c_id=c_id, LC=LC)

    def wipe_and_sync(self, i, peers):
        self.stop_node(i)
        shutil.rmtree(os.path.join(self.nodes[i].datadir, CHAIN))
        self.start_node(i, extra_args=self.node_args(i))
        for j in peers:
            self.connect_nodes(i, j)
        self.wait_until(lambda: self.nodes[i].getbestblockhash() == self.nodes[peers[0]].getbestblockhash(), timeout=120)

    # ---------------------------------------------------------------- sA
    def case_sA(self):
        self.log.info("(sA) the lock is lost and comes back first; the claim completes")
        self.wallets()
        b = self.btc()
        n1 = self.nodes[1]
        a = self.nodes[SELLER]
        h0 = b.getblockcount()
        lock = self.make_lock(50_000_000, h0, h0 + 50, h0 + 1000)
        b.generatetoaddress(1, self.bbuyer.getnewaddress())
        n1.sendrawtransaction(lock["hex"])
        L = self.produce(1, ALL_KEYS, txs_must=[lock["txid"]])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        pay = self.pay(lock)
        b.generatetoaddress(1, self.bbuyer.getnewaddress())
        self.record("sA", "lock", "%s in certified L %s (anchor %d); the buyer paid %s, in Bitcoin block %d" %
                    (lock["txid"][:12], L["hash"][:12], L["block"]["anchorheight"], pay[:12], b.getblockcount()))
        # Bitcoin replaces L's anchor and the payment's block (2 deep); the payment is mined again.
        since = [self.log_size(i) for i in range(N)]
        self.bitcoin_reorg(2)
        self.wait_lost(L["hash"], HONEST)
        evicted = pay not in b.getrawmempool() and self.bbuyer.gettransaction(pay).get("confirmations", 0) <= 0
        if evicted:
            # Its nLockTime (the wallet's anti-fee-sniping height) is not final at the new tip's next
            # height, so bitcoind evicted it; the buyer's completion service sends it again.
            self.record("sA", "payment after the reorganization", "evicted by bitcoind (nLockTime not final at the new height); sent again")
            b.sendrawtransaction(self.bbuyer.gettransaction(pay)["hex"])
            b.generatetoaddress(1, self.bbuyer.getnewaddress())
        self.wait_until(lambda: self.bbuyer.gettransaction(pay).get("confirmations", 0) > 0, timeout=30)
        self.record("sA", "reorg", "Bitcoin 2 deep; L lost on %s; the payment mined again in %s" %
                    (HONEST, self.bbuyer.gettransaction(pay).get("blockhash", "?")[:12]))
        # The seller respends the lock's inputs and a quorum certifies it: refused by every holder.
        t = tx_from_hex(lock["hex"])
        ins = [{"txid": "%064x" % i.prevout.hash, "vout": i.prevout.n} for i in t.vin]
        total = sum(Decimal(str(a.gettxout(i["txid"], i["vout"], False)["value"])) for i in ins)
        raw = a.createrawtransaction(ins, [{a.getnewaddress(): total - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        r_hex = a.signrawtransactionwithwallet(raw)["hex"]
        x = None
        try:
            x = self.force_on_seller("sA", [r_hex])
        except AssertionError as e:
            self.record("sA", "seller's respend block", "not built: %s" % e)
        if x:
            self.refused_by_any("sA", HONEST, x["hash"], since)
        # The seller leads; an honest node assembles: the lock first.
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        assert_equal(n1.getblock(h["hash"])["tx"][1], lock["txid"])
        self.sync_tips(HONEST + [LATE])
        self.record("sA", "replay", "block %s at height %d led by the seller's key: tx[1] = the lock" % (h["hash"][:12], h["height"]))
        # The claim, against the payment's new block.
        self.anchored_block(b.getblockcount())
        c, pr = self.buyer_claim(lock, pay)
        n1.sendrawtransaction(c.serialize().hex())
        hc = self.produce(1, ALL_KEYS, leaders=[SELLER], txs_must=[c.rehash()])
        self.sync_tips(HONEST + [LATE])
        self.record("sA", "outcome", "the buyer's claim %s mined at height %d (proof of Bitcoin block %d); the asset is the buyer's" %
                    (c.rehash()[:12], hc["height"], pr["height"]))
        self.realign()

    def refused_by_any(self, case, idx, h, since):
        def ok():
            return all(self.log_has(i, h, since[i]) and self.nodes[i].getbestblockhash() != h for i in idx)
        self.wait_until(ok, timeout=30)
        lines = [l for l in self.log_has(idx[0], h, since[idx[0]]) if "Lost blocks" in l or "ERROR" in l]
        self.record(case, "refusal (node %d)" % idx[0], (lines[0].split("] ", 3)[-1] if lines else "?")[:300])

    # ---------------------------------------------------------------- sB
    def case_sB(self):
        self.log.info("(sB) the claim is lost; the payment is mined again; the claim comes back")
        st = self.lost_claim("sB")
        n1 = self.nodes[1]
        since = [self.log_size(i) for i in range(N)]
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        recs = self.records_in(n1, h["hash"])
        self.record("sB", "record", "block %s at height %d (anchor %d) carries %d record output(s), %d bytes" %
                    (h["hash"][:12], h["height"], h["block"]["anchorheight"], len(recs), sum(len(r["scriptPubKey"]["hex"]) // 2 for r in recs)))
        assert len(recs) == 1
        self.sync_tips(range(N))
        # The seller's refund, forced: refused by every node from the chain's record.
        r = self.refund(st["lock"])
        x = self.force_on_seller("sB", [r.serialize().hex()])
        self.refused_by_any("sB", HONEST + [LATE], x["hash"], since)
        self.realign()
        # The claim with a fresh proof: the same id.
        c2, pr = self.buyer_claim(st["lock"], st["pay"])
        assert_equal(c2.rehash(), st["c_id"])
        n1.sendrawtransaction(c2.serialize().hex())
        hc = self.produce(1, ALL_KEYS, leaders=[SELLER], txs_must=[st["c_id"]])
        self.sync_tips(HONEST + [LATE])
        self.record("sB", "outcome", "the claim %s, re-proven (Bitcoin block %d), mined at height %d with the same id" %
                    (st["c_id"][:12], pr["height"], hc["height"]))
        self.realign()

    # ---------------------------------------------------------------- sC
    def case_sC(self):
        self.log.info("(sC) the claim is lost and the payment is gone; the record holds until the window ends")
        st = self.lost_claim("sC", gone=True)
        n1 = self.nodes[1]
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        recs = self.records_in(n1, h["hash"])
        assert len(recs) == 1
        self.sync_tips(range(N))
        self.record("sC", "record", "block %s at height %d (anchor %d): %d bytes; binds the lock until anchor %d" %
                    (h["hash"][:12], h["height"], h["block"]["anchorheight"], len(recs[0]["scriptPubKey"]["hex"]) // 2,
                     st["LC"]["block"]["anchorheight"] + W))
        # A node synced from nothing: no lost block held; it judges from the chain alone.
        self.wipe_and_sync(LATE, HONEST)
        assert_equal(self.nodes[LATE].getlostblocks()["blocks"], [])
        since = [self.log_size(i) for i in range(N)]
        r = self.refund(st["lock"])
        x = self.force_on_seller("sC", [r.serialize().hex()])
        self.refused_by_any("sC", HONEST, x["hash"], since)
        try:
            self.nodes[LATE].submitblock(self.nodes[SELLER].getblock(x["hash"], 0))
        except JSONRPCException:
            pass
        line = self.log_has(LATE, "bad-txns-recorded-respend", since[LATE])
        self.record("sC", "the node synced from nothing", (line[0].split("] ", 3)[-1] if line else "no refusal line")[:280])
        assert line
        self.realign()
        # The window ends: Bitcoin passes the lost block's anchor + W; the refund is mined.
        end = st["LC"]["block"]["anchorheight"] + W
        b = self.btc()
        if b.getblockcount() <= end:
            b.generatetoaddress(end + 1 - b.getblockcount(), self.bbuyer.getnewaddress())
        self.anchored_block(end + 1)
        self.wait_until(lambda: all(not self.lost(i) for i in HONEST), timeout=30)
        n1.sendrawtransaction(r.serialize().hex())
        hr = self.produce(1, ALL_KEYS, txs_must=[r.rehash()])
        self.sync_tips(HONEST + [LATE])
        self.record("sC", "outcome", "after the window (anchor %d > %d) the seller's refund %s is mined at height %d" %
                    (hr["block"]["anchorheight"], end, r.rehash()[:12], hr["height"]))
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
    LostSwapTest().main()
