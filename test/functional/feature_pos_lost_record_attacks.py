#!/usr/bin/env python3
"""What a coalition holding a seat quorum at ONE height can do with a lost-block record.

Every case is an attack the record's rules must stop, or a limit of them. Each
forced negative is mined into a block and refused for its own reason; the case
raises when the attack gets through. Node 0 is the coalition's node (the rule
off, anchors unchecked, so it can hold invented headers).

Cases (LOST_CASES, default all that share one network; a case@stale uses a real
stale Bitcoin block as the lost anchor instead of an invented hash):
  fab        an invented lost block spends someone's coin with no signature
  mass       the same, binding hundreds of coins at once
  theft      the seller's refund, not yet valid, recorded to hold the buyer out
  keyless    a forged claim on an open offer's leaf (the same-leaf yield)
  grind      ground anchors seat the coalition on the next committee
  evid       certified evidence over P2P makes honest producers write a record
  flood      sibling blocks force fee-free transactions into honest blocks
  disp       a lower-hash sibling takes an honest lost block's carry
  powneg     Bitcoin headers that must not prove a lost anchor
  grace      a payment mined again: the claim returns with no record (LOST_GRACE)
  gracegone  a payment gone: the holders' refusal, then the record
  duplicate  a record of a transaction spending one outpoint twice (use @stale)
  siblingrespend  a lower-hash certified sibling, shown after the loss, carries
             the seller's respend
  siblingbranch   the same, against the second block of a two-block lost branch
  siblingbefore   the same, on a real stale anchor, shown before the loss
  branchleaf a key branch of the bound leaf, with a proof no opcode reads, must not pass
  recordrace a record binds a coin to a sibling's respend before the duty names the
             honest lost transaction (use @stale)
  evidmempool  an evidence-only transaction that stands returns under ordinary fee policy (use @stale)
  stream     a quorum streams 30 certified siblings: W + 1 held and one plan computed
  capflood1  one seat floods 12 siblings: a node's restraint holds
  capflood4  a quorum floods 12 siblings: the restraint lifts
  rebind     a later record tries to move a bound coin to another transaction (use @stale)
  gracetwo   a refund certified at two heights while no record binds the lock

A case in OPEN states a limit the rule does not close yet: the run reports it
and passes, unless LOST_STRICT=1. A case in OPEN that stops failing is reported
too, so that the entry is removed.
"""
import hashlib, os, time
from decimal import Decimal

import feature_btc_payment_proof as bp
from feature_pos_lost_attacks import merkle_branch, merkle_root, ser_record
from feature_pos_lost_blocks import SELLER, HONEST, ALL_KEYS, N, W
from feature_pos_lost_swap import LostSwapTest
from pos_lost_attack_util import LostP2PDataStore, block_from_hex
from test_framework.authproxy import JSONRPCException
from test_framework.messages import (COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset, CTxOutValue, CTxOutWitness,
                                     ser_uint256, tx_from_hex, uint256_from_str)
from test_framework.messages import hash256
from test_framework.util import assert_equal, calcfastmerkleroot
import test_framework.util as fu

CASES = os.environ.get("LOST_CASES", "fab,powneg").split(",")
STALE = any(c.endswith("@stale") for c in CASES)
CASES = [c.split("@")[0] for c in CASES]
GRACE = int(os.environ.get("LOST_GRACE", "0"))
#: The record format of the binary under test. Only a run against a build that
#: predates the current format sets it.
RECORD_VERSION = int(os.environ.get("LOST_RECORD_VERSION", "3"))
V3 = RECORD_VERSION >= 3
YIELD = V3
OPEN = {}


def sha(b):
    return hashlib.sha256(b).digest()


class LostRecordAttacksTest(LostSwapTest):
    def put_record(self, data):
        """As LostAttacksTest.put_record, with any leader and every key offered (a committee smaller than the stakers)."""
        from feature_pos_lost_attacks import push
        from test_framework.messages import CTxOutWitness, hash256
        from test_framework.util import calcfastmerkleroot
        import test_framework.util as fu
        n1 = self.nodes[1]
        self.disconnect_all(1)
        hh, k = self.gen(n1)
        raw = bytes.fromhex(n1.getblock(hh, 0))
        blk = n1.getblock(hh, 2)
        cb = tx_from_hex(blk["tx"][0]["hex"])
        old_cb = cb.serialize()
        assert raw.count(old_cb) == 1
        ci = [j for j, o in enumerate(cb.vout) if o.scriptPubKey[:6] == bytes.fromhex("6a24aa21a9ed")][-1]
        cb.vout.insert(ci, CTxOut(0, b"\x6a\x04SQLR" + push(data), cb.vout[0].nAsset))
        if cb.wit.vtxoutwit:
            cb.wit.vtxoutwit.insert(ci, CTxOutWitness())
        fu.node_fastmerkle = n1
        txs = [cb] + [tx_from_hex(t["hex"]) for t in blk["tx"][1:]]
        wroot = bytes.fromhex(calcfastmerkleroot([t.calc_witness_hash() for t in txs]))[::-1]
        nonce = cb.wit.vtxinwit[0].scriptWitness.stack[0]
        cb.vout[-1 if ci == len(cb.vout) - 1 else ci + 1].scriptPubKey = b"\x6a\x24\xaa\x21\xa9\xed" + hash256(wroot + nonce)
        cb.rehash()
        txids = [cb.hash] + [t["txid"] for t in blk["tx"][1:]]
        root = merkle_root([bytes.fromhex(t)[::-1] for t in txids])
        new_raw = raw[:36] + root + raw[68:]
        new_raw = new_raw.replace(old_cb, cb.serialize())
        n1.invalidateblock(hh)
        since = self.log_size(1)
        rc = n1.recertifyposblock(new_raw.hex(), self.wif(k), [self.wif(j) for j in ALL_KEYS])
        r = n1.submitblock(rc["hex"])
        return rc["hash"], r, since

    def set_test_params(self):
        super().set_test_params()
        self.extra = {SELLER: ["-validateanchor=0"]} if not set(CASES) & {"carried", "rbf", "sB", "sC", "grace", "gracegone", "disp", "sD", "siblingrespend", "siblingbranch", "siblingbefore", "recordrace", "stream", "capflood1", "capflood4", "evidmempool", "gracetwo", "branchleaf"} else {}
        if "grind" in CASES:
            for i in range(N):
                self.extra.setdefault(i, []).append("-poscommitteesize=3")

    # ---- helpers
    def wif(self, i):
        return self.stakers[i][0]

    def isolate(self):
        a = self.nodes[SELLER]
        self.disconnect_all(SELLER)
        a.setnetworkactive(False)

    def rejoin(self):
        a = self.nodes[SELLER]
        for h in getattr(self, "fabs", []):
            try:
                a.invalidateblock(h)
            except JSONRPCException:
                pass
        self.fabs = []
        a.setnetworkactive(True)
        for j in range(1, N):
            try:
                self.connect_nodes(j, SELLER)
            except Exception:
                pass
        for j in range(2, N):
            try:
                self.connect_nodes(1, j)
            except Exception:
                pass
        self.wait_until(lambda: all(self.nodes[i].getbestblockhash() == self.nodes[1].getbestblockhash() for i in HONEST), timeout=60)
        n1 = self.nodes[1]
        self.undo = []
        end = time.time() + 8
        while time.time() < end and a.getbestblockhash() != n1.getbestblockhash():
            time.sleep(0.25)
        if a.getbestblockhash() != n1.getbestblockhash():
            # An honest block identical to one this node generated and set aside.
            bad = {t["hash"] for t in a.getchaintips() if t["status"] == "invalid"}
            t = n1.getblockcount()
            for ht in range(max(1, t - 6), t + 1):
                if n1.getblockhash(ht) in bad:
                    a.reconsiderblock(n1.getblockhash(ht))
        self.wait_until(lambda: a.getbestblockhash() == n1.getbestblockhash(), timeout=60)

    def gen(self, node):
        last = None
        for _ in range(120):
            for k in ALL_KEYS:
                try:
                    r = node.generateposblock(self.wif(k), [self.wif(j) for j in ALL_KEYS if j != k])
                    return r["hash"], k
                except JSONRPCException as e:
                    last = e.error["message"]
            time.sleep(0.25)
        raise AssertionError("no block: %s" % last)

    def fab_header(self, anchor_h, anchor_int, signers, tx=None, connect=False):
        """A header on the coalition node's tip with an anchor of our choosing, certified by `signers` only."""
        a = self.nodes[SELLER]
        h, k = self.gen(a)
        raw = a.getblock(h, 0)
        a.invalidateblock(h)
        self.undo = getattr(self, "undo", []) + [h]
        blk = block_from_hex(raw)
        blk.m_anchor_height, blk.m_anchor_hash = anchor_h, anchor_int
        entry = None
        if tx is not None:
            cb = blk.vtx[0]
            cb.rehash()
            tx.rehash()
            leaves = [ser_uint256(cb.sha256), ser_uint256(tx.sha256)]
            blk.hashMerkleRoot = uint256_from_str(merkle_root(leaves))
            entry = (1, merkle_branch(leaves, 1), tx.serialize_with_witness() if V3 else tx.serialize_without_witness())
        blk.sha256 = None
        rc = a.recertifyposblock(blk.serialize().hex(), self.wif(k), [self.wif(s) for s in signers])
        out = block_from_hex(rc["hex"])
        if connect:
            assert_equal(a.submitblock(rc["hex"]), None)
            assert_equal(a.getbestblockhash(), rc["hash"])
            self.fabs = getattr(self, "fabs", []) + [rc["hash"]]
        return out, entry, rc["countersignatures"]

    def anchor_choice(self, f):
        """The lost anchor: an invented hash, or (case@stale) a real Bitcoin block made stale, with its header."""
        b = self.btc()
        if not STALE:
            self.btc_mine(1)
            time.sleep(1.5)
            return f["anchorheight"] + 1, int.from_bytes(os.urandom(32), "little"), []
        s = self.btc_mine(1)[0]
        raw = bytes.fromhex(b.getblockheader(s, False))
        h = b.getblockheader(s)["height"]
        b.invalidateblock(s)
        self.btc_mine(1)
        time.sleep(1.5)
        assert b.getblockhash(h) != s
        return h, int(s, 16), [raw]

    def mk(self, lost, entries, btc=()):
        if not V3:
            from feature_pos_lost_attacks import ser_bytes, ser_cs
            out = bytes([2]) + ser_cs(len(lost)) + b"".join(ser_bytes(h) for h in lost) + ser_cs(len(entries))
            for k, pos, branch, tx in entries:
                out += k.to_bytes(4, "little") + pos.to_bytes(4, "little") + ser_cs(len(branch)) + b"".join(branch) + ser_bytes(tx)
            return out
        return ser_record(list(btc), lost, entries)

    def stale_chain(self, anchor):
        """The Bitcoin headers from the best chain to `anchor`, parents first."""
        b = self.btc()
        out = []
        while b.getblockheader(anchor)["confirmations"] < 0:
            out.append(bytes.fromhex(b.getblockheader(anchor, False)))
            anchor = b.getblockheader(anchor)["previousblockhash"]
        return out[::-1]

    def blocked(self, case, what):
        self.record(case, "OUTCOME", "BLOCKED: " + what)

    def invented_tx(self, outpoints, value=500000000):
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.nodes[1].dumpassetlabels()["bitcoin"])[::-1])
        t = CTransaction()
        t.nVersion = 2
        for (txid, n) in outpoints:
            t.vin.append(CTxIn(COutPoint(txid, n), nSequence=0xffffffff))
        t.vout.append(CTxOut(CTxOutValue(value), b"\x51", asset))
        t.rehash()
        return t

    def victim_coin(self):
        a, n1 = self.nodes[SELLER], self.nodes[1]
        addr = n1.getnewaddress()
        txid = a.sendtoaddress(address=addr, amount=Decimal("5"), fee_asset_label="bitcoin")
        self.produce(SELLER, ALL_KEYS, txs_must=[txid])
        self.sync_tips(range(N))
        vout = [o["n"] for o in n1.getrawtransaction(txid, 1)["vout"] if o["scriptPubKey"].get("address") == addr][0]
        raw = n1.createrawtransaction([{"txid": txid, "vout": vout}],
                                      [{n1.getnewaddress(): Decimal("4.999")}, {"fee": Decimal("0.001")}])
        return txid, vout, n1.signrawtransactionwithwallet(raw)["hex"]

    def force_refused(self, case, tx_hex, why="bad-txns-recorded-respend"):
        """The coalition's node builds a block with `tx_hex`; every honest node must refuse it for `why`."""
        a = self.nodes[SELLER]
        since = [self.log_size(i) for i in range(N)]
        tid = tx_from_hex(tx_hex).rehash()
        try:
            a.sendrawtransaction(tx_hex)
        except JSONRPCException as e:
            self.record(case, "coalition mempool", e.error["message"][:120])
        x = self.produce(SELLER, ALL_KEYS, leaders=ALL_KEYS, txs_must=[tid])
        self.wait_until(lambda: all(self.log_has(i, why, since[i]) for i in HONEST), timeout=30)
        for i in HONEST:
            assert self.nodes[i].getbestblockhash() != x["hash"]
        line = self.log_has(HONEST[0], why, since[HONEST[0]])[0]
        self.record(case, "forced block %s at height %d" % (x["hash"][:12], x["height"]), line.split("] ", 2)[-1][:230])
        self.realign()
        return x

    def force_quiet(self, case, tx_hex, why="bad-txns-recorded-respend"):
        """As force_refused, but the transaction is never relayed: the coalition builds the block alone and
        hands it to an honest node, so that no honest mempool holds it afterwards."""
        a, n1 = self.nodes[SELLER], self.nodes[1]
        self.isolate()
        tid = tx_from_hex(tx_hex).rehash()
        try:
            a.sendrawtransaction(tx_hex)
        except JSONRPCException as e:
            if "already" not in e.error["message"]:
                self.record(case, "coalition mempool", e.error["message"][:120])
        h, _ = self.gen(a)
        assert tid in a.getblock(h)["tx"], "the forced transaction is not in the block"
        since = self.log_size(1)
        r = n1.submitblock(a.getblock(h, 0))
        line = self.log_has(1, why, since)
        self.record(case, "forced block at height %d" % a.getblockheader(h)["height"], "submitblock %r; %s" % (r, line[0].split("] ", 2)[-1][:150] if line else "NO LINE"))
        assert line and n1.getbestblockhash() != h
        a.invalidateblock(h)
        a.setnetworkactive(True)
        for j in range(1, N):
            self.connect_nodes(j, SELLER)
        self.wait_until(lambda: a.getbestblockhash() == n1.getbestblockhash(), timeout=60)

    def spendable(self, case, tx_hex):
        n1 = self.nodes[1]
        tid = tx_from_hex(tx_hex).rehash()
        try:
            n1.sendrawtransaction(tx_hex)
        except JSONRPCException as e:
            assert "already in block chain" in e.error["message"], e.error
        at = self.block_of(tid, n1)
        if at is None:
            at = self.produce(1, ALL_KEYS, txs_must=[tid])["height"]
        self.sync_tips(range(N))
        hdr = n1.getblockheader(n1.getblockhash(at))
        self.record(case, "spent by its owner", "%s at height %d, anchor %d" % (tid[:12], at, hdr["anchorheight"]))

    def anchor_to(self, h):
        b = self.btc()
        if b.getblockcount() < h:
            self.btc_mine(h - b.getblockcount())
            time.sleep(1.5)
        self.anchored_block(h)

    def put(self, case, data):
        h, r, since = self.put_record(data)
        n1 = self.nodes[1]
        bad = self.log_has(1, "carries a record that is not valid", since)
        ok = n1.getbestblockhash() == h
        self.record(case, "B with the record (%d bytes)" % len(data),
                    "submitblock %r; tip is B: %s; %s" % (r, ok, bad[0].split("Lost blocks: ", 1)[-1][:170] if bad else ""))
        return h, ok, since

    # ---- fab / mass
    def case_fab(self, extra_inputs=0, case="fab"):
        a, n1 = self.nodes[SELLER], self.nodes[1]
        txid, vout, spend_hex = self.victim_coin()
        f = self.tip(n1)
        ah, invented, btc = self.anchor_choice(f)
        ops = [(int(txid, 16), vout)] + [(int.from_bytes(os.urandom(32), "little"), i) for i in range(extra_inputs)]
        tf = self.invented_tx(ops)
        self.isolate()
        hdr, e, seats = self.fab_header(ah, invented, [0, 1, 2, 3], tx=tf)
        self.record(case, "invented lost header", "%s on F (height %d): anchor %d = %064x (no such block), %d seats; Tf %s spends %d outpoints, no witness" %
                    (hdr.hash[:12], f["height"], hdr.m_anchor_height, invented, seats, tf.hash[:12], len(ops)))
        if not STALE:
            try:
                self.btc().getblockheader("%064x" % invented)
                raise AssertionError("the invented anchor is known to bitcoind")
            except JSONRPCException as ex:
                self.record(case, "bitcoind on that anchor", ex.error["message"])
        data = self.mk([hdr.header_bytes()], [(0, e[0], e[1], e[2])], btc)
        h, ok, since = self.put(case, data)
        if not ok:
            return self.blocked(case, "the record is refused")
        lines = self.log_has(1, "bound input", since)
        self.record(case, "node 1", lines[0].split("Lost blocks: ", 1)[-1][:120])
        self.rejoin()
        self.sync_tips(range(N))
        if extra_inputs:
            self.record(case, "bytes per bound coin", "%.1f" % (len(data) / len(ops)))
        self.force_refused(case, spend_hex)
        # The exact end of the bind: B is the block at the lost height.
        a_r = n1.getblockheader(h)["anchorheight"]
        end = max(hdr.m_anchor_height, a_r) + W
        self.record(case, "bind", "lost anchor %d, rebuilt height's anchor %d, W %d: binds while the anchor <= %d" % (hdr.m_anchor_height, a_r, W, end))
        self.anchor_to(end)
        assert_equal(self.tip(n1)["anchorheight"], end)
        self.sync_tips(range(N))
        self.force_refused(case + "-at-end", spend_hex)
        self.anchor_to(end + 1)
        self.sync_tips(range(N))
        try:
            a.removeprunedfunds  # no-op reference
        except Exception:
            pass
        self.spendable(case, spend_hex)
        raise AssertionError("ATTACK SUCCEEDED (%s): the record froze the owner's coin until the lapse" % case)

    def case_mass(self):
        self.case_fab(extra_inputs=540, case="mass")

    # ---- grind
    def committee(self, seed):
        pks = [bytes.fromhex(p) for (_, p) in self.stakers]
        ranked = sorted(range(N), key=lambda i: int.from_bytes(sha(seed + pks[i]), "little"))
        return set(ranked[:3])

    @staticmethod
    def seed(anchor_int, height):
        return sha(ser_uint256(anchor_int) + int(height).to_bytes(4, "little"))

    def grind(self, target, height):
        tries = 0
        while True:
            tries += 1
            x = int.from_bytes(os.urandom(32), "little")
            if self.committee(self.seed(x, height)) == target:
                return x, tries

    def case_grind(self):
        n1 = self.nodes[1]
        a = self.nodes[SELLER]
        txid, vout, spend_hex = self.victim_coin()
        f = self.tip(n1)
        fh = f["height"]
        fa = int(f["anchorhash"], 16)
        C = self.committee(self.seed(fa, fh + 1))
        sched = n1.getposschedule()
        names = {p: i for i, (_, p) in enumerate(self.stakers)}
        real = {names[m if isinstance(m, str) else m.get("pubkey")] for m in sched["committee"]}
        self.record("grind", "replica of the committee at F+1", "%s; node says %s" % (sorted(C), sorted(real)))
        assert_equal(C, real)
        self.record("grind", "coalition", "the 3 of 6 stakers seated at height %d (quorum 3)" % (fh + 1))
        self.btc_mine(4)
        time.sleep(1.5)
        tf = self.invented_tx([(int(txid, 16), vout)])
        x0, t0 = self.grind(C, fh + 2)
        x1, t1 = self.grind(C, fh + 3)
        x2 = int.from_bytes(os.urandom(32), "little")
        self.record("grind", "ground anchors", "X0 in %d tries seats the coalition at %d; X1 in %d tries at %d" % (t0, fh + 2, t1, fh + 3))
        self.isolate()
        # Control: an anchor not ground: the coalition is not the next committee.
        xbad = int.from_bytes(os.urandom(32), "little")
        while self.committee(self.seed(xbad, fh + 2)) == C:
            xbad = int.from_bytes(os.urandom(32), "little")
        k0b, _, _ = self.fab_header(f["anchorheight"] + 1, xbad, sorted(C), connect=True)
        try:
            k1b, eb, sb = self.fab_header(f["anchorheight"] + 2, x1, sorted(C), tx=tf)
            ctrl = self.mk([k0b.header_bytes(), k1b.header_bytes()], [(1, eb[0], eb[1], eb[2])])
        except JSONRPCException as ex:
            ctrl, sb = None, ex.error["message"]
        self.record("grind", "control: unground X0, the coalition signs the next header", "seats %s" % sb)
        a.invalidateblock(k0b.hash)
        assert_equal(a.getbestblockhash(), f["hash"])
        k0, _, s0 = self.fab_header(f["anchorheight"] + 1, x0, sorted(C), connect=True)
        k1, _, s1 = self.fab_header(f["anchorheight"] + 2, x1, sorted(C), connect=True)
        k2, e2, s2 = self.fab_header(f["anchorheight"] + 3, x2, sorted(C), tx=tf)
        self.record("grind", "branch", "3 invented headers at heights %d..%d, seats %d, %d, %d, signed only by the coalition" % (fh + 1, fh + 3, s0, s1, s2))
        a.invalidateblock(k0.hash)
        if ctrl:
            _, okc, _ = self.put("grind-control", ctrl)
            assert not okc
            self.nodes[1].reconsiderblock(self.nodes[1].getbestblockhash())
        data = self.mk([k0.header_bytes(), k1.header_bytes(), k2.header_bytes()], [(2, e2[0], e2[1], e2[2])])
        h, ok, since = self.put("grind", data)
        if not ok:
            return self.blocked("grind", "the record of ground anchors is refused")
        self.record("grind", "node 1", self.log_has(1, "bound input", since)[0].split("Lost blocks: ", 1)[-1][:120])
        self.rejoin()
        self.sync_tips(range(N))
        self.force_refused("grind", spend_hex)
        raise AssertionError("ATTACK SUCCEEDED (grind): ground anchors certified a record")

    # ---- theft
    def case_theft(self):
        self.wallets()
        b, n1, a = self.btc(), self.nodes[1], self.nodes[SELLER]
        h0 = b.getblockcount()
        D = h0 + 3
        lock = self.make_lock(50_000_000, h0, D, D)
        self.send_lock(lock)
        self.produce(1, ALL_KEYS, txs_must=[lock["txid"]])
        self.sync_tips(range(N))
        pay = self.pay(lock)
        b.generatetoaddress(1, self.bbuyer.getnewaddress())
        p = b.getblockcount()
        self.anchored_block(p)
        c, pr = self.buyer_claim(lock, pay)
        self.record("theft", "swap", "lock %s, floor %d, D = R = %d; the buyer paid %s in Bitcoin block %d (<= D); her claim %s is valid" %
                    (lock["txid"][:12], h0, D, pay[:12], p, c.rehash()[:12]))
        assert n1.testmempoolaccept([c.serialize().hex()])[0]["allowed"]
        # The seller's refund, signed by him, not valid for a long while.
        r = self.refund(lock)
        assert not n1.testmempoolaccept([r.serialize().hex()])[0]["allowed"]
        # The chain reaches D at the pace of the network; F is the first block anchored at D.
        self.anchor_to(D)
        self.sync_tips(range(N))
        f = self.tip(n1)
        first_at_d = f["height"]
        ah, an, btc = self.anchor_choice(f)
        self.isolate()
        hdr, e, seats = self.fab_header(ah, an, [0, 1, 2, 3], tx=r)
        data = self.mk([hdr.header_bytes()], [(0, e[0], e[1], e[2])], btc)
        h, ok, since = self.put("theft", data)
        if not ok:
            self.rejoin()
            self.sync_tips(range(N))
            cid = n1.sendrawtransaction(c.serialize().hex())
            hc = self.produce(1, ALL_KEYS, txs_must=[cid])
            self.record("theft", "the buyer's claim", "mined at height %d" % hc["height"])
            return self.blocked("theft", "the record is refused; the buyer is paid")
        self.rejoin()
        self.sync_tips(range(N))
        self.record("theft", "record", "binds the lock to the seller's refund; lost anchor %d (invented), %d seats at one height" % (hdr.m_anchor_height, seats))
        # Sequentia blocks at 10 per Bitcoin block, the buyer's claim tried at every step of 10.
        c_hex = c.serialize().hex()
        tried = 0
        opened = None
        for i in range(1, 100):
            if i % 10 == 0:
                self.btc_mine(1)
                time.sleep(1.2)
            self.produce(1, ALL_KEYS)
            self.sync_tips(range(N))
            t = self.tip(n1)
            if i % 10 == 5:
                self.force_quiet("theft-claim-%d" % t["height"], c_hex)
                tried += 1
            ra = self.nodes[2].testmempoolaccept([r.serialize().hex()])[0]
            if i % 10 == 9:
                self.record("theft", "step %d" % i, "height %d anchor %d btc %d; refund: %s" % (t["height"], t["anchorheight"], self.btc().getblockcount(), ra.get("reject-reason", "allowed")))
            if ra["allowed"]:
                opened = t
                break
        assert opened, "the refund never opened"
        self.record("theft", "the claim", "refused by consensus in %d forced blocks over %d Sequentia blocks since the first anchored at D (height %d)" % (tried, opened["height"] - first_at_d, first_at_d))
        rid = self.nodes[2].sendrawtransaction(r.serialize().hex())
        hr = self.produce(2, ALL_KEYS, txs_must=[rid])
        self.sync_tips(range(N))
        wtx = self.bbuyer.gettransaction(pay)
        self.record("theft", "outcome", "the refund %s is mined at height %d, anchor %d; the lock is gone; the buyer's payment has %d confirmations" %
                    (rid[:12], hr["height"], hr["block"]["anchorheight"], wtx["confirmations"]))
        assert wtx["confirmations"] > 0
        cres = n1.testmempoolaccept([c_hex])[0]
        self.record("theft", "the buyer's claim now", str(cres.get("reject-reason")))
        assert not cres["allowed"]
        raise AssertionError("ATTACK SUCCEEDED (theft): the seller holds the asset and the payment")

    # ---- the duty path: evidence alone, no block of the coalition's
    def evidence_block(self, txs, anchor_h, anchor_int, signers, below=None):
        """A whole block on the coalition node's tip carrying `txs`, certified by `signers`, never connected."""
        a = self.nodes[SELLER]
        h, k = self.gen(a)
        raw = a.getblock(h, 0)
        a.invalidateblock(h)
        self.undo = getattr(self, "undo", []) + [h]
        blk = block_from_hex(raw)
        blk.vtx = [blk.vtx[0]] + list(txs)
        for t in blk.vtx:
            t.rehash()
        blk.hashMerkleRoot = uint256_from_str(merkle_root([ser_uint256(t.sha256) for t in blk.vtx]))
        blk.m_anchor_height, blk.m_anchor_hash = anchor_h, anchor_int
        blk.sha256 = None
        while below is not None:
            blk.sha256 = None
            blk.rehash()
            if blk.sha256 < below:
                break
            blk.nTime += 1
        blk.sha256 = None
        rc = a.recertifyposblock(blk.serialize().hex(), self.wif(k), [self.wif(s) for s in signers])
        return block_from_hex(rc["hex"])

    def show_evidence(self, blk, to=2, btc=()):
        from feature_pos_lost_attacks import ser_bytes, ser_cs
        class msg_poslost:
            msgtype = b"poslost"
            def __init__(self, b): self.b = b
            def serialize(self):
                if not V3:
                    return self.b.serialize()
                return self.b.serialize() + ser_cs(len(btc)) + b"".join(ser_bytes(h) for h in btc)
        peer = self.nodes[to].add_p2p_connection(LostP2PDataStore())
        peer.send_message(msg_poslost(blk))
        peer.sync_with_ping()
        self.nodes[to].disconnect_p2ps()

    def blank_wit(self, t):
        t.wit.vtxinwit = [CTxInWitness() for _ in t.vin]
        t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
        return t

    def case_evid(self):
        """One certified invented block, shown over P2P. The coalition leads nothing."""
        n1, n2 = self.nodes[1], self.nodes[2]
        txid, vout, spend_hex = self.victim_coin()
        f = self.tip(n1)
        ah, an, btc = self.anchor_choice(f)
        tf = self.blank_wit(self.invented_tx([(int(txid, 16), vout)]))
        self.isolate()
        blk = self.evidence_block([tf], ah, an, [0, 1, 2, 3])
        blk.rehash()
        self.show_evidence(blk, btc=btc)
        end = time.time() + 12
        while time.time() < end and not all(blk.hash in self.lost(i) for i in HONEST):
            time.sleep(0.25)
        if not all(blk.hash in self.lost(i) for i in HONEST):
            self.rejoin()
            self.sync_tips(range(N))
            self.spendable("evid", spend_hex)
            return self.blocked("evid", "no honest node takes the evidence")
        self.record("evid", "evidence", "invented block %s, Tf with no signature: held as lost by every honest node %s" % (blk.hash[:12], HONEST))
        try:
            n1.sendrawtransaction(spend_hex)
            self.record("evid", "the owner's spend, mempool", "accepted")
        except JSONRPCException as e:
            self.record("evid", "the owner's spend, mempool", e.error["message"][:140])
        since = self.log_size(1)
        h = self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
        recs = self.records_in(n1, h["hash"])
        self.record("evid", "next block, by honest nodes alone", "height %d carries %d record output(s) of %d bytes; %s" %
                    (h["height"], len(recs), sum(len(r["scriptPubKey"]["hex"]) // 2 for r in recs),
                     (self.log_has(1, "bound input", since) or ["no binding"])[0].split("Lost blocks: ", 1)[-1][:90]))
        if not recs:
            self.rejoin()
            self.sync_tips(range(N))
            self.spendable("evid", spend_hex)
            return self.blocked("evid", "held as evidence, but it binds nothing and no record is written")
        self.rejoin()
        self.sync_tips(range(N))
        self.force_quiet("evid", spend_hex, why="respend")
        raise AssertionError("ATTACK SUCCEEDED (evid): honest producers wrote a freezing record from evidence")

    def case_flood(self, siblings=3, per=620):
        """Sibling invented blocks of fee-free spends, one coin per block, all at one height."""
        a, n1 = self.nodes[SELLER], self.nodes[1]
        txid_v, vout_v, spend_hex = self.victim_coin()
        info = a.decodescript("51")
        addr = info["segwit"]["address"]
        spk = bytes.fromhex(info["segwit"]["hex"])
        funds = [a.sendtoaddress(address=addr, amount=Decimal("1"), fee_asset_label="bitcoin") for _ in range(siblings)]
        self.produce(SELLER, ALL_KEYS, txs_must=funds)
        self.sync_tips(range(N))
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(n1.dumpassetlabels()["bitcoin"])[::-1])
        f = self.tip(n1)
        ah, an, btc = self.anchor_choice(f)
        chain = []
        for fund in funds:
            n = [o["n"] for o in a.getrawtransaction(fund, 1)["vout"] if o["scriptPubKey"]["hex"] == spk.hex()][0]
            prev = (int(fund, 16), n)
            for _ in range(per):
                t = CTransaction()
                t.nVersion = 2
                t.vin.append(CTxIn(COutPoint(prev[0], prev[1]), nSequence=0xffffffff))
                t.vout.append(CTxOut(CTxOutValue(100000000), spk, asset))
                self.blank_wit(t)
                t.wit.vtxinwit[0].scriptWitness.stack = [b"\x51"]
                t.rehash()
                chain.append(t)
                prev = (t.sha256, 0)
        self.isolate()
        for j in range(siblings):
            blk = self.evidence_block(chain[j * per:(j + 1) * per], ah, an if STALE else int.from_bytes(os.urandom(32), "little"), [0, 1, 2, 3])
            blk.rehash()
            self.show_evidence(blk, btc=btc)
            end = time.time() + 15
            while time.time() < end and not all(blk.hash in self.lost(i) for i in HONEST):
                time.sleep(0.25)
            if not all(blk.hash in self.lost(i) for i in HONEST):
                self.rejoin()
                self.sync_tips(range(N))
                self.spendable("flood", spend_hex)
                return self.blocked("flood", "no honest node takes the evidence")
        self.record("flood", "evidence", "%d sibling invented blocks at height %d, %d fee-free spends, one coin per block, one certificate each" % (siblings, f["height"] + 1, siblings * per))
        forced = {t.hash for t in chain}
        vid = n1.sendrawtransaction(spend_hex)
        since = self.log_size(1)
        for step in range(siblings + 1):
            h = self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
            ids = h["block"]["tx"][1:]
            nf = sum(1 for x in ids if x in forced)
            self.record("flood", "honest block %d" % step, "height %d: %d forced fee-free, %d other; payer's transaction in it: %s" % (h["height"], nf, len(ids) - nf, vid in ids if vid else "n/a"))
            if vid and vid in ids:
                break
        times = [int(l.rsplit("classified in ", 1)[1].split()[0]) for l in self.log_has(1, "classified in", since)]
        self.record("flood", "plan time on node 1", "max %.0f ms over %d plans" % (max(times) / 1000, len(times)) if times else "none logged")
        # The bar: one sibling's transactions are forced, the rest only follow,
        # capped, so what pays is mined in the first honest block.
        first = self.flood_first_block(n1, since, forced, vid)
        assert first, "ATTACK SUCCEEDED (flood): the paying transaction waited behind forced siblings"

    def flood_first_block(self, node, since, forced, vid):
        """Whether the first honest block after the evidence carried the paying transaction."""
        for h in range(node.getblockcount(), 0, -1):
            ids = node.getblock(node.getblockhash(h))["tx"][1:]
            if vid in ids:
                prior = node.getblock(node.getblockhash(h - 1))["tx"][1:]
                return not any(x in forced for x in prior)
        return False

    # ---- question 2
    def fresh_claim(self, st, pay):
        c2, pr2 = self.buyer_claim(st["lock"], pay)
        return c2, pr2

    def case_carried(self):
        """A block that carries the re-proven claim but no record: what do holders say today?"""
        st = self.lost_claim("carried")
        n1, b = self.nodes[1], self.btc()
        a = self.nodes[SELLER]
        wtx = self.bbuyer.gettransaction(st["pay"])
        self.record("carried", "payment", "mined again, %s confirmations" % wtx["confirmations"])
        self.anchored_block(b.getblockcount() - wtx["confirmations"] + 1)
        self.sync_tips(HONEST)
        c2, _ = self.fresh_claim(st, st["pay"])
        assert_equal(c2.rehash(), st["c_id"])
        self.record("carried", "fresh proof", "same txid %s; wtxid differs: %s" % (st["c_id"][:12], c2.getwtxid() != st["c"].getwtxid()))
        since = [self.log_size(i) for i in range(N)]
        try:
            a.sendrawtransaction(c2.serialize().hex())
        except JSONRPCException as e:
            self.record("carried", "coalition mempool", e.error["message"][:100])
        x = self.produce(SELLER, ALL_KEYS, leaders=ALL_KEYS, txs_must=[st["c_id"]])
        time.sleep(3)
        lines = self.log_has(1, x["hash"], since[1])
        rf = [l for l in lines if "refusing" in l]
        self.record("carried", "honest holder on the block carrying it without a record",
                    (rf[0].split("Lost blocks: ", 1)[-1][:260] if rf else "accepted; tip is it: %s" % (n1.getbestblockhash() == x["hash"])))

    def case_powneg(self):
        """Headers that must not prove a lost anchor, each refused for its own reason."""
        from test_framework.messages import hash256
        n1, b = self.nodes[1], self.btc()
        txid, vout, _ = self.victim_coin()
        f = self.tip(n1)
        self.btc_mine(1)
        time.sleep(1.5)
        h = b.getblockcount()
        base = bytearray.fromhex(b.getblockheader(b.getblockhash(h), False))
        parent = bytes.fromhex(b.getblockhash(h - 1))[::-1]
        def nonce_for(prefix, high):
            hb = bytearray(prefix)
            for n in range(1 << 16):
                hb[76:80] = n.to_bytes(4, "little")
                v = int.from_bytes(hash256(bytes(hb)), "little")
                if (v >> 255 == 1) == high and bytes(hb) != bytes(base):
                    return bytes(hb), v
            raise AssertionError("no nonce")
        base2 = bytearray(base); base2[4:36] = parent
        weak, wv = nonce_for(base2, True)
        base3 = bytearray(base); base3[4:36] = os.urandom(32)
        loose, lv = nonce_for(base3, False)
        for name, anchor, btc, why in (("no header", int.from_bytes(os.urandom(32), "little"), [], "lost-anchor-unproven"),
                                       ("header below the target's work", wv, [weak], "btc-header-work"),
                                       ("header with an unknown parent", lv, [loose], "btc-header-unlinked")):
            self.isolate()
            hdr, e, _ = self.fab_header(h, anchor, [0, 1, 2, 3], tx=self.invented_tx([(int(txid, 16), vout)]))
            self.nodes[SELLER].setnetworkactive(True)
            _, ok, since = self.put("powneg", self.mk([hdr.header_bytes()], [(0, e[0], e[1], e[2])], btc))
            line = self.log_has(1, "carries a record that is not valid", since)
            self.record("powneg", name, line[-1].split("not valid: ", 1)[-1][:80] if line else "NO LINE")
            assert not ok and line and why in line[-1], (name, line)
            for j in range(2, N):
                self.connect_nodes(1, j)

    def records_since(self, fork):
        n1 = self.nodes[1]
        return sum(len(self.records_in(n1, n1.getblockhash(x))) for x in range(fork + 1, n1.getblockcount() + 1))

    def case_grace(self):
        """The payment is mined again: no record is written, and the claim returns inside the grace."""
        st = self.lost_claim("grace")
        n1 = self.nodes[1]
        fork = [b for b in n1.getlostblocks()["blocks"] if b["hash"] == st["LC"]["hash"]][0]["forkheight"]
        at = None
        for i in range(min(GRACE, 12)):
            h = self.produce(1, ALL_KEYS)
            if st["c_id"] in h["block"]["tx"]:
                at = h["height"]
                break
            if st["c_id"] not in n1.getrawmempool():
                time.sleep(2)
            if st["c_id"] not in n1.getrawmempool():
                try:
                    c2, _ = self.buyer_claim(st["lock"], st["pay"])
                    n1.sendrawtransaction(c2.serialize().hex())
                    self.record("grace", "claim", "resubmitted by the completion service")
                except JSONRPCException as e:
                    self.record("grace", "resubmit", e.error["message"][:100])
        self.record("grace", "OUTCOME", "claim mined at height %s (fork %d), record outputs written since the fork: %d" % (at, fork, self.records_since(fork)))
        assert at is not None
        assert_equal(self.records_since(fork), 0)

    def case_gracegone(self):
        """The payment is gone: the duty holds the lock during the grace, then the record does."""
        st = self.lost_claim("gracegone", gone=True)
        n1 = self.nodes[1]
        fork = [b for b in n1.getlostblocks()["blocks"] if b["hash"] == st["LC"]["hash"]][0]["forkheight"]
        rhex = self.refund(st["lock"]).serialize().hex()
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        assert_equal(self.records_since(fork), 0)
        self.force_quiet("gracegone-in-grace", rhex, why="bad-txns-lost-respend")
        while n1.getblockcount() < fork + GRACE:
            self.produce(1, ALL_KEYS)
        assert_equal(self.records_since(fork), 0)
        h = self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        self.record("gracegone", "first block past the grace", "height %d (fork %d, grace %d): %d record output(s)" % (h["height"], fork, GRACE, len(self.records_in(n1, h["hash"]))))
        assert self.records_since(fork) >= 1
        self.force_quiet("gracegone-after", rhex, why="respend")

    def case_disp(self):
        """A sibling with a lower hash takes the carry of an honest lost block: the seller's respend must stay refused."""
        st = self.gap("disp")
        a, n1 = self.nodes[SELLER], self.nodes[1]
        hdr = n1.getblockheader(st["L"])
        assert_equal(a.getbestblockhash(), hdr["previousblockhash"])
        self.isolate()
        blk = self.evidence_block([], hdr["anchorheight"], int(hdr["anchorhash"], 16), [0, 1, 2, 3], below=int(st["L"], 16))
        blk.rehash()
        assert blk.sha256 < int(st["L"], 16)
        self.show_evidence(blk, btc=self.stale_chain(hdr["anchorhash"]))
        self.wait_until(lambda: all(blk.hash in self.lost(i) for i in HONEST), timeout=30)
        self.record("disp", "sibling", "%s, same parent and anchor as L %s, lower hash, 4 seats, carries nothing" % (blk.hash[:12], st["L"][:12]))
        self.force_quiet("disp", st["r_hex"], why="bad-txns-lost-respend")
        try:
            n1.sendrawtransaction(st["t_hex"])
        except JSONRPCException as e:
            self.record("disp", "T to the mempool", e.error["message"][:90])
        at = None
        for _ in range(4):
            h = self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
            if st["t_id"] in h["block"]["tx"]:
                at = h["height"]
                break
        self.record("disp", "OUTCOME", "T mined at height %s; R in the honest chain: %s" % (at, self.block_of(st["r_id"], n1)))
        assert at is not None and self.block_of(st["r_id"], n1) is None

    def case_keyless(self):
        """Attack on the fix: a claim leaf with no buyer key. The coalition mines one stale Bitcoin block holding its own
        payment to the seller's script, records its own claim on it, and holds the real buyer out until the refund."""
        from test_framework.key import compute_xonly_pubkey, generate_privkey
        from test_framework.address import program_to_witness
        from test_framework.script import CScript
        self.wallets()
        b, n1, a = self.btc(), self.nodes[1], self.nodes[SELLER]
        h0 = b.getblockcount()
        D = h0 + 3
        sk_s = generate_privkey()
        pk_s = compute_xonly_pubkey(sk_s)[0]
        desc = self.bseller.getdescriptorinfo("wsh(and_v(v:pk(%s),older(1)))" % ("02" + pk_s.hex()))["descriptor"]
        addr = self.bseller.deriveaddresses(desc)[0]
        info = self.bseller.getaddressinfo(addr)
        program, version = bytes.fromhex(info["witness_program"]), info["witness_version"]
        sat = 50_000_000
        claim = CScript([bp.OP_INSPECTBTCPAYMENT, bp.OP_DROP, bp.OP_DUP, h0, bp.OP_GREATERTHAN, bp.OP_VERIFY, D, bp.OP_LESSTHANOREQUAL,
                         bp.OP_VERIFY, bp.FIELD_OUTPUT, bp.OP_INSPECTBTCTX, version, bp.OP_EQUALVERIFY, program, bp.OP_EQUALVERIFY,
                         bp.le8(sat), bp.OP_GREATERTHANOREQUAL64])
        refund = CScript([D, bp.OP_CHECKANCHORHEIGHTVERIFY, bp.OP_DROP, pk_s, bp.OP_CHECKSIG])
        tap = bp.taproot_construct(bp.NUMS, [("claim", claim), ("refund", refund)])
        lock_addr = program_to_witness(1, tap.scriptPubKey[2:])
        funded = a.fundrawtransaction(a.createrawtransaction([], [{lock_addr: Decimal("10")}]),
                                      {"fee_asset": "bitcoin", "replaceable": True, "change_type": "bech32"})["hex"]
        t = tx_from_hex(a.signrawtransactionwithwallet(funded)["hex"])
        txid = t.rehash()
        vout = [i for i, o in enumerate(t.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        lock = dict(sk_b=sk_s, sk_s=sk_s, tap=tap, claim=claim, refund=refund, txid=txid, vout=vout, txout=t.vout[vout], addr=addr,
                    amount=sat, floor=h0, deadline=D, refund_at=D, asset_amount=10 * 100000000)
        n1.sendrawtransaction(t.serialize().hex())
        self.produce(1, ALL_KEYS, txs_must=[txid])
        self.sync_tips(range(N))
        lock["buyer_spk"] = bytes.fromhex(n1.getaddressinfo(n1.getnewaddress())["scriptPubKey"])
        def keyless(record, pay_vout, spk=None):
            c = self.spend_lock(lock, "claim", [record], [bp.scriptnum(pay_vout)], out_spk=spk or lock["buyer_spk"])
            c.wit.vtxinwit[0].scriptWitness.stack = [bp.scriptnum(pay_vout), bytes(claim),
                                                     c.wit.vtxinwit[0].scriptWitness.stack[-1]]
            c.rehash()
            return c
        pay = self.bbuyer.sendtoaddress(addr, Decimal(sat) / Decimal(100000000))
        b.generatetoaddress(1, self.bbuyer.getnewaddress())
        p = b.getblockcount()
        self.anchored_block(p)
        pr = self.proof_for(pay, self.coin_anchor(txid))
        real = keyless(pr["record"], self.vout_paying(pay, addr, pr["blockhash"]))
        assert n1.testmempoolaccept([real.serialize().hex()])[0]["allowed"], "the buyer's claim is not valid"
        self.record("keyless", "swap", "keyless claim leaf; the buyer paid %s in block %d (<= D = %d); her claim %s is valid" % (pay[:12], p, D, real.rehash()[:12]))
        self.anchor_to(D - 1)
        self.sync_tips(range(N))
        f = self.tip(n1)
        first = f["height"]
        # The stale block, holding the coalition's own payment, never on Bitcoin's best chain.
        rawp = self.bbuyer.fundrawtransaction(self.bbuyer.createrawtransaction([], [{addr: Decimal(sat) / Decimal(100000000)}]))["hex"]
        rawp = self.bbuyer.signrawtransactionwithwallet(rawp)["hex"]
        s = b.generateblock(self.bbuyer.getnewaddress(), [rawp])["hash"]
        s_raw = bytes.fromhex(b.getblockheader(s, False))
        fake = b.decoderawtransaction(rawp)["txid"]
        pf = self.proof_for(fake, self.coin_anchor(txid), blockhash=s, link=False)
        fv = [o["n"] for o in b.decoderawtransaction(rawp)["vout"] if o["scriptPubKey"].get("address") == addr][0]
        # The coalition's claim pays the coalition, as an open offer's claim pays the payer's own script.
        forged = keyless(pf["record"], fv, bytes.fromhex(a.getaddressinfo(a.getnewaddress())["scriptPubKey"]))
        assert forged.rehash() != real.rehash()
        b.invalidateblock(s)
        b.generateblock(self.bbuyer.getnewaddress(), [])
        time.sleep(1.5)
        assert b.getblockhash(D) != s
        self.isolate()
        hdr, e, seats = self.fab_header(D, int(s, 16), [0, 1, 2, 3], tx=forged)
        if YIELD:
            ev = self.evidence_block([forged], D, int(s, 16), [0, 1, 2, 3])
            ev.rehash()
            self.show_evidence(ev, btc=[s_raw])
            self.wait_until(lambda: all(ev.hash in self.lost(i) for i in HONEST), timeout=30)
            self.record("keyless", "evidence", "the same forged claim is also held as lost by every honest node")
        # Evidence creates no duty to record, so the coalition writes it itself:
        # the forged claim is valid here but for its proof.
        data = self.mk([hdr.header_bytes()], [(0, e[0], e[1], e[2])], [s_raw] if V3 else [])
        h, ok, since = self.put("keyless", data)
        if not ok:
            return self.blocked("keyless", "the record is refused")
        self.rejoin()
        self.sync_tips(range(N))
        self.record("keyless", "record", "binds the lock to the coalition's claim on stale block %s (%d seats at one height)" % (s[:12], seats))
        r = self.refund(lock)
        tried, opened = 0, None
        for i in range(1, 100):
            if i % 10 == 0:
                b.generateblock(self.bbuyer.getnewaddress(), [])
                time.sleep(1.2)
            self.produce(1, ALL_KEYS)
            self.sync_tips(range(N))
            tn = self.tip(n1)
            if i % 10 == 5 and not YIELD:
                self.force_quiet("keyless-claim-%d" % tn["height"], real.serialize().hex())
                tried += 1
            nexth = tn["height"] + 1
            floor_open = nexth >= bp.FLOOR_DEPTH and n1.getblockheader(n1.getblockhash(nexth - bp.FLOOR_DEPTH))["anchorheight"] >= D
            # With the evidence held the mempool refuses the refund until the lapse, so the floor is read from the chain.
            if (floor_open if YIELD else self.nodes[2].testmempoolaccept([r.serialize().hex()])[0]["allowed"]):
                opened = tn
                break
        assert opened, "the refund never opened"
        if YIELD:
            end_y = max(D, n1.getblockheader(h)["anchorheight"]) + W
            self.record("keyless", "at the forced refund", "height %d, anchor %d <= %d (still bound); the floor is open" % (opened["height"], opened["anchorheight"], end_y))
            assert opened["anchorheight"] <= end_y
            # A refund dressed with a valid proof of the buyer's payment: another leaf, so it must stay bound.
            dressed = self.spend_lock(lock, "refund", [pr["record"], bp.floor_record(D)], [])
            self.force_raw("keyless-refund-with-proof", dressed, "bad-txns-recorded-respend")
            cid = n1.sendrawtransaction(real.serialize().hex())
            hc = self.produce(1, ALL_KEYS, txs_must=[cid])
            self.sync_tips(HONEST)
            self.record("keyless", "OUTCOME", "BLOCKED: the buyer's claim %s, same leaf with a real proof, passes the binding and is mined at height %d" % (cid[:12], hc["height"]))
            assert self.block_of(r.rehash(), n1) is None
            return
        # The refund is bound too (to the coalition's claim): it opens at the lapse, and whoever leads that block decides.
        a_r = n1.getblockheader(h)["anchorheight"]
        end = max(D, a_r) + W
        self.record("keyless", "bind", "the refund's floor is open at height %d, but the lock is bound while the anchor <= %d" % (opened["height"], end))
        while self.tip(n1)["anchorheight"] < end:
            self.anchor_to(self.tip(n1)["anchorheight"] + 1)
        self.sync_tips(range(N))
        self.force_quiet("keyless-at-end", real.serialize().hex())
        self.stop_node(SELLER)
        self.start_node(SELLER, extra_args=self.node_args(SELLER) + ["-persistmempool=0"])
        for j in range(1, N):
            self.connect_nodes(j, SELLER)
        self.wait_until(lambda: a.getbestblockhash() == n1.getbestblockhash(), timeout=60)
        self.btc_mine(end + 1 - b.getblockcount())
        time.sleep(1.5)
        self.isolate()
        rid = a.sendrawtransaction(r.serialize().hex())
        hh, _ = self.gen(a)
        assert rid in a.getblock(hh)["tx"]
        res = n1.submitblock(a.getblock(hh, 0))
        took = n1.getbestblockhash() == hh
        self.record("keyless", "OUTCOME", "%s: the first block past the bind, led by the coalition, carries the refund %s (anchor %d): submitblock %r; honest tip: %s; the real claim was refused in %d forced blocks; the buyer's payment has %d confirmations" %
                    ("STOLEN" if took else "NOT TAKEN", rid[:12], a.getblockheader(hh)["anchorheight"], res, took, tried + 1, self.bbuyer.gettransaction(pay)["confirmations"]))
        assert took
        raise AssertionError("ATTACK SUCCEEDED (keyless): the open offer was stolen")

    # ---- the second reviewer's probes
    def anyone_spend(self, txid, vout, spk, asset, copies):
        t = CTransaction()
        t.nVersion = 2
        for _ in range(copies):
            t.vin.append(CTxIn(COutPoint(int(txid, 16), vout), nSequence=0xffffffff))
        fee = 100000
        t.vout.append(CTxOut(CTxOutValue(100000000 * copies - fee), spk, asset))
        t.vout.append(CTxOut(CTxOutValue(fee), b"", asset))
        self.blank_wit(t)
        for w in t.wit.vtxinwit:
            w.scriptWitness.stack = [b"\x51"]
        t.rehash()
        return t

    def case_duplicate(self):
        """A transaction no block can carry must bind nothing."""
        case = "duplicate"
        a, n1 = self.nodes[SELLER], self.nodes[1]
        info = a.decodescript("51")["segwit"]
        spk = bytes.fromhex(info["hex"])
        txid = a.sendtoaddress(address=info["address"], amount=Decimal("1"), fee_asset_label="bitcoin")
        self.produce(SELLER, ALL_KEYS, txs_must=[txid])
        self.sync_tips(range(N))
        vout = [o["n"] for o in a.getrawtransaction(txid, 1)["vout"] if o["scriptPubKey"]["hex"] == spk.hex()][0]
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(n1.dumpassetlabels()["bitcoin"])[::-1])
        evil = self.anyone_spend(txid, vout, spk, asset, 2)
        honest = self.anyone_spend(txid, vout, spk, asset, 1)
        ah, an, btc = self.anchor_choice(self.tip(n1))
        self.isolate()
        hdr, entry, seats = self.fab_header(ah, an, [0, 1, 2, 3], tx=evil)
        data = self.mk([hdr.header_bytes()], [(0, entry[0], entry[1], entry[2])], btc)
        _, ok, since = self.put(case, data)
        line = self.log_has(1, "carries a record that is not valid", since)
        if ok:
            raise AssertionError("ATTACK SUCCEEDED (duplicate): a transaction no block can carry was recorded")
        assert line and "bad-txns-inputs-duplicate" in line[-1], line
        self.record(case, "refused for", line[-1].split("not valid: ", 1)[-1][:90])
        self.rejoin()
        self.sync_tips(range(N))
        self.spendable(case, honest.serialize().hex())
        self.blocked(case, "the record is refused and the owner's spend is mined")

    def case_siblingrespend(self):
        """A certified sibling of an honest lost block carries the seller's respend."""
        case = "siblingrespend"
        st = self.gap(case)
        a, n1 = self.nodes[SELLER], self.nodes[1]
        hdr = n1.getblockheader(st["L"])
        assert_equal(a.getbestblockhash(), hdr["previousblockhash"])
        self.isolate()
        blk = self.evidence_block([tx_from_hex(st["r_hex"])], hdr["anchorheight"], int(hdr["anchorhash"], 16),
                                  [0, 1, 2, 3], below=int(st["L"], 16))
        blk.rehash()
        assert blk.sha256 < int(st["L"], 16)
        self.show_evidence(blk, btc=self.stale_chain(hdr["anchorhash"]))
        self.wait_until(lambda: all(blk.hash in self.lost(i) for i in HONEST), timeout=30)
        h = self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
        self.sync_tips(HONEST)
        t_at, r_at = self.block_of(st["t_id"], n1), self.block_of(st["r_id"], n1)
        self.record(case, "outcome", "honest block at height %d: T mined at %s, the seller's respend at %s" % (h["height"], t_at, r_at))
        if r_at is not None or t_at is None:
            raise AssertionError("ATTACK SUCCEEDED (siblingrespend): the seller's respend replaced the honest lost transaction")
        self.blocked(case, "the honest lost transaction is mined")

    def case_recordrace(self):
        """T is not due at the first rebuilt height (its locktime), so no duty names it there. A coalition
        block at that height records the sibling's respend R: the first binding would then shut T out."""
        case = "recordrace"
        a, n1 = self.nodes[SELLER], self.nodes[1]
        coin = self.fund()
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        r = tx_from_hex(r_hex)
        self.btc_mine(1)
        time.sleep(1.5)
        x = self.btc().getblockcount()
        P = self.produce(1, ALL_KEYS)
        fork = P["height"] - 1
        raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"], "sequence": 0xfffffffd}],
                                     [{n1.getnewaddress(): coin["amount"] - Decimal("0.0001")}, {"fee": Decimal("0.0001")}], fork + 1)
        t_hex = a.signrawtransactionwithwallet(raw)["hex"]
        t_id = tx_from_hex(t_hex).rehash()
        n1.sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        anchor = n1.getblockheader(L["hash"])["anchorhash"]
        assert_equal(n1.getblockheader(L["hash"])["anchorheight"], x)
        self.isolate()
        a.invalidateblock(P["hash"])
        hdr, e, _ = self.fab_header(x, int(anchor, 16), [0, 1, 2, 3], tx=r)
        a.reconsiderblock(P["hash"])
        self.rejoin()
        self.sync_tips(range(N))
        stale = self.stale_after_reorg(anchor)
        self.wait_until(lambda: all(L["hash"] in self.lost(i) for i in HONEST), timeout=60)
        self.wait_until(lambda: n1.getblockcount() == fork, timeout=30)
        _, ok, since = self.put(case, self.mk([hdr.header_bytes()], [(0, e[0], e[1], e[2])], stale))
        if ok:
            raise AssertionError("ATTACK SUCCEEDED (recordrace): a record bound the coin to the sibling's respend")
        line = self.log_has(1, "bad-txns-lost-respend", since)
        assert line and "a record binds" in line[0], line
        self.record(case, "refused for", line[0].split("Lost blocks: ", 1)[-1][:150])
        self.rejoin()
        self.sync_tips(HONEST)
        for _ in range(3):
            if self.block_of(t_id, n1) is not None:
                break
            self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
        t_at, r_at = self.block_of(t_id, n1), self.block_of(r.rehash(), n1)
        assert t_at is not None and r_at is None, (t_at, r_at)
        self.blocked(case, "the record is refused; T is mined at height %d" % t_at)

    def hand_block(self, i, tx):
        """A block on node i's tip carrying only `tx` (or nothing), all keys signing, not connected anywhere."""
        n = self.nodes[i]
        hh, k = self.gen(n)
        blk = block_from_hex(n.getblock(hh, 0))
        blk.vtx = [blk.vtx[0]] + ([tx] if tx is not None else [])
        cb = blk.vtx[0]
        # The coinbase claims nothing: this block's fees differ from the template's.
        keep = [j for j, o in enumerate(cb.vout) if o.nValue.getAmount() == 0]
        wits = cb.wit.vtxoutwit
        cb.vout = [cb.vout[j] for j in keep]
        if wits:
            cb.wit.vtxoutwit = [wits[j] for j in keep]
        ci = [j for j, o in enumerate(cb.vout) if o.scriptPubKey[:6] == bytes.fromhex("6a24aa21a9ed")][-1]
        fu.node_fastmerkle = n
        wroot = bytes.fromhex(calcfastmerkleroot([t.calc_witness_hash() for t in blk.vtx]))[::-1]
        cb.vout[ci].scriptPubKey = b"\x6a\x24\xaa\x21\xa9\xed" + hash256(wroot + cb.wit.vtxinwit[0].scriptWitness.stack[0])
        for t in blk.vtx:
            t.rehash()
        blk.hashMerkleRoot = uint256_from_str(merkle_root([ser_uint256(t.sha256) for t in blk.vtx]))
        blk.sha256 = None
        n.invalidateblock(hh)
        rc = n.recertifyposblock(blk.serialize().hex(), self.wif(k), [self.wif(j) for j in ALL_KEYS])
        return rc["hex"], rc["hash"]

    def drop_peers(self, i):
        n = self.nodes[i]
        for p in n.getpeerinfo():
            n.disconnectnode(nodeid=p["id"])
        self.wait_until(lambda: n.getconnectioncount() == 0, timeout=20)

    def holds(self, i):
        return {b["hash"]: b["own"] for b in self.nodes[i].getlostblocks()["blocks"]}

    def case_evidmempool(self):
        """What stands of an evidence-only block goes back to the mempool with no priority."""
        case = "evidmempool"
        n1 = self.nodes[1]
        coin = self.fund()
        e = tx_from_hex(self.spend(coin, n1.getnewaddress(), Decimal("0.001")))
        e_id = e.rehash()
        ah, an, btc = self.anchor_choice(self.tip(n1))
        self.isolate()
        blk = self.evidence_block([e], ah, an, [0, 1, 2, 3])
        blk.rehash()
        self.show_evidence(blk, btc=btc)
        self.wait_until(lambda: all(blk.hash in self.lost(i) for i in HONEST), timeout=30)
        self.wait_until(lambda: e_id in n1.getrawmempool(), timeout=30)
        fees = n1.getmempoolentry(e_id)["fees"]
        assert_equal(fees["base"], fees["modified"])
        self.record(case, "mempool", "the evidence transaction is pooled with fees %s, no priority" % fees["base"])
        h = self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
        assert e_id in h["block"]["tx"]
        self.blocked(case, "mined as an ordinary transaction at height %d" % h["height"])

    def capflood(self, case, signers):
        """Node 3 holds the honest block only as evidence; a coalition floods siblings of it."""
        n1, n3 = self.nodes[1], self.nodes[3]
        t_hex, r_hex, t_id, r_id = self.coin_and_spends()
        self.drop_peers(3)
        self.btc_mine(1)
        n1.sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        self.produce(1, ALL_KEYS)
        self.wait_until(lambda: all(self.nodes[i].getbestblockhash() == n1.getbestblockhash() for i in (0, 1, 2, 4, 5)), timeout=30)
        hdr = n1.getblockheader(L["hash"])
        raw_l = block_from_hex(n1.getblock(L["hash"], 0))
        stale = self.stale_after_reorg(hdr["anchorhash"])
        self.wait_until(lambda: all(L["hash"] in self.lost(i) for i in (1, 2, 4)), timeout=60)
        self.wait_until(lambda: self.nodes[SELLER].getblockcount() < L["height"], timeout=30)
        self.drop_peers(SELLER)
        raw_l.rehash()
        self.show_evidence(raw_l, to=3, btc=stale)
        self.wait_until(lambda: L["hash"] in self.holds(3), timeout=30)
        hx, _ = self.hand_block(SELLER, tx_from_hex(r_hex))
        def verdict():
            try:
                return "signs it: %r" % n3.testproposedblock(hx)
            except JSONRPCException as e:
                return "refuses: %s" % e.error["message"][:60]
        before = verdict()
        assert before.startswith("refuses: bad-txns-lost-respend"), before
        for _ in range(12):
            dummy = self.blank_wit(self.invented_tx([(int.from_bytes(os.urandom(32), "little"), 0)]))
            b = self.evidence_block([dummy], hdr["anchorheight"], int(hdr["anchorhash"], 16), signers)
            self.show_evidence(b, to=3, btc=stale)
        time.sleep(4)
        after = verdict()
        self.record(case, "outcome", "12 siblings by %d seat(s): node 3 holds %d blocks; before: %s; after: %s" % (len(signers), len(self.holds(3)), before, after))
        return after, len(self.holds(3))

    def case_capflood1(self):
        after, held = self.capflood("capflood1", [0])
        if not after.startswith("refuses: bad-txns-lost-respend"):
            raise AssertionError("ATTACK SUCCEEDED (capflood1): one seat lifted a node's restraint")
        assert held <= 1 + 2, held
        self.blocked("capflood1", "one seat cannot lift the restraint")

    def case_capflood4(self):
        after, held = self.capflood("capflood4", [0, 1, 2, 3])
        assert after.startswith("signs it"), after
        assert_equal(held, W + 1)
        self.record("capflood4", "OUTCOME", "a quorum that signs past the cap lifts the restraint, as the design states")

    def case_stream(self):
        """A quorum at one height streams certified siblings of an honest lost block."""
        case = "stream"
        st = self.gap(case)
        n1 = self.nodes[1]
        hdr = n1.getblockheader(st["L"])
        stale = self.stale_chain(hdr["anchorhash"])
        self.isolate()
        since = self.log_size(1)
        for _ in range(30):
            dummy = self.blank_wit(self.invented_tx([(int.from_bytes(os.urandom(32), "little"), 0)]))
            s = self.evidence_block([dummy], hdr["anchorheight"], int(hdr["anchorhash"], 16), [0, 1, 2, 3])
            self.show_evidence(s, btc=stale)
            time.sleep(1.2)
            try:
                n1.getnewblockhex()
            except JSONRPCException as e:
                assert "bad-posvrf-missing" in e.error["message"], e.error
        time.sleep(3)
        held = sum(1 for b in n1.getlostblocks()["blocks"] if not b["own"])
        self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
        plans = self.log_has(1, "plan computed", since)
        t_at, r_at = self.block_of(st["t_id"], n1), self.block_of(st["r_id"], n1)
        self.record(case, "OUTCOME", "30 siblings: node 1 holds %d, %d plan(s) computed; T mined at %s, R at %s" % (held, len(plans), t_at, r_at))
        assert_equal(held, W + 1)
        assert_equal(len(plans), 1)
        assert t_at is not None and r_at is None

    def case_rebind(self):
        """The first binding stands: a second record cannot move the coin to another transaction."""
        case = "rebind"
        a, n1 = self.nodes[SELLER], self.nodes[1]
        info = a.decodescript("51")["segwit"]
        spk = bytes.fromhex(info["hex"])
        txid = a.sendtoaddress(address=info["address"], amount=Decimal("1"), fee_asset_label="bitcoin")
        self.produce(SELLER, ALL_KEYS, txs_must=[txid])
        self.sync_tips(range(N))
        vout = [o["n"] for o in a.getrawtransaction(txid, 1)["vout"] if o["scriptPubKey"]["hex"] == spk.hex()][0]
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(n1.dumpassetlabels()["bitcoin"])[::-1])
        t1 = self.anyone_spend(txid, vout, spk, asset, 1)
        t2 = self.anyone_spend(txid, vout, b"\x00\x14" + bytes(20), asset, 1)
        assert t1.rehash() != t2.rehash()
        ah, an, btc = self.anchor_choice(self.tip(n1))
        self.isolate()
        h1, e1, _ = self.fab_header(ah, an, [0, 1, 2, 3], tx=t1)
        h2, e2, _ = self.fab_header(ah, an, [0, 1, 2, 3], tx=t2)
        _, ok, _ = self.put(case + "-first", self.mk([h1.header_bytes()], [(0, e1[0], e1[1], e1[2])], btc))
        assert ok, "the first record was refused"
        _, ok, since = self.put(case + "-second", self.mk([h2.header_bytes()], [(0, e2[0], e2[1], e2[2])], btc))
        line = self.log_has(1, "carries a record that is not valid", since)
        if ok:
            raise AssertionError("ATTACK SUCCEEDED (rebind): a later record moved a bound coin")
        assert line and "entry-conflicts-with-a-binding" in line[-1], line
        self.record(case, "second record refused for", line[-1].split("not valid: ", 1)[-1][:70])
        for j in range(2, N):
            self.connect_nodes(1, j)
        self.rejoin()
        self.sync_tips(range(N))
        self.force_raw(case + "-other-tx", t2, "bad-txns-recorded-respend")
        self.spendable(case, t1.serialize().hex())
        self.blocked(case, "the first binding stands: T2 is refused and T1 is mined")

    def sibling_outcome(self, case, t_id, r_id):
        n1 = self.nodes[1]
        h = self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
        self.sync_tips(HONEST)
        t_at, r_at = self.block_of(t_id, n1), self.block_of(r_id, n1)
        self.record(case, "outcome", "honest block at height %d: T mined at %s, the seller's respend at %s" % (h["height"], t_at, r_at))
        if r_at is not None or t_at is None:
            raise AssertionError("ATTACK SUCCEEDED (%s): the seller's respend replaced the honest lost transaction" % case)
        self.blocked(case, "the honest lost transaction is mined")

    def coin_and_spends(self):
        a, n1 = self.nodes[SELLER], self.nodes[1]
        coin = self.fund()
        t_hex = self.spend(coin, n1.getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        return t_hex, r_hex, tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()

    def case_siblingbranch(self):
        case = "siblingbranch"
        a, n1 = self.nodes[SELLER], self.nodes[1]
        t_hex, r_hex, t_id, r_id = self.coin_and_spends()
        self.btc_mine(1)
        x = self.btc().getblockcount()
        P = self.produce(1, ALL_KEYS)
        n1.sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        hl = n1.getblockheader(L["hash"])
        assert_equal(P["block"]["anchorheight"], x)
        assert_equal(hl["anchorheight"], x)
        self.isolate()
        a.invalidateblock(L["hash"])
        sib = self.evidence_block([tx_from_hex(r_hex)], x, int(hl["anchorhash"], 16), [0, 1, 2, 3], below=int(L["hash"], 16))
        sib.rehash()
        a.reconsiderblock(L["hash"])
        self.rejoin()
        self.sync_tips(range(N))
        stale = self.stale_after_reorg(hl["anchorhash"])
        self.wait_until(lambda: all(L["hash"] in self.lost(i) and P["hash"] in self.lost(i) for i in HONEST), timeout=60)
        self.show_evidence(sib, btc=stale)
        self.wait_until(lambda: all(sib.hash in self.lost(i) for i in HONEST), timeout=30)
        self.sibling_outcome(case, t_id, r_id)

    def stale_after_reorg(self, anchor):
        self.bitcoin_reorg(1)
        return self.stale_chain(anchor)

    def case_siblingbefore(self):
        case = "siblingbefore"
        self.wallets()
        b, n1 = self.btc(), self.nodes[1]
        t_hex, r_hex, t_id, r_id = self.coin_and_spends()
        self.btc_mine(1)
        x = b.getblockcount()
        tip = b.getbestblockhash()
        # A real block at the same height, left off Bitcoin's best chain.
        b.invalidateblock(tip)
        side = b.generateblock(self.bbuyer.getnewaddress(), [])["hash"]
        side_raw = bytes.fromhex(b.getblockheader(side, False))
        b.invalidateblock(side)
        b.reconsiderblock(tip)
        assert_equal(b.getbestblockhash(), tip)
        time.sleep(1.5)
        self.isolate()
        sib = self.evidence_block([tx_from_hex(r_hex)], x, int(side, 16), [0, 1, 2, 3])
        sib.rehash()
        self.rejoin()
        self.sync_tips(range(N))
        n1.sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        assert_equal(L["block"]["anchorheight"], x)
        self.show_evidence(sib, btc=[side_raw])
        self.wait_until(lambda: all(sib.hash in self.lost(i) for i in HONEST), timeout=30)
        self.bitcoin_reorg(1)
        self.wait_until(lambda: all(L["hash"] in self.lost(i) for i in HONEST), timeout=60)
        self.sibling_outcome(case, t_id, r_id)

    def case_gracetwo(self):
        """A quorum at two consecutive heights certifies a refund of a lock whose claim is coming back."""
        case = "gracetwo"
        st = self.lost_claim(case, gone=False)
        n1 = self.nodes[1]
        self.anchored_block(self.btc().getblockcount())
        fresh, _ = self.buyer_claim(st["lock"], st["pay"])
        verdict = n1.testmempoolaccept([fresh.serialize().hex()])[0]
        assert verdict["allowed"], verdict
        refund = self.refund(st["lock"])
        rid = refund.rehash()
        self.isolate()
        self.stop_node(SELLER)
        self.start_node(SELLER, extra_args=self.node_args(SELLER) + ["-persistmempool=0"])
        a = self.nodes[SELLER]
        a.setnetworkactive(False)
        try:
            a.sendrawtransaction(refund.serialize().hex())
            x = self.produce(SELLER, [0, 1, 2, 3], leaders=[SELLER], txs_must=[rid])
            y = self.produce(SELLER, [0, 1, 2, 3], leaders=[SELLER])
        except (JSONRPCException, AssertionError) as e:
            self.record(case, "coalition", "cannot build the refund's block: %s" % str(e)[:140])
            return self.blocked(case, "the chain's record already binds the lock")
        n1.submitheader(a.getblockheader(x["hash"], False))
        n1.submitheader(a.getblockheader(y["hash"], False))
        since = self.log_size(1)
        ry = n1.submitblock(a.getblock(y["hash"], 0))
        rx = n1.submitblock(a.getblock(x["hash"], 0))
        r_at = self.block_of(rid, n1)
        refused = self.log_has(1, "bad-txns-recorded-respend", since)
        self.record(case, "headers first", "submitblock X %r, Y %r; refund mined at %s; %s" %
                    (rx, ry, r_at, refused[0].split("] ", 2)[-1][:150] if refused else "no record refusal"))
        if r_at is not None:
            raise AssertionError("ATTACK SUCCEEDED (gracetwo): the refund was mined while the buyer's claim was valid")
        assert refused, "refused for another reason than the record"
        self.blocked(case, "the record refuses the refund on every node")

    def case_branchleaf(self):
        """The yield needs an executed read of the proof: one leaf with a claim branch and a
        seller's key branch. A record binds the coin to a claim; the seller then spends through
        the same leaf's key branch, carrying a real proof that no executed opcode reads."""
        from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr
        from test_framework.address import program_to_witness
        from test_framework.script import CScript, OP_CHECKSIG, OP_ELSE, OP_ENDIF, OP_IF, SIGHASH_DEFAULT, TaprootSignatureHash
        case = "branchleaf"
        self.wallets()
        b, n1, a = self.btc(), self.nodes[1], self.nodes[SELLER]
        h0 = b.getblockcount()
        D = h0 + 40
        sk_s = generate_privkey()
        pk_s = compute_xonly_pubkey(sk_s)[0]
        desc = self.bseller.getdescriptorinfo("wsh(and_v(v:pk(%s),older(1)))" % ("02" + pk_s.hex()))["descriptor"]
        addr = self.bseller.deriveaddresses(desc)[0]
        info = self.bseller.getaddressinfo(addr)
        program, version = bytes.fromhex(info["witness_program"]), info["witness_version"]
        sat = 50_000_000
        leaf = CScript([OP_IF, pk_s, OP_CHECKSIG, OP_ELSE, bp.OP_INSPECTBTCPAYMENT, bp.OP_DROP, bp.OP_DUP, h0, bp.OP_GREATERTHAN,
                        bp.OP_VERIFY, D, bp.OP_LESSTHANOREQUAL, bp.OP_VERIFY, bp.FIELD_OUTPUT, bp.OP_INSPECTBTCTX, version,
                        bp.OP_EQUALVERIFY, program, bp.OP_EQUALVERIFY, bp.le8(sat), bp.OP_GREATERTHANOREQUAL64, OP_ENDIF])
        tap = bp.taproot_construct(bp.NUMS, [("claim", leaf)])
        lock_addr = program_to_witness(1, tap.scriptPubKey[2:])
        funded = a.fundrawtransaction(a.createrawtransaction([], [{lock_addr: Decimal("10")}]),
                                      {"fee_asset": "bitcoin", "replaceable": True, "change_type": "bech32"})["hex"]
        t = tx_from_hex(a.signrawtransactionwithwallet(funded)["hex"])
        txid = t.rehash()
        vout = [i for i, o in enumerate(t.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        lock = dict(sk_b=sk_s, sk_s=sk_s, tap=tap, claim=leaf, txid=txid, vout=vout, txout=t.vout[vout], addr=addr,
                    amount=sat, floor=h0, deadline=D, refund_at=D, asset_amount=10 * 100000000)
        n1.sendrawtransaction(t.serialize().hex())
        self.produce(1, ALL_KEYS, txs_must=[txid])
        self.sync_tips(range(N))
        control = bytes([tap.leaves["claim"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["claim"].merklebranch
        buyer_spk = bytes.fromhex(n1.getaddressinfo(n1.getnewaddress())["scriptPubKey"])
        def by_claim(record, pay_vout, spk):
            c = self.spend_lock(lock, "claim", [record], [], out_spk=spk)
            c.wit.vtxinwit[0].scriptWitness.stack = [bp.scriptnum(pay_vout), b"", bytes(leaf), control]
            c.rehash()
            return c
        def by_key(record, spk):
            c = self.spend_lock(lock, "claim", [record], [], out_spk=spk)
            sighash = TaprootSignatureHash(c, [lock["txout"]], SIGHASH_DEFAULT, self.genesis, 0, scriptpath=True, script=leaf)
            c.wit.vtxinwit[0].scriptWitness.stack = [sign_schnorr(sk_s, sighash), b"\x01", bytes(leaf), control]
            c.rehash()
            return c
        # A payment in a block Bitcoin then replaces: the coalition's claim, valid here but for its proof.
        rawp = self.bbuyer.fundrawtransaction(self.bbuyer.createrawtransaction([], [{addr: Decimal(sat) / Decimal(100000000)}]))["hex"]
        rawp = self.bbuyer.signrawtransactionwithwallet(rawp)["hex"]
        s = b.generateblock(self.bbuyer.getnewaddress(), [rawp])["hash"]
        s_raw = bytes.fromhex(b.getblockheader(s, False))
        pay = b.decoderawtransaction(rawp)["txid"]
        pf = self.proof_for(pay, self.coin_anchor(txid), blockhash=s, link=False)
        pv = [o["n"] for o in b.decoderawtransaction(rawp)["vout"] if o["scriptPubKey"].get("address") == addr][0]
        recorded = by_claim(pf["record"], pv, buyer_spk)
        height_s = b.getblockheader(s)["height"]
        b.invalidateblock(s)
        b.generateblock(self.bbuyer.getnewaddress(), [])
        b.generateblock(self.bbuyer.getnewaddress(), [])
        time.sleep(1.5)
        assert b.getblockhash(height_s) != s
        self.isolate()
        hdr, e, seats = self.fab_header(height_s, int(s, 16), [0, 1, 2, 3], tx=recorded)
        ev = self.evidence_block([recorded], height_s, int(s, 16), [0, 1, 2, 3])
        ev.rehash()
        self.show_evidence(ev, btc=[s_raw])
        self.wait_until(lambda: all(ev.hash in self.lost(i) for i in HONEST), timeout=30)
        _, ok, _ = self.put(case, self.mk([hdr.header_bytes()], [(0, e[0], e[1], e[2])], [s_raw]))
        assert ok, "the record of the claim was refused"
        self.rejoin()
        self.sync_tips(range(N))
        # A real proof for the key branch to carry: the same payment, mined on the best chain.
        pay2 = self.bbuyer.sendtoaddress(addr, Decimal(sat) / Decimal(100000000))
        b.generatetoaddress(1, self.bbuyer.getnewaddress())
        self.anchored_block(b.getblockcount())
        pr2 = self.proof_for(pay2, self.coin_anchor(txid))
        seller_spk = bytes.fromhex(a.getaddressinfo(a.getnewaddress())["scriptPubKey"])
        unread = by_key(pr2["record"], seller_spk)
        self.force_raw("branchleaf-key-branch", unread, "bad-txns-recorded-respend")
        # The claim branch of the same leaf, with that real proof, passes the binding.
        real = by_claim(pr2["record"], self.vout_paying(pay2, addr, pr2["blockhash"]), buyer_spk)
        cid = n1.sendrawtransaction(real.serialize().hex())
        hc = self.produce(1, ALL_KEYS, txs_must=[cid])
        self.record(case, "OUTCOME", "BLOCKED: the key branch with an unread proof is refused; the claim branch is mined at height %d" % hc["height"])

    def force_raw(self, case, tx, why):
        """A block carrying `tx` built by hand: for what the mempool's policy refuses and consensus must judge."""
        from test_framework.messages import CTxOutWitness, hash256
        from test_framework.util import calcfastmerkleroot
        import test_framework.util as fu
        n1 = self.nodes[1]
        self.disconnect_all(1)
        hh, k = self.gen(n1)
        blk = block_from_hex(n1.getblock(hh, 0))
        blk.vtx.append(tx)
        cb = blk.vtx[0]
        ci = [j for j, o in enumerate(cb.vout) if o.scriptPubKey[:6] == bytes.fromhex("6a24aa21a9ed")][-1]
        fu.node_fastmerkle = n1
        wroot = bytes.fromhex(calcfastmerkleroot([t.calc_witness_hash() for t in blk.vtx]))[::-1]
        nonce = cb.wit.vtxinwit[0].scriptWitness.stack[0]
        cb.vout[ci].scriptPubKey = b"\x6a\x24\xaa\x21\xa9\xed" + hash256(wroot + nonce)
        for t in blk.vtx:
            t.rehash()
        blk.hashMerkleRoot = uint256_from_str(merkle_root([ser_uint256(t.sha256) for t in blk.vtx]))
        blk.sha256 = None
        n1.invalidateblock(hh)
        since = self.log_size(1)
        rc = n1.recertifyposblock(blk.serialize().hex(), self.wif(k), [self.wif(j) for j in ALL_KEYS])
        r = n1.submitblock(rc["hex"])
        line = self.log_has(1, why, since)
        self.record(case, "block forced by hand", "submitblock %r; %s" % (r, line[0].split("] ", 2)[-1][:170] if line else "NO LINE"))
        assert line and n1.getbestblockhash() != rc["hash"]
        for j in range(2, N):
            self.connect_nodes(1, j)
        self.connect_nodes(1, SELLER)
        self.sync_tips(HONEST)

    def case_rbf(self):
        """The payment is replaced (another txid): the claim keeps its id with a proof of the replacement."""
        self.wallets()
        b, n1 = self.btc(), self.nodes[1]
        h0 = b.getblockcount()
        lock = self.make_lock(50_000_000, h0, h0 + 50, h0 + 1000)
        self.send_lock(lock)
        self.produce(1, ALL_KEYS, txs_must=[lock["txid"]])
        self.sync_tips(range(N))
        pay = self.pay(lock)
        ph = b.generatetoaddress(1, self.bbuyer.getnewaddress())[0]
        self.anchored_block(b.getblockcount())
        c, _ = self.buyer_claim(lock, pay)
        b.invalidateblock(ph)
        bumped = self.bbuyer.bumpfee(pay)["txid"]
        assert bumped != pay
        b.generatetoaddress(2, self.bbuyer.getnewaddress())
        time.sleep(2)
        self.anchored_block(b.getblockcount())
        self.sync_tips(HONEST)
        c2, pr2 = self.buyer_claim(lock, bumped)
        self.record("rbf", "payment replaced", "%s -> %s" % (pay[:12], bumped[:12]))
        self.record("rbf", "claim", "txid unchanged: %s; no signature redone: %s" %
                    (c2.rehash() == c.rehash(), c2.wit.vtxinwit[0].scriptWitness.stack[-3] == c.wit.vtxinwit[0].scriptWitness.stack[-3]))

    def case_ownmark(self):
        """Two certified siblings connected in turn: the one on the chain when Bitcoin took the anchor is the node's own."""
        self.log.info("(ownmark) own means on the chain when Bitcoin took it")
        n = self.nodes[1]
        coin = self.fund()
        t_hex = self.spend(coin, n.getnewaddress(), Decimal("0.0001"))
        u_hex = self.spend(coin, n.getnewaddress(), Decimal("0.001"))
        t_id, u_id = tx_from_hex(t_hex).rehash(), tx_from_hex(u_hex).rehash()
        self.disconnect_all(1)
        self.btc_mine(1)
        n.sendrawtransaction(t_hex)
        s = self.produce(1, ALL_KEYS, txs_must=[t_id])
        # A certified block leaves a chain while Bitcoin keeps its anchor only by
        # an operator's hand or by reconciliation: fork choice never replaces it.
        n.invalidateblock(s["hash"])
        n.sendrawtransaction(u_hex)
        l = self.produce(1, ALL_KEYS, txs_must=[u_id])
        assert_equal(l["height"], s["height"])
        assert_equal(l["block"]["anchorheight"], s["block"]["anchorheight"])
        self.record("ownmark", "siblings", "S %s (T) left the chain with its anchor on Bitcoin; L %s (U) on the chain" % (s["hash"][:12], l["hash"][:12]))
        self.bitcoin_reorg(1)
        self.wait_until(lambda: all(h in self.lost(1) for h in (s["hash"], l["hash"])), timeout=60)
        marks = {h: self.lost(1)[h]["own"] for h in (s["hash"], l["hash"])}
        self.record("ownmark", "own marks", {"S": marks[s["hash"]], "L": marks[l["hash"]]})
        b = self.produce(1, ALL_KEYS)
        txs = n.getblock(b["hash"])["tx"]
        self.record("ownmark", "outcome", "next block carries %s first" % ("U, the transaction of the block on the chain" if txs[1] == u_id else "T, of the block that had left" if txs[1] == t_id else txs[1:]))
        assert_equal(marks[l["hash"]], True)
        assert_equal(marks[s["hash"]], False)
        assert_equal(txs[1], u_id)
        assert t_id not in txs

    def case_ownflip(self):
        """A block held as evidence that returns to the chain and is then taken by Bitcoin is the node's own, across a restart."""
        self.log.info("(ownflip) evidence that was on the chain when Bitcoin took it")
        n = self.nodes[1]
        coin = self.fund()
        t_hex = self.spend(coin, n.getnewaddress(), Decimal("0.0001"))
        u_hex = self.spend(coin, n.getnewaddress(), Decimal("0.001"))
        t_id, u_id = tx_from_hex(t_hex).rehash(), tx_from_hex(u_hex).rehash()
        self.disconnect_all(1)
        # A slow watcher, so the store still holds the block when it reconnects.
        self.restart_node(1, extra_args=self.node_args(1) + ["-anchorpollinterval=30"])
        n.loadwallet("w")
        self.btc_mine(1)
        self.wait_until(lambda: n.getbestblockhash() is not None, timeout=10)
        n.sendrawtransaction(t_hex)
        s = self.produce(1, ALL_KEYS, txs_must=[t_id])
        n.invalidateblock(s["hash"])
        n.sendrawtransaction(u_hex)
        l = self.produce(1, ALL_KEYS, txs_must=[u_id])
        n.invalidateblock(l["hash"])
        n.reconsiderblock(s["hash"])
        assert_equal(n.getbestblockhash(), s["hash"])
        since = self.log_size(1)
        self.bitcoin_reorg(1)
        self.wait_until(lambda: all(h in self.lost(1) for h in (s["hash"], l["hash"])), timeout=90)
        assert self.log_has(1, "%s, held as evidence, was on this chain" % s["hash"], since), "the store had dropped S before Bitcoin moved: rerun"
        marks = {h: self.lost(1)[h]["own"] for h in (s["hash"], l["hash"])}
        self.record("ownflip", "own marks", {"S (on the chain)": marks[s["hash"]], "L (left it)": marks[l["hash"]]})
        assert_equal(marks[s["hash"]], True)
        assert_equal(marks[l["hash"]], False)
        self.restart_node(1, extra_args=self.node_args(1))
        n.loadwallet("w")
        self.wait_until(lambda: all(h in self.lost(1) for h in (s["hash"], l["hash"])), timeout=60)
        after = {h: self.lost(1)[h]["own"] for h in (s["hash"], l["hash"])}
        self.record("ownflip", "own marks after a restart", {"S": after[s["hash"]], "L": after[l["hash"]]})
        assert_equal(after, marks)
        b = self.produce(1, ALL_KEYS)
        txs = n.getblock(b["hash"])["tx"]
        self.record("ownflip", "outcome", "next block carries %s first" % ("T" if txs[1] == t_id else "U" if txs[1] == u_id else txs[1:]))
        assert_equal(txs[1], t_id)

    def run_test(self):
        try:
            for c in CASES:
                try:
                    getattr(self, "case_" + c)()
                    if c in OPEN:
                        self.record(c, "OPEN CASE NOW PASSES", "remove it from OPEN")
                except AssertionError as e:
                    if c not in OPEN or os.environ.get("LOST_STRICT") or "ATTACK SUCCEEDED" not in str(e):
                        raise
                    self.record(c, "OPEN", "%s; %s" % (OPEN[c], e))
        finally:
            import json
            out = os.environ.get("LOST_RESULTS", "")
            if out:
                with open(out, "w") as fh:
                    json.dump(self.results, fh, indent=1)
            self.stop_bitcoind()


if __name__ == '__main__':
    LostRecordAttacksTest().main()
