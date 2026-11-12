#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The proof of a bitcoin payment (doc/sequentia/bitcoin-payment-proof.md).

A real bitcoind is the parent chain. A Sequentia node anchored to it, with
-validateanchor=1 and -par=1, runs the swap: a seller locks an asset under a
claim leaf (buyer's key + a proof of a payment to the seller's address) and a
refund leaf (seller's key + an anchor floor); the buyer pays with an ordinary
bitcoin transaction; anyone completes the claim with the proof.

Every negative is forced into a block with generateblock and the reason the
block was refused is asserted. Requires BITCOIND (default /usr/local/bin/bitcoind).
"""

import hashlib
import json
import os
import struct
import subprocess
import time
import urllib.parse
from decimal import Decimal

from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.address import program_to_witness
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr
from test_framework.messages import (COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutAsset, CTxOutWitness,
                                     CTxOutValue, tx_from_hex)
from test_framework.script import (CScript, CScriptOp, OP_CHECKSIG, OP_CHECKSIGVERIFY, OP_DROP, OP_DUP,
                                   OP_EQUALVERIFY, OP_GREATERTHAN, OP_GREATERTHANOREQUAL64, OP_LESSTHANOREQUAL,
                                   OP_VERIFY, OP_1, SIGHASH_DEFAULT, TaprootSignatureHash, taproot_construct)
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, initialize_datadir, rpc_port, p2p_port

OP_INSPECTBTCPAYMENT = CScriptOp(0xe5)
OP_INSPECTBTCOUTPUT = CScriptOp(0xe6)
OP_CHECKANCHORHEIGHTVERIFY = CScriptOp(0xe7)

BITCOIND = os.environ.get("BITCOIND", "/usr/local/bin/bitcoind")
NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")
SEQ_NONFINAL_RBF = 0xfffffffd


def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def read_varint(b, i):
    n = b[i]
    if n < 0xfd:
        return n, i + 1
    if n == 0xfd:
        return struct.unpack("<H", b[i + 1:i + 3])[0], i + 3
    if n == 0xfe:
        return struct.unpack("<I", b[i + 1:i + 5])[0], i + 5
    return struct.unpack("<Q", b[i + 1:i + 9])[0], i + 9


def strip_btc_tx(raw):
    """A Bitcoin transaction's bytes without its witness (what its txid hashes)."""
    if raw[4] != 0:
        return raw
    out = raw[:4]
    i = 6
    start = i
    nin, i = read_varint(raw, i)
    for _ in range(nin):
        i += 36
        sl, i = read_varint(raw, i)
        i += sl + 4
    nout, i = read_varint(raw, i)
    for _ in range(nout):
        i += 8
        sl, i = read_varint(raw, i)
        i += sl
    out += raw[start:i]
    for _ in range(nin):
        items, i = read_varint(raw, i)
        for _ in range(items):
            sl, i = read_varint(raw, i)
            i += sl
    out += raw[i:i + 4]
    assert i + 4 == len(raw)
    return out


def merkle_branch(leaves, index):
    branch = []
    level = list(leaves)
    while len(level) > 1:
        if len(level) % 2:
            level.append(level[-1])
        branch.append(level[index ^ 1])
        level = [dsha(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
        index >>= 1
    return branch


def payment_record(height, coin_anchor, header, coinbase_sha, index, pbranch, cbranch, payment, depth=None, links=(),
                   nlinks=None):
    d = len(pbranch) if depth is None else depth
    n = len(links) if nlinks is None else nlinks
    return (b"\x01" + struct.pack("<I", height) + struct.pack("<I", coin_anchor) + header + coinbase_sha +
            bytes([d]) + struct.pack("<I", index) + b"".join(pbranch) + b"".join(cbranch) +
            struct.pack("<H", n) + b"".join(links) + payment)


def floor_record(h):
    return b"\x02" + struct.pack("<I", h)


def le8(v):
    return struct.pack("<q", v)


def scriptnum(n):
    return CScript([n])[1:] if n > 16 or n < 0 else (b"" if n == 0 else bytes([n]))


class BtcPaymentProofTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if not os.path.exists(BITCOIND):
            raise SkipTest("needs Bitcoin Core's bitcoind (set BITCOIND)")

    # ------------------------------------------------------------------ setup
    def btc_rpc(self, wallet=None):
        url = "http://%s:%s@127.0.0.1:%d" % (urllib.parse.quote(self.btc_user), urllib.parse.quote(self.btc_pass),
                                              self.btc_port)
        if wallet:
            url += "/wallet/" + wallet
        return AuthServiceProxy(url, timeout=120)

    def seq_args(self, extra=None):
        return [
            "-con_blocksubsidy=5000000000", "-initialfreecoins=0", "-anyonecanspendaremine=1",
            "-blindedaddresses=0", "-con_default_blinded_addresses=0", "-validatepegin=0",
            "-con_any_asset_fees=1", "-fallbackfee=0.0001", "-maxtxfee=100.0", "-par=1",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % self.btc_port,
            "-mainchainrpcuser=%s" % self.btc_user, "-mainchainrpcpassword=%s" % self.btc_pass,
            "-parentgenesisblockhash=%s" % self.btc_genesis, "-debug=mempool", "-debug=bench", "-txindex=1",
        ] + (extra or [])

    def setup_network(self, split=False):
        self.btc_port = rpc_port(3)
        btcdir = os.path.join(self.options.tmpdir, "btc")
        os.makedirs(btcdir)
        self.btc_proc = subprocess.Popen([BITCOIND, "-datadir=" + btcdir, "-regtest", "-port=%d" % p2p_port(3),
                                          "-rpcport=%d" % self.btc_port, "-listen=0", "-server",
                                          "-fallbackfee=0.0001", "-printtoconsole=0"],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cookie = os.path.join(btcdir, "regtest", ".cookie")
        self.wait_until(lambda: os.path.exists(cookie), timeout=60)
        self.btc_user, self.btc_pass = open(cookie).read().strip().split(":", 1)

        def up():
            try:
                self.btc_rpc().getblockcount()
                return True
            except Exception:
                return False
        self.wait_until(up, timeout=60)
        b = self.btc_rpc()
        self.btc_genesis = b.getblockhash(0)
        for w in ["miner", "buyer", "seller"]:
            b.createwallet(w)
        self.bminer = self.btc_rpc("miner")
        self.bbuyer = self.btc_rpc("buyer")
        self.bseller = self.btc_rpc("seller")
        self.btc_mine(110)
        self.bminer.sendtoaddress(self.bbuyer.getnewaddress(), 40)
        self.btc_mine(1)

        # node 0: the rule from genesis. node 1: the rule never active (an old
        # node), to show the rule only tightens.
        self.add_nodes(2, [self.seq_args(), self.seq_args(["-con_btc_payment_height=100000000"])],
                       chain=["elementsregtest", "elementsregtest"])
        self.start_node(0)
        self.start_node(1)
        self.connect_nodes(0, 1)
        for n in self.nodes:
            n.createwallet(wallet_name="w", descriptors=True)

    def btc_mine(self, n, addr=None):
        return self.bminer.generatetoaddress(n, addr or self.bminer.getnewaddress())

    def stop_bitcoind(self):
        if getattr(self, "btc_proc", None) and self.btc_proc.poll() is None:
            try:
                self.btc_rpc().stop()
                self.btc_proc.wait(timeout=60)
            except Exception:
                self.btc_proc.terminate()
                self.btc_proc.wait(timeout=60)

    # ------------------------------------------------------------ Sequentia
    def mine(self, n=1, node=None):
        node = node or self.nodes[0]
        return self.generatetoaddress(node, n, node.getnewaddress(), sync_fun=self.no_op)

    def anchor_of(self, blockhash, node=None):
        return (node or self.nodes[0]).getblockheader(blockhash)["anchorheight"]

    def mine_anchored(self, h):
        """Mine until the tip anchors at or above Bitcoin height h."""
        for _ in range(60):
            b = self.mine(1)[0]
            if self.anchor_of(b) >= h:
                return b
            time.sleep(0.3)
        raise AssertionError("no block anchored at >= %d" % h)

    def force(self, txs, expect=None):
        """generateblock with exactly these transactions. With `expect`, assert
        it is refused and the reason contains it."""
        node = self.nodes[0]
        hexes = [t if isinstance(t, str) else t.serialize().hex() for t in txs]
        if expect is None:
            return node.generateblock(node.getnewaddress(), hexes, invalid_call=False)["hash"]
        try:
            node.generateblock(node.getnewaddress(), hexes, invalid_call=False)
        except JSONRPCException as e:
            msg = e.error["message"]
            assert expect in msg, "expected %r, got %r" % (expect, msg)
            self.log.info("    refused in a block: %s", msg)
            self.results.append((self.current, msg))
            return msg
        raise AssertionError("block accepted, expected refusal: " + expect)

    # ------------------------------------------------------------- the lock
    def new_lock(self, amount_btc_sat, floor, deadline, refund_at, program=None, version=None, asset_amount=10):
        sk_b, sk_s = generate_privkey(), generate_privkey()
        pk_b, pk_s = compute_xonly_pubkey(sk_b)[0], compute_xonly_pubkey(sk_s)[0]
        if program is None:
            # The seller's receiving script: P2WSH of <seller> CHECKSIGVERIFY 1 CSV.
            desc = self.bseller.getdescriptorinfo("wsh(and_v(v:pk(%s),older(1)))" % ("02" + pk_s.hex()))["descriptor"]
            addr = self.bseller.deriveaddresses(desc)[0]
            info = self.bseller.getaddressinfo(addr)
            program, version = bytes.fromhex(info["witness_program"]), info["witness_version"]
        else:
            addr = None
        claim = CScript([pk_b, OP_CHECKSIGVERIFY, OP_INSPECTBTCPAYMENT, OP_DROP, OP_DUP, floor, OP_GREATERTHAN,
                         OP_VERIFY, deadline, OP_LESSTHANOREQUAL, OP_VERIFY, OP_INSPECTBTCOUTPUT, version,
                         OP_EQUALVERIFY, program, OP_EQUALVERIFY, le8(amount_btc_sat), OP_GREATERTHANOREQUAL64])
        refund = CScript([refund_at, OP_CHECKANCHORHEIGHTVERIFY, OP_DROP, pk_s, OP_CHECKSIG])
        tap = taproot_construct(NUMS, [("claim", claim), ("refund", refund)])
        lock_addr = program_to_witness(1, tap.scriptPubKey[2:])
        node = self.nodes[0]
        txid = node.sendtoaddress(address=lock_addr, amount=asset_amount, assetlabel=self.asset,
                                  fee_asset_label=self.asset, replaceable=False)
        tx = tx_from_hex(node.getrawtransaction(txid))
        vout = [i for i, o in enumerate(tx.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        return dict(sk_b=sk_b, sk_s=sk_s, tap=tap, claim=claim, refund=refund, txid=txid, vout=vout,
                    txout=tx.vout[vout], addr=addr, amount=amount_btc_sat, floor=floor, deadline=deadline,
                    refund_at=refund_at, asset_amount=int(Decimal(asset_amount) * 100000000))

    def lock_coin_anchor(self, lock):
        bh = self.nodes[0].getrawtransaction(lock["txid"], True)["blockhash"]
        return self.anchor_of(bh)

    # ------------------------------------------------------------- payments
    def pay(self, addr, sat, wallet=None):
        w = wallet or self.bbuyer
        txid = w.sendtoaddress(addr, Decimal(sat) / Decimal(100000000))
        return txid

    def recorded_anchor_at_or_above(self, h, node=None):
        """The lowest Bitcoin height >= h that the active chain recorded as an anchor."""
        node = node or self.nodes[0]
        cur = node.getblockheader(node.getbestblockhash())
        if cur["anchorheight"] < h:
            return None
        while "previousblockhash" in cur:
            prev = node.getblockheader(cur["previousblockhash"])
            if prev["anchorheight"] < h:
                break
            cur = prev
        return cur["anchorheight"], cur["anchorhash"]

    def link_headers(self, h, a):
        b = self.btc_rpc()
        return [bytes.fromhex(b.getblockheader(b.getblockhash(x), False)) for x in range(h + 1, a + 1)]

    def proof_for(self, txid, coin_anchor, blockhash=None, link=True):
        b = self.btc_rpc()
        if blockhash is None:
            blockhash = self.bbuyer.gettransaction(txid)["blockhash"]
        blk = b.getblock(blockhash, 1)
        leaves = [bytes.fromhex(t)[::-1] for t in blk["tx"]]
        index = blk["tx"].index(txid)
        header = bytes.fromhex(b.getblockheader(blockhash, False))
        cb = strip_btc_tx(bytes.fromhex(b.getrawtransaction(blk["tx"][0], False, blockhash)))
        pay = strip_btc_tx(bytes.fromhex(b.getrawtransaction(txid, False, blockhash)))
        links = []
        rec_anchor = self.recorded_anchor_at_or_above(blk["height"]) if link else None
        if rec_anchor is not None:
            links = self.link_headers(blk["height"], rec_anchor[0])
        return dict(height=blk["height"], header=header, coinbase=cb, index=index, leaves=leaves, links=links,
                    pbranch=merkle_branch(leaves, index), cbranch=merkle_branch(leaves, 0), payment=pay,
                    record=payment_record(blk["height"], coin_anchor, header, hashlib.sha256(cb).digest(), index,
                                          merkle_branch(leaves, index), merkle_branch(leaves, 0), pay, links=links),
                    blockhash=blockhash)

    def vout_paying(self, txid, addr, blockhash=None):
        b = self.btc_rpc()
        t = b.getrawtransaction(txid, True, blockhash) if blockhash else self.bbuyer.gettransaction(txid, True, True)["decoded"]
        return [o["n"] for o in t["vout"] if o["scriptPubKey"].get("address") == addr][0]

    # --------------------------------------------------------------- spends
    def spend_lock(self, lock, leaf, slot, stack_items, fee=5000, sign_with=None, out_spk=None, nseq=SEQ_NONFINAL_RBF):
        node = self.nodes[0]
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin.append(CTxIn(COutPoint(int(lock["txid"], 16), lock["vout"]), nSequence=nseq))
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
        spk = out_spk if out_spk is not None else bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        tx.vout.append(CTxOut(CTxOutValue(lock["asset_amount"] - fee), spk, asset))
        tx.vout.append(CTxOut(CTxOutValue(fee), b"", asset))
        tx.wit.vtxinwit.append(CTxInWitness())
        tx.wit.vtxoutwit = [CTxOutWitness() for _ in tx.vout]
        tx.wit.vtxinwit[0].peginWitness.stack = list(slot)
        tap = lock["tap"]
        script = lock[leaf]
        sec = sign_with if sign_with is not None else (lock["sk_b"] if leaf == "claim" else lock["sk_s"])
        sighash = TaprootSignatureHash(tx, [lock["txout"]], SIGHASH_DEFAULT, self.genesis, 0, scriptpath=True,
                                       script=script)
        sig = sign_schnorr(sec, sighash)
        control = bytes([tap.leaves[leaf].version + tap.negflag]) + tap.internal_pubkey + tap.leaves[leaf].merklebranch
        tx.wit.vtxinwit[0].scriptWitness.stack = list(stack_items) + [sig, bytes(script), control]
        tx.rehash()
        return tx

    def claim(self, lock, record, vout, **kw):
        return self.spend_lock(lock, "claim", [record], [scriptnum(vout)], **kw)

    def refund(self, lock, floor=None, **kw):
        return self.spend_lock(lock, "refund", [floor_record(lock["refund_at"] if floor is None else floor)], [], **kw)

    def step(self, name):
        self.current = name
        self.log.info(name)

    # ------------------------------------------------------------------ run
    def run_test(self):
        try:
            self.results = []
            self.setup_chain_state()
            self.test_swap_and_negatives()
        finally:
            self.write_results()
            self.stop_bitcoind()

    def write_results(self):
        path = os.path.join(self.options.tmpdir, "results.json")
        with open(path, "w") as f:
            json.dump(self.results, f, indent=1)
        self.log.info("results in %s", path)

    def setup_chain_state(self):
        node = self.nodes[0]
        self.genesis = int(node.getblockhash(0), 16)
        self.mine(101)
        self.sync_blocks()
        self.asset = node.issueasset(assetamount=100000, tokenamount=0, blind=False, fee_asset="bitcoin")["asset"]
        self.mine(1)
        rates = node.getfeeexchangerates()
        rates[self.asset] = 100000000
        node.setfeeexchangerates(rates)
        self.nodes[1].setfeeexchangerates(rates)

    # ------------------------------------------------------------ helpers
    def fresh_lock(self, amount=50_000_000, floor=None, deadline_in=20, margin=6, **kw):
        """A lock built now: F = the Sequentia tip's anchor, D and R ahead of Bitcoin's tip."""
        node = self.nodes[0]
        btc_tip = self.btc_rpc().getblockcount()
        F = self.anchor_of(node.getbestblockhash()) if floor is None else floor
        lock = self.new_lock(amount, F, btc_tip + deadline_in, btc_tip + deadline_in + margin, **kw)
        self.mine(1)
        lock["C"] = self.lock_coin_anchor(lock)
        return lock

    def paid(self, lock, sat=None, wallet=None, addr=None):
        """Pay the lock's address and mine the payment; return (txid, proof, vout)."""
        a = addr or lock["addr"]
        txid = self.pay(a, lock["amount"] if sat is None else sat, wallet)
        bh = self.btc_mine(1)[0]
        self.mine_anchored(self.btc_rpc().getblockheader(bh)["height"])
        pr = self.proof_for(txid, lock["C"], bh)
        return txid, pr, self.vout_paying(txid, a, bh)

    def wait_for(self, pred, what, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            v = pred()
            if v:
                return v
            time.sleep(0.25)
        raise AssertionError("timed out: " + what)

    def double_spend(self, txid):
        """Replace an unconfirmed payment by one paying the buyer back."""
        b = self.btc_rpc()
        raw = b.getrawtransaction(txid, True)
        ins = [{"txid": i["txid"], "vout": i["vout"]} for i in raw["vin"]]
        total = sum(self.bbuyer.gettransaction(i["txid"])["details"] and
                    Decimal(b.getrawtransaction(i["txid"], True)["vout"][i["vout"]]["value"]) for i in ins) \
            if False else None
        # Fund from the same inputs back to the buyer, at a higher fee.
        vin_value = Decimal(0)
        for i in ins:
            prev = self.bbuyer.gettransaction(i["txid"], True, True)["decoded"]
            vin_value += Decimal(str(prev["vout"][i["vout"]]["value"]))
        out_value = vin_value - Decimal("0.001")
        new = b.createrawtransaction(ins, [{self.bbuyer.getnewaddress(): out_value}])
        signed = self.bbuyer.signrawtransactionwithwallet(new)["hex"]
        return b.sendrawtransaction(signed)

    # -------------------------------------------------------------- scenarios
    def test_swap_and_negatives(self):
        node = self.nodes[0]
        self.step("1. The swap: lock, plain bitcoin payment, claim with the proof")
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        good = self.claim(lock, pr["record"], vout)
        vs = node.decoderawtransaction(good.serialize().hex())["vsize"]
        self.log.info("    proof %d bytes (depth %d); claim vsize %d", len(pr["record"]), len(pr["pbranch"]), vs)
        self.results.append(("proof_bytes_depth%d" % len(pr["pbranch"]), len(pr["record"])))
        self.results.append(("claim_vsize", vs))
        node.sendrawtransaction(good.serialize().hex())
        blk = self.mine_anchored(pr["height"])
        assert good.hash in node.getblock(blk)["tx"]
        self.log.info("    claim mined in %s (anchor %d, payment at %d)", blk[:16], self.anchor_of(blk), pr["height"])
        self.sync_blocks()

        self.step("2. Negatives forced into blocks")
        self.neg_floor()
        self.neg_wrong_script()
        self.neg_short_amount()
        self.neg_after_deadline()
        self.neg_forged_path_and_64_bytes()
        self.neg_coin_anchor_and_unconfirmed()
        self.neg_canonical_records()
        self.neg_second_lock()
        self.neg_refund()
        self.neg_relative_floor()
        self.neg_not_in_anchor_chain()
        self.neg_below_anchor()
        self.reorg_double_spend()
        self.reorg_remined()
        self.same_txid_replacement()
        self.bitcoin_reorg_sweep()
        self.links_and_branches()
        self.reverse_direction()
        self.congestion()
        self.measure_distinct_blocks()
        self.only_tightens()

    def neg_floor(self):
        self.step("N1 a payment at or below the lock's floor F")
        # The buyer pays first; the lock is built after, with F at or above it.
        addr_lock = self.fresh_lock()
        ptx, pr, vout = self.paid(addr_lock)
        self.mine_anchored(pr["height"])
        late_lock = self.new_lock(addr_lock["amount"], pr["height"], pr["height"] + 20, pr["height"] + 26,
                                  program=bytes.fromhex(self.bseller.getaddressinfo(addr_lock["addr"])["witness_program"]),
                                  version=0)
        self.mine(1)
        late_lock["C"] = self.lock_coin_anchor(late_lock)
        rec = self.proof_for(ptx, late_lock["C"], pr["blockhash"])["record"]
        self.force([self.claim(late_lock, rec, vout)], "Script failed an OP_VERIFY operation")

    def neg_wrong_script(self):
        self.step("N2 a payment to another script")
        lock = self.fresh_lock()
        other = self.bbuyer.getnewaddress(address_type="bech32")
        ptx, pr, vout = self.paid(lock, addr=other)
        self.mine_anchored(pr["height"])
        self.force([self.claim(lock, pr["record"], vout)], "Script failed an OP_EQUALVERIFY operation")

    def neg_short_amount(self):
        self.step("N3 a payment one satoshi short")
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock, sat=lock["amount"] - 1)
        self.mine_anchored(pr["height"])
        self.force([self.claim(lock, pr["record"], vout)],
                   "Script evaluated without error but finished with a false/empty top stack element")

    def neg_after_deadline(self):
        self.step("N4 a payment mined after the deadline D")
        lock = self.fresh_lock(deadline_in=1)
        self.btc_mine(2)
        ptx, pr, vout = self.paid(lock)
        assert pr["height"] > lock["deadline"]
        self.mine_anchored(pr["height"])
        self.force([self.claim(lock, pr["record"], vout)], "Script failed an OP_VERIFY operation")

    def neg_forged_path_and_64_bytes(self):
        self.step("N5 a forged merkle path; a wrong coinbase hash; an inner node shown as a 64-byte payment")
        lock = self.fresh_lock()
        # A block of several transactions, the payment not paired with the coinbase.
        for _ in range(4):
            self.bminer.sendtoaddress(self.bminer.getnewaddress(), 1)
        ptx = self.pay(lock["addr"], lock["amount"])
        bh = self.btc_mine(1)[0]
        self.mine_anchored(self.btc_rpc().getblockcount())
        pr = self.proof_for(ptx, lock["C"], bh)
        vout = self.vout_paying(ptx, lock["addr"], bh)
        assert len(pr["leaves"]) >= 3 and len(pr["pbranch"]) >= 2
        cs = hashlib.sha256(pr["coinbase"]).digest()
        bad_branch = [bytes([pr["pbranch"][0][0] ^ 1]) + pr["pbranch"][0][1:]] + pr["pbranch"][1:]
        rec = payment_record(pr["height"], lock["C"], pr["header"], cs, pr["index"], bad_branch, pr["cbranch"], pr["payment"])
        self.force([self.claim(lock, rec, vout)], "bad-btcpay-merkle")
        rec = payment_record(pr["height"], lock["C"], pr["header"], bytes(32), pr["index"], pr["pbranch"], pr["cbranch"], pr["payment"])
        self.force([self.claim(lock, rec, vout)], "bad-btcpay-merkle")
        # The shallow forgery: the payment's level-1 node's two children, 64 bytes,
        # presented as a transaction one level up, with a coinbase preimage the
        # forger can compute (SHA256 of the coinbase's level-1 pair). The merkle
        # check passes; only the 64-byte rule refuses it.
        idx = 2  # any pair that does not hold the coinbase
        fb = merkle_branch(pr["leaves"], idx)
        pair = (pr["leaves"] + ([pr["leaves"][-1]] if len(pr["leaves"]) % 2 else []))
        sixty_four = pair[2] + pair[3]
        cb_pair = pair[0] + pair[1]
        shallow_cs = hashlib.sha256(cb_pair).digest()
        # Verify in Python that the forged record reaches the header's root.
        root = bytes.fromhex(self.btc_rpc().getblockheader(bh)["merkleroot"])[::-1]
        node_ = dsha(sixty_four)
        k = idx >> 1
        for sib in fb[1:]:
            node_ = dsha(sib + node_) if k & 1 else dsha(node_ + sib)
            k >>= 1
        assert node_ == root, "the 64-byte forgery reaches the merkle root"
        cnode = hashlib.sha256(shallow_cs).digest()
        for sib in pr["cbranch"][1:]:
            cnode = dsha(cnode + sib)
        assert cnode == root, "the forger's coinbase preimage reaches the merkle root"
        rec = payment_record(pr["height"], lock["C"], pr["header"], shallow_cs, idx >> 1, fb[1:],
                             pr["cbranch"][1:], sixty_four)
        self.force([self.claim(lock, rec, vout)], "bad-btcpay-tx-64-bytes")
        self.results.append(("N5 shallow forgery reaches root in python", True))
        # And the honest proof of the same block opens it.
        self.force([self.claim(lock, pr["record"], vout)])

    def neg_coin_anchor_and_unconfirmed(self):
        self.step("N6 a wrong coin anchor C; a claim of an unconfirmed lock")
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        self.mine_anchored(pr["height"])
        rec = self.proof_for(ptx, lock["C"] + 1, pr["blockhash"])["record"]
        self.force([self.claim(lock, rec, vout)], "bad-btcpay-coin-anchor")
        # Unconfirmed lock: the mempool refuses the claim.
        btc_tip = self.btc_rpc().getblockcount()
        l2 = self.new_lock(lock["amount"], 0, btc_tip + 20, btc_tip + 26,
                           program=bytes.fromhex(self.bseller.getaddressinfo(lock["addr"])["witness_program"]), version=0)
        c2 = self.claim(l2, self.proof_for(ptx, 0, pr["blockhash"])["record"], vout)
        try:
            self.nodes[0].sendrawtransaction(c2.serialize().hex())
            raise AssertionError("accepted")
        except JSONRPCException as e:
            assert "btcpay-coin-unconfirmed" in e.error["message"], e.error["message"]
            self.log.info("    mempool: %s", e.error["message"])
            self.results.append((self.current + " (mempool)", e.error["message"]))
        self.mine(1)

    def neg_canonical_records(self):
        self.step("N7 one encoding: a duplicate record, a non-canonical index, an unknown type")
        lock = self.fresh_lock()
        # A block of coinbase + 2: the payment is last on an odd level.
        self.bminer.sendtoaddress(self.bminer.getnewaddress(), 1)
        ptx = self.pay(lock["addr"], lock["amount"])
        bh = self.btc_mine(1)[0]
        self.mine_anchored(self.btc_rpc().getblockcount())
        pr = self.proof_for(ptx, lock["C"], bh)
        vout = self.vout_paying(ptx, lock["addr"], bh)
        self.force([self.spend_lock(lock, "claim", [pr["record"], pr["record"]], [scriptnum(vout)])], "bad-btcpay-duplicate")
        # The last leaf of an odd level is its own sibling; only index 2 (left) is canonical for it.
        assert len(pr["leaves"]) == 3
        last = self.btc_rpc().getblock(bh, 1)["tx"][2]
        lp = self.proof_for(last, lock["C"], bh)
        cs = hashlib.sha256(lp["coinbase"]).digest()
        rec = payment_record(lp["height"], lock["C"], lp["header"], cs, 3, lp["pbranch"], lp["cbranch"], lp["payment"])
        self.force([self.claim(lock, rec, 0)], "bad-btcpay-index")
        unknown = self.spend_lock(lock, "claim", [pr["record"], b"\x07junk"], [scriptnum(vout)])
        try:
            self.nodes[0].sendrawtransaction(unknown.serialize().hex())
            raise AssertionError("accepted")
        except JSONRPCException as e:
            assert "extra-pegin-witness" in e.error["message"], e.error["message"]
            self.log.info("    relay: %s", e.error["message"])
            self.results.append((self.current + " (relay)", e.error["message"]))
        # Consensus ignores an unknown type: a block with it is valid.
        self.force([unknown])
        self.results.append((self.current + " (unknown type in a block)", "accepted"))

    def neg_second_lock(self):
        self.step("N8 one payment against a second lock")
        lock = self.fresh_lock()
        same_addr = self.new_lock(lock["amount"], self.anchor_of(self.nodes[0].getbestblockhash()),
                                  lock["deadline"], lock["refund_at"],
                                  program=bytes.fromhex(self.bseller.getaddressinfo(lock["addr"])["witness_program"]),
                                  version=0)
        fresh_addr = self.fresh_lock()
        self.mine(1)
        same_addr["C"] = self.lock_coin_anchor(same_addr)
        ptx, pr, vout = self.paid(lock)
        self.mine_anchored(pr["height"])
        self.force([self.claim(fresh_addr, self.proof_for(ptx, fresh_addr["C"], pr["blockhash"])["record"], vout)],
                   "Script failed an OP_EQUALVERIFY operation")
        # A second lock naming the same address is opened by the same payment:
        # the seller's own error, which a fresh address per lock prevents.
        self.force([self.claim(lock, pr["record"], vout),
                    self.claim(same_addr, self.proof_for(ptx, same_addr["C"], pr["blockhash"])["record"], vout)])
        self.results.append((self.current + " (same address twice)", "both opened, as specified in 8.6"))

    def neg_refund(self):
        self.step("N9 the refund before R, with another floor, and at R")
        lock = self.fresh_lock(deadline_in=2, margin=6)
        self.mine(1)
        self.force([self.refund(lock)], "bad-anchorlock-not-reached")
        self.btc_mine(lock["refund_at"] - self.btc_rpc().getblockcount())
        self.mine_anchored(lock["refund_at"])
        self.force([self.refund(lock, floor=lock["refund_at"] + 0 - 1)], "Anchor floor differs")
        self.force([self.refund(lock, floor=None)])

    def neg_relative_floor(self):
        self.step("N10 the relative floor a covenant asks for (h > C)")
        node = self.nodes[0]
        cov = CScript([OP_INSPECTBTCPAYMENT, OP_GREATERTHAN])
        tap = taproot_construct(NUMS, [("cov", cov)])
        addr = program_to_witness(1, tap.scriptPubKey[2:])
        # A payment mined before the covenant output exists.
        early = self.bbuyer.sendtoaddress(self.bbuyer.getnewaddress(address_type="bech32"), Decimal("0.1"))
        eb = self.btc_mine(1)[0]
        self.mine_anchored(self.btc_rpc().getblockcount())
        txid = node.sendtoaddress(address=addr, amount=1, assetlabel=self.asset, fee_asset_label=self.asset)
        self.mine(1)
        tx = tx_from_hex(node.getrawtransaction(txid))
        vout = [i for i, o in enumerate(tx.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        C = self.anchor_of(node.getrawtransaction(txid, True)["blockhash"])
        lockish = dict(txid=txid, vout=vout, txout=tx.vout[vout], tap=tap, cov=cov, asset_amount=100000000)

        def spend(rec):
            t = CTransaction()
            t.nVersion = 2
            t.vin.append(CTxIn(COutPoint(int(txid, 16), vout), nSequence=SEQ_NONFINAL_RBF))
            asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
            t.vout.append(CTxOut(CTxOutValue(100000000 - 5000), bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"]), asset))
            t.vout.append(CTxOut(CTxOutValue(5000), b"", asset))
            t.wit.vtxinwit.append(CTxInWitness())
            t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
            t.wit.vtxinwit[0].peginWitness.stack = [rec]
            control = bytes([tap.leaves["cov"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["cov"].merklebranch
            t.wit.vtxinwit[0].scriptWitness.stack = [bytes(cov), control]
            t.rehash()
            return t
        self.force([spend(self.proof_for(early, C, eb)["record"])],
                   "Script evaluated without error but finished with a false/empty top stack element")
        late = self.bbuyer.sendtoaddress(self.bbuyer.getnewaddress(address_type="bech32"), Decimal("0.1"))
        lb = self.btc_mine(1)[0]
        self.mine_anchored(self.btc_rpc().getblockcount())
        self.force([spend(self.proof_for(late, C, lb)["record"])])

    def neg_not_in_anchor_chain(self):
        self.step("N11 a proven block that is not in the anchor chain (orphaned)")
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        self.mine_anchored(pr["height"])
        b = self.btc_rpc()
        b.invalidateblock(pr["blockhash"])
        self.double_spend(ptx)
        self.btc_mine(3)
        # Wait for the watcher to drop Sequentia blocks anchored on the orphan.
        self.wait_for(lambda: self.anchor_of(self.nodes[0].getbestblockhash()) < pr["height"] or
                      self.nodes[0].getblockheader(self.nodes[0].getbestblockhash())["anchorhash"] != pr["blockhash"],
                      "the watcher follows Bitcoin")
        self.mine_anchored(b.getblockcount())
        self.force([self.claim(lock, pr["record"], vout)], "bad-btcpay-not-in-anchor-chain")

    def restart_seq0(self, extra=None):
        self.restart_node(0, self.seq_args(extra))
        self.nodes[0].loadwallet("w")
        self.nodes[0].setfeeexchangerates(self.nodes[1].getfeeexchangerates())
        self.connect_nodes(0, 1)

    def neg_below_anchor(self):
        self.step("N12 a claim in a block anchored below its payment")
        lock = self.fresh_lock()
        # A producer whose anchor trails Bitcoin's tip (-anchorminconf=3).
        self.restart_seq0(["-anchorminconf=3"])
        ptx = self.pay(lock["addr"], lock["amount"])
        bh = self.btc_mine(1)[0]
        vout = self.vout_paying(ptx, lock["addr"], bh)
        self.mine(1)
        pr = self.proof_for(ptx, lock["C"], bh)
        assert self.anchor_of(self.nodes[0].getbestblockhash()) < pr["height"] and not pr["links"]
        self.force([self.claim(lock, pr["record"], vout)], "bad-btcpay-not-in-anchor-chain")
        self.restart_seq0()
        self.mine_anchored(pr["height"])
        self.force([self.claim(lock, self.proof_for(ptx, lock["C"], bh)["record"], vout)])

    def reorg_double_spend(self):
        self.step("N13 a reorganization with the payment double-spent: the claim is unminable for good")
        node = self.nodes[0]
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        c = self.claim(lock, pr["record"], vout)
        node.sendrawtransaction(c.serialize().hex())
        blk = self.mine_anchored(pr["height"])
        assert c.hash in node.getblock(blk)["tx"]
        b = self.btc_rpc()
        b.invalidateblock(pr["blockhash"])
        self.double_spend(ptx)
        self.btc_mine(3)
        self.wait_for(lambda: node.getblockheader(blk)["confirmations"] < 0, "the claim's block is disconnected")
        assert c.hash not in node.getrawmempool()
        self.mine(2)
        assert c.hash not in node.getrawmempool()
        self.force([c], "bad-btcpay-not-in-anchor-chain")
        self.btc_mine(5)
        self.mine_anchored(b.getblockcount())
        self.force([c], "bad-btcpay-not-in-anchor-chain")
        self.results.append((self.current + " (after 5 more Bitcoin blocks)", "still refused"))

    def reorg_remined(self):
        self.step("N14 a reorganization with the payment mined again: the node re-attaches a fresh proof itself")
        node = self.nodes[0]
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        c = self.claim(lock, pr["record"], vout)
        node.sendrawtransaction(c.serialize().hex())
        blk = self.mine_anchored(pr["height"])
        assert c.hash in node.getblock(blk)["tx"]
        old_wtxid = node.getrawtransaction(c.hash, True)["hash"]
        b = self.btc_rpc()
        b.invalidateblock(pr["blockhash"])
        self.btc_mine(2)  # the payment is in the first new block
        new_bh = self.bbuyer.gettransaction(ptx)["blockhash"]
        assert new_bh != pr["blockhash"]
        self.wait_for(lambda: node.getblockheader(blk)["confirmations"] < 0, "the claim's block is disconnected")
        # The claim waits until the chain records an anchor at the payment's new
        # block; then, with nobody re-signing, the node links a fresh proof itself.
        self.mine_anchored(b.getblockcount())
        self.wait_for(lambda: c.hash in node.getrawmempool(), "the node re-attaches a fresh proof", timeout=30)
        new_wtxid = node.getmempoolentry(c.hash)["wtxid"]
        assert new_wtxid != old_wtxid
        blk2 = self.mine(1)[0]
        assert c.hash in node.getblock(blk2)["tx"]
        self.results.append((self.current, "claim %s re-mined with a fresh proof (wtxid %s -> %s)" % (c.hash, old_wtxid[:16], new_wtxid[:16])))

    def same_txid_replacement(self):
        self.step("N15 a heavier proof of another qualifying payment is displaced by the lighter one")
        node = self.nodes[0]
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        # The seller pays its own address from a large transaction.
        outs = {self.bminer.getnewaddress(): Decimal("0.01") for _ in range(60)}
        outs[lock["addr"]] = Decimal(lock["amount"]) / Decimal(100000000)
        heavy_tx = self.bminer.sendmany("", outs)
        hb = self.btc_mine(1)[0]
        self.mine_anchored(self.btc_rpc().getblockcount())
        hp = self.proof_for(heavy_tx, lock["C"], hb)
        hv = self.vout_paying(heavy_tx, lock["addr"], hb)
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        heavy = self.claim(lock, hp["record"], hv, out_spk=spk)
        light = self.claim(lock, pr["record"], vout, out_spk=spk)
        assert heavy.hash == light.hash
        node.sendrawtransaction(heavy.serialize().hex())
        try:
            node.sendrawtransaction(heavy.serialize().hex())
        except JSONRPCException:
            pass
        heavy_wtxid = node.getmempoolentry(heavy.hash)["wtxid"]
        node.sendrawtransaction(light.serialize().hex())
        light_wtxid = node.getmempoolentry(light.hash)["wtxid"]
        assert light_wtxid != heavy_wtxid
        try:
            node.sendrawtransaction(heavy.serialize().hex())
            raise AssertionError("heavier variant accepted over the lighter one")
        except JSONRPCException as e:
            assert "txn-same-nonwitness-data-in-mempool" in e.error["message"], e.error["message"]
            self.results.append((self.current + " (heavy after light)", e.error["message"]))
        self.results.append((self.current, "light (%d vB) replaced heavy (%d vB)" % (
            node.decoderawtransaction(light.serialize().hex())["vsize"], node.decoderawtransaction(heavy.serialize().hex())["vsize"])))
        self.mine_anchored(self.btc_rpc().getblockcount())

    def bitcoin_reorg_sweep(self):
        self.step("N17 (PV13 S1) a payment not yet recorded as an anchor, then orphaned: the claim is refused by the mempool and by this node's own template")
        node = self.nodes[0]
        lock = self.fresh_lock()
        ptx = self.pay(lock["addr"], lock["amount"])
        bh = self.btc_mine(1)[0]
        vout = self.vout_paying(ptx, lock["addr"], bh)
        pr = self.proof_for(ptx, lock["C"], bh)
        assert self.anchor_of(node.getbestblockhash()) < pr["height"]
        c = self.claim(lock, pr["record"], vout)
        try:
            node.sendrawtransaction(c.serialize().hex())
            raise AssertionError("accepted before its payment was recorded as an anchor")
        except JSONRPCException as e:
            assert "btcpay-not-in-anchor-chain" in e.error["message"], e.error["message"]
            self.results.append((self.current + " (mempool)", e.error["message"]))
        b = self.btc_rpc()
        b.invalidateblock(bh)
        self.double_spend(ptx)
        self.btc_mine(2)
        time.sleep(0.5)  # before the watcher's next tick
        self.force([c], "bad-btcpay-not-in-anchor-chain")
        # And with one link to the new branch's block at that height: not its ancestor.
        newb = b.getblockhash(pr["height"] + 1)
        rec = payment_record(pr["height"], lock["C"], pr["header"], hashlib.sha256(pr["coinbase"]).digest(), pr["index"],
                             pr["pbranch"], pr["cbranch"], pr["payment"], links=[bytes.fromhex(b.getblockheader(newb, False))])
        self.mine_anchored(b.getblockcount())
        self.force([self.claim(lock, rec, vout)], "bad-btcpay-links")

    def links_and_branches(self):
        self.step("N21 linked headers: valid links, a broken link, a false height, too many links, an anchor recorded only on another branch")
        node = self.nodes[0]
        lock = self.fresh_lock()
        ptx = self.pay(lock["addr"], lock["amount"])
        bh = self.btc_mine(1)[0]
        h = self.btc_rpc().getblockheader(bh)["height"]
        vout = self.vout_paying(ptx, lock["addr"], bh)
        # Recorded at h on a branch that is then replaced.
        side = self.mine_anchored(h)
        self.btc_mine(3)
        node.invalidateblock(side)
        self.mine_anchored(h + 3)
        assert self.recorded_anchor_at_or_above(h)[0] == h + 3
        pr = self.proof_for(ptx, lock["C"], bh, link=False)
        cs = hashlib.sha256(pr["coinbase"]).digest()
        args = (lock["C"], pr["header"], cs, pr["index"], pr["pbranch"], pr["cbranch"], pr["payment"])
        self.force([self.claim(lock, payment_record(h, *args), vout)], "bad-btcpay-not-in-anchor-chain")
        links = self.link_headers(h, h + 3)
        broken = [links[0], links[2], links[1]]
        self.force([self.claim(lock, payment_record(h, *args, links=broken), vout)], "bad-btcpay-links")
        self.force([self.claim(lock, payment_record(h + 1, *args, links=links), vout)], "bad-btcpay-not-in-anchor-chain")
        self.force([self.claim(lock, payment_record(h, *args, links=[links[0]] * 3, nlinks=2017), vout)], "bad-btcpay-encoding")
        good = self.claim(lock, payment_record(h, *args, links=links), vout)
        node.sendrawtransaction(good.serialize().hex())
        blk = self.mine(1)[0]
        assert good.hash in node.getblock(blk)["tx"]
        self.results.append((self.current + " (3 links to the recorded anchor)", "mined"))
        node.reconsiderblock(side)

    def reverse_direction(self):
        self.step("N18 the other direction: the taker holds the asset and locks it on another node; the maker pays")
        n0, n1 = self.nodes[0], self.nodes[1]
        self.connect_nodes(0, 1) if not n0.getpeerinfo() else None
        if not n1.listwallets():
            n1.loadwallet("w")
        addr1 = n1.getnewaddress()
        n0.sendtoaddress(address=addr1, amount=20, assetlabel=self.asset, fee_asset_label=self.asset)
        self.mine(1)
        self.sync_blocks()
        # n1 builds the lock (the node without the rule can still build and broadcast it).
        saved = self.nodes[0]
        lock = None
        try:
            self.nodes[0] = n1
            btc_tip = self.btc_rpc().getblockcount()
            lock = self.new_lock(30_000_000, self.anchor_of(n1.getbestblockhash(), n1), btc_tip + 20, btc_tip + 26)
        finally:
            self.nodes[0] = saved
        self.sync_mempools()
        self.mine(1)
        self.sync_blocks()
        lock["C"] = self.lock_coin_anchor(lock)
        txid = self.bminer.sendtoaddress(lock["addr"], Decimal("0.3"))
        bh = self.btc_mine(1)[0]
        self.mine_anchored(self.btc_rpc().getblockcount())
        pr = self.proof_for(txid, lock["C"], bh)
        c = self.claim(lock, pr["record"], self.vout_paying(txid, lock["addr"], bh))
        n0.sendrawtransaction(c.serialize().hex())
        blk = self.mine_anchored(pr["height"])
        assert c.hash in n0.getblock(blk)["tx"]
        self.sync_blocks()
        self.results.append((self.current, "claim mined"))

    def congestion(self):
        self.step("N19 blocks full of higher-fee transactions through the margin: the top fee tier gets in")
        node = self.nodes[0]
        self.restart_seq0(["-blockmaxweight=12000"])
        # Coins for the fillers.
        outs = {node.getnewaddress(): 1 for _ in range(60)}
        node.sendmany(amounts=outs, fee_asset="bitcoin")
        for _ in range(3):
            self.mine(1)
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        tiers = [self.claim(lock, pr["record"], vout, fee=f, out_spk=spk) for f in (400, 4000, 40000)]
        node.sendrawtransaction(tiers[0].serialize().hex())
        fill = []
        for _ in range(50):
            fill.append(node.sendtoaddress(address=node.getnewaddress(), amount=Decimal("0.5"), fee_asset_label="bitcoin",
                                           fee_rate=50))
        blk = self.mine(1)[0]
        assert self.anchor_of(blk) >= pr["height"]
        txs = node.getblock(blk)["tx"]
        assert tiers[0].hash not in txs, "the lowest tier is crowded out"
        n_fill = len([t for t in fill if t in txs])
        self.log.info("    a full block took %d fillers and not the lowest tier", n_fill)
        # The completion service moves up the tiers.
        node.sendrawtransaction(tiers[2].serialize().hex())
        blk = self.mine(1)[0]
        txs = node.getblock(blk)["tx"]
        assert tiers[2].hash in txs, "the top tier gets in"
        self.results.append((self.current, "tier 400 crowded out by %d fillers at 50/vB; tier 40000 mined in the next full block (%d fillers left behind)" %
                             (n_fill, len([t for t in fill if t in node.getrawmempool()]))))
        self.restart_seq0()
        self.mine(5)

    def measure_distinct_blocks(self):
        n = int(os.environ.get("BTCPAY_MEASURE_N", "296"))
        self.step("N20 cost of a block whose proofs name %d distinct Bitcoin blocks, cold cache" % n)
        node = self.nodes[0]
        btc_tip = self.btc_rpc().getblockcount()
        lock = self.new_lock(1000, 0, btc_tip + 100000, btc_tip + 100006, asset_amount=Decimal("0.001"))
        # n lock outputs of the same script.
        addr = program_to_witness(1, lock["tap"].scriptPubKey[2:])
        outpoints = []
        for k in range(n):
            txid = node.sendtoaddress(address=addr, amount=Decimal("0.001"), assetlabel=self.asset,
                                      fee_asset_label=self.asset)
            t = tx_from_hex(node.getrawtransaction(txid))
            v = [i for i, o in enumerate(t.vout) if o.scriptPubKey == lock["tap"].scriptPubKey][0]
            outpoints.append((txid, v, t.vout[v]))
            if k % 20 == 19:
                self.mine(1)
        self.mine(1)
        coin_anchor = {txid: self.anchor_of(node.getrawtransaction(txid, True)["blockhash"]) for txid, _, _ in outpoints}
        proofs = []
        for k in range(n):
            ptx = self.bbuyer.sendtoaddress(lock["addr"], Decimal("0.00001000"))
            bh = self.btc_mine(1)[0]
            self.mine(1)  # record each Bitcoin block as an anchor
            proofs.append((self.proof_for(ptx, coin_anchor[outpoints[k][0]], bh), self.vout_paying(ptx, lock["addr"], bh)))
        self.mine_anchored(self.btc_rpc().getblockcount())
        claims = []
        for k, (pr, v) in enumerate(proofs):
            l = dict(lock, vout=outpoints[k][1], txout=outpoints[k][2], txid=outpoints[k][0], asset_amount=100000)
            claims.append(self.claim(l, pr["record"], v, fee=1000))
        t0 = time.time()
        blk = self.force(claims)
        t_build = time.time() - t0
        weight = node.getblock(blk)["weight"]
        self.log.info("    block %s: %d claims, weight %d; built and checked warm in %.2fs", blk[:16], n, weight, t_build)
        # A fresh node: every proven block is a cache miss.
        initialize_datadir(self.options.tmpdir, 2, "elementsregtest")
        self.add_nodes(1, [self.seq_args()], chain=["elementsregtest"])
        self.start_node(2)
        t0 = time.time()
        self.connect_nodes(2, 0)
        self.sync_blocks([self.nodes[0], self.nodes[2]], timeout=600)
        t_sync = time.time() - t0
        log = open(os.path.join(self.nodes[2].datadir, "elementsregtest", "debug.log")).read().splitlines()
        lines = [l for l in log if "BtcPayment:" in l and "%d distinct" % n in l]
        self.log.info("    cold node: %s", lines[-1] if lines else "(no bench line)")
        self.results.append((self.current, {"claims": n, "weight": weight, "warm_build_s": round(t_build, 2),
                                            "cold_sync_whole_chain_s": round(t_sync, 2),
                                            "cold_bench": lines[-1] if lines else None}))
        self.stop_node(2)

    def only_tightens(self):
        self.step("N16 the rule only tightens: a node without it accepts every block, and accepts a claim without proof that this node refuses")
        self.sync_blocks(self.nodes[:2])
        assert_equal(self.nodes[1].getbestblockhash(), self.nodes[0].getbestblockhash())
        lock = self.fresh_lock()
        self.sync_blocks(self.nodes[:2])
        noproof = self.spend_lock(lock, "claim", [], [b""])
        self.force([noproof], "No valid proof of a bitcoin payment")
        self.disconnect_nodes(0, 1)
        n1 = self.nodes[1]
        n1.generateblock(n1.getnewaddress(), [noproof.serialize().hex()], invalid_call=False)
        self.results.append((self.current + " (old node)", "mined the claim without proof"))


if __name__ == '__main__':
    BtcPaymentProofTest().main()
