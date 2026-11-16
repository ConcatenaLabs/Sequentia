#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Lost blocks under attack (doc/sequentia/03-bitcoin-anchoring.md §3).

Each case is an attack an independent review found on the rule's first form;
each asserts the outcome the rule must give and why.

- lx:    a lost transaction that is not yet due (the wallet's own nLockTime),
         with Bitcoin passing the lost anchor + W before the chain rebuilds the
         lost block's height. The respend X is never taken; T is mined.
- desc:  a respend X whose only claim to the duty's exception is a descendant
         that looks certified but whose aggregate does not verify. X is not
         taken; the descendant fails; T is mined.
- root0: a record whose first lost header carries no certificate, so the
         entry's committee is seeded by an anchor nobody certified. The block
         carrying it is invalid for the certificate of lost header 0.
- root1: the control: the same record with the first header certified. The
         block is valid and the record binds the coin.
- late1: two keys K1 and K2 registered on the chain after the fork point F
         sign the first lost header with three stakers of F's: five of eight
         seats, a quorum (5) only if K1 and K2 count; the three alone are
         below F's quorum (4 of 6). The record's committee is drawn from the
         registry at F, where neither exists: the block carrying it is
         invalid for the certificate of lost header 0, and the coin stays
         free.
- late0: the control: K1 and K2 are registered after F as before, and the
         first lost header is certified by four stakers of F's under F's
         registry. The record is valid: a stake change on the chain after F
         does not stop an honest record.

Run one case per process: LOST_CASES=lx (or desc, root0, root1, late0, late1).
"""
import os
import time
from decimal import Decimal

from feature_pos_lost_blocks import LostBlocksTest, SELLER, HONEST, LATE, ALL_KEYS, N, W, make_key
from pos_lost_attack_util import (LostP2PDataStore, block_from_hex, corrupt_aggregate, msg_anchor_headers)
from test_framework.authproxy import JSONRPCException
from test_framework.messages import CTxOut, CTxOutAsset, CTxOutValue, CTxOutWitness, hash256, msg_block, tx_from_hex
from test_framework.util import calcfastmerkleroot
from test_framework.util import assert_equal

CASES = os.environ.get("LOST_CASES", "lx").split(",")


def ser_cs(n):
    if n < 0xfd:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + n.to_bytes(2, "little")
    return b"\xfe" + n.to_bytes(4, "little")


def ser_bytes(b):
    return ser_cs(len(b)) + b


def push(data):
    n = len(data)
    if n < 0x4c:
        return bytes([n]) + data
    if n <= 0xff:
        return b"\x4c" + bytes([n]) + data
    if n <= 0xffff:
        return b"\x4d" + n.to_bytes(2, "little") + data
    return b"\x4e" + n.to_bytes(4, "little") + data


def merkle_root(leaves):
    level = list(leaves)
    while len(level) > 1:
        if len(level) & 1:
            level.append(level[-1])
        level = [hash256(level[k] + level[k + 1]) for k in range(0, len(level), 2)]
    return level[0]


def merkle_branch(leaves, pos):
    level, branch = list(leaves), []
    while len(level) > 1:
        if len(level) & 1:
            level.append(level[-1])
        branch.append(level[pos ^ 1])
        level = [hash256(level[k] + level[k + 1]) for k in range(0, len(level), 2)]
        pos >>= 1
    return branch


def ser_record(version, btc_headers, lost, entries):
    """LostRecord: version 1 carries Bitcoin headers, version 2 does not."""
    out = bytes([version])
    if version == 1:
        out += ser_cs(len(btc_headers)) + b"".join(ser_bytes(h) for h in btc_headers)
    out += ser_cs(len(lost)) + b"".join(ser_bytes(h) for h in lost)
    out += ser_cs(len(entries))
    for k, pos, branch, tx in entries:
        out += k.to_bytes(4, "little") + pos.to_bytes(4, "little")
        out += ser_cs(len(branch)) + b"".join(branch) + ser_bytes(tx)
    return out


class LostAttacksTest(LostBlocksTest):
    def set_test_params(self):
        super().set_test_params()
        if any(c.startswith("late") for c in CASES):
            # Room on the committee for the two late keys (8 seats, quorum 5).
            self.extra = {i: ["-poscommitteesize=9"] for i in range(N)}

    def stored(self, node, block_hash):
        try:
            return node.getblock(block_hash, 0)
        except Exception:
            return None

    def new_rule(self):
        try:
            self.nodes[1].help("checklockancestry")
            return "unknown command" not in self.nodes[1].help("checklockancestry").lower()
        except JSONRPCException:
            return False

    # -------------------------------------------------------------- lx (F4)
    def case_lx(self):
        a, n1 = self.nodes[SELLER], self.nodes[1]
        coin = self.fund()
        self.btc_mine(1)
        anchor = self.btc().getblockcount()
        f = self.tip(n1)
        self.produce(1, ALL_KEYS)
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        tip_h = a.getblockcount()
        raw_t = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"], "sequence": 0xfffffffd}],
                                       [{n1.getnewaddress(): coin["amount"] - Decimal("0.0001")}, {"fee": Decimal("0.0001")}], tip_h)
        t_hex = a.signrawtransactionwithwallet(raw_t)["hex"]
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        t_id = n1.sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        self.produce(1, ALL_KEYS)
        self.sync_tips(range(N))
        assert_equal(L["block"]["anchorheight"], anchor)
        self.record("lx", "T", "%s nLockTime %d in L at height %d (F %d), anchored at Bitcoin %d" % (t_id[:16], tip_h, L["height"], f["height"], anchor))
        since = [self.log_size(i) for i in range(N)]
        self.bitcoin_reorg(1)
        self.wait_lost(L["hash"], HONEST)
        a.sendrawtransaction(r_hex)
        r_id = tx_from_hex(r_hex).rehash()
        x = self.produce(SELLER, ALL_KEYS, leaders=[SELLER], txs_must=[r_id])
        self.assert_refused("lx", HONEST, x["hash"], since)
        # Bitcoin passes a_L + W before the chain rebuilds L's height.
        self.btc_mine(anchor + W + 1 - self.btc().getblockcount())
        self.record("lx", "Bitcoin", "%d > a_L + W = %d; honest tip height %d < L's height %d" %
                    (self.btc().getblockcount(), anchor + W, n1.getblockcount(), L["height"]))
        time.sleep(8)
        took = [i for i in HONEST if self.nodes[i].getbestblockhash() == x["hash"]]
        self.record("lx", "honest nodes that took X after Bitcoin passed a_L + W", took)
        assert_equal(took, [])
        # The honest chain rebuilds L's height; T falls due and is mined.
        for _ in range(L["height"] - f["height"] + 1):
            self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
        self.sync_tips(HONEST)
        t_at, r_at = self.block_of(t_id, n1), self.block_of(r_id, n1)
        self.record("lx", "outcome", "T mined at %s (L's height %d); R in the honest chain: %s" % (t_at, L["height"], r_at))
        assert t_at is not None and t_at <= L["height"]
        assert r_at is None

    # ------------------------------------------------------- desc (KR41 medium)
    def case_desc(self):
        st = self.gap("desc")
        attacker = self.nodes[SELLER]
        victim = self.nodes[HONEST[0]]
        self.disconnect_all(SELLER)
        attacker.setnetworkactive(False)
        x = self.produce(SELLER, [SELLER], leaders=[SELLER], txs_must=[st["r_id"]])
        y = self.produce(SELLER, list(range(6)), leaders=[SELLER])
        x_block = block_from_hex(attacker.getblock(x["hash"], 0))
        y_block = corrupt_aggregate(block_from_hex(attacker.getblock(y["hash"], 0)))
        assert_equal(x["countersignatures"], 1)
        assert_equal(x_block.hashPrevBlock, int(victim.getbestblockhash(), 16))
        since = self.log_size(HONEST[0])
        relay = victim.add_p2p_connection(LostP2PDataStore())
        relay.block_store[y_block.sha256] = y_block
        relay.last_block_hash = y_block.sha256
        relay.send_message(msg_anchor_headers([x_block, y_block]))
        self.wait_until(lambda: self.stored(victim, y["hash"]) == y_block.serialize().hex(), timeout=60)
        relay.block_store[x_block.sha256] = x_block
        relay.send_message(msg_block(x_block))
        self.wait_until(lambda: self.log_has(HONEST[0], "Lost blocks: refusing block %s" % x["hash"], since), timeout=60)
        time.sleep(3)
        lines = self.log_has(HONEST[0], "looks certified but its certificate does not verify", since)
        self.record("desc", "victim on X's arrival", (lines[0].split("Lost blocks: ", 1)[-1][:200] if lines else "no line"))
        assert lines, "the unverified descendant was not set aside"
        assert victim.getbestblockhash() != x["hash"]
        assert not self.log_has(HONEST[0], "breaks the duty but has a certified descendant", since)
        # The honest chain goes on and replays T.
        victim.disconnect_p2ps()
        h = self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5])
        self.sync_tips(HONEST)
        self.record("desc", "outcome", "victim tip %s; T at %s; R at %s" %
                    (victim.getbestblockhash()[:12], self.block_of(st["t_id"], victim), self.block_of(st["r_id"], victim)))
        assert self.block_of(st["t_id"], victim) is not None
        assert self.block_of(st["r_id"], victim) is None

    # ------------------------------------------------- root0 / root1 (F3)
    def case_root(self, certify_first):
        case = "root%d" % int(certify_first)
        a, n1, b = self.nodes[SELLER], self.nodes[1], self.btc()
        coin = self.fund()
        tv_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        tv = tx_from_hex(tv_hex)
        tv_id = tv.rehash()
        f_hash = n1.getbestblockhash()
        # The seller's own branch, made alone: A0 (signed by the seller only,
        # or by a quorum in the control), then A1 carrying Tv, certified by
        # four keys under the committee A0's anchor seeds.
        self.disconnect_all(SELLER)
        a.setnetworkactive(False)
        self.btc_mine(1)  # a Bitcoin block only the seller's branch anchors to
        time.sleep(2)
        a0 =self.produce(SELLER, [0, 1, 2, 3] if certify_first else [SELLER], leaders=[SELLER])
        a.sendrawtransaction(tv_hex)
        a1 = self.produce(SELLER, [0, 1, 2, 3], leaders=[SELLER], txs_must=[tv_id])
        s_hash = a1["block"]["anchorhash"]
        self.record(case, "branch", "A0 %s (%d countersignature(s)) and A1 %s (Tv) on F %s, anchored at Bitcoin %d %s" %
                    (a0["hash"][:12], a0["countersignatures"], a1["hash"][:12], f_hash[:12], a1["block"]["anchorheight"], s_hash[:12]))
        # Bitcoin replaces that anchor.
        s_hdr = b.getblockheader(s_hash, False)
        self.bitcoin_reorg(b.getblockcount() - a1["block"]["anchorheight"] + 1)
        assert b.getblockheader(s_hash)["confirmations"] < 1
        # The record, by hand.
        a1_full = a.getblock(a1["hash"], 2)
        leaves = [bytes.fromhex(t["txid"])[::-1] for t in a1_full["tx"]]
        pos = [t["txid"] for t in a1_full["tx"]].index(tv_id)
        entry = (1, pos, merkle_branch(leaves, pos), tv.serialize_without_witness())
        lost = [bytes.fromhex(a.getblockheader(a0["hash"], False)), bytes.fromhex(a.getblockheader(a1["hash"], False))]
        version = 2 if self.new_rule() else 1
        data = ser_record(version, [bytes.fromhex(s_hdr)], lost, [entry])
        # B on the honest chain, its coinbase given the record, re-certified.
        self.sync_tips(HONEST)
        self.disconnect_all(1)
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        raw = bytes.fromhex(n1.getblock(h["hash"], 0))
        blk = n1.getblock(h["hash"], 2)
        cb = tx_from_hex(blk["tx"][0]["hex"])
        old_cb = cb.serialize()
        assert raw.count(old_cb) == 1
        # The record goes before the witness commitment, which is then
        # recomputed: in Elements the coinbase's witness hash covers one
        # output witness per output.
        ci = [k for k, o in enumerate(cb.vout) if o.scriptPubKey[:6] == bytes.fromhex("6a24aa21a9ed")][-1]
        cb.vout.insert(ci, CTxOut(0, b"\x6a\x04SQLR" + push(data), cb.vout[0].nAsset))
        if cb.wit.vtxoutwit:
            cb.wit.vtxoutwit.insert(ci, CTxOutWitness())
        import test_framework.util as fu
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
        n1.invalidateblock(h["hash"])
        since = self.log_size(1)
        rc = n1.recertifyposblock(new_raw.hex(), self.stakers[SELLER][0], [self.stakers[j][0] for j in ALL_KEYS if j != SELLER])
        r = n1.submitblock(rc["hex"])
        lines = self.log_has(1, "carries a record that is not valid", since)
        self.record(case, "B with the record (%d bytes, version %d)" % (len(data), version),
                    "submitblock %r; node 1's tip is B: %s; %s" % (r, n1.getbestblockhash() == rc["hash"],
                                                                 lines[0].split("Lost blocks: ", 1)[-1][:200] if lines else "no record complaint"))
        if certify_first:
            assert_equal(n1.getbestblockhash(), rc["hash"])
            assert self.log_has(1, "records 1 bound input", since)
        else:
            assert n1.getbestblockhash() != rc["hash"]
            assert lines and "certificate of lost header 0" in lines[0], lines

    # ------------------------------------------------- late0 / late1 (M1)
    def put_record(self, data):
        """B on node 1's chain, its coinbase carrying the record, certified by
        the six stakers; returns (B's hash, the log offset before submitting)."""
        n1 = self.nodes[1]
        self.disconnect_all(1)
        h = self.produce(1, ALL_KEYS, leaders=[SELLER])
        raw = bytes.fromhex(n1.getblock(h["hash"], 0))
        blk = n1.getblock(h["hash"], 2)
        cb = tx_from_hex(blk["tx"][0]["hex"])
        old_cb = cb.serialize()
        assert raw.count(old_cb) == 1
        ci = [k for k, o in enumerate(cb.vout) if o.scriptPubKey[:6] == bytes.fromhex("6a24aa21a9ed")][-1]
        cb.vout.insert(ci, CTxOut(0, b"\x6a\x04SQLR" + push(data), cb.vout[0].nAsset))
        if cb.wit.vtxoutwit:
            cb.wit.vtxoutwit.insert(ci, CTxOutWitness())
        import test_framework.util as fu
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
        n1.invalidateblock(h["hash"])
        since = self.log_size(1)
        rc = n1.recertifyposblock(new_raw.hex(), self.stakers[SELLER][0], [self.stakers[j][0] for j in ALL_KEYS if j != SELLER])
        r = n1.submitblock(rc["hex"])
        return rc["hash"], r, since

    def case_late(self, late_key_signs):
        case = "late%d" % int(late_key_signs)
        a, n1, n5, b = self.nodes[SELLER], self.nodes[1], self.nodes[LATE], self.btc()
        online = [0, 1, 2, 3, 4]
        coin = self.fund()
        tv_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        tv = tx_from_hex(tv_hex)
        tv_id = tv.rehash()
        # K1 and K2, not stakers at F: their stake outputs (one atom each, a
        # weight like each of the six's, with their BLS keys) are signed now
        # and sent after F.
        late = [make_key() for _ in range(2)]
        regs = []
        for k_wif, k_pub in late:
            bls = n1.getblsregistration(k_wif)
            script = a.getstakescript(pubkey=k_pub, csv_blocks=600, blspubkey=bls["blspubkey"], pop=bls["pop"])["script"]
            kc = self.fund()
            reg_raw = a.createrawtransaction([{"txid": kc["txid"], "vout": kc["vout"]}],
                                             [{a.getnewaddress(): Decimal("0.00000001")},
                                              {a.getnewaddress(): kc["amount"] - Decimal("0.00010001")}, {"fee": Decimal("0.0001")}])
            rt = tx_from_hex(reg_raw)
            rt.vout[0] = CTxOut(CTxOutValue(1), bytes.fromhex(script), rt.vout[1].nAsset)
            regs.append((a.signrawtransactionwithwallet(rt.serialize().hex())["hex"], "-staker=%s:1%s" % (k_pub, bls["spec"])))
        self.sync_tips(range(N))
        f_hash = n1.getbestblockhash()
        assert not any(k_pub in n1.getstakerinfo() for _, k_pub in late)
        # The first lost header A0 on F, carrying Tv, made apart.
        if late_key_signs:
            # Node 5 stops at F and comes back with K1 and K2 as configured
            # stakers: its registry at F then holds them, as the chain's
            # registry does after their stake outputs. It signs A0 with three
            # of the six and K1 and K2.
            self.stop_node(LATE)
            self.start_node(LATE, extra_args=self.node_args(LATE) + [spec for _, spec in regs] + ["-checkblocks=0", "-networkactive=0"])
            assert_equal(n5.getbestblockhash(), f_hash)
            src, signers = n5, [self.stakers[1][0], self.stakers[2][0]] + [k_wif for k_wif, _ in late]
        else:
            self.disconnect_all(SELLER)
            a.setnetworkactive(False)
            online = [1, 2, 3, 4]
            src, signers = a, [self.stakers[j][0] for j in (1, 2, 3)]
        self.btc_mine(1)  # a Bitcoin block only A0 anchors to
        time.sleep(2)
        src.sendrawtransaction(tv_hex)
        a0, last = None, None
        for _ in range(200):
            try:
                a0 = src.generateposblock(self.stakers[SELLER][0], signers)
                break
            except JSONRPCException as e:
                last = e.error["message"]
                time.sleep(0.25)
        assert a0, last
        a0_blk = src.getblock(a0["hash"], 2)
        assert tv_id in [t["txid"] for t in a0_blk["tx"]]
        assert_equal(a0_blk["previousblockhash"], f_hash)
        self.record(case, "A0", "%s on F %s, %d seats signed (%s), anchored at Bitcoin %d" %
                    (a0["hash"][:12], f_hash[:12], a0["countersignatures"], "three of F's stakers, K1 and K2" if late_key_signs else "four of F's stakers",
                     a0_blk["anchorheight"]))
        # Bitcoin replaces A0's anchor.
        self.bitcoin_reorg(b.getblockcount() - a0_blk["anchorheight"] + 1)
        assert b.getblockheader(a0_blk["anchorhash"])["confirmations"] < 1
        # K1 and K2 register on the honest chain after F.
        ids = [n1.sendrawtransaction(h) for h, _ in regs]
        g = self.produce(1, ALL_KEYS, txs_must=ids)
        self.sync_tips([1, 2, 3, 4])
        assert all(k_pub in n1.getstakerinfo() for _, k_pub in late)
        self.record(case, "K1, K2", "stake outputs in block %s at height %d, after F; the committee is now %d seats" %
                    (g["hash"][:12], g["height"], len(n1.getposschedule()["committee"])))
        # The record, by hand, in B.
        leaves = [bytes.fromhex(t["txid"])[::-1] for t in a0_blk["tx"]]
        pos = [t["txid"] for t in a0_blk["tx"]].index(tv_id)
        entry = (0, pos, merkle_branch(leaves, pos), tv.serialize_without_witness())
        data = ser_record(2, [], [bytes.fromhex(src.getblockheader(a0["hash"], False))], [entry])
        b_hash, r, since = self.put_record(data)
        lines = self.log_has(1, "carries a record that is not valid", since)
        self.record(case, "B with the record (%d bytes)" % len(data),
                    "submitblock %r; node 1's tip is B: %s; %s" % (r, n1.getbestblockhash() == b_hash,
                                                                 lines[0].split("Lost blocks: ", 1)[-1][:200] if lines else "no record complaint"))
        if late_key_signs:
            assert n1.getbestblockhash() != b_hash, "a record certified with a key registered after its fork point was accepted"
            assert lines and "certificate of lost header 0" in lines[0], lines
            # The coin stays free: its owner's spend is mined.
            spend = self.spend(coin, a.getnewaddress(), Decimal("0.002"))
            sid = n1.sendrawtransaction(spend)
            h = self.produce(1, ALL_KEYS, txs_must=[sid])
            self.record(case, "the coin", "spent by its owner at height %d" % h["height"])
        else:
            assert_equal(n1.getbestblockhash(), b_hash)
            assert self.log_has(1, "records 1 bound input", since)

    def run_test(self):
        try:
            for c in CASES:
                if c == "lx":
                    self.case_lx()
                elif c == "desc":
                    self.case_desc()
                elif c in ("root0", "root1"):
                    self.case_root(c == "root1")
                elif c in ("late0", "late1"):
                    self.case_late(c == "late1")
        finally:
            import json
            out = os.environ.get("LOST_RESULTS", "")
            if out:
                with open(out, "w") as fh:
                    json.dump(self.results, fh, indent=1)
            self.stop_bitcoind()


if __name__ == '__main__':
    LostAttacksTest().main()
