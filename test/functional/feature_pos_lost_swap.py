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
      nothing judges it from the chain alone); then the seller takes it back;
  sD  the claim and 121 descendants are lost: their records do not fit one
      block and are spread over the next ones, each record within its bytes;
  sR  a stake change in the lost branch before the claim: the claim's block
      certificate does not verify under the registry the record is judged with;
      the holders' refusal holds the lock until the window ends.
"""

import os
import shutil
import time
from decimal import Decimal

import feature_btc_payment_proof as bp
from feature_btc_payment_proof import BtcPaymentProofTest
from feature_pos_lost_blocks import LostBlocksTest, SELLER, HONEST, LATE, ALL_KEYS, N, W, CHAIN
from test_framework.address import program_to_witness
from test_framework.authproxy import JSONRPCException
from test_framework.key import compute_xonly_pubkey, generate_privkey
from test_framework.messages import tx_from_hex
from test_framework.script import CScript
from test_framework.util import assert_equal

CASES = os.environ.get("LOST_CASES", "sA,sB,sC,sD,sR").split(",")


class LostSwapTest(LostBlocksTest):
    def set_test_params(self):
        super().set_test_params()

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
        # The change stays a segwit v0 output. Paying a taproot output, the wallet would otherwise make its change
        # taproot too, and the next lock funded from it would carry an invalid signature (Sequentia #349).
        funded = a.fundrawtransaction(raw, {"fee_asset": "bitcoin", "replaceable": True, "change_type": "bech32"})["hex"]
        t_hex = a.signrawtransactionwithwallet(funded)["hex"]
        t = tx_from_hex(t_hex)
        txid = t.rehash()
        vout = [i for i, o in enumerate(t.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        return dict(sk_b=sk_b, sk_s=sk_s, tap=tap, claim=claim, refund=refund, txid=txid, vout=vout, hex=t_hex,
                    txout=t.vout[vout], addr=addr, amount=sat, floor=floor, deadline=deadline, refund_at=refund_at,
                    asset_amount=10 * 100000000)

    def send_lock(self, lock):
        return self.nodes[1].sendrawtransaction(lock["hex"])

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
        self.send_lock(lock)
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
        self.send_lock(lock)
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


    # ---------------------------------------------------------------- sD
    def case_sD(self):
        self.log.info("(sD) records that do not fit one block are spread over the next ones")
        n1 = self.nodes[1]
        nkids = int(os.environ.get("LOST_SD_KIDS", "120"))
        # As lost_claim, with descendants of the claim certified in the same lost branch.
        self.wallets()
        b = self.btc()
        h0 = b.getblockcount()
        lock = self.make_lock(50_000_000, h0, h0 + 50, h0)
        self.send_lock(lock)
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
        # A child splitting the claim's coin, and a grandchild per output, certified on top.
        val = Decimal(str(n1.getrawtransaction(c_id, 1)["vout"][0]["value"]))
        addrs = [n1.getnewaddress() for _ in range(nkids)]
        outs = [{ad: Decimal("0.05")} for ad in addrs]
        outs.append({n1.getnewaddress(): val - Decimal("0.05") * nkids - Decimal("0.01")})
        outs.append({"fee": Decimal("0.01")})
        raw = n1.createrawtransaction([{"txid": c_id, "vout": 0}], outs)
        kid = n1.sendrawtransaction(n1.signrawtransactionwithwallet(raw)["hex"])
        L2 = self.produce(1, ALL_KEYS, txs_must=[kid])
        kdec = n1.getrawtransaction(kid, 1)
        gkids = []
        for o in kdec["vout"]:
            if o["scriptPubKey"].get("address") in addrs:
                r = n1.createrawtransaction([{"txid": kid, "vout": o["n"]}], [{n1.getnewaddress(): Decimal("0.049")}, {"fee": Decimal("0.001")}])
                gkids.append(n1.sendrawtransaction(n1.signrawtransactionwithwallet(r)["hex"]))
        L3 = self.produce(1, ALL_KEYS, txs_must=gkids[:1])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        self.record("sD", "lost branch", "claim %s in LC (h %d), its child in h %d, %d grandchildren in h %d; all anchored at Bitcoin %d" %
                    (c_id[:12], LC["height"], L2["height"], len(gkids), L3["height"], p))
        b.invalidateblock(p_hash)
        b.generatetoaddress(2, self.bbuyer.getnewaddress())
        self.wait_lost(LC["hash"], HONEST)
        since = [self.log_size(i) for i in range(N)]
        recorded = set()
        per_block = []
        for _ in range(8):
            h = self.produce(1, ALL_KEYS, leaders=[SELLER])
            recs = self.records_in(n1, h["hash"])
            nbytes = sum(len(r["scriptPubKey"]["hex"]) // 2 for r in recs)
            ids = set()
            for line in self.log_has(1, "Lost blocks: block %s at height %d records" % (h["hash"], h["height"]), since[1]):
                pass
            per_block.append((h["height"], len(recs), nbytes))
            if not recs and per_block[:-1] and per_block[-2][1] == 0:
                break
        self.sync_tips(HONEST + [LATE])
        lines = self.log_has(1, "records", since[1])
        tmpl = self.log_has(1, "to record of", since[1])
        self.record("sD", "records per block (height, outputs, bytes)", per_block)
        self.record("sD", "plans", [l.split("template on ", 1)[-1][66:260] for l in tmpl[:4]])
        assert sum(1 for x in per_block if x[1]) >= 2, per_block
        assert all(x[2] <= 25000 + 200 for x in per_block)
        self.realign()

    # ---------------------------------------------------------------- sR
    def case_sR(self):
        self.log.info("(sR) a stake change in the lost branch before the claim")
        n1 = self.nodes[1]
        a = self.nodes[SELLER]
        self.wallets()
        b = self.btc()
        h0 = b.getblockcount()
        lock = self.make_lock(50_000_000, h0, h0 + 50, h0)
        self.send_lock(lock)
        self.produce(1, ALL_KEYS, txs_must=[lock["txid"]])
        for _ in range(bp.FLOOR_DEPTH + 1):
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        pay = self.pay(lock)
        p_hash = b.generatetoaddress(1, self.bbuyer.getnewaddress())[0]
        p = b.getblockcount()
        self.anchored_block(p)
        # L1: a new staker K locks stake (its key never signs).
        from feature_pos_lost_blocks import make_key
        k_wif, k_pub = make_key()
        bls = n1.getblsregistration(k_wif)
        script = a.getstakescript(pubkey=k_pub, csv_blocks=600, blspubkey=bls["blspubkey"], pop=bls["pop"])["script"]
        coin = self.fund()
        # K stakes one atom: a weight like each of the six stakers' (they are registered with weight 1).
        # A stake of the whole coin (1e8 atoms) would hold all but 6e-8 of the weight, so the six's
        # exponential-race slots would open hours late and every block they make is time-too-new.
        reg_raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],
                                         [{a.getnewaddress(): Decimal("0.00000001")},
                                          {a.getnewaddress(): coin["amount"] - Decimal("0.00010001")}, {"fee": Decimal("0.0001")}])
        rt = tx_from_hex(reg_raw)
        from test_framework.messages import CTxOut, CTxOutValue, CTxOutAsset
        rt.vout[0] = CTxOut(CTxOutValue(1), bytes.fromhex(script), CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1]))
        reg_hex = a.signrawtransactionwithwallet(rt.serialize().hex())["hex"]
        reg = n1.sendrawtransaction(reg_hex)
        reg_before = sorted(n1.getstakerinfo().keys())
        L1 = self.produce(1, ALL_KEYS, txs_must=[reg])
        reg_after = n1.getstakerinfo()
        assert k_pub in reg_after
        c, pr = self.buyer_claim(lock, pay)
        c_id = c.rehash()
        n1.sendrawtransaction(c.serialize().hex())
        # K signs LC: its certificate then names a member the fork point's registry does not have.
        self.wait_until(lambda: c_id in n1.getrawmempool(), timeout=30)
        signers = [self.stakers[j][0] for j in ALL_KEYS if j != 1] + [k_wif]
        LC = n1.generateposblock(self.stakers[1][0], signers)
        assert c_id in n1.getblock(LC["hash"])["tx"]
        LC["block"] = n1.getblock(LC["hash"])
        self.record("sR", "LC's certificate", "%d countersignatures, K's among them" % LC["countersignatures"])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        assert n1.getblockheader(LC["hash"])["poscertified"]
        self.record("sR", "lost branch", "L1 (h %d) registers K's stake (%d stakers -> %d); claim %s in certified LC (h %d); anchored at Bitcoin %d" %
                    (L1["height"], len(reg_before), len(reg_after), c_id[:12], LC["height"], p))
        b.invalidateblock(p_hash)
        b.generatetoaddress(2, self.bbuyer.getnewaddress())
        self.wait_lost(LC["hash"], HONEST)
        since = [self.log_size(i) for i in range(N)]
        rows = []
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        rows.append((h["height"], reg in n1.getblock(h["hash"])["tx"], len(self.records_in(n1, h["hash"]))))
        self.sync_tips(range(N))
        why = self.log_has(1, "cannot be recorded", since[1])
        self.record("sR", "node 1 at the first block", why[0].split("Lost blocks: ", 1)[-1][:260] if why else "no 'cannot be recorded' line")
        # With no record on the chain yet, the seller's refund, forced, meets only the holders' refusal.
        r = self.refund(lock)
        x = self.force_on_seller("sR", [r.serialize().hex()])
        self.refused_by_any("sR", HONEST, x["hash"], since)
        self.realign()
        for _ in range(2):
            h = self.produce(1, ALL_KEYS, leaders=[SELLER])
            rows.append((h["height"], reg in n1.getblock(h["hash"])["tx"], len(self.records_in(n1, h["hash"]))))
        self.sync_tips(range(N))
        self.record("sR", "honest blocks (height, K's stake replayed, records)", rows)
        since = [self.log_size(i) for i in range(N)]
        x2 = self.force_on_seller("sR", [r.serialize().hex()])
        self.refused_by_any("sR", HONEST, x2["hash"], since)
        self.realign()
        # The payment is still on Bitcoin: the claim, proven again, keeps its id and is mined.
        c2, pr2 = self.buyer_claim(lock, pay)
        assert_equal(c2.rehash(), c_id)
        n1.sendrawtransaction(c2.serialize().hex())
        hc = self.produce(1, ALL_KEYS, leaders=[SELLER], txs_must=[c_id])
        self.sync_tips(HONEST + [LATE])
        self.record("sR", "outcome", "the claim %s, re-proven (Bitcoin block %d), mined at height %d with the same id" %
                    (c_id[:12], pr2["height"], hc["height"]))
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
        self.record("sC", "record", "block %s at height %d (anchor %d): %d bytes; binds the lock until the lapse (anchor above %d once LC's height is rebuilt)" %
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
        # The lapse (doc 03): the chain rebuilds LC's height, then W Bitcoin
        # blocks pass above both LC's anchor and the anchor at that height;
        # the refund is mined.
        end = self.pass_lapse(1)
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
