#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Attacks on the lost-block rule (doc 03 §3, "Transactions of a lost block")
around a swap of a Sequentia coin for on-chain bitcoin, each asserting the
outcome the rule must give.

Cases (LOST_CASES, one per process):
  hx    an honest record block is re-certified (for a version-1 record, with
        its Bitcoin headers stripped first); after the window, a node synced
        from nothing on a Bitcoin daemon that never saw the lost block's anchor
        reaches the network's tip;
  flip  a record block is judged while the node's Bitcoin daemon has gone back
        to the lost branch, and then forward again: the node ends on the
        network's chain and never calls the block's record bad;
  fp    a sale lock funded from a payment-proof claim certified in the same
        block: the buyer's ancestry check (checklockancestry) calls it not
        payable and names the claim; a lock funded from an ordinary coin is
        payable;
  big   two claims lost for good in one block, the first too large to record
        even alone: it is skipped and logged, and the second is recorded;
  cap   a sale lock funded through 1,000 ordinary transactions from the
        seller's own payment-proof claim: the ancestry walk runs out of budget
        before the claim, and the lock is not payable, for that reason, in
        checklockancestry and in checklock; the one-call check calls an
        ordinary, final lock payable.
"""

import json
import os
import shutil
import subprocess
import time
import urllib.parse
from decimal import Decimal

import feature_btc_payment_proof as bp
from feature_pos_lost_swap import LostSwapTest
from feature_pos_lost_blocks import SELLER, HONEST, LATE, ALL_KEYS, N, W, BITCOIND, CHAIN
from test_framework.address import program_to_witness
from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.messages import hash256, tx_from_hex
from test_framework.util import assert_equal, p2p_port, rpc_port

CASES = os.environ.get("LOST_CASES", "hx").split(",")
RECORD_PREFIX = "6a0453514c52"  # OP_RETURN 'SQLR'


def read_cs(b, i):
    x = b[i]
    if x < 0xfd:
        return x, i + 1
    if x == 0xfd:
        return int.from_bytes(b[i + 1:i + 3], "little"), i + 3
    if x == 0xfe:
        return int.from_bytes(b[i + 1:i + 5], "little"), i + 5
    return int.from_bytes(b[i + 1:i + 9], "little"), i + 9


def push(data):
    n = len(data)
    if n < 0x4c:
        return bytes([n]) + data
    if n <= 0xff:
        return b"\x4c" + bytes([n]) + data
    if n <= 0xffff:
        return b"\x4d" + n.to_bytes(2, "little") + data
    return b"\x4e" + n.to_bytes(4, "little") + data


def parse_record_script(spk):
    assert spk[:6] == bytes.fromhex(RECORD_PREFIX), spk[:6].hex()
    op = spk[6]
    if op < 0x4c:
        n, i = op, 7
    elif op == 0x4c:
        n, i = spk[7], 8
    elif op == 0x4d:
        n, i = int.from_bytes(spk[7:9], "little"), 9
    else:
        n, i = int.from_bytes(spk[7:11], "little"), 11
    data = spk[i:i + n]
    assert i + n == len(spk)
    return data


def strip_headers(data):
    """A version-1 record (u8 version, vector<bytes> btc_headers, lost, entries) without its Bitcoin headers."""
    assert data[0] == 1, data[0]
    version = data[0:1]
    count, i = read_cs(data, 1)
    headers = []
    for _ in range(count):
        ln, i = read_cs(data, i)
        headers.append(data[i:i + ln])
        i += ln
    return version + b"\x00" + data[i:], headers


def merkle_root(leaves):
    level = list(leaves)
    while len(level) > 1:
        if len(level) & 1:
            level.append(level[-1])
        level = [hash256(level[k] + level[k + 1]) for k in range(0, len(level), 2)]
    return level[0]


class LostSwapAttacksTest(LostSwapTest):
    def set_test_params(self):
        super().set_test_params()

    # ------------------------------------------------------- a fresh daemon
    def btc2(self):
        url = "http://%s:%s@127.0.0.1:%d" % (urllib.parse.quote(self.b2_user), urllib.parse.quote(self.b2_pass), self.b2_port)
        return AuthServiceProxy(url, timeout=120)

    def start_fresh_bitcoind(self):
        """A second Bitcoin daemon that learns daemon 1's best chain only."""
        self.b2_port = rpc_port(N + 4)
        p2p = p2p_port(N + 4)
        d = os.path.join(self.options.tmpdir, "btc2")
        os.makedirs(d)
        self.b2_proc = subprocess.Popen([BITCOIND, "-datadir=" + d, "-regtest", "-port=%d" % p2p, "-rpcport=%d" % self.b2_port,
                                         "-listen=1", "-bind=127.0.0.1", "-server", "-printtoconsole=0"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.log.info("fresh bitcoind pid %d", self.b2_proc.pid)
        cookie = os.path.join(d, "regtest", ".cookie")
        self.wait_until(lambda: os.path.exists(cookie), timeout=60)
        self.b2_user, self.b2_pass = open(cookie).read().strip().split(":", 1)

        def up():
            try:
                return self.btc2().getblockcount() >= 0
            except Exception:
                return False
        self.wait_until(up, timeout=60)
        self.btc().addnode("127.0.0.1:%d" % p2p, "onetry")
        self.wait_until(lambda: self.btc2().getbestblockhash() == self.btc().getbestblockhash(), timeout=120)

    def stop_fresh_bitcoind(self):
        if getattr(self, "b2_proc", None) and self.b2_proc.poll() is None:
            try:
                self.btc2().stop()
                self.b2_proc.wait(timeout=60)
            except Exception:
                self.b2_proc.terminate()
                self.b2_proc.wait(timeout=60)

    def args_with_daemon2(self, i):
        out = []
        for a in self.node_args(i):
            if a.startswith("-mainchainrpcport="):
                a = "-mainchainrpcport=%d" % self.b2_port
            elif a.startswith("-mainchainrpcuser="):
                a = "-mainchainrpcuser=%s" % self.b2_user
            elif a.startswith("-mainchainrpcpassword="):
                a = "-mainchainrpcpassword=%s" % self.b2_pass
            out.append(a)
        return out

    def lapse_end(self, lc_hash):
        """The Bitcoin height above which a lost block's protection lapses on node 1's chain:
        W above max(the lost anchor, the anchor of the block at its height)."""
        n1 = self.nodes[1]
        hdr = n1.getblockheader(lc_hash)
        at = n1.getblockheader(n1.getblockhash(hdr["height"]))["anchorheight"]
        return max(hdr["anchorheight"], at) + W

    def pass_window(self, lc_hash):
        b = self.btc()
        # The chain rebuilds the lost block's height first.
        while self.nodes[1].getblockcount() <= self.nodes[1].getblockheader(lc_hash)["height"]:
            self.produce(1, ALL_KEYS)
        end = self.lapse_end(lc_hash)
        if b.getblockcount() <= end:
            b.generatetoaddress(end + 1 - b.getblockcount(), self.bbuyer.getnewaddress())
        self.anchored_block(end + 1)
        for _ in range(3):
            self.produce(1, ALL_KEYS)
        return end

    # ------------------------------------------------------------------ hx
    def case_hx(self):
        self.log.info("(hx) a record block re-certified (v1: without its Bitcoin headers); a fresh node on a fresh daemon syncs it")
        st = self.lost_claim("hx", gone=True)
        n1 = self.nodes[1]
        lc_hdr = n1.getblockheader(st["LC"]["hash"])
        stale_anchor = lc_hdr["anchorhash"]
        # Node 1 alone assembles the block that owes the record.
        self.disconnect_all(1)
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        blk2 = n1.getblock(h["hash"], 2)
        cb = blk2["tx"][0]
        recs = [(k, o) for k, o in enumerate(cb["vout"]) if o["scriptPubKey"]["hex"].startswith(RECORD_PREFIX)]
        assert_equal(len(recs), 1)
        k, o = recs[0]
        data = parse_record_script(bytes.fromhex(o["scriptPubKey"]["hex"]))
        version = data[0]
        raw = bytes.fromhex(n1.getblock(h["hash"], 0))
        headers = []
        if version == 1:
            # The attack: the same record without its Bitcoin headers, under the same keys.
            new_data, headers = strip_headers(data)
            assert len(headers) >= 1
            cbtx = tx_from_hex(cb["hex"])
            assert_equal(cbtx.rehash(), cb["txid"])
            cbtx.vout[k].scriptPubKey = b"\x6a\x04SQLR" + push(new_data)
            new_cb_hex = cbtx.serialize().hex()
            old_cb = bytes.fromhex(cb["hex"])
            assert raw.count(old_cb) == 1
            txids = [t["txid"] for t in blk2["tx"]]
            assert_equal(merkle_root([bytes.fromhex(t)[::-1] for t in txids]), raw[36:68])
            txids[0] = tx_from_hex(new_cb_hex).rehash()
            new_raw = raw[:36] + merkle_root([bytes.fromhex(t)[::-1] for t in txids]) + raw[68:]
            new_raw = new_raw.replace(old_cb, bytes.fromhex(new_cb_hex))
        else:
            # Version 2 carries no Bitcoin headers: nothing to strip; the block is kept as produced.
            new_raw = raw
        self.record("hx", "honest record", "block %s h %d: version %d, %d bytes, %d Bitcoin header(s) stripped" %
                    (h["hash"][:12], h["height"], version, len(data), len(headers)))
        n1.invalidateblock(h["hash"])
        rc = n1.recertifyposblock(new_raw.hex(), self.stakers[SELLER][0], [self.stakers[j][0] for j in ALL_KEYS if j != SELLER])
        if rc["hash"] == h["hash"]:
            n1.reconsiderblock(h["hash"])
        r = n1.submitblock(rc["hex"])
        x = rc["hash"]
        self.record("hx", "re-certified block", "%s (%d seats) submitted on node 1: %r" % (x[:12], rc["countersignatures"], r))
        assert_equal(n1.getbestblockhash(), x)
        for j in [0, 2, 3, 4]:
            self.connect_nodes(1, j)
        self.sync_tips([0, 1, 2, 3, 4])
        assert all(self.in_chain(self.nodes[j], x) for j in [0, 1, 2, 3, 4])
        # The window ends; the holders drop the lost block, so no evidence of it is relayed.
        end = self.pass_window(st["LC"]["hash"])
        self.sync_tips([0, 1, 2, 3, 4])
        self.wait_until(lambda: all(not self.lost(i) for i in HONEST), timeout=90)
        self.record("hx", "after the window", "Bitcoin %d > %d; honest nodes hold no lost block; tip height %d" %
                    (self.btc().getblockcount(), end, n1.getblockcount()))
        # A Bitcoin daemon that never saw the lost block's anchor.
        self.start_fresh_bitcoind()
        try:
            self.btc2().getblockheader(stale_anchor)
            known = True
        except JSONRPCException:
            known = False
        assert not known, "the fresh daemon knows the stale anchor"
        # Node 5 synced from nothing on that daemon.
        self.stop_node(LATE)
        shutil.rmtree(os.path.join(self.nodes[LATE].datadir, CHAIN))
        self.start_node(LATE, extra_args=self.args_with_daemon2(LATE))
        late = self.nodes[LATE]
        for j in HONEST:
            self.connect_nodes(LATE, j)
        try:
            self.wait_until(lambda: late.getbestblockhash() == n1.getbestblockhash(), timeout=120)
            ok = True
        except AssertionError:
            ok = False
        bad = self.log_has(LATE, "bad-lost-record") + self.log_has(LATE, "carries a record that is not valid")
        tips = [(t["height"], t["hash"][:12], t["status"]) for t in late.getchaintips()][:4]
        self.record("hx", "node 5 (fresh daemon)", "at the network's tip: %s; height %d (network %d); tips %s" %
                    (ok, late.getblockcount(), n1.getblockcount(), tips))
        self.record("hx", "node 5 bad-lost-record lines", [l.split("] ", 1)[-1][:260] for l in bad[:2]])
        if not ok and headers:
            # Diagnosis only: the stripped headers taught to the fresh daemon by hand.
            for hd in headers:
                self.btc2().submitheader(hd.hex())
            late.reconsiderblock(x)
            try:
                self.wait_until(lambda: late.getbestblockhash() == n1.getbestblockhash(), timeout=60)
                ctl = True
            except AssertionError:
                ctl = False
            self.record("hx", "control (headers taught by hand): node 5 reaches the tip", ctl)
        assert ok, "node 5 on a fresh daemon did not reach the network's tip"
        assert not bad, bad[0]

    # ---------------------------------------------------------------- flip
    def case_flip(self):
        self.log.info("(flip) a record judged while the daemon has gone back, then forward again")
        self.lost_claim("flip", gone=True)
        b = self.btc()
        n1, late = self.nodes[1], self.nodes[LATE]
        lc = n1.getblockheader(self.flip_lc)
        stale = lc["anchorhash"]
        a_first = b.getblockhash(lc["anchorheight"])
        self.start_fresh_bitcoind()
        self.btc2().submitblock(b.getblock(stale, 0))
        self.btc2().setnetworkactive(False)
        self.stop_node(LATE)
        shutil.rmtree(os.path.join(late.datadir, CHAIN))
        args = [a for a in self.args_with_daemon2(LATE) if not a.startswith("-anchorpollinterval=")] + ["-anchorpollinterval=600"]
        self.start_node(LATE, extra_args=args)
        for j in HONEST:
            self.connect_nodes(LATE, j)
        self.sync_tips(range(N))
        self.disconnect_all(LATE)
        # B: the honest block that records the claim, on the same anchor as its parent.
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        recs = self.records_in(n1, h["hash"])
        assert_equal(len(recs), 1)
        self.sync_tips([0, 1, 2, 3, 4])
        r0 = late.submitheader(n1.getblockheader(h["hash"], False))
        self.btc2().invalidateblock(a_first)
        self.record("flip", "daemon 2 back on the lost branch", "best is the stale anchor: %s" % (self.btc2().getbestblockhash() == stale))
        r = late.submitblock(n1.getblock(h["hash"], 0))
        self.record("flip", "node 5 takes B", "submitheader %r, submitblock %r" % (r0, r))
        self.btc2().reconsiderblock(a_first)
        self.btc2().setnetworkactive(True)
        self.wait_until(lambda: self.btc2().getbestblockhash() == b.getbestblockhash(), timeout=60)
        self.stop_node(LATE)
        args = [a for a in args if not a.startswith("-anchorpollinterval=")] + ["-anchorpollinterval=1"]
        self.start_node(LATE, extra_args=args)
        for _ in range(3):
            self.produce(1, ALL_KEYS)
        for j in HONEST:
            self.connect_nodes(LATE, j)
        try:
            self.wait_until(lambda: late.getbestblockhash() == n1.getbestblockhash(), timeout=60)
            ok = True
        except AssertionError:
            ok = False
        bad = self.log_has(LATE, "bad-lost-record")
        tips = [(t["height"], t["hash"][:12], t["status"]) for t in late.getchaintips()][:4]
        self.record("flip", "outcome", "node 5 on the network's chain: %s; height %d (network %d); tips %s" %
                    (ok, late.getblockcount(), n1.getblockcount(), tips))
        self.record("flip", "node 5 bad-lost-record lines", [l.split("] ", 1)[-1][:260] for l in bad[:2]])
        assert ok, "node 5 is not on the network's chain"
        assert not bad, bad[0]

    def lost_claim(self, case, refund_at_now=True, gone=False):
        st = super().lost_claim(case, refund_at_now=refund_at_now, gone=gone)
        self.flip_lc = st["LC"]["hash"]
        return st

    # ------------------------------------------------------------------ fp
    def case_fp(self):
        self.log.info("(fp) a sale lock funded from a claim certified with it: not payable")
        self.wallets()
        b = self.btc()
        n1, a = self.nodes[1], self.nodes[SELLER]
        b.createwallet("sself")
        sself = self.btc("sself")
        self.bbuyer.sendtoaddress(sself.getnewaddress(), Decimal("1"))
        b.generatetoaddress(1, self.bbuyer.getnewaddress())
        h0 = b.getblockcount()
        lock0 = self.make_lock(10_000, h0, h0 + 50, h0)
        self.send_lock(lock0)
        self.produce(1, ALL_KEYS, txs_must=[lock0["txid"]])
        for _ in range(bp.FLOOR_DEPTH + 1):
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        sp = sself.sendtoaddress(lock0["addr"], Decimal("0.0001"))
        p_hash = b.generatetoaddress(1, self.bbuyer.getnewaddress())[0]
        self.anchored_block(b.getblockcount())
        seller_spk = a.getaddressinfo(a.getnewaddress())["scriptPubKey"]
        lock0["buyer_spk"] = bytes.fromhex(seller_spk)
        pr = self.proof_for(sp, self.coin_anchor(lock0["txid"]), blockhash=p_hash)
        P = self.claim(lock0, pr["record"], self.vout_paying(sp, lock0["addr"], p_hash), out_spk=lock0["buyer_spk"])
        p_id = P.rehash()
        n1.sendrawtransaction(P.serialize().hex())
        p_out = [o for o in n1.decoderawtransaction(P.serialize().hex())["vout"] if o["scriptPubKey"]["hex"] == seller_spk][0]
        lockS = self.make_lock(50_000_000, h0, h0 + 50, h0 + 1000)
        lock_addr = program_to_witness(1, lockS["tap"].scriptPubKey[2:])
        v = Decimal(str(p_out["value"]))
        raw = a.createrawtransaction([{"txid": p_id, "vout": p_out["n"]}], [{lock_addr: v - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        t_hex = a.signrawtransactionwithwallet(raw, [{"txid": p_id, "vout": p_out["n"], "scriptPubKey": seller_spk, "amount": v}])["hex"]
        t_id = n1.sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[p_id, t_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        assert n1.getblockheader(L["hash"])["poscertified"]
        self.record("fp", "L", "%s at height %d anchored at Bitcoin %d: P %s (a payment-proof claim), T %s (the sale lock, spends P's output)" %
                    (L["hash"][:12], L["height"], L["block"]["anchorheight"], p_id[:12], t_id[:12]))
        # The buyer's ancestry check, before it pays.
        chk = n1.checklockancestry(t_id)
        self.record("fp", "checklockancestry(T)", chk)
        assert_equal(chk["payable"], False)
        assert {"txid": p_id, "class": "payment-proof claim"} in chk["failing"], chk
        # Control: lock0, funded from an ordinary coin of the seller's.
        ctl = n1.checklockancestry(lock0["txid"])
        self.record("fp", "checklockancestry(lock0) (control)", ctl)
        assert_equal(ctl["payable"], True)
        assert_equal(ctl["failing"], [])
        self.record("fp", "outcome", "T is not payable (P %s named as a payment-proof claim); the buyer does not pay" % p_id[:12])

    # ----------------------------------------------------------------- big
    def padded_claim(self, lock, record, vout, pad, fee, out_spk):
        """spend_lock's claim, with `pad` zero-value OP_RETURN outputs of 9,000 bytes each before the fee output."""
        tx = bp.CTransaction()
        tx.nVersion = 2
        tx.vin.append(bp.CTxIn(bp.COutPoint(int(lock["txid"], 16), lock["vout"]), nSequence=bp.SEQ_NONFINAL_RBF))
        asset = bp.CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
        tx.vout.append(bp.CTxOut(bp.CTxOutValue(lock["asset_amount"] - fee), out_spk, asset))
        for k in range(pad):
            tx.vout.append(bp.CTxOut(bp.CTxOutValue(0), b"\x6a" + push(bytes([k + 1]) * 9000), asset))
        tx.vout.append(bp.CTxOut(bp.CTxOutValue(fee), b"", asset))
        tx.wit.vtxinwit.append(bp.CTxInWitness())
        tx.wit.vtxoutwit = [bp.CTxOutWitness() for _ in tx.vout]
        tx.wit.vtxinwit[0].peginWitness.stack = [record]
        tap = lock["tap"]
        script = lock["claim"]
        sighash = bp.TaprootSignatureHash(tx, [lock["txout"]], bp.SIGHASH_DEFAULT, self.genesis, 0, scriptpath=True, script=script)
        sig = bp.sign_schnorr(lock["sk_b"], sighash)
        control = bytes([tap.leaves["claim"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["claim"].merklebranch
        tx.wit.vtxinwit[0].scriptWitness.stack = [bp.scriptnum(vout), sig, bytes(script), control]
        tx.rehash()
        return tx

    def case_big(self):
        self.log.info("(big) two claims lost for good in one block, the first too large to record alone")
        self.wallets()
        b = self.btc()
        n1 = self.nodes[1]
        h0 = b.getblockcount()
        locks = [self.make_lock(50_000_000, h0, h0 + 50, h0) for _ in range(2)]
        for lk in locks:
            self.send_lock(lk)
        self.produce(1, ALL_KEYS, txs_must=[lk["txid"] for lk in locks])
        for _ in range(bp.FLOOR_DEPTH + 1):
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        pays = [self.pay(lk) for lk in locks]
        p_hash = b.generatetoaddress(1, self.bbuyer.getnewaddress())[0]
        p = b.getblockcount()
        assert all(self.bbuyer.gettransaction(x)["blockhash"] == p_hash for x in pays)
        self.anchored_block(p)
        spk = bytes.fromhex(n1.getaddressinfo(n1.getnewaddress())["scriptPubKey"])
        claims = []
        for k, (lk, pay) in enumerate(zip(locks, pays)):
            pr = self.proof_for(pay, self.coin_anchor(lk["txid"]))
            vout = self.vout_paying(pay, lk["addr"], pr["blockhash"])
            if k == 0:
                c = self.padded_claim(lk, pr["record"], vout, pad=3, fee=50_000_000, out_spk=spk)
            else:
                c = self.claim(lk, pr["record"], vout, out_spk=spk)
            claims.append(c)
        big, small = claims
        big_size = len(big.serialize_without_witness())
        assert big_size > 26000, big_size
        ids = [c.rehash() for c in claims]
        for c in claims:
            n1.sendrawtransaction(c.serialize().hex())
        LC = self.produce(1, ALL_KEYS, txs_must=ids)
        order = LC["block"]["tx"]
        assert order.index(ids[0]) < order.index(ids[1]), order
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        assert n1.getblockheader(LC["hash"])["poscertified"]
        self.record("big", "LC", "%s at height %d anchored at Bitcoin %d: big claim %s (%d bytes without witness) at index %d, small claim %s at index %d" %
                    (LC["hash"][:12], LC["height"], p, ids[0][:12], big_size, order.index(ids[0]), ids[1][:12], order.index(ids[1])))
        # Bitcoin replaces the payments' block; both payments are double-spent there.
        prevs = {}
        pay_set = set(pays)
        for x in pays:
            for vin in b.getrawtransaction(x, True, p_hash)["vin"]:
                if vin["txid"] not in pay_set:
                    prevs[(vin["txid"], vin["vout"])] = True
        b.invalidateblock(p_hash)
        total = Decimal(0)
        for (txid, n) in prevs:
            total += Decimal(str(self.bbuyer.gettransaction(txid, True, True)["decoded"]["vout"][n]["value"]))
        raw = self.bbuyer.createrawtransaction([{"txid": t, "vout": n} for (t, n) in prevs],
                                               [{self.bbuyer.getnewaddress(): total - Decimal("0.002")}])
        b.sendrawtransaction(self.bbuyer.signrawtransactionwithwallet(raw)["hex"], 0)
        b.generatetoaddress(2, self.bbuyer.getnewaddress())
        self.wait_lost(LC["hash"], HONEST)
        assert all(self.bbuyer.gettransaction(x).get("confirmations", 0) <= 0 for x in pays)
        self.record("big", "reorg", "Bitcoin block %d replaced; both payments double-spent; LC lost on %s" % (p, HONEST))
        # An honest holder makes the next blocks.
        since = [self.log_size(i) for i in range(N)]
        recorded = []
        for _ in range(3):
            h = self.produce(1, ALL_KEYS, leaders=[SELLER])
            for o in self.records_in(n1, h["hash"]):
                data = parse_record_script(bytes.fromhex(o["scriptPubKey"]["hex"]))
                recorded.append((h["height"], data[0], len(data),
                                 bytes.fromhex(locks[0]["txid"])[::-1] in data, bytes.fromhex(locks[1]["txid"])[::-1] in data))
        self.sync_tips(HONEST)
        never = self.log_has(1, "%s can never be recorded" % ids[0], since[1])
        binds = self.log_has(1, "records 1 bound input", since[1])
        plans = [l.split("plan computed on ", 1)[-1][64:300] for l in self.log_has(1, "plan computed", since[1])]
        self.record("big", "records (height, version, bytes, carries the big claim, carries the small claim)", recorded)
        self.record("big", "node 1: can never be recorded", never[0].split("Lost blocks: ", 1)[-1][:260] if never else "no line")
        self.record("big", "node 1: bound inputs", binds[0].split("Lost blocks: ", 1)[-1][:200] if binds else "no line")
        self.record("big", "node 1: plans", plans[:2])
        assert any(r[4] for r in recorded), "the small claim was not recorded"
        assert not any(r[3] for r in recorded), "the big claim was recorded"
        assert never, "no 'can never be recorded' line for the big claim"

    # ----------------------------------------------------------------- cap
    def case_cap(self):
        self.log.info("(cap) a failing ancestor hidden behind 1,000 ordinary transactions")
        self.wallets()
        b = self.btc()
        n1, a = self.nodes[1], self.nodes[SELLER]
        b.createwallet("sself")
        sself = self.btc("sself")
        self.bbuyer.sendtoaddress(sself.getnewaddress(), Decimal("1"))
        b.generatetoaddress(1, self.bbuyer.getnewaddress())
        h0 = b.getblockcount()
        lock0 = self.make_lock(10_000, h0, h0 + 50, h0)
        self.send_lock(lock0)
        self.produce(1, ALL_KEYS, txs_must=[lock0["txid"]])
        for _ in range(bp.FLOOR_DEPTH + 1):
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        # The seller pays itself on Bitcoin and claims its own lock: a claim it
        # can make fail for good by double-spending that payment.
        sp = sself.sendtoaddress(lock0["addr"], Decimal("0.0001"))
        p_hash = b.generatetoaddress(1, self.bbuyer.getnewaddress())[0]
        self.anchored_block(b.getblockcount())
        seller_spk = a.getaddressinfo(a.getnewaddress())["scriptPubKey"]
        lock0["buyer_spk"] = bytes.fromhex(seller_spk)
        pr = self.proof_for(sp, self.coin_anchor(lock0["txid"]), blockhash=p_hash)
        claim = self.claim(lock0, pr["record"], self.vout_paying(sp, lock0["addr"], p_hash), out_spk=lock0["buyer_spk"])
        claim_id = claim.rehash()
        n1.sendrawtransaction(claim.serialize().hex())
        self.produce(1, ALL_KEYS, txs_must=[claim_id])
        p_out = [o for o in n1.decoderawtransaction(claim.serialize().hex())["vout"] if o["scriptPubKey"]["hex"] == seller_spk][0]
        # 1,000 ordinary hops, 20 per certified block.
        fee = Decimal("0.00001")
        prev_id, prev_n = claim_id, p_out["n"]
        value = Decimal(str(p_out["value"]))
        prev = {"txid": prev_id, "vout": prev_n, "scriptPubKey": seller_spk, "amount": value}
        hops, batch = 1000, []
        for k in range(hops):
            addr = a.getnewaddress()
            spk = a.getaddressinfo(addr)["scriptPubKey"]
            raw = a.createrawtransaction([{"txid": prev_id, "vout": prev_n}], [{addr: value - fee}, {"fee": fee}])
            signed = a.signrawtransactionwithwallet(raw, [prev])
            assert signed["complete"]
            prev_id = a.sendrawtransaction(signed["hex"])
            batch.append(prev_id)
            prev_n = 0
            value -= fee
            prev = {"txid": prev_id, "vout": prev_n, "scriptPubKey": spk, "amount": value}
            if (k + 1) % 20 == 0:
                self.produce(1, ALL_KEYS, txs_must=batch)
                batch = []
        lock = self.make_lock(50_000, h0, h0 + 50, h0 + 1000)
        lock_addr = program_to_witness(1, lock["tap"].scriptPubKey[2:])
        raw = a.createrawtransaction([{"txid": prev_id, "vout": prev_n}], [{lock_addr: value - fee}, {"fee": fee}])
        signed = a.signrawtransactionwithwallet(raw, [prev])
        assert signed["complete"]
        lock_id = n1.sendrawtransaction(signed["hex"])
        final = self.produce(1, ALL_KEYS, txs_must=[lock_id])
        self.sync_tips(range(N))
        self.record("cap", "lock", "%s at height %d, funded through %d ordinary transactions from the seller's own claim %s" %
                    (lock_id[:12], final["height"], hops, claim_id[:12]))
        chk = n1.checklockancestry(lock_id)
        self.record("cap", "checklockancestry(lock)", chk)
        assert chk["payable"] is False, "the ancestry walk stopped at its budget and called the lock payable: %s" % chk
        assert_equal(chk["complete"], False)
        assert "larger than this check examines" in chk["reason"], chk
        one = n1.checklock(lock_id)
        self.record("cap", "checklock(lock)", one)
        assert_equal(one["payable"], False)
        assert_equal(one["ancestry_complete"], False)
        assert any("larger than this check examines" in r for r in one["reasons"]), one
        # The one-call check on an ordinary, final lock: payable.
        ordinary = a.sendtoaddress(address=a.getnewaddress(), amount=Decimal("0.5"), fee_asset_label="bitcoin")
        o = self.produce(1, ALL_KEYS, txs_must=[ordinary])
        for _ in range(2):
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        self.wait_until(lambda: n1.checklock(ordinary)["payable"], timeout=120)
        ctl = n1.checklock(ordinary)
        self.record("cap", "checklock(ordinary lock) (control)", ctl)
        assert_equal(ctl["reasons"], [])
        assert ctl["final"] and ctl["bitcoin_uncontested"] and ctl["ancestry_payable"] and ctl["ancestry_complete"]
        self.record("cap", "outcome", "the padded lock is not payable (budget spent before the claim); the ordinary lock at height %d is" % o["height"])

    def run_test(self):
        try:
            for c in CASES:
                getattr(self, "case_" + c)()
        finally:
            out = os.environ.get("LOST_RESULTS", "")
            if out:
                with open(out, "w") as f:
                    json.dump(self.results, f, indent=1, default=str)
            self.stop_fresh_bitcoind()
            self.stop_bitcoind()


if __name__ == '__main__':
    LostSwapAttacksTest().main()
