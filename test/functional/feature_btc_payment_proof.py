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
block was refused is asserted. Requires Bitcoin Core, named by PARENT_BITCOIND
(default /usr/local/bin/bitcoind); BITCOIND names the Sequentia node under test.
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
                                   OP_VERIFY, OP_1, OP_2DROP, OP_IF, OP_ENDIF, SIGHASH_DEFAULT, TaprootSignatureHash, taproot_construct)
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, assert_raises_rpc_error, initialize_datadir, rpc_port, p2p_port

OP_INSPECTBTCPAYMENT = CScriptOp(0xe5)
OP_INSPECTBTCOUTPUT = CScriptOp(0xe6)
OP_CHECKANCHORHEIGHTVERIFY = CScriptOp(0xe7)

BITCOIND = os.environ.get("PARENT_BITCOIND", "/usr/local/bin/bitcoind")
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
            raise SkipTest("needs Bitcoin Core's bitcoind (set PARENT_BITCOIND)")

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
                                          "-rpcport=%d" % self.btc_port, "-listen=0", "-server", "-debug=rpc",
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

    def double_spend(self, txid, wallet=None):
        """Replace an unconfirmed payment by one paying the payer back."""
        b = self.btc_rpc()
        if wallet is not None:
            raw = b.getrawtransaction(txid, True)
            ins = [{"txid": i["txid"], "vout": i["vout"]} for i in raw["vin"]]
            value = sum(Decimal(str(wallet.gettransaction(i["txid"], True, True)["decoded"]["vout"][i["vout"]]["value"]))
                        for i in ins)
            new = b.createrawtransaction(ins, [{wallet.getnewaddress(): value - Decimal("0.001")}])
            return b.sendrawtransaction(wallet.signrawtransactionwithwallet(new)["hex"])
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
        self.refund_waits_for_the_anchor()
        self.proof_preferred_over_floor()
        self.neg_relative_floor()
        self.same_block_coin_anchor()
        self.records_in_order()
        self.neg_not_in_anchor_chain()
        self.neg_below_anchor()
        self.reorg_double_spend()
        self.reorg_remined()
        self.same_txid_replacement()
        self.records_nobody_reads()
        self.every_variant_kept()
        self.variant_fee_budget()
        self.seller_fills_the_store()
        self.current_variant_kept()
        self.replacement_keeps_descendants()
        self.store_order_irrelevant()
        self.anchoring_first()
        self.bitcoin_reorg_sweep()
        self.links_and_branches()
        self.false_height_record()
        self.one_encoding_edges()
        self.reverse_direction()
        self.congestion()
        self.measure_distinct_blocks()
        self.opcode_cost()
        self.orphaned_and_longest_links()
        self.variant_flood()
        self.waiting_set()
        self.waiting_set_holds_the_disconnected_blocks()
        self.lighter_by_the_incremental_rate()
        self.search_budget()
        self.lagging_daemon()
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

    def refund_waits_for_the_anchor(self):
        self.step("N23 (PV13 S6) a refund whose floor Bitcoin has reached, but no block has anchored, is refused by the mempool and never displaces the claim")
        node = self.nodes[0]
        b = self.btc_rpc()
        lock = self.fresh_lock(deadline_in=2, margin=6)
        ptx, pr, vout = self.paid(lock)
        c = self.claim(lock, pr["record"], vout, fee=5000)
        # A stall: Bitcoin reaches R and no Sequentia block is made.
        self.btc_mine(lock["refund_at"] - b.getblockcount())
        tip_anchor = self.anchor_of(node.getbestblockhash())
        assert tip_anchor < lock["refund_at"] == b.getblockcount()
        r = self.refund(lock, fee=50000)
        # On its own the refund cannot be mined yet, so it is not pooled.
        assert_raises_rpc_error(-26, "anchorlock-not-reached", node.sendrawtransaction, r.serialize().hex())
        self.results.append((self.current, "refund refused by the mempool at Bitcoin tip R=%d, tip anchored %d: anchorlock-not-reached" % (lock["refund_at"], tip_anchor)))
        # Against the pooled claim it is refused first for the claim's proof (N27).
        node.sendrawtransaction(c.serialize().hex())
        assert_raises_rpc_error(-26, "btcpay-floor-replaces-proof", node.sendrawtransaction, r.serialize().hex())
        assert c.hash in node.getrawmempool()
        blk = self.mine(1)[0]
        assert c.hash in node.getblock(blk)["tx"]
        self.log.info("    the claim was mined in the next block (anchored %d)", self.anchor_of(blk))

    def proof_preferred_over_floor(self):
        self.step("N27 a floor-only spend never replaces a proof-reading one; a proof-reading one replaces it whatever the fees; key spends follow the fees (no pin)")
        node = self.nodes[0]
        b = self.btc_rpc()
        # (a) After R is anchored, the claim is pooled first: the refund, paying
        # ten times more, is refused, and the claim is mined.
        lock = self.fresh_lock(deadline_in=2, margin=6)
        ptx, pr, vout = self.paid(lock)
        self.btc_mine(lock["refund_at"] - b.getblockcount())
        self.mine_anchored(lock["refund_at"])
        c = self.claim(lock, pr["record"], vout, fee=5000)
        node.sendrawtransaction(c.serialize().hex())
        r = self.refund(lock, fee=50000)
        assert_raises_rpc_error(-26, "btcpay-floor-replaces-proof", node.sendrawtransaction, r.serialize().hex())
        self.results.append((self.current + " (a)", "refund at fee 50000 after R refused against the pooled claim at 5000: btcpay-floor-replaces-proof"))
        blk = self.mine(1)[0]
        assert c.hash in node.getblock(blk)["tx"]
        self.log.info("    (a) the refund after R did not replace the pooled claim; the claim was mined")
        # (b) After R, the refund is pooled first (paying more, not signalling
        # replacement): the claim replaces it, and is mined.
        lock = self.fresh_lock(deadline_in=2, margin=6)
        ptx, pr, vout = self.paid(lock)
        self.btc_mine(lock["refund_at"] - b.getblockcount())
        self.mine_anchored(lock["refund_at"])
        r = self.refund(lock, fee=50000, nseq=0xfffffffe)
        node.sendrawtransaction(r.serialize().hex())
        c = self.claim(lock, pr["record"], vout, fee=5000)
        node.sendrawtransaction(c.serialize().hex())
        pool = node.getrawmempool()
        assert c.hash in pool and r.hash not in pool
        blk = self.mine(1)[0]
        assert c.hash in node.getblock(blk)["tx"]
        self.results.append((self.current + " (b)", "claim at fee 5000 replaced the pooled refund at 50000 (no RBF signal) and was mined"))
        self.log.info("    (b) the claim replaced the pooled refund (higher fee, no signal) and was mined")
        # (c) No pin: a coin with a keyless proof leaf and a key leaf. Anyone
        # spends it by the proof leaf with an old payment at a low fee, choosing
        # its nSequence (nothing signs it): signalling replacement or not
        # (PV15 X4), the owner's key spend, paying more, replaces it; a key spend
        # paying less does not.
        for nseq in (SEQ_NONFINAL_RBF, 0xfffffffe, 0xffffffff):
            sk = generate_privkey()
            pk = compute_xonly_pubkey(sk)[0]
            proof_leaf = CScript([OP_INSPECTBTCPAYMENT, OP_2DROP, OP_1])
            key_leaf = CScript([pk, OP_CHECKSIG])
            tap = taproot_construct(NUMS, [("proof", proof_leaf), ("key", key_leaf)])
            txid = node.sendtoaddress(address=program_to_witness(1, tap.scriptPubKey[2:]), amount=10,
                                      assetlabel=self.asset, fee_asset_label=self.asset, replaceable=False)
            t = tx_from_hex(node.getrawtransaction(txid))
            n = [i for i, o in enumerate(t.vout) if o.scriptPubKey == tap.scriptPubKey][0]
            blk = self.mine(1)[0]
            coin = dict(tap=tap, proof=proof_leaf, key=key_leaf, txid=txid, vout=n, txout=t.vout[n], asset_amount=1000000000)
            old = self.proof_for(ptx, self.anchor_of(blk), pr["blockhash"])
            asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
            spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
            anyone = CTransaction()
            anyone.nVersion = 2
            anyone.vin.append(CTxIn(COutPoint(int(txid, 16), n), nSequence=nseq))
            anyone.vout.append(CTxOut(CTxOutValue(1000000000 - 3000), spk, asset))
            anyone.vout.append(CTxOut(CTxOutValue(3000), b"", asset))
            anyone.wit.vtxinwit.append(CTxInWitness())
            anyone.wit.vtxoutwit = [CTxOutWitness() for _ in anyone.vout]
            anyone.wit.vtxinwit[0].peginWitness.stack = [old["record"]]
            control = bytes([tap.leaves["proof"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["proof"].merklebranch
            anyone.wit.vtxinwit[0].scriptWitness.stack = [bytes(proof_leaf), control]
            anyone.rehash()
            node.sendrawtransaction(anyone.serialize().hex())
            cheap = self.spend_lock(coin, "key", [], [], fee=2000, sign_with=sk, out_spk=spk)
            assert_raises_rpc_error(-26, "insufficient fee", node.sendrawtransaction, cheap.serialize().hex())
            dear = self.spend_lock(coin, "key", [], [], fee=50000, sign_with=sk, out_spk=spk)
            node.sendrawtransaction(dear.serialize().hex())
            pool = node.getrawmempool()
            assert dear.hash in pool and anyone.hash not in pool
            self.results.append((self.current + " (c) nSequence 0x%08x" % nseq, "key spend at 50000 replaced a keyless proof spend at 3000; at 2000 refused: insufficient fee"))
            self.log.info("    (c) nSequence 0x%08x, no pin: the key spend paying more replaced the keyless proof spend; paying less it was refused", nseq)
            self.mine(1)

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

    def covenant_coin(self, leaf):
        """A coin of 1 unit under a one-leaf taproot with an unspendable key; returns
        the funding transaction (unconfirmed) and a spender taking a slot."""
        node = self.nodes[0]
        tap = taproot_construct(NUMS, [("leaf", leaf)])
        txid = node.sendtoaddress(address=program_to_witness(1, tap.scriptPubKey[2:]), amount=1,
                                  assetlabel=self.asset, fee_asset_label=self.asset)
        tx = tx_from_hex(node.getrawtransaction(txid))
        vout = [i for i, o in enumerate(tx.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        control = bytes([tap.leaves["leaf"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["leaf"].merklebranch
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])

        def spend(slot):
            t = CTransaction()
            t.nVersion = 2
            t.vin.append(CTxIn(COutPoint(int(txid, 16), vout), nSequence=SEQ_NONFINAL_RBF))
            asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
            t.vout.append(CTxOut(CTxOutValue(100000000 - 5000), spk, asset))
            t.vout.append(CTxOut(CTxOutValue(5000), b"", asset))
            t.wit.vtxinwit.append(CTxInWitness())
            t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
            t.wit.vtxinwit[0].peginWitness.stack = list(slot)
            t.wit.vtxinwit[0].scriptWitness.stack = [bytes(leaf), control]
            t.rehash()
            return t
        return txid, spend

    def same_block_coin_anchor(self):
        self.step("N36 (PV15 P05) a claim in the same block as its coin takes C = that block's own anchor")
        # Specification 4.2 check 7: C is the anchor height of the block that
        # created the coin; for a coin created in the claim's own block, that
        # block's anchor. A relative floor (h > C) then never opens there: no
        # proof in a block is above the block's own anchor. With the parent's
        # anchor it would.
        node = self.nodes[0]
        b = self.btc_rpc()
        txid, spend = self.covenant_coin(CScript([OP_INSPECTBTCPAYMENT, OP_GREATERTHAN]))
        lock_hex = node.getrawtransaction(txid)
        pay = self.bbuyer.sendtoaddress(self.bbuyer.getnewaddress(address_type="bech32"), Decimal("0.01"))
        pb = self.btc_mine(1)[0]
        p = b.getblockheader(pb)["height"]
        parent_anchor = self.anchor_of(node.getbestblockhash())
        assert parent_anchor < p
        pr = self.proof_for(pay, parent_anchor, pb)
        assert_equal(len(pr["links"]), 0)
        self.force([lock_hex, spend([pr["record"]])], "bad-btcpay-coin-anchor")
        own = payment_record(p, p, pr["header"], hashlib.sha256(pr["coinbase"]).digest(), pr["index"], pr["pbranch"],
                             pr["cbranch"], pr["payment"])
        self.force([lock_hex, spend([own])],
                   "Script evaluated without error but finished with a false/empty top stack element")
        # The anchor those blocks would have carried: the same template, without the claim.
        blk = self.force([lock_hex])
        assert_equal(self.anchor_of(blk), p)
        self.results.append((self.current, "C = parent's anchor %d: bad-btcpay-coin-anchor; C = the block's own "
                             "anchor %d: the relative floor h > C fails" % (parent_anchor, p)))

    def records_in_order(self):
        self.step("N37 (PV15 P12) relay takes the records in their one order; a block may carry another")
        node = self.nodes[0]
        F = self.anchor_of(node.getbestblockhash())
        leaf = CScript([F, OP_CHECKANCHORHEIGHTVERIFY, OP_DROP, OP_INSPECTBTCPAYMENT, OP_2DROP, OP_1])
        coins = [self.covenant_coin(leaf) for _ in range(2)]
        self.mine(1)
        C = self.anchor_of(node.getrawtransaction(coins[0][0], True)["blockhash"])
        pay = self.bbuyer.sendtoaddress(self.bbuyer.getnewaddress(address_type="bech32"), Decimal("0.01"))
        pb = self.btc_mine(1)[0]
        self.mine_anchored(self.btc_rpc().getblockheader(pb)["height"])
        rec = self.proof_for(pay, C, pb)["record"]
        swapped = coins[0][1]([floor_record(F), rec])
        assert_raises_rpc_error(-26, "extra-pegin-witness", node.sendrawtransaction, swapped.serialize().hex())
        node.sendrawtransaction(coins[0][1]([rec, floor_record(F)]).serialize().hex())
        blk = self.force([coins[1][1]([floor_record(F), rec])])
        self.results.append((self.current, "floor before proof: relay extra-pegin-witness; in order: accepted; "
                             "out of order forced into a block: valid (%s)" % blk[:16]))
        self.mine(1)

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

    def records_nobody_reads(self):
        self.step("N24 (PV13 S5) records on someone else's transaction: relay refuses a record no executed opcode read; the original displaces a pooled variant")
        node = self.nodes[0]
        unread = "Bitcoin payment record not read by any executed opcode"
        # (a) An ordinary wallet payment (a P2WPKH input): an outsider adds a
        # floor, or a whole payment record, to its input.
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        wpkh = node.getnewaddress("", "bech32")
        t = node.sendtoaddress(address=wpkh, amount=5, assetlabel=self.asset, fee_asset_label=self.asset)
        self.mine(1)
        n = [o["n"] for o in node.getrawtransaction(t, True)["vout"] if o["scriptPubKey"].get("address") == wpkh][0]
        raw = node.createrawtransaction(inputs=[{"txid": t, "vout": n}], outputs=[
            {node.getnewaddress(): Decimal("4.999"), "asset": self.asset}, {"fee": Decimal("0.001"), "fee_asset": self.asset}])
        signed = node.signrawtransactionwithwallet(raw)["hex"]
        orig = tx_from_hex(signed)
        assert node.testmempoolaccept([signed])[0]["allowed"]
        assert orig.wit.vtxinwit[0].peginWitness.stack == []
        # A payment record that is valid for this coin, but that its script never reads.
        own = self.proof_for(ptx, self.anchor_of(node.getrawtransaction(t, True)["blockhash"]), pr["blockhash"])
        for what, items in [("a floor", [floor_record(self.btc_rpc().getblockcount())]), ("a payment record", [own["record"]])]:
            v = tx_from_hex(signed)
            v.wit.vtxinwit[0].peginWitness.stack = list(items)
            assert v.hash == orig.hash
            assert_raises_rpc_error(-26, unread, node.sendrawtransaction, v.serialize().hex())
            self.results.append((self.current + " (%s on a P2WPKH input)" % what, unread))
            self.log.info("    %s on a P2WPKH input: refused by relay: %s", what, unread)
            if what == "a floor":
                floor_variant = v.serialize().hex()
        # The verdict reads only the transaction and the outputs it spends: the
        # same bytes get the same answer at another tip and after a restart
        # (no cache, a fresh mempool).
        self.btc_mine(1)
        self.mine_anchored(self.btc_rpc().getblockcount())
        self.restart_seq0()
        node = self.nodes[0]
        assert unread in node.testmempoolaccept([floor_variant])[0]["reject-reason"]
        assert node.testmempoolaccept([signed])[0]["allowed"]
        self.log.info("    the same verdicts at a new tip, after a restart")
        # The rule is relay policy: forced into a block, the variant is valid.
        v.rehash()
        blk = self.force([v])
        assert v.hash in node.getblock(blk)["tx"]
        self.log.info("    the variant forced into a block is valid (policy only): %s", blk[:16])
        # (b) A claim carrying a floor its leaf never reads.
        c = self.spend_lock(lock, "claim", [pr["record"], floor_record(lock["floor"])], [scriptnum(vout)])
        assert_raises_rpc_error(-26, unread, node.sendrawtransaction, c.serialize().hex())
        self.log.info("    a claim with a floor its leaf never reads: refused by relay")
        good = self.claim(lock, pr["record"], vout)
        node.sendrawtransaction(good.serialize().hex())
        self.mine(1)
        # (c) Same-txid replacement whenever the pooled one carries records: a
        # leaf that reads a floor only on its IF branch, pooled with the floor,
        # is displaced by the same spend taking the other branch without it.
        sk = generate_privkey()
        pk = compute_xonly_pubkey(sk)[0]
        floor = self.anchor_of(node.getbestblockhash())
        opt = CScript([pk, OP_CHECKSIGVERIFY, OP_IF, floor, OP_CHECKANCHORHEIGHTVERIFY, OP_DROP, OP_ENDIF, OP_1])
        tap = taproot_construct(NUMS, [("opt", opt)])
        txid = node.sendtoaddress(address=program_to_witness(1, tap.scriptPubKey[2:]), amount=10,
                                  assetlabel=self.asset, fee_asset_label=self.asset, replaceable=False)
        t = tx_from_hex(node.getrawtransaction(txid))
        n = [i for i, o in enumerate(t.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        self.mine(1)
        olock = dict(tap=tap, opt=opt, txid=txid, vout=n, txout=t.vout[n], asset_amount=1000000000, sk_b=sk)
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        with_floor = self.spend_lock(olock, "opt", [floor_record(floor)], [b"\x01"], sign_with=sk, out_spk=spk)
        without = self.spend_lock(olock, "opt", [], [b""], sign_with=sk, out_spk=spk)
        assert with_floor.hash == without.hash
        # The floor on the branch that does not read it: refused.
        skipped = self.spend_lock(olock, "opt", [floor_record(floor)], [b""], sign_with=sk, out_spk=spk)
        assert_raises_rpc_error(-26, unread, node.sendrawtransaction, skipped.serialize().hex())
        self.log.info("    a floor read only on the branch not taken: refused by relay")
        node.sendrawtransaction(with_floor.serialize().hex())
        node.sendrawtransaction(without.serialize().hex())
        assert_equal(node.getmempoolentry(without.hash)["wtxid"], node.decoderawtransaction(without.serialize().hex())["hash"])
        self.results.append((self.current + " (c)", "the variant without records displaced the pooled one with a floor"))
        self.log.info("    the spend without records displaced the pooled variant carrying a floor")
        node.sendrawtransaction(with_floor.serialize().hex())
        assert_equal(node.getmempoolentry(without.hash)["wtxid"], node.decoderawtransaction(without.serialize().hex())["hash"])
        assert_equal(node.testmempoolaccept([with_floor.serialize().hex()])[0]["reject-reason"],
                     "txn-same-nonwitness-data-in-mempool")
        self.mine(1)

    def every_variant_kept(self):
        self.step("N25 (PV13 S7) a lighter proof of the seller's own payment, then that payment orphaned: the claim comes back with the buyer's proof")
        node = self.nodes[0]
        b = self.btc_rpc()
        for order in ["buyer first", "seller first"]:
            lock = self.fresh_lock()
            amt = Decimal(lock["amount"]) / Decimal(100000000)
            # The seller holds one coin of the amount and a little more, so its
            # self-payment is one input and one output: lighter than the buyer's.
            coin_addr = self.bseller.getnewaddress(address_type="bech32")
            ftx = self.bminer.sendtoaddress(coin_addr, amt + Decimal("0.0001"))
            self.btc_mine(1)
            fvout = self.vout_paying(ftx, coin_addr, self.bminer.gettransaction(ftx)["blockhash"])
            ptx, pr, vout = self.paid(lock)
            stx = self.bseller.sendall(recipients=[lock["addr"]],
                                       options={"fee_rate": 2, "inputs": [{"txid": ftx, "vout": fvout}]})["txid"]
            sh = self.btc_mine(1)[0]
            self.mine_anchored(b.getblockheader(sh)["height"])
            sp = self.proof_for(stx, lock["C"], sh)
            svout = self.vout_paying(stx, lock["addr"], sh)
            spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
            cb = self.claim(lock, pr["record"], vout, out_spk=spk)
            cs = self.claim(lock, sp["record"], svout, out_spk=spk)
            assert cs.hash == cb.hash and len(sp["payment"]) < len(pr["payment"])
            wb = node.decoderawtransaction(cb.serialize().hex())["hash"]
            ws = node.decoderawtransaction(cs.serialize().hex())["hash"]
            if order == "buyer first":
                node.sendrawtransaction(cb.serialize().hex())
                node.sendrawtransaction(cs.serialize().hex())
                assert_equal(node.getmempoolentry(cs.hash)["wtxid"], ws)
            else:
                node.sendrawtransaction(cs.serialize().hex())
                # The heavier buyer's proof does not replace the seller's, but is
                # validated and kept.
                assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                        cb.serialize().hex())
                assert_equal(node.getmempoolentry(cs.hash)["wtxid"], ws)
            self.log.info("    %s: the pooled proof is the seller's lighter one", order)
            # The seller's block leaves Bitcoin's chain and its payment is double-spent.
            b.invalidateblock(sh)
            raw = b.getrawtransaction(stx, True)
            new = b.createrawtransaction([{"txid": i["txid"], "vout": i["vout"]} for i in raw["vin"]],
                                         [{self.bseller.getnewaddress(): amt + Decimal("0.0001") - Decimal("0.001")}])
            b.sendrawtransaction(self.bseller.signrawtransactionwithwallet(new)["hex"])
            self.btc_mine(2)
            # The node brings the claim back with the buyer's proof, and it is mined.
            self.wait_for(lambda: cb.hash in node.getrawmempool() and
                          node.getmempoolentry(cb.hash)["wtxid"] != ws, "the claim re-attached", timeout=60)
            blk = self.mine_anchored(b.getblockcount())
            assert cb.hash in node.getblock(blk)["tx"]
            self.results.append((self.current + " (%s)" % order, "re-attached with the buyer's proof and mined"))
            self.log.info("    %s: the claim came back with the buyer's proof and was mined", order)
        self.variant_bound()

    def debug_lines(self, node, needle):
        log = open(os.path.join(node.datadir, "elementsregtest", "debug.log")).read().splitlines()
        return [line for line in log if needle in line]

    def variant_bound(self):
        """6.2: one variant per proven Bitcoin block, at most 8; when full the
        newest seen is the one not kept, so the first seen and every earlier one
        stay. A proof of equal weight never replaces the pooled one (P07, P08)."""
        node = self.nodes[0]
        b = self.btc_rpc()
        lock = self.fresh_lock()
        pays, blocks = [], []
        amt = Decimal(lock["amount"]) / Decimal(100000000)
        for k in range(9):
            if k == 8:
                # Two payments of one shape in one block, the lock's output first
                # in both, so their claims weigh the same.
                pays.append(self.bbuyer.send(outputs=[{lock["addr"]: amt}], options={"change_position": 1})["txid"])
                twin = self.bbuyer.send(outputs=[{lock["addr"]: amt}], options={"change_position": 1})["txid"]
            else:
                pays.append(self.pay(lock["addr"], lock["amount"]))
            blocks.append(self.btc_mine(1)[0])
        self.mine_anchored(b.getblockheader(blocks[-1])["height"])
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])

        def claim_for(p, bh):
            return self.claim(lock, self.proof_for(p, lock["C"], bh)["record"], self.vout_paying(p, lock["addr"], bh),
                              out_spk=spk)
        latest = claim_for(pays[8], blocks[8])
        same_block = claim_for(twin, blocks[8])
        older = [claim_for(pays[k], blocks[k]) for k in range(7, -1, -1)]  # each heavier than the last
        assert len({c.hash for c in [latest, same_block] + older}) == 1
        wl = node.decoderawtransaction(latest.serialize().hex())
        assert_equal(node.decoderawtransaction(same_block.serialize().hex())["weight"], wl["weight"])
        node.sendrawtransaction(latest.serialize().hex())
        # P07: an equal-weight proof of another payment does not replace the
        # pooled one ("strictly lighter, so no loop"), and (c): its Bitcoin block
        # is proven already, so it is not kept either.
        assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                same_block.serialize().hex())
        assert_equal(node.getmempoolentry(latest.hash)["wtxid"], wl["hash"])
        wsame = node.decoderawtransaction(same_block.serialize().hex())["hash"]
        assert self.debug_lines(node, "variant %s of %s not kept: a variant proving that Bitcoin block is kept already"
                                % (wsame, latest.hash)), "the same-block proof was kept"
        for c in older:
            assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                    c.serialize().hex())
            assert_equal(node.getmempoolentry(latest.hash)["wtxid"], wl["hash"])
        kept = self.debug_lines(node, "variant(s) of %s kept" % latest.hash)
        assert_equal(len(kept), 8)
        assert "8 variant(s)" in kept[-1], kept[-1]
        # P08: when full, the newest seen is the one not kept; nothing is dropped.
        wnew = node.decoderawtransaction(older[-1].serialize().hex())["hash"]
        assert self.debug_lines(node, "variant %s of %s not kept: 8 variants are kept already" % (wnew, latest.hash)), \
            "the ninth distinct block was kept"
        assert not self.debug_lines(node, ", dropped "), "a kept variant was dropped"
        self.results.append((self.current + " (bound)", "9 Bitcoin blocks and a second payment in one of them: "
                             "8 kept, one per block, first seen first; the same-block one and the ninth not kept"))
        self.log.info("    one variant per proven Bitcoin block, 8 at most, the newest seen not kept; "
                      "an equal-weight proof does not replace the pooled one")
        self.mine(1)

    def variant_fee_budget(self):
        self.step("N30 (PV15 X2 b1) a claim's kept variants are paid for by its own fee")
        node = self.nodes[0]
        b = self.btc_rpc()
        lock = self.fresh_lock()
        pays, blocks = [], []
        for _ in range(3):
            pays.append(self.pay(lock["addr"], lock["amount"]))
            blocks.append(self.btc_mine(1)[0])
        self.mine_anchored(b.getblockheader(blocks[-1])["height"])
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        relay_per_kvb = int(node.getnetworkinfo()["relayfee"] * 100000000)
        recs = [(self.proof_for(pays[k], lock["C"], blocks[k])["record"],
                 self.vout_paying(pays[k], lock["addr"], blocks[k])) for k in (2, 1, 0)]
        vs = [node.decoderawtransaction(self.claim(lock, r, v, out_spk=spk).serialize().hex())["vsize"] for r, v in recs]
        # The fee pays for the storage of the first two variants, not the third.
        fee = relay_per_kvb * (vs[0] + vs[1]) // 1000 + 1
        assert relay_per_kvb * (vs[0] + vs[1] + vs[2]) // 1000 > fee
        claims = [self.claim(lock, r, v, out_spk=spk, fee=fee) for r, v in recs]
        node.sendrawtransaction(claims[0].serialize().hex())
        assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                claims[1].serialize().hex())
        assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                claims[2].serialize().hex())
        kept = self.debug_lines(node, "variant(s) of %s kept" % claims[0].hash)
        assert_equal(len(kept), 2)
        w2 = node.decoderawtransaction(claims[2].serialize().hex())["hash"]
        assert self.debug_lines(node, "variant %s of %s not kept: its fee %d pays for" % (w2, claims[0].hash, fee)), \
            "a variant the claim's fee does not pay for was kept"
        self.results.append((self.current, "fee %d, vsizes %s: 2 kept, the third not kept (its fee pays for %d vB)" %
                             (fee, vs, fee * 1000 // relay_per_kvb)))
        self.log.info("    fee %d pays for %d vB: two variants (%d + %d vB) kept, the third (%d vB) not",
                      fee, fee * 1000 // relay_per_kvb, vs[0], vs[1], vs[2])
        self.mine_anchored(b.getblockcount())

    def seller_coins(self, k, sat):
        """k coins of the seller's, each of `sat` and a little more, confirmed."""
        amt = Decimal(sat) / Decimal(100000000) + Decimal("0.0001")
        addrs = [self.bseller.getnewaddress(address_type="bech32") for _ in range(k)]
        ftx = self.bminer.send(outputs=[{a: amt} for a in addrs])["txid"]
        raw = self.btc_rpc().getrawtransaction(ftx, True)
        self.btc_mine(1)
        return [(ftx, [o["n"] for o in raw["vout"] if o["scriptPubKey"].get("address") == a][0]) for a in addrs]

    def self_pay(self, coin, addr):
        """The seller pays the lock's address from one coin: 1 in, 1 out, lighter than the buyer's."""
        return self.bseller.sendall(recipients=[addr], options={"fee_rate": 2, "inputs": [
            {"txid": coin[0], "vout": coin[1]}]})["txid"]

    def undo_self_pay(self, stx):
        """Double-spend a seller self-payment that left Bitcoin's chain."""
        b = self.btc_rpc()
        raw = self.bseller.gettransaction(stx, True, True)["decoded"]
        value = sum(Decimal(str(self.bseller.gettransaction(i["txid"], True, True)["decoded"]["vout"][i["vout"]]["value"]))
                    for i in raw["vin"])
        new = b.createrawtransaction([{"txid": i["txid"], "vout": i["vout"]} for i in raw["vin"]],
                                     [{self.bseller.getnewaddress(): value - Decimal("0.001")}])
        return b.sendrawtransaction(self.bseller.signrawtransactionwithwallet(new)["hex"])

    def watcher_left(self, bh):
        """Wait until the node's anchor watcher has dropped every block anchored
        to `bh` or above it: the tip's anchor is on Bitcoin's best chain again."""
        node = self.nodes[0]
        b = self.btc_rpc()

        def done():
            h = node.getblockheader(node.getbestblockhash())
            return h["anchorhash"] != bh and b.getblockhash(h["anchorheight"]) == h["anchorhash"]
        self.wait_for(done, "the watcher follows Bitcoin")
        time.sleep(1.5)  # one more watcher tick: nothing left to drop

    def seller_fills_the_store(self):
        self.step("N32 (PV15 X2 c) the seller's own payments against the per-id bound")
        node = self.nodes[0]
        b = self.btc_rpc()
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])

        # (a) The seller's proof pooled, the buyer's kept, then 7 more payments
        # of the seller's in the same Bitcoin block: they take no slot. The claim
        # is mined; that block leaves Bitcoin's chain with every payment of the
        # seller's double-spent; the node brings the claim back with the buyer's
        # proof.
        lock = self.fresh_lock()
        coins = self.seller_coins(8, lock["amount"])
        ptx = self.pay(lock["addr"], lock["amount"])
        for _ in range(12):  # the buyer's block is fuller, so its proof is the heavier
            self.bminer.sendtoaddress(self.bminer.getnewaddress(), 0.001)
        y = self.btc_mine(1)[0]
        stxs = [self.self_pay(c, lock["addr"]) for c in coins]
        x = self.btc_mine(1)[0]
        assert all(t in b.getblock(x)["tx"] for t in stxs)
        self.mine_anchored(b.getblockheader(x)["height"])
        cs = [self.claim(lock, self.proof_for(t, lock["C"], x)["record"], self.vout_paying(t, lock["addr"], x),
                         out_spk=spk) for t in stxs]
        cb = self.claim(lock, self.proof_for(ptx, lock["C"], y)["record"], self.vout_paying(ptx, lock["addr"], y),
                        out_spk=spk)
        node.sendrawtransaction(cs[0].serialize().hex())
        for c in [cb] + cs[1:]:
            assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                    c.serialize().hex())
        assert_equal(len(self.debug_lines(node, "variant(s) of %s kept" % cb.hash)), 2)
        ws = node.decoderawtransaction(cs[0].serialize().hex())["hash"]
        blk = self.mine_anchored(b.getblockheader(x)["height"])
        blk = node.getbestblockhash()
        assert cb.hash in node.getblock(blk)["tx"]
        assert_equal(node.getblock(blk, 2)["tx"][[t["txid"] for t in node.getblock(blk, 2)["tx"]].index(cb.hash)]["hash"], ws)
        b.invalidateblock(x)
        for t in stxs:
            self.undo_self_pay(t)
        self.btc_mine(2)
        self.watcher_left(x)
        self.mine_anchored(b.getblockcount())  # an anchor at or above the buyer's payment, recorded again
        self.wait_for(lambda: cb.hash in node.getrawmempool(), "the claim re-attached", timeout=60)
        wb = node.decoderawtransaction(cb.serialize().hex())["hash"]
        got = node.getmempoolentry(cb.hash)["wtxid"]
        assert got != ws
        blk = self.mine_anchored(b.getblockcount())
        assert cb.hash in node.getblock(blk)["tx"]
        self.results.append((self.current + " (a)", "8 seller payments in one Bitcoin block take one slot; after it "
                             "was orphaned the claim came back with the buyer's proof (stored %s, re-attached %s) "
                             "and was mined" % (wb[:16], got[:16])))
        self.log.info("    (a) 8 seller payments in one block, one slot: the claim came back with the buyer's proof")

        # (b) The seller fills all 8 slots, with payments in 8 Bitcoin blocks,
        # before the buyer's proof is seen: the buyer's is not kept. The claim
        # still comes back while any one of the 8 stands, and pays the buyer.
        lock = self.fresh_lock()
        coins = self.seller_coins(8, lock["amount"])
        ptx = self.pay(lock["addr"], lock["amount"])
        y = self.btc_mine(1)[0]
        stxs, xs = [], []
        for c in coins:
            stxs.append(self.self_pay(c, lock["addr"]))
            xs.append(self.btc_mine(1)[0])
        self.mine_anchored(b.getblockheader(xs[-1])["height"])
        cs = [self.claim(lock, self.proof_for(stxs[k], lock["C"], xs[k])["record"],
                         self.vout_paying(stxs[k], lock["addr"], xs[k]), out_spk=spk) for k in range(7, -1, -1)]
        cb = self.claim(lock, self.proof_for(ptx, lock["C"], y)["record"], self.vout_paying(ptx, lock["addr"], y),
                        out_spk=spk)
        node.sendrawtransaction(cs[0].serialize().hex())
        for c in cs[1:] + [cb]:
            assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                    c.serialize().hex())
        wb = node.decoderawtransaction(cb.serialize().hex())["hash"]
        assert self.debug_lines(node, "variant %s of %s not kept: 8 variants are kept already" % (wb, cb.hash))
        # Only the pooled proof's block leaves Bitcoin's chain.
        b.invalidateblock(xs[-1])
        self.undo_self_pay(stxs[-1])
        self.btc_mine(2)
        self.watcher_left(xs[-1])
        self.mine_anchored(b.getblockcount())
        ws = node.decoderawtransaction(cs[0].serialize().hex())["hash"]
        self.wait_for(lambda: cb.hash in node.getrawmempool() and node.getmempoolentry(cb.hash)["wtxid"] != ws,
                      "the claim re-attached with a proof that stands", timeout=60)
        blk = self.mine_anchored(b.getblockcount())
        assert cb.hash in node.getblock(blk)["tx"]
        assert_equal(node.getblock(blk, 2)["tx"][[t["txid"] for t in node.getblock(blk, 2)["tx"]].index(cb.hash)]["vout"][0]["scriptPubKey"]["hex"], spk.hex())
        self.results.append((self.current + " (b)", "the seller filled 8 slots from 8 Bitcoin blocks first: the buyer's "
                             "proof not kept; with one of the 8 orphaned the claim came back with another of the "
                             "seller's proofs and paid the buyer"))
        self.log.info("    (b) 8 slots filled by the seller first: the buyer's not kept; one orphaned block is not "
                      "enough, the claim came back and paid the buyer")

    def current_variant_kept(self):
        self.step("N34 the proof a claim is pooled or mined with is kept, whatever was seen first")
        node = self.nodes[0]
        b = self.btc_rpc()
        lock = self.fresh_lock()
        amt = Decimal(lock["amount"]) / Decimal(100000000)
        # Both payments in one Bitcoin block: the seller's (1 in, 2 out) is seen
        # first and takes that block's slot; the buyer's (1 in, 1 out) is lighter
        # and replaces it in the pool.
        scoin = self.seller_coins(1, lock["amount"] + 1000000)[0]
        bcoin_addr = self.bbuyer.getnewaddress(address_type="bech32")
        ftx = self.bminer.sendtoaddress(bcoin_addr, amt + Decimal("0.0001"))
        fraw = b.getrawtransaction(ftx, True)
        self.btc_mine(1)
        bvout = [o["n"] for o in fraw["vout"] if o["scriptPubKey"].get("address") == bcoin_addr][0]
        stx = self.bseller.send(outputs=[{lock["addr"]: amt}], options={
            "inputs": [{"txid": scoin[0], "vout": scoin[1]}], "add_inputs": False, "change_position": 1})["txid"]
        ptx = self.bbuyer.sendall(recipients=[lock["addr"]], options={"fee_rate": 2, "inputs": [
            {"txid": ftx, "vout": bvout}]})["txid"]
        y = self.btc_mine(1)[0]
        assert stx in b.getblock(y)["tx"] and ptx in b.getblock(y)["tx"]
        self.mine_anchored(b.getblockheader(y)["height"])
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        cs = self.claim(lock, self.proof_for(stx, lock["C"], y)["record"], self.vout_paying(stx, lock["addr"], y),
                        out_spk=spk)
        cb = self.claim(lock, self.proof_for(ptx, lock["C"], y)["record"], self.vout_paying(ptx, lock["addr"], y),
                        out_spk=spk)
        ws = node.decoderawtransaction(cs.serialize().hex())["hash"]
        wb = node.decoderawtransaction(cb.serialize().hex())["hash"]
        assert node.decoderawtransaction(cb.serialize().hex())["weight"] < node.decoderawtransaction(cs.serialize().hex())["weight"]
        node.sendrawtransaction(cs.serialize().hex())
        node.sendrawtransaction(cb.serialize().hex())
        assert_equal(node.getmempoolentry(cb.hash)["wtxid"], wb)
        blk = self.mine(1)[0]
        mined = node.getblock(blk, 2)["tx"]
        assert_equal([t["hash"] for t in mined if t["txid"] == cb.hash], [wb])
        # Y leaves Bitcoin's chain: the seller's payment is double-spent, the
        # buyer's is mined again in another block.
        b.invalidateblock(y)
        self.undo_self_pay(stx)
        y2 = self.btc_mine(2)[0]
        assert ptx in b.getblock(y2)["tx"]
        self.watcher_left(y)
        self.mine_anchored(b.getblockcount())
        self.wait_for(lambda: cb.hash in node.getrawmempool(), "the claim re-attached with the proof it was mined with",
                      timeout=60)
        got = node.getmempoolentry(cb.hash)["wtxid"]
        assert got not in (ws, wb)
        blk = self.mine_anchored(b.getblockcount())
        assert cb.hash in node.getblock(blk)["tx"]
        self.results.append((self.current, "seller's proof first (slot of block Y), buyer's lighter one pooled and "
                             "mined; Y orphaned, seller's payment double-spent, buyer's mined again: the claim came "
                             "back with a fresh proof of the buyer's payment (%s) and was mined" % got[:16]))
        self.log.info("    the claim came back with a fresh proof of the payment it was mined with")

    def seller_first_then_orphaned(self, mode):
        """PV15's re-check of the store, Q1 and Q2. The seller's proof reaches the
        node first. Q1 (SAME): both payments in one Bitcoin block, the seller's
        lighter: the buyer's is never stored, and only the search by paid script
        finds it. Q2 (HEAVY): the seller's heavy payment one block earlier, its
        proof using the claim's whole fee budget; the buyer's lighter proof
        replaces it in the pool and the claim is mined with it: that variant is
        kept as the one the claim was mined with. Then the seller's payment leaves
        Bitcoin's chain, double-spent, and the buyer's is mined again."""
        node = self.nodes[0]
        b = self.btc_rpc()
        lock = self.fresh_lock(deadline_in=40)
        amt = Decimal(lock["amount"]) / Decimal(100000000)
        if mode == "SAME":
            coin = self.seller_coins(1, lock["amount"])[0]
            stx = self.self_pay(coin, lock["addr"])
        else:
            coin = self.seller_coins(1, lock["amount"] + 130 * 1000 + 20000)[0]
            outs = [{lock["addr"]: amt}] + [{self.bseller.getnewaddress(address_type="bech32"): Decimal("0.00001")}
                                            for _ in range(130)]
            stx = self.bseller.send(outputs=outs, options={"inputs": [{"txid": coin[0], "vout": coin[1]}],
                                                           "add_inputs": False, "fee_rate": 2})["txid"]
            x = self.btc_mine(1)[0]
        ptx = self.pay(lock["addr"], lock["amount"])
        y = self.btc_mine(1)[0]
        if mode == "SAME":
            x = y
        assert stx in b.getblock(x)["tx"] and ptx in b.getblock(y)["tx"]
        self.mine_anchored(b.getblockcount())
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        rs = (self.proof_for(stx, lock["C"], x)["record"], self.vout_paying(stx, lock["addr"], x))
        rb = (self.proof_for(ptx, lock["C"], y)["record"], self.vout_paying(ptx, lock["addr"], y))
        vs = node.decoderawtransaction(self.claim(lock, *rs, out_spk=spk).serialize().hex())["vsize"]
        relay_per_kvb = int(node.getnetworkinfo()["relayfee"] * 100000000)
        fee = 5000 if mode == "SAME" else relay_per_kvb * (vs + 50) // 1000
        cs = self.claim(lock, *rs, out_spk=spk, fee=fee)
        cb = self.claim(lock, *rb, out_spk=spk, fee=fee)
        ws = node.decoderawtransaction(cs.serialize().hex())["hash"]
        wb = node.decoderawtransaction(cb.serialize().hex())["hash"]
        node.sendrawtransaction(cs.serialize().hex())
        if mode == "SAME":
            assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                    cb.serialize().hex())
            assert self.debug_lines(node, "variant %s of %s not kept: a variant proving that Bitcoin block" % (wb, cb.hash))
            mined_with = ws
        else:
            node.sendrawtransaction(cb.serialize().hex())
            mined_with = wb
        assert_equal(node.getmempoolentry(cb.hash)["wtxid"], mined_with)
        blk = self.mine(1)[0]
        assert_equal([t["hash"] for t in node.getblock(blk, 2)["tx"] if t["txid"] == cb.hash], [mined_with])
        b.invalidateblock(x)
        self.undo_self_pay(stx)
        self.btc_mine(1)
        if ptx not in b.getrawmempool() and self.bbuyer.gettransaction(ptx)["confirmations"] <= 0:
            b.sendrawtransaction(self.bbuyer.gettransaction(ptx)["hex"])  # its lock time is final again
        self.btc_mine(2)
        assert self.bbuyer.gettransaction(ptx)["confirmations"] > 0
        self.watcher_left(x)
        self.mine_anchored(b.getblockcount())
        self.wait_for(lambda: cb.hash in node.getrawmempool(), "the claim re-attached", timeout=60)
        got = node.getmempoolentry(cb.hash)["wtxid"]
        how = [line for line in self.debug_lines(node, "re-attached") if cb.hash in line][-1].split("re-attached ")[1]
        blk = self.mine_anchored(b.getblockcount())
        assert cb.hash in node.getblock(blk)["tx"]
        return got, how, fee, vs

    def store_order_irrelevant(self):
        self.step("N35 (PV15 re-check Q1, Q2) whatever the store holds, the claim comes back with the buyer's payment")
        got, how, fee, vs = self.seller_first_then_orphaned("SAME")
        assert how.startswith("a searched proof"), how
        self.results.append((self.current + " Q1", "both payments in one block, the seller's proof first: the "
                             "buyer's never stored; after the orphan the claim came back (%s)" % how.strip()))
        self.log.info("    Q1: the claim came back with %s", how.strip())
        got, how, fee, vs = self.seller_first_then_orphaned("HEAVY")
        self.results.append((self.current + " Q2", "fee %d, the seller's proof %d vB first; the buyer's pooled and "
                             "mined, kept as the variant mined with; the claim came back (%s)" % (fee, vs, how.strip())))
        self.log.info("    Q2: fee %d for a %d vB first proof; the claim came back with %s", fee, vs, how.strip())

    def craft_anchor(self, node, new_height=None, new_hash=None):
        """A block from getnewblockhex with its anchor rewritten, submitted."""
        parent = node.getblockheader(node.getbestblockhash())
        raw = bytearray.fromhex(node.getnewblockhex())
        pos = -1
        for c in [parent["anchorhash"], self.btc_rpc().getbestblockhash()]:
            p = raw.find(bytes.fromhex(c)[::-1])
            if 0 < p < 220:
                pos = p
                break
        assert pos > 0, "anchor not found in the template header"
        if new_height is not None:
            raw[pos - 4:pos] = struct.pack("<I", new_height)
        if new_hash is not None:
            raw[pos:pos + 32] = bytes.fromhex(new_hash)[::-1]
        return node.submitblock(raw.hex())

    def false_height_record(self):
        self.step("N33 (PV15 A1f) a block repeating its parent's anchor hash at a larger height lends it no height")
        # R2's converse (a hash has one height) is consensus only from
        # pos_hardening_height; with it off, such a block is valid on a node
        # whose watcher has not looked yet. The proof's link must be the first
        # block recording that hash.
        self.disconnect_nodes(0, 1)
        self.restart_node(0, self.seq_args(["-poshardeningheight=0", "-anchorpollinterval=600"]))
        node = self.nodes[0]
        node.loadwallet("w")
        node.setfeeexchangerates(self.nodes[1].getfeeexchangerates())
        b = self.btc_rpc()
        p = b.getblockcount() + 1
        lock = self.new_lock(50_000_000, p, p + 20, p + 26)  # F = p: a payment at p must not open it
        self.mine(1)
        lock["C"] = self.lock_coin_anchor(lock)
        ptx = self.pay(lock["addr"], lock["amount"])
        bh = self.btc_mine(1)[0]
        assert_equal(b.getblockheader(bh)["height"], p)
        self.mine_anchored(p)
        vout = self.vout_paying(ptx, lock["addr"], bh)
        pr = self.proof_for(ptx, lock["C"], bh)
        assert_equal(len(pr["links"]), 0)
        self.force([self.claim(lock, pr["record"], vout)], "Script failed an OP_VERIFY operation")
        k = 2
        assert_equal(self.craft_anchor(node, new_height=p + k), None)
        tip = node.getblockheader(node.getbestblockhash())
        assert_equal((tip["anchorheight"], tip["anchorhash"]), (p + k, bh))
        self.btc_mine(k + 1)
        cs = hashlib.sha256(pr["coinbase"]).digest()
        inflated = payment_record(p + k, lock["C"], pr["header"], cs, pr["index"], pr["pbranch"], pr["cbranch"],
                                  pr["payment"])
        self.force([self.claim(lock, inflated, vout)], "bad-btcpay-not-in-anchor-chain")
        # The same hash recorded again further on, not by the next block: the
        # anchor check (R3) of a node that watches Bitcoin refuses that block,
        # since its hash differs from its parent's.
        crafted = node.getblockheader(node.getbestblockhash())
        self.mine(1)
        assert node.getblockheader(node.getbestblockhash())["anchorhash"] != bh
        self.btc_mine(1)
        res = self.craft_anchor(node, new_height=b.getblockcount(), new_hash=bh)
        assert res is not None, "a block repeating an older anchor hash was accepted"
        self.log.info("    a block recording an older anchor hash at a later height, not after it: %s", res)
        self.results.append((self.current, "inflated h=%d (true %d) linked to the repeated record: "
                             "bad-btcpay-not-in-anchor-chain; non-consecutive repeat refused by the watching node: %s"
                             % (p + k, p, res)))
        node.invalidateblock(crafted["hash"])
        self.restart_seq0()
        self.mine(1)
        self.sync_blocks()

    def block_records(self, blockhash):
        """A maker of payment records for transactions of one Bitcoin block,
        linked to the lowest recorded anchor at or above it (fetched once)."""
        b = self.btc_rpc()
        blk = b.getblock(blockhash, 1)
        leaves = [bytes.fromhex(t)[::-1] for t in blk["tx"]]
        header = bytes.fromhex(b.getblockheader(blockhash, False))
        cbsha = hashlib.sha256(strip_btc_tx(bytes.fromhex(b.getrawtransaction(blk["tx"][0], False, blockhash)))).digest()
        rec_anchor = self.recorded_anchor_at_or_above(blk["height"])
        links = self.link_headers(blk["height"], rec_anchor[0])

        def make(txid, coin_anchor):
            i = blk["tx"].index(txid)
            pay = strip_btc_tx(bytes.fromhex(b.getrawtransaction(txid, False, blockhash)))
            return payment_record(blk["height"], coin_anchor, header, cbsha, i, merkle_branch(leaves, i),
                                  merkle_branch(leaves, 0), pay, links=links)
        return make

    def variant_flood(self):
        self.step("N31 (PV15 X2) a flood of other claims' variants does not flush a claim's stored proof")
        nattack, nlinks, bound = 30, 1000, 2000000
        self.restart_seq0(["-btcpaymaxvariantbytes=%d" % bound])
        node = self.nodes[0]
        b = self.btc_rpc()
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        # The victim, as in N25 (seller first): the seller's lighter proof
        # pooled, the buyer's validated and kept.
        lock = self.fresh_lock(deadline_in=3000)
        coin = self.seller_coins(1, lock["amount"])[0]
        ptx = self.pay(lock["addr"], lock["amount"])
        y = self.btc_mine(1)[0]
        stx = self.self_pay(coin, lock["addr"])
        x = self.btc_mine(1)[0]
        self.mine_anchored(b.getblockheader(x)["height"])
        cs = self.claim(lock, self.proof_for(stx, lock["C"], x)["record"], self.vout_paying(stx, lock["addr"], x),
                        out_spk=spk)
        cb = self.claim(lock, self.proof_for(ptx, lock["C"], y)["record"], self.vout_paying(ptx, lock["addr"], y),
                        out_spk=spk)
        ws = node.decoderawtransaction(cs.serialize().hex())["hash"]
        node.sendrawtransaction(cs.serialize().hex())
        assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction, cb.serialize().hex())
        assert_equal(len(self.debug_lines(node, "variant(s) of %s kept" % cb.hash)), 2)
        # The attacker: coins under a leaf any proof of any payment opens, a light
        # claim of each pooled at a fee of its own, and heavy variants proving
        # arbitrary Bitcoin transactions nobody paid anyone for.
        anyleaf = CScript([OP_INSPECTBTCPAYMENT, OP_2DROP, OP_1])
        tap = taproot_construct(NUMS, [("any", anyleaf)])
        addr = program_to_witness(1, tap.scriptPubKey[2:])
        coins = []
        for i in range(nattack):
            if i % 10 == 9:
                self.mine(1)
            t = node.sendtoaddress(address=addr, amount=1, assetlabel=self.asset, fee_asset_label=self.asset)
            coins.append((t, [k for k, o in enumerate(tx_from_hex(node.getrawtransaction(t)).vout)
                              if o.scriptPubKey == tap.scriptPubKey][0]))
        self.mine(1)
        anchors = {t: self.anchor_of(node.getrawtransaction(t, True)["blockhash"]) for t, _ in coins}
        heavy_src = [self.bminer.sendtoaddress(self.bminer.getnewaddress(), 0.001) for _ in range(2 * nattack)]
        b0 = self.btc_mine(1)[0]
        self.btc_mine(nlinks - 1)
        light_src = [self.bminer.sendtoaddress(self.bminer.getnewaddress(), 0.001) for _ in range(nattack)]
        top = self.btc_mine(1)[0]
        self.mine_anchored(b.getblockcount())
        heavy_rec, light_rec = self.block_records(b0), self.block_records(top)
        fee = 50000
        control = bytes([tap.leaves["any"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["any"].merklebranch

        def spend(coin, record):
            t = CTransaction()
            t.nVersion = 2
            t.vin.append(CTxIn(COutPoint(int(coin[0], 16), coin[1]), nSequence=SEQ_NONFINAL_RBF))
            asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
            t.vout.append(CTxOut(CTxOutValue(100000000 - fee), spk, asset))
            t.vout.append(CTxOut(CTxOutValue(fee), b"", asset))
            t.wit.vtxinwit.append(CTxInWitness())
            t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
            t.wit.vtxinwit[0].peginWitness.stack = [record]
            t.wit.vtxinwit[0].scriptWitness.stack = [bytes(anyleaf), control]
            t.rehash()
            return t
        heavy_bytes = 0
        for i, c in enumerate(coins):
            node.sendrawtransaction(spend(c, light_rec(light_src[i], anchors[c[0]])).serialize().hex())
            for j in range(2):
                h = spend(c, heavy_rec(heavy_src[2 * i + j], anchors[c[0]]))
                heavy_bytes += len(h.serialize())
                assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                        h.serialize().hex())
        dropped = self.debug_lines(node, "BtcPayment: variants of ")
        assert dropped, "the bound was never reached"
        assert not [line for line in dropped if cb.hash in line], "the victim's variants were dropped"
        self.log.info("    flood: %d light claims pooled, %d heavy variants (%.1f MB, %d links): %d transaction ids "
                      "dropped, lowest fee per stored vB first; the victim's kept", nattack, 2 * nattack,
                      heavy_bytes / 1e6, nlinks, len(dropped))
        # The seller's payment leaves Bitcoin's chain and is double-spent.
        tip_anchor = node.getblockheader(node.getbestblockhash())["anchorhash"]
        b.invalidateblock(x)
        self.undo_self_pay(stx)
        self.btc_mine(2)
        self.watcher_left(tip_anchor)
        self.mine_anchored(b.getblockcount())
        self.wait_for(lambda: cb.hash in node.getrawmempool() and node.getmempoolentry(cb.hash)["wtxid"] != ws,
                      "the claim re-attached after the flood", timeout=60)
        blk = self.mine_anchored(b.getblockcount())
        assert cb.hash in node.getblock(blk)["tx"]
        self.results.append((self.current, "%d heavy variants (%.1f MB) against a %d-byte bound: %d ids dropped, "
                             "the victim's proof kept; its claim came back with the buyer's proof and was mined"
                             % (2 * nattack, heavy_bytes / 1e6, bound, len(dropped))))
        self.log.info("    the victim's claim came back with the buyer's proof and was mined")
        self.mine(1)
        self.restart_seq0()

    def replacement_keeps_descendants(self):
        self.step("N38 (PV15 X3) a better proof of a pooled claim keeps the claim's children in the pool")
        # A same-txid replacement evicts the pooled one's descendants with it,
        # though they still spend valid outputs (same txid). A buyer's onward
        # spend, or a child paying for the claim's inclusion, must come back.
        node = self.nodes[0]
        b = self.btc_rpc()
        lock = self.fresh_lock()
        ptx, pr, vout = self.paid(lock)
        coin = self.seller_coins(1, lock["amount"])[0]
        stx = self.self_pay(coin, lock["addr"])
        sh = self.btc_mine(1)[0]
        self.mine_anchored(b.getblockheader(sh)["height"])
        addr = node.getnewaddress()
        spk = bytes.fromhex(node.getaddressinfo(addr)["scriptPubKey"])
        cb = self.claim(lock, self.proof_for(ptx, lock["C"], pr["blockhash"])["record"], vout, out_spk=spk)
        cs = self.claim(lock, self.proof_for(stx, lock["C"], sh)["record"], self.vout_paying(stx, lock["addr"], sh),
                        out_spk=spk)
        assert node.decoderawtransaction(cs.serialize().hex())["weight"] < node.decoderawtransaction(cb.serialize().hex())["weight"]
        node.sendrawtransaction(cb.serialize().hex())
        # The buyer's child, spending the claim's output and paying a high fee.
        child = CTransaction()
        child.nVersion = 2
        child.vin.append(CTxIn(COutPoint(int(cb.hash, 16), 0), nSequence=SEQ_NONFINAL_RBF))
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
        kid_fee = 20000
        child.vout.append(CTxOut(CTxOutValue(lock["asset_amount"] - 5000 - kid_fee),
                                 bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"]), asset))
        child.vout.append(CTxOut(CTxOutValue(kid_fee), b"", asset))
        signed = node.signrawtransactionwithwallet(child.serialize().hex())
        assert signed["complete"], signed
        kid = node.sendrawtransaction(signed["hex"])
        assert kid in node.getrawmempool()
        # The seller's lighter proof replaces the buyer's: same txid.
        node.sendrawtransaction(cs.serialize().hex())
        assert_equal(node.getmempoolentry(cs.hash)["wtxid"], node.decoderawtransaction(cs.serialize().hex())["hash"])
        assert kid in node.getrawmempool(), "the claim's child was evicted by a same-txid replacement"
        blk = self.mine(1)[0]
        assert kid in node.getblock(blk)["tx"] and cs.hash in node.getblock(blk)["tx"]
        self.results.append((self.current, "a lighter proof replaced the pooled claim; its child (fee %d) stayed in "
                             "the pool and was mined with it" % kid_fee))
        self.log.info("    the claim's child stayed in the pool through the same-txid replacement and was mined")

    # ------------------------------------------------------- the waiting set
    def attacker_coins(self, n):
        """n coins, each under a leaf of its own that any proof of any payment
        opens: what a claim nobody can satisfy needs."""
        node = self.nodes[0]
        coins = []
        per_tx = 400
        for start in range(0, n, per_tx):
            leaves, amounts = {}, {}
            for k in range(start, min(n, start + per_tx)):
                leaf = CScript([OP_INSPECTBTCPAYMENT, OP_2DROP, k + 17, OP_DROP, OP_1])
                tap = taproot_construct(NUMS, [("l", leaf)])
                addr = program_to_witness(1, tap.scriptPubKey[2:])
                leaves[tap.scriptPubKey] = (leaf, tap)
                amounts[addr] = Decimal("0.01")
            ftx = node.sendmany(amounts=amounts, output_assets={a: self.asset for a in amounts}, fee_asset=self.asset)
            self.mine(1)
            ft = tx_from_hex(node.getrawtransaction(ftx))
            C = self.anchor_of(node.getrawtransaction(ftx, True)["blockhash"])
            for v, o in enumerate(ft.vout):
                if o.scriptPubKey in leaves:
                    coins.append(dict(txid=ftx, vout=v, C=C, leaf=leaves[o.scriptPubKey][0], tap=leaves[o.scriptPubKey][1]))
        return coins

    def attacker_spend(self, coin, record, fee, spk):
        t = CTransaction()
        t.nVersion = 2
        t.vin.append(CTxIn(COutPoint(int(coin["txid"], 16), coin["vout"]), nSequence=SEQ_NONFINAL_RBF))
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
        t.vout.append(CTxOut(CTxOutValue(1000000 - fee), spk, asset))
        t.vout.append(CTxOut(CTxOutValue(fee), b"", asset))
        t.wit.vtxinwit.append(CTxInWitness())
        t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
        t.wit.vtxinwit[0].peginWitness.stack = [record]
        tap = coin["tap"]
        control = bytes([tap.leaves["l"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["l"].merklebranch
        t.wit.vtxinwit[0].scriptWitness.stack = [bytes(coin["leaf"]), control]
        t.rehash()
        return t

    def fill_waiting_set(self, coins, fee):
        """One claim per coin, each proving one payment that is then orphaned and
        double-spent: every claim is evicted, held, and can never be satisfied.
        Returns the claims' fee per vB."""
        node = self.nodes[0]
        b = self.btc_rpc()
        # Paid to a script outside the node's wallet: a wallet holding a thousand
        # rejected transactions re-checks them all under the chain lock.
        spk = self.outside_spk
        ptx = self.bbuyer.sendtoaddress(self.bbuyer.getnewaddress(), 1)
        ph = self.btc_mine(1)[0]
        self.mine_anchored(b.getblockheader(ph)["height"])
        make = self.block_records(ph)
        records = {}
        claims = []
        for c in coins:
            if c["C"] not in records:
                records[c["C"]] = make(ptx, c["C"])
            t = self.attacker_spend(c, records[c["C"]], fee, spk)
            node.sendrawtransaction(t.serialize().hex())
            claims.append(t.hash)
        rate = fee / node.getmempoolentry(claims[0])["vsize"]
        b.invalidateblock(ph)
        self.double_spend(ptx)
        self.btc_mine(2)
        self.wait_for(lambda: not set(claims) & set(node.getrawmempool()), "the claims evicted", timeout=180)
        return claims, rate

    def honest_evicted(self, fee):
        """A buyer's claim mined, then its payment's block orphaned and the payment
        mined again: the claim is evicted and waits for its proof to be re-linked."""
        node = self.nodes[0]
        b = self.btc_rpc()
        lock = self.fresh_lock(deadline_in=400)
        ptx, pr, vout = self.paid(lock)
        c = self.claim(lock, pr["record"], vout, fee=fee)
        node.sendrawtransaction(c.serialize().hex())
        rate = fee / node.getmempoolentry(c.hash)["vsize"]
        blk = self.mine_anchored(pr["height"])
        assert c.hash in node.getblock(blk)["tx"]
        b.invalidateblock(pr["blockhash"])
        self.btc_mine(2)
        assert self.bbuyer.gettransaction(ptx)["confirmations"] > 0
        self.wait_for(lambda: node.getblockheader(blk)["confirmations"] < 0, "the claim's block is disconnected")
        self.mine_anchored(b.getblockcount())
        return c, rate

    def waiting_set(self):
        self.step("N39 (PV15 X6) the waiting set: held by fee per vB, forgotten when the lock coin is spent, "
                  "and after one window")
        node = self.nodes[0]
        b = self.btc_rpc()
        n = 1000
        # The regtest default re-checks the whole pool after every change; with
        # a thousand claims pooled that is most of the run.
        fast = ["-checkmempool=0", "-btcpaymaxwaiting=%d" % n]   # the eviction rules, on a set of 1,000
        self.outside_spk = bytes([0, 20]) + os.urandom(20)
        coins = self.attacker_coins(n + 2)

        def reattached(c, timeout=60):
            self.wait_for(lambda: c.hash in node.getrawmempool(), "%s re-attached" % c.hash, timeout=timeout)
            blk = self.mine_anchored(b.getblockcount())
            assert c.hash in node.getblock(blk)["tx"]

        # (a) A full set of claims paying little: a claim paying more per vB
        # takes the place of the lowest, and comes back.
        self.restart_seq0(fast)
        attackers, low = self.fill_waiting_set(coins[:n], 2000)
        h1, h1_rate = self.honest_evicted(20000)
        assert h1_rate > low
        reattached(h1)
        dropped = [line for line in self.debug_lines(node, "dropped from the waiting set") if h1.hash in line]
        assert dropped, "the claim paying more was not held"
        self.log.info("    (a) %d claims at %.1f per vB held; the buyer's at %.1f per vB took the lowest one's place "
                      "and came back", n, low, h1_rate)
        self.results.append((self.current + " (a)", "%d claims at %.1f per vB waiting; a claim at %.1f per vB held in "
                             "the lowest one's place and re-attached" % (n, low, h1_rate)))

        # (b) A full set of claims paying more: a claim paying less is not held;
        # once a held claim's lock coin is spent on the active chain, it is
        # forgotten and the next claim is held, and comes back.
        self.restart_seq0(fast)
        attackers, high = self.fill_waiting_set(coins[:n], 50000)
        h0, h0_rate = self.honest_evicted(5000)
        assert h0_rate < high
        self.wait_for(lambda: [line for line in self.debug_lines(node, "not held for re-attachment") if h0.hash in line],
                      "the cheaper claim is not held")
        spk = self.outside_spk
        ptx = self.bbuyer.sendtoaddress(self.bbuyer.getnewaddress(), 1)
        ph = self.btc_mine(1)[0]
        self.mine_anchored(b.getblockheader(ph)["height"])
        spent = coins[0]
        other = self.attacker_spend(spent, self.block_records(ph)(ptx, spent["C"]), 3000, spk)
        assert other.hash != attackers[0]
        node.sendrawtransaction(other.serialize().hex())
        blk = self.mine(1)[0]
        assert other.hash in node.getblock(blk)["tx"]
        self.wait_for(lambda: self.debug_lines(node, "%s no longer waits: its lock coin" % attackers[0]),
                      "the claim whose lock coin is spent is forgotten", timeout=30)
        h2, h2_rate = self.honest_evicted(5000)
        reattached(h2)
        assert not [line for line in self.debug_lines(node, "not held for re-attachment") if h2.hash in line]
        self.log.info("    (b) %d claims at %.1f per vB held: a claim at %.1f per vB not held; one lock coin spent: "
                      "that claim forgotten, the next claim held and re-attached", n, high, h0_rate)
        self.results.append((self.current + " (b)", "%d claims at %.1f per vB waiting: a claim at %.1f per vB not held; "
                             "one held claim's lock coin spent: forgotten at once; the next claim held and re-attached"
                             % (n, high, h2_rate)))

        # (c) The same full set: after one window of Bitcoin blocks every held
        # claim is forgotten, and a claim paying less is held and comes back.
        self.restart_seq0(fast)
        attackers, high = self.fill_waiting_set(coins[2:n + 2], 50000)
        time.sleep(3)  # the worker notes the Bitcoin height each claim was held at
        self.btc_mine(144)
        self.wait_for(lambda: len(self.debug_lines(node, "not re-attached in 144 Bitcoin blocks")) >= n,
                      "every held claim forgotten after one window", timeout=120)
        self.mine_anchored(b.getblockcount())
        h3, h3_rate = self.honest_evicted(5000)
        reattached(h3)
        self.log.info("    (c) after 144 Bitcoin blocks the %d claims at %.1f per vB are forgotten; a claim at %.1f "
                      "per vB is held and re-attached", n, high, h3_rate)
        self.results.append((self.current + " (c)", "%d claims at %.1f per vB forgotten after 144 Bitcoin blocks; a "
                             "claim at %.1f per vB held and re-attached" % (n, high, h3_rate)))
        self.restart_seq0()

    def exact_btc_coin(self, sat):
        b = self.bminer
        mid = b.getnewaddress("", "bech32")
        t = b.sendtoaddress(mid, Decimal(sat) / Decimal(100000000))
        raw = b.getrawtransaction(t, True)
        return {"txid": t, "vout": [o["n"] for o in raw["vout"] if o["scriptPubKey"].get("address") == mid][0]}

    def many_outputs_tx(self, coin, spk, n, sat):
        """A Bitcoin transaction spending `coin` into n outputs of `sat` to `spk`."""
        raw = (struct.pack("<i", 2) + b"\x01" + bytes.fromhex(coin["txid"])[::-1] + struct.pack("<I", coin["vout"]) +
               b"\x00" + b"\xff\xff\xff\xff")
        raw += (bytes([0xfd]) + struct.pack("<H", n)) if n >= 0xfd else bytes([n])
        raw += (struct.pack("<q", sat) + bytes([len(spk)]) + spk) * n
        raw += b"\x00\x00\x00\x00"
        return self.bminer.signrawtransactionwithwallet(raw.hex())["hex"]

    def tick_lines(self, node, since):
        out = []
        for line in self.debug_lines(node, "BtcPayment: re-attach tick ")[since:]:
            out.append(int(line.split("re-attach tick ")[1].split(" ms")[0]))
        return out

    def search_budget(self):
        self.step("N41 (PV15 re-check, low) the search's budget: thousands of candidates in Bitcoin's best chain, "
                  "each refused by the script, never make a tick run past its budget")
        node = self.nodes[0]
        b = self.btc_rpc()
        ndecoy, nout = 4, 500
        lock = self.fresh_lock(amount=100000, deadline_in=60)
        spk = bytes.fromhex(self.bseller.getaddressinfo(lock["addr"])["scriptPubKey"])
        coins = [self.exact_btc_coin(nout * 99000 + 200000) for _ in range(ndecoy)]
        self.btc_mine(1)
        ptx, pr, vout = self.paid(lock)
        c = self.claim(lock, pr["record"], vout)
        node.sendrawtransaction(c.serialize().hex())
        blk = self.mine_anchored(pr["height"])
        assert c.hash in node.getblock(blk)["tx"]
        ticks0 = len(self.debug_lines(node, "BtcPayment: re-attach tick "))
        # The buyer's payment is double-spent; the new branch holds 2,000
        # outputs paying the seller's script a little less than the price.
        b.invalidateblock(pr["blockhash"])
        self.double_spend(ptx)
        decoys = [b.sendrawtransaction(self.many_outputs_tx(k, spk, nout, 99000)) for k in coins]
        self.btc_mine(2)
        assert all(self.bminer.gettransaction(d)["confirmations"] > 0 for d in decoys)
        self.wait_for(lambda: node.getblockheader(blk)["confirmations"] < 0, "the claim's block is disconnected")
        self.mine_anchored(b.getblockcount())
        refused = "re-attach of %s (a searched proof) refused" % c.hash
        self.wait_for(lambda: len(self.debug_lines(node, refused)) >= ndecoy * nout, "every candidate tried", timeout=180)
        ticks = self.tick_lines(node, ticks0)
        assert c.hash not in node.getrawmempool()
        self.log.info("    %d candidates tried, each refused by the script, over %d ticks; longest tick %d ms",
                      len(self.debug_lines(node, refused)), len(ticks), max(ticks))
        self.results.append((self.current, "%d candidates refused over %d ticks, longest %d ms (budget 500)"
                             % (len(self.debug_lines(node, refused)), len(ticks), max(ticks))))
        assert max(ticks) < 800, "a tick ran %d ms" % max(ticks)

    def lighter_by_the_incremental_rate(self):
        self.step("N40 (PV15 X3, low) a lighter proof of a pooled claim replaces it only when the fee rate rises by "
                  "the incremental relay rate")
        node = self.nodes[0]
        b = self.btc_rpc()
        incr = int(node.getnetworkinfo()["incrementalfee"] * 100000000)  # per kvB
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        for fee, expect_replace in ((400, False), (20000, True)):
            lock = self.fresh_lock()
            ptx = self.pay(lock["addr"], lock["amount"])
            ph = self.btc_mine(1)[0]
            h = b.getblockheader(ph)["height"]
            self.mine_anchored(h)
            self.btc_mine(1)
            self.mine_anchored(h + 1)
            # Two valid records of one payment: linked to the anchor at its own
            # block (no link) and to the next one (one link, 80 bytes heavier).
            pr = self.proof_for(ptx, lock["C"], ph)
            assert_equal(pr["links"], [])
            heavy_rec = payment_record(pr["height"], lock["C"], pr["header"], hashlib.sha256(pr["coinbase"]).digest(),
                                       pr["index"], pr["pbranch"], pr["cbranch"], pr["payment"],
                                       links=self.link_headers(h, h + 1))
            vout = self.vout_paying(ptx, lock["addr"], ph)
            light = self.claim(lock, pr["record"], vout, fee=fee, out_spk=spk)
            heavy = self.claim(lock, heavy_rec, vout, fee=fee, out_spk=spk)
            vl = node.decoderawtransaction(light.serialize().hex())["vsize"]
            vh = node.decoderawtransaction(heavy.serialize().hex())["vsize"]
            wh = node.decoderawtransaction(heavy.serialize().hex())["hash"]
            wl = node.decoderawtransaction(light.serialize().hex())["hash"]
            gain = fee * 1000 // vl - fee * 1000 // vh
            node.sendrawtransaction(heavy.serialize().hex())
            assert_equal(gain >= incr, expect_replace)
            if gain < incr:
                assert_raises_rpc_error(-26, "txn-same-nonwitness-data-in-mempool", node.sendrawtransaction,
                                        light.serialize().hex())
                assert_equal(node.getmempoolentry(heavy.hash)["wtxid"], wh)
                what = "refused, the pooled proof kept"
            else:
                node.sendrawtransaction(light.serialize().hex())
                assert_equal(node.getmempoolentry(heavy.hash)["wtxid"], wl)
                what = "replaced the pooled proof"
            self.log.info("    fee %d: %d vB against %d vB, the rate rises by %d per kvB (incremental %d): %s",
                          fee, vl, vh, gain, incr, what)
            self.results.append((self.current + " fee %d" % fee, "%d vB against %d vB, rate +%d per kvB, incremental "
                                 "%d: %s" % (vl, vh, gain, incr, what)))
            blk = self.mine(1)[0]
            assert heavy.hash in node.getblock(blk)["tx"]

    def lagging_daemon(self):
        self.step("N42 a node whose Bitcoin daemon is behind: it neither accepts nor condemns a claim it cannot "
                  "check yet, and re-attaches a claim itself once its daemon catches up")
        node = self.nodes[0]
        b = self.btc_rpc()
        # A second Bitcoin daemon, fed by the first over p2p and cut off at will,
        # and a third Sequentia node watching it.
        d1 = os.path.join(self.options.tmpdir, "btc1")
        os.makedirs(d1)
        port1, p2p1 = rpc_port(5), p2p_port(5)  # 3 is the first daemon's
        proc = subprocess.Popen([BITCOIND, "-datadir=" + d1, "-regtest", "-port=%d" % p2p1, "-bind=127.0.0.1",
                                 "-rpcport=%d" % port1, "-server", "-connect=0", "-fallbackfee=0.0001",
                                 "-printtoconsole=0"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cookie = os.path.join(d1, "regtest", ".cookie")
        self.wait_until(lambda: os.path.exists(cookie), timeout=60)
        u1, p1 = open(cookie).read().strip().split(":", 1)
        url1 = "http://%s:%s@127.0.0.1:%d" % (urllib.parse.quote(u1), urllib.parse.quote(p1), port1)
        peer = "127.0.0.1:%d" % p2p1

        def b1():
            return AuthServiceProxy(url1, timeout=120)

        def up():
            try:
                b1().getblockcount()
                return True
            except Exception:
                return False

        def feed():
            b.addnode(peer, "onetry")
            self.wait_for(lambda: b1().getbestblockhash() == b.getbestblockhash(), "the second daemon catches up", 120)

        def cut():
            b.disconnectnode(address=peer)
            self.wait_for(lambda: not b1().getpeerinfo(), "the second daemon is cut off")
        try:
            self.wait_until(up, timeout=60)
            feed()
            args = [a for a in self.seq_args() if not a.startswith(("-mainchainrpcport=", "-mainchainrpcuser=",
                                                                    "-mainchainrpcpassword="))]
            args += ["-mainchainrpcport=%d" % port1, "-mainchainrpcuser=%s" % u1, "-mainchainrpcpassword=%s" % p1]
            if len(self.nodes) > 2:   # a node 2 added earlier: start it again, watching the second daemon
                if self.nodes[2].running:
                    self.stop_node(2)
                self.nodes[2].extra_args = args
            else:
                initialize_datadir(self.options.tmpdir, 2, "elementsregtest")
                self.add_nodes(1, [args], chain=["elementsregtest"])
            c = self.nodes[2]
            self.start_node(2, args)
            c.setfeeexchangerates(node.getfeeexchangerates())
            self.connect_nodes(2, 0)
            self.wait_for(lambda: c.getbestblockhash() == node.getbestblockhash(), "the third node syncs", 300)

            # (a) The claim's block anchors above what the lagging daemon knows.
            cut()
            lock = self.fresh_lock()
            self.wait_for(lambda: c.getbestblockhash() == node.getbestblockhash(), "the lock reaches the third node")
            ptx, pr, vout = self.paid(lock)
            assert b1().getblockcount() < pr["height"]
            claim = self.claim(lock, pr["record"], vout)
            node.sendrawtransaction(claim.serialize().hex())
            k = self.mine(1)[0]
            assert claim.hash in node.getblock(k)["tx"]
            held_at = c.getbestblockhash()
            time.sleep(5)
            assert_equal(c.getbestblockhash(), held_at)
            assert not [t for t in c.getchaintips() if t["status"] == "invalid"], c.getchaintips()
            try:
                c.sendrawtransaction(claim.serialize().hex())
                raise AssertionError("the lagging node pooled a claim it cannot place")
            except JSONRPCException as e:
                why = e.error["message"]
            assert "btcpay-not-in-anchor-chain" in why, why
            self.log.info("    (a) daemon behind: the claim's block is left aside (tip unchanged, nothing marked "
                          "invalid); the claim is refused by its mempool: %s", why)
            feed()
            # A block left aside for an anchor the daemon had not reached is
            # taken again when it is announced again: the next block does that.
            time.sleep(3)
            assert_equal(c.getbestblockhash(), held_at)
            self.mine(1)
            self.wait_for(lambda: c.getbestblockhash() == node.getbestblockhash(), "the third node follows", 120)
            assert claim.hash in c.getblock(k)["tx"]
            self.log.info("    (a) daemon caught up: at the next block the third node took the claim's block")
            self.results.append((self.current + " (a)", "daemon behind: block left aside, nothing invalid, mempool: "
                                 "%s; caught up: block taken" % why))

            # (b) The node alone, its daemon behind: the payment's block leaves its
            # daemon's chain; the payment is mined again where it cannot see yet.
            self.disconnect_nodes(0, 2)
            cut()
            y = pr["blockhash"]
            b1().invalidateblock(y)
            self.wait_for(lambda: c.getblockheader(k)["confirmations"] < 0, "the third node disconnects the claim")
            b.invalidateblock(y)
            self.btc_mine(2)
            assert self.bbuyer.gettransaction(ptx)["confirmations"] > 0
            time.sleep(8)
            assert claim.hash not in c.getrawmempool()
            assert not [line for line in self.debug_lines(c, "re-attached") if claim.hash in line]
            ticks = self.tick_lines(c, 0)
            self.log.info("    (b) daemon behind the payment's new block: the claim waits (ticks: longest %d ms)",
                          max(ticks) if ticks else 0)
            feed()
            self.generatetoaddress(c, 1, node.getnewaddress(), sync_fun=self.no_op)
            self.wait_for(lambda: claim.hash in c.getrawmempool(), "the third node re-attaches the claim", 60)
            how = [line for line in self.debug_lines(c, "re-attached") if claim.hash in line][-1].split("re-attached ")[1]
            blk = self.generatetoaddress(c, 1, node.getnewaddress(), sync_fun=self.no_op)[0]
            assert claim.hash in c.getblock(blk)["tx"]
            self.log.info("    (b) daemon caught up: the node re-attached %s itself and mined it", how.strip())
            self.results.append((self.current + " (b)", "claim waited while the daemon was behind; re-attached %s "
                                 "once it caught up" % how.strip()))
        finally:
            if len(self.nodes) > 2 and self.nodes[2].running:
                self.stop_node(2)
            try:
                b1().stop()
                proc.wait(timeout=60)
            except Exception:
                proc.terminate()
                proc.wait(timeout=60)
        # Node 0 follows Bitcoin on its own; back to its normal state.
        self.mine_anchored(b.getblockcount())

    def waiting_set_holds_the_disconnected_blocks(self):
        self.step("N43 (PV15 X6) the waiting set holds what the disconnected blocks carry: 1,100 claims nobody can "
                  "satisfy, mined with an honest claim paying less, do not keep it out")
        node = self.nodes[0]
        b = self.btc_rpc()
        n = 1100
        self.restart_seq0(["-checkmempool=0"])
        self.outside_spk = bytes([0, 20]) + os.urandom(20)
        coins = self.attacker_coins(n)
        lock = self.fresh_lock(deadline_in=400)
        # The attacker's payment and the buyer's in one Bitcoin block X.
        atx = self.bminer.sendtoaddress(self.bminer.getnewaddress(), 1)
        ptx = self.pay(lock["addr"], lock["amount"])
        x = self.btc_mine(1)[0]
        self.mine_anchored(b.getblockheader(x)["height"])
        make = self.block_records(x)
        records = {}
        attackers = []
        for c in coins:
            if c["C"] not in records:
                records[c["C"]] = make(atx, c["C"])
            t = self.attacker_spend(c, records[c["C"]], 1000, self.outside_spk)
            node.sendrawtransaction(t.serialize().hex())
            attackers.append(t.hash)
        pr = self.proof_for(ptx, lock["C"], x)
        honest = self.claim(lock, pr["record"], self.vout_paying(ptx, lock["addr"], x), fee=600)
        node.sendrawtransaction(honest.serialize().hex())
        high = 1000 / node.getmempoolentry(attackers[0])["vsize"]
        low = 600 / node.getmempoolentry(honest.hash)["vsize"]
        assert low < high
        blk = self.mine(1)[0]
        mined = set(node.getblock(blk)["tx"])
        assert honest.hash in mined and all(t in mined for t in attackers)
        # X leaves Bitcoin's chain: the attacker's payment is double-spent, the
        # buyer's is mined again.
        b.invalidateblock(x)
        self.double_spend(atx, self.bminer)
        self.btc_mine(2)
        assert self.bbuyer.gettransaction(ptx)["confirmations"] > 0
        self.wait_for(lambda: node.getblockheader(blk)["confirmations"] < 0, "the claims' block is disconnected", 120)
        self.watcher_left(x)
        t0 = time.time()
        self.mine_anchored(b.getblockcount())
        self.wait_for(lambda: honest.hash in node.getrawmempool(), "the honest claim re-attached", 120)
        took = time.time() - t0
        assert not [line for line in self.debug_lines(node, "not held for re-attachment") if honest.hash in line]
        blk2 = self.mine(1)[0]
        assert honest.hash in node.getblock(blk2)["tx"]
        assert not set(attackers) & set(node.getblock(blk2)["tx"])
        self.log.info("    %d claims at %.1f per vB nobody can satisfy and the buyer's at %.1f per vB, one block: the "
                      "buyer's claim re-attached %.1f s after the new anchor and mined", n, high, low, took)
        self.results.append((self.current, "%d unsatisfiable claims at %.1f per vB and the buyer's at %.1f per vB in the "
                             "disconnected block: the buyer's re-attached in %.1f s and mined" % (n, high, low, took)))
        self.restart_seq0()

    def anchor_driven_delay(self):
        """Seconds from Bitcoin orphaning the tip's anchor until the node disconnects that block."""
        node = self.nodes[0]
        b = self.btc_rpc()
        # A fresh Bitcoin block that only this Sequentia block anchors to, so the
        # reorganization takes this block and nothing below it.
        self.btc_mine(1)
        blk = self.mine_anchored(b.getblockcount())
        assert self.anchor_of(node.getblockheader(blk)["previousblockhash"]) < b.getblockcount()
        t0 = time.time()
        b.invalidateblock(node.getblockheader(blk)["anchorhash"])
        self.btc_mine(2)
        self.wait_for(lambda: node.getblockheader(blk)["confirmations"] < 0, "anchor-driven disconnect", timeout=600)
        return time.time() - t0

    def btc_rpc_count(self, method):
        log = open(os.path.join(self.options.tmpdir, "btc", "regtest", "debug.log")).read()
        return log.count("method=" + method + " ")

    def anchoring_first(self):
        self.step("N26 (PV13 S4) 100 claims waiting for a payment that was double-spent do not delay an anchor-driven reorganization")
        node = self.nodes[0]
        b = self.btc_rpc()
        base = self.anchor_driven_delay()
        n = 100
        leaves, amounts = [], {}
        for k in range(n):
            leaf = CScript([OP_INSPECTBTCPAYMENT, OP_2DROP, k + 17, OP_DROP, OP_1])
            tap = taproot_construct(NUMS, [("l", leaf)])
            addr = program_to_witness(1, tap.scriptPubKey[2:])
            leaves.append((leaf, tap))
            amounts[addr] = Decimal("0.01")
        ftx = node.sendmany(amounts=amounts, output_assets={a: self.asset for a in amounts}, fee_asset=self.asset)
        self.mine(1)
        ft = tx_from_hex(node.getrawtransaction(ftx))
        C = self.anchor_of(node.getrawtransaction(ftx, True)["blockhash"])
        ptx = self.bbuyer.sendtoaddress(self.bbuyer.getnewaddress(), 1)
        ph = self.btc_mine(1)[0]
        self.mine_anchored(b.getblockheader(ph)["height"])
        pr = self.proof_for(ptx, C, ph)
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        claims = []
        for leaf, tap in leaves:
            v = [i for i, o in enumerate(ft.vout) if o.scriptPubKey == tap.scriptPubKey][0]
            t = CTransaction()
            t.nVersion = 2
            t.vin.append(CTxIn(COutPoint(int(ftx, 16), v), nSequence=SEQ_NONFINAL_RBF))
            t.vout.append(CTxOut(CTxOutValue(1000000 - 2000), spk, asset))
            t.vout.append(CTxOut(CTxOutValue(2000), b"", asset))
            t.wit.vtxinwit.append(CTxInWitness())
            t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
            t.wit.vtxinwit[0].peginWitness.stack = [pr["record"]]
            control = bytes([tap.leaves["l"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["l"].merklebranch
            t.wit.vtxinwit[0].scriptWitness.stack = [bytes(leaf), control]
            t.rehash()
            node.sendrawtransaction(t.serialize().hex())
            claims.append(t.hash)
        # Bitcoin orphans the payment's block and the payment is double-spent:
        # every claim is evicted and waits for a payment that is gone.
        b.invalidateblock(ph)
        self.double_spend(ptx)
        self.btc_mine(2)
        self.wait_for(lambda: not any(c in node.getrawmempool() for c in claims), "the claims evicted", timeout=120)
        g0 = self.btc_rpc_count("getblock")
        time.sleep(10)
        load = self.btc_rpc_count("getblock") - g0
        delay = self.anchor_driven_delay()
        self.log.info("    anchor-driven disconnect: %.1f s with no claim waiting, %.1f s with %d; %d getblock calls in 10 s",
                      base, delay, n, load)
        self.results.append((self.current, "disconnect %.1f s (baseline %.1f s); %d getblock calls in 10 s" % (delay, base, load)))
        assert delay < base + 3, "anchor-driven reorganization delayed: %.1f s against %.1f s" % (delay, base)
        # At most one read of the recent blocks per Bitcoin tip, whatever the number of claims.
        assert load <= 2 * 144, load
        self.mine_anchored(b.getblockcount())

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
        # A false lower height whose links still end at a recorded anchor: the
        # anchor is not at the height the record implies.
        self.force([self.claim(lock, payment_record(h - 1, *args, links=links), vout)], "bad-btcpay-not-in-anchor-chain")
        self.force([self.claim(lock, payment_record(h, *args, links=[links[0]] * 3, nlinks=2017), vout)], "bad-btcpay-encoding")
        good = self.claim(lock, payment_record(h, *args, links=links), vout)
        node.sendrawtransaction(good.serialize().hex())
        blk = self.mine(1)[0]
        assert good.hash in node.getblock(blk)["tx"]
        self.results.append((self.current + " (3 links to the recorded anchor)", "mined"))
        node.reconsiderblock(side)

    def one_encoding_edges(self):
        self.step("N28 (PV13 F20, S10) every encoding check forced into a block for its own reason")
        node = self.nodes[0]
        b = self.btc_rpc()
        ptx = self.pay(self.bbuyer.getnewaddress(), 10000000)
        bh = self.btc_mine(1)[0]
        self.mine_anchored(b.getblockheader(bh)["height"])
        read = CScript([OP_INSPECTBTCPAYMENT, OP_2DROP, OP_1])
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])

        def fund(spk_script):
            if spk_script[0] == 0x51:
                addr = program_to_witness(1, spk_script[2:])
            else:
                addr = program_to_witness(0, spk_script[2:])
            txid = node.sendtoaddress(address=addr, amount=1, assetlabel=self.asset, fee_asset_label=self.asset)
            blk = self.mine(1)[0]
            t = tx_from_hex(node.getrawtransaction(txid))
            n = [i for i, o in enumerate(t.vout) if o.scriptPubKey == spk_script][0]
            return txid, n, self.anchor_of(blk)

        def spend(txid, n, slot, wstack):
            t = CTransaction()
            t.nVersion = 2
            t.vin.append(CTxIn(COutPoint(int(txid, 16), n), nSequence=SEQ_NONFINAL_RBF))
            t.vout.append(CTxOut(CTxOutValue(100000000 - 5000), bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"]), asset))
            t.vout.append(CTxOut(CTxOutValue(5000), b"", asset))
            t.wit.vtxinwit.append(CTxInWitness())
            t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
            t.wit.vtxinwit[0].peginWitness.stack = list(slot)
            t.wit.vtxinwit[0].scriptWitness.stack = list(wstack)
            t.rehash()
            return t

        def case(label, leaf, slot, expect):
            tap = taproot_construct(NUMS, [("l", leaf)])
            txid, n, C = fund(tap.scriptPubKey)
            control = bytes([tap.leaves["l"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["l"].merklebranch
            t = spend(txid, n, slot(C) if callable(slot) else slot, [bytes(leaf), control])
            self.log.info("    %s", label)
            self.force([t], expect)

        pr = self.proof_for(ptx, 0, bh)
        cs = hashlib.sha256(pr["coinbase"]).digest()

        def rec(C, **kw):
            a = dict(height=pr["height"], index=pr["index"], pbranch=pr["pbranch"], cbranch=pr["cbranch"],
                     payment=pr["payment"], depth=None)
            a.update(kw)
            return payment_record(a["height"], C, pr["header"], cs, a["index"], a["pbranch"], a["cbranch"], a["payment"],
                                  depth=a["depth"], links=pr["links"])
        # The baseline opens.
        tap = taproot_construct(NUMS, [("l", read)])
        txid, n, C = fund(tap.scriptPubKey)
        control = bytes([tap.leaves["l"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["l"].merklebranch
        self.force([spend(txid, n, [rec(C)], [bytes(read), control])])
        # The parser: one encoding for each fact.
        case("depth 0", read, lambda C: [rec(C, depth=0, pbranch=[], cbranch=[])], "bad-btcpay-encoding")
        case("depth 25", read, lambda C: [rec(C, depth=25, pbranch=[bytes(32)] * 25, cbranch=[bytes(32)] * 25)], "bad-btcpay-encoding")
        case("a payment of 100,001 bytes", read, lambda C: [rec(C, payment=b"\x02" * 100001)], "bad-btcpay-encoding")
        case("a trailing byte after the payment (a second encoding if allowed)", read,
             lambda C: [rec(C, payment=pr["payment"] + b"\x00")], "bad-btcpay-tx-encoding")
        case("an index bit above the depth (a second encoding if allowed)", read,
             lambda C: [rec(C, index=pr["index"] | (1 << len(pr["pbranch"])))], "bad-btcpay-index")
        # A payment of 64 bytes that parses as a transaction: refused for its size.
        sixty_four = (struct.pack("<i", 2) + b"\x01" + bytes(range(1, 33)) + struct.pack("<I", 0) + b"\x00" +
                      struct.pack("<I", 0xffffffff) + b"\x01" + struct.pack("<q", 1000) + b"\x04" + b"\x51" * 4 +
                      struct.pack("<I", 0))
        assert len(sixty_four) == 64
        case("a 64-byte payment that parses", read, lambda C: [rec(C, payment=sixty_four)], "bad-btcpay-tx-64-bytes")
        # The coinbase shown as the payment, and a payment shown at the coinbase's index.
        case("the coinbase as the payment", read, lambda C: [rec(C, payment=pr["coinbase"])], "bad-btcpay-tx-coinbase")
        case("a payment at index 0", read, lambda C: [rec(C, index=0)], "bad-btcpay-index")
        # The floor record.
        chk = CScript([5, OP_CHECKANCHORHEIGHTVERIFY, OP_DROP, OP_1])
        case("a duplicate floor", chk, [floor_record(5), floor_record(5)], "bad-anchorlock-duplicate")
        case("a floor of 6 bytes (a second encoding if allowed)", chk, [floor_record(5) + b"\x00"], "bad-anchorlock-encoding")
        case("a floor above the script's, which the anchor meets (a second encoding if allowed)", chk, [floor_record(6)],
             "Anchor floor differs from the one the script requires")
        # Outside tapscript the opcode has no meaning.
        ws = CScript([OP_INSPECTBTCPAYMENT, OP_2DROP, OP_1])
        wsh = CScript([0, hashlib.sha256(bytes(ws)).digest()])
        txid, n, C = fund(wsh)
        self.log.info("    0xe5 in a segwit v0 script")
        self.force([spend(txid, n, [rec(C)], [bytes(ws)])], "Opcode missing or not understood")

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

    def opcode_cost(self):
        k = 50000
        self.step("N22 (PV13 S3) one standard transaction running OP_INSPECTBTCPAYMENT %d times over an 80 kB proof" % k)
        node = self.nodes[0]
        early = self.bbuyer.sendtoaddress(self.bbuyer.getnewaddress(address_type="bech32"), Decimal("0.1"))
        eb = self.btc_mine(1)[0]
        self.btc_mine(1000)
        cov = CScript([OP_INSPECTBTCPAYMENT, CScriptOp(0x6d)] * k + [OP_1])  # OP_2DROP
        tap = taproot_construct(NUMS, [("cov", cov)])
        addr = program_to_witness(1, tap.scriptPubKey[2:])
        txid = node.sendtoaddress(address=addr, amount=1, assetlabel=self.asset, fee_asset_label=self.asset)
        self.mine_anchored(self.btc_rpc().getblockcount())
        tx = tx_from_hex(node.getrawtransaction(txid))
        vout = [i for i, o in enumerate(tx.vout) if o.scriptPubKey == tap.scriptPubKey][0]
        C = self.anchor_of(node.getrawtransaction(txid, True)["blockhash"])
        rec = self.proof_for(early, C, eb)["record"]
        t = CTransaction()
        t.nVersion = 2
        t.vin.append(CTxIn(COutPoint(int(txid, 16), vout), nSequence=SEQ_NONFINAL_RBF))
        asset = CTxOutAsset(b"\x01" + bytes.fromhex(self.asset)[::-1])
        t.vout.append(CTxOut(CTxOutValue(100000000 - 300000), bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"]), asset))
        t.vout.append(CTxOut(CTxOutValue(300000), b"", asset))
        t.wit.vtxinwit.append(CTxInWitness())
        t.wit.vtxoutwit = [CTxOutWitness() for _ in t.vout]
        t.wit.vtxinwit[0].peginWitness.stack = [rec]
        control = bytes([tap.leaves["cov"].version + tap.negflag]) + tap.internal_pubkey + tap.leaves["cov"].merklebranch
        t.wit.vtxinwit[0].scriptWitness.stack = [bytes(cov), control]
        t.rehash()
        dec = node.decoderawtransaction(t.serialize().hex())
        t0 = time.time()
        node.sendrawtransaction(t.serialize().hex())
        accept_s = time.time() - t0
        t0 = time.time()
        blk = self.mine(1)[0]
        mine_s = time.time() - t0
        assert t.hash in node.getblock(blk)["tx"]
        self.log.info("    proof %d bytes, tx weight %d: accepted in %.2fs, block made and checked in %.2fs",
                      len(rec), dec["weight"], accept_s, mine_s)
        assert accept_s < 5 and mine_s < 5, (accept_s, mine_s)
        self.results.append((self.current, {"proof_bytes": len(rec), "weight": dec["weight"],
                                            "accept_s": round(accept_s, 2), "block_s": round(mine_s, 2)}))

    def orphaned_and_longest_links(self):
        self.step("N29 a proof of an orphaned block at a height the chain recorded; 2,016 links and 2,017")
        node = self.nodes[0]
        b = self.btc_rpc()
        lock = self.fresh_lock(deadline_in=2200, margin=6)
        # The payment is mined in X at h and recorded; Bitcoin then replaces X by
        # Y at the same height (the payment not in it), and the chain records Y.
        ptx = self.pay(lock["addr"], lock["amount"])
        x = self.btc_mine(1)[0]
        h = b.getblockheader(x)["height"]
        self.mine_anchored(h)
        orphan = self.proof_for(ptx, lock["C"], x, link=False)
        vout = self.vout_paying(ptx, lock["addr"], x)
        b.invalidateblock(x)
        b.prioritisetransaction(ptx, 0, -100000000)
        y = self.btc_mine(1)[0]
        assert b.getblockheader(y)["height"] == h and ptx not in b.getblock(y)["tx"]
        self.wait_for(lambda: self.anchor_of(node.getbestblockhash()) < h or
                      node.getblockheader(node.getbestblockhash())["anchorhash"] == y, "the watcher follows")
        blk = self.mine_anchored(h)
        assert node.getblockheader(blk)["anchorhash"] == y
        self.force([self.claim(lock, orphan["record"], vout)], "bad-btcpay-not-in-anchor-chain")
        self.log.info("    the orphaned block X at a height the chain recorded (as Y): refused")
        # The payment mined again; the longest proof the rule allows, and one more.
        b.prioritisetransaction(ptx, 0, 100000000)
        p2 = self.btc_mine(1)[0]
        h2 = b.getblockheader(p2)["height"]
        assert ptx in b.getblock(p2)["tx"]
        self.btc_mine(2016)
        self.mine_anchored(h2 + 2016)
        self.btc_mine(1)
        self.mine_anchored(h2 + 2017)
        pr = self.proof_for(ptx, lock["C"], p2, link=False)
        cs = hashlib.sha256(pr["coinbase"]).digest()
        args = (lock["C"], pr["header"], cs, pr["index"], pr["pbranch"], pr["cbranch"], pr["payment"])
        links = self.link_headers(h2, h2 + 2017)
        self.force([self.claim(lock, payment_record(h2, *args, links=links), self.vout_paying(ptx, lock["addr"], p2))],
                   "bad-btcpay-encoding")
        self.log.info("    2,017 links to a recorded anchor: refused")
        good = self.claim(lock, payment_record(h2, *args, links=links[:2016]), self.vout_paying(ptx, lock["addr"], p2))
        blk = self.force([good])
        self.results.append((self.current, "2,016 links mined, 2,017 refused: bad-btcpay-encoding; orphaned block at a recorded height refused"))
        self.log.info("    2,016 links to a recorded anchor: mined in %s", blk[:16])

    def only_tightens(self):
        self.step("N16 the rule only tightens: a node without it accepts every block, and accepts a claim without proof that this node refuses")
        self.sync_blocks(self.nodes[:2])
        assert_equal(self.nodes[1].getbestblockhash(), self.nodes[0].getbestblockhash())
        lock = self.fresh_lock()
        self.sync_blocks(self.nodes[:2])
        noproof = self.spend_lock(lock, "claim", [], [b""])
        self.force([noproof], "No valid proof of a bitcoin payment")
        node = self.nodes[0]
        wpkh = node.getnewaddress("", "bech32")
        t = node.sendtoaddress(address=wpkh, amount=1, assetlabel=self.asset, fee_asset_label=self.asset)
        self.mine(1)
        self.sync_blocks(self.nodes[:2])
        n = [o["n"] for o in node.getrawtransaction(t, True)["vout"] if o["scriptPubKey"].get("address") == wpkh][0]
        raw = node.createrawtransaction(inputs=[{"txid": t, "vout": n}], outputs=[
            {node.getnewaddress(): Decimal("0.999"), "asset": self.asset}, {"fee": Decimal("0.001"), "fee_asset": self.asset}])
        v = tx_from_hex(node.signrawtransactionwithwallet(raw)["hex"])
        v.wit.vtxinwit[0].peginWitness.stack = [floor_record(0xffffffff)]
        self.force([v], "bad-anchorlock-not-reached")
        self.disconnect_nodes(0, 1)
        n1 = self.nodes[1]
        n1.generateblock(n1.getnewaddress(), [noproof.serialize().hex()], invalid_call=False)
        self.results.append((self.current + " (old node)", "mined the claim without proof"))
        # Nor does it check records: a floor no anchor meets, on an ordinary input
        # (a coin both nodes have, spent by node 0's wallet, mined by node 1).
        n1.generateblock(n1.getnewaddress(), [v.serialize().hex()], invalid_call=False)
        self.results.append((self.current + " (old node)", "mined a floor no anchor meets"))


if __name__ == '__main__':
    BtcPaymentProofTest().main()
