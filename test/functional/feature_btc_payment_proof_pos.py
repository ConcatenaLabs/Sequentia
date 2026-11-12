#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The proof of a bitcoin payment on a chain of committee-certified blocks.

The 60 blocks of section 4.4 of doc/sequentia/bitcoin-payment-proof.md are
counted in blocks, whatever certified them. This runs the swap on a
proof-of-stake chain with a VRF-elected leader and an aggregate committee of
three (quorum two), anchored to a real bitcoind, and fills the window from the
first block anchored at the payment with certified blocks, escaping-stall
blocks certified by the leader alone, and stalls in which Sequentia makes no
block while Bitcoin advances.

At every height of the window a refund, forced into a block, is refused with
`bad-anchorlock-not-reached` and a claim with its proof passes the same block
checks; the claim of one lock is mined at the window's last block and the refund
of another at the block after it. An escaping-stall block needs the anchor to
advance three Bitcoin blocks past its parent's and the anchors' median-time-past
to advance 600 seconds, so bitcoind mines at a Bitcoin cadence under mocktime.

Requires Bitcoin Core, named by PARENT_BITCOIND (default /usr/local/bin/bitcoind).
"""

from io import BytesIO
import hashlib
import os
import random
import subprocess
import time

from feature_btc_payment_proof import BITCOIND, FLOOR_DEPTH, BtcPaymentProofTest, payment_record
from test_framework.address import byte_to_base58
from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.blocktools import WITNESS_COMMITMENT_HEADER, get_witness_script
from test_framework.messages import (COIN, CBlock, COutPoint, CTransaction, CTxIn, CTxOut, deser_compact_size,
                                     deser_string, hash256, ser_compact_size, ser_string, ser_uint256,
                                     uint256_from_str)
from test_framework.script import CScript
import test_framework.util
from test_framework.util import assert_equal, p2p_port, rpc_port

COMMITTEE_SIZE = 3            # quorum 2: the leader alone is below it
PARENT_BLOCK_SECONDS = 600    # one Bitcoin interval between parent blocks
STALL_GAP = 3                 # POS_ESCAPING_STALL_ANCHOR_GAP


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class BtcPaymentProofPosTest(BtcPaymentProofTest):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.stakers = [make_staker() for _ in range(COMMITTEE_SIZE)]

    def seq_args(self, extra=None):
        return [
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1", "-poscommitteesize=%d" % COMMITTEE_SIZE,
            "-posslotinterval=1", "-signblockscript=51",
            "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1", "-con_blocksubsidy=0",
            "-anyonecanspendaremine=1", "-acceptnonstdtxn=1",
            "-blindedaddresses=0", "-con_default_blinded_addresses=0", "-validatepegin=0",
            "-con_any_asset_fees=1", "-fallbackfee=0.0001", "-maxtxfee=100.0", "-par=1",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % self.btc_port,
            "-mainchainrpcuser=%s" % self.btc_user, "-mainchainrpcpassword=%s" % self.btc_pass,
            "-parentgenesisblockhash=%s" % self.btc_genesis, "-debug=mempool", "-txindex=1",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers] + (extra or [])

    def setup_network(self, split=False):
        self.btc_port = rpc_port(3)
        btcdir = os.path.join(self.options.tmpdir, "btc")
        os.makedirs(btcdir)
        # Bitcoin's clock runs under mocktime, a block every 600 seconds, from
        # five days back so that no block is ever in the future.
        self.btc_time = int(time.time()) - 5 * 86400
        self.btc_proc = subprocess.Popen([BITCOIND, "-datadir=" + btcdir, "-regtest", "-port=%d" % p2p_port(3),
                                          "-rpcport=%d" % self.btc_port, "-listen=0", "-server",
                                          "-fallbackfee=0.0001", "-printtoconsole=0", "-mocktime=%d" % self.btc_time],
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
        self.add_nodes(1, [self.seq_args()], chain=["elementsregtest"])
        self.start_node(0)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        test_framework.util.node_fastmerkle = self.nodes[0]   # the witness merkle root, computed by the node

    def btc_mine(self, n, addr=None):
        """n Bitcoin blocks, each 600 seconds after the last."""
        addr = addr or self.bminer.getnewaddress()
        b = self.btc_rpc()
        out = []
        for _ in range(n):
            self.btc_time += PARENT_BLOCK_SECONDS
            b.setmocktime(self.btc_time)
            out += self.bminer.generatetoaddress(1, addr)
        return out

    # ------------------------------------------------------- PoS blocks
    def produce(self, certified, expect_height=None):
        """One block: certified by the leader and the committee, or by the leader
        alone (an escaping-stall block). The leader is whichever staker the slot
        elects; the call is retried while no staker's slot has opened."""
        node = self.nodes[0]
        wifs = [w for w, _ in self.stakers]
        last = None
        for _ in range(240):
            for i, leader in enumerate(wifs):
                committee = [w for j, w in enumerate(wifs) if j != i] if certified else []
                try:
                    r = node.generateposblock(leader, committee)
                except JSONRPCException as e:
                    last = e.error["message"]
                    continue
                if certified:
                    assert r["countersignatures"] >= 2, r
                else:
                    assert_equal(r["countersignatures"], 1)
                if expect_height is not None:
                    assert_equal(r["height"], expect_height)
                return r["hash"]
            time.sleep(0.25)
        raise AssertionError("no block produced (certified=%s): %s" % (certified, last))

    def mine(self, n=1, node=None):
        return [self.produce(True) for _ in range(n)]

    def setup_chain_state(self):
        node = self.nodes[0]
        self.genesis = int(node.getblockhash(0), 16)
        self.btc_mine(12)   # fill the median-time-past window at the Bitcoin cadence
        self.mine(1)
        # Endow the wallet from a genesis output anyone can spend.
        g = node.getblock(node.getblockhash(0), 2)
        coin = [(t["txid"], o["n"], int(o["value"] * COIN)) for t in g["tx"] for o in t["vout"]
                if o["scriptPubKey"]["hex"] == "51" and o.get("value", 0) > 0 and node.gettxout(t["txid"], o["n"])][0]
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        fund = CTransaction()
        fund.nVersion = 2
        fund.vin = [CTxIn(COutPoint(int(coin[0], 16), coin[1]))]
        endow, fee = 5000 * COIN, 100_000
        fund.vout = [CTxOut(endow, spk), CTxOut(coin[2] - endow - fee, CScript([0x51])), CTxOut(fee)]
        node.sendrawtransaction(fund.serialize().hex())
        self.mine(1)
        self.asset = node.issueasset(assetamount=100000, tokenamount=0, blind=False, fee_asset="bitcoin")["asset"]
        self.mine(1)
        rates = node.getfeeexchangerates()
        rates[self.asset] = 100000000
        node.setfeeexchangerates(rates)
        self.mine(FLOOR_DEPTH + 5)

    # ------------------------------------------- blocks built by hand
    def template(self, certified):
        """A block template from getposblocktemplate: the committee of three, or
        the leader alone. Returns (template, leader's key)."""
        node = self.nodes[0]
        wifs = [w for w, _ in self.stakers]
        pubs = [p for _, p in self.stakers]
        last = None
        for _ in range(240):
            seed = bytes.fromhex(node.getposschedule()["seed"])[::-1].hex()
            for i in range(len(wifs)):
                idx = range(len(wifs)) if certified else [i]
                members = [{"pubkey": pubs[j], "vrfproof": node.vrfprove(wifs[j], seed)["proof"]} for j in idx]
                try:
                    return node.getposblocktemplate(wifs[i], members), wifs[i]
                except JSONRPCException as e:
                    last = e.error["message"]
            time.sleep(0.25)
        raise AssertionError("no template (certified=%s): %s" % (certified, last))

    def split_block(self, hexblk, signhash):
        """(header bytes before the merkle root, merkle root, header bytes after it
        up to the challenge, challenge, the transactions). The block hash is the
        header without the solution; it must equal the template's signhash (in
        internal byte order)."""
        raw = bytes.fromhex(hexblk)
        f = BytesIO(raw)
        f.read(4 + 32 + 32 + 4)
        for extra in (4, 0):   # the height in the header, or not
            f.seek(72 + extra + 36)
            challenge = deser_string(f)
            head = raw[:72 + extra + 36] + ser_string(challenge)
            if hash256(head).hex() == signhash:
                deser_string(f)   # the solution, empty in a template
                n = deser_compact_size(f)
                txs = []
                for _ in range(n):
                    t = CTransaction()
                    t.deserialize(f)
                    txs.append(t)
                assert_equal(f.read(), b"")
                return raw[:36], raw[36:68], raw[68:72 + extra + 36], challenge, txs
        raise AssertionError("cannot place the header fields of the template")

    def with_txs(self, tmpl, extra_txs):
        """The template's block with `extra_txs` appended, its witness commitment
        and merkle root made again. Returns (block hex, its signhash)."""
        pre, merkle, post, challenge, txs = self.split_block(tmpl["block"], tmpl["signhash"])
        holder = CBlock()

        def roots(vtx):
            holder.vtx = vtx
            return holder.calc_merkle_root(), holder.calc_witness_merkle_root()
        cb = txs[0]
        at = [k for k, o in enumerate(cb.vout) if bytes(o.scriptPubKey)[2:6] == WITNESS_COMMITMENT_HEADER]
        assert_equal(len(at), 1)
        nonce = cb.wit.vtxinwit[0].scriptWitness.stack[0]
        # Control: the same computation gives back the template's own roots.
        mr, wr = roots(txs)
        assert_equal(ser_uint256(mr), merkle)
        assert_equal(get_witness_script(uint256_from_str(bytes.fromhex(wr)[::-1]), uint256_from_str(nonce)),
                     cb.vout[at[0]].scriptPubKey)
        vtx = txs + list(extra_txs)
        _, wr = roots(vtx)
        cb.vout[at[0]] = CTxOut(0, get_witness_script(uint256_from_str(bytes.fromhex(wr)[::-1]), uint256_from_str(nonce)))
        cb.rehash()
        mr, _ = roots(vtx)
        head = pre + ser_uint256(mr) + post + ser_string(challenge)
        body = ser_compact_size(len(vtx)) + b"".join(t.serialize() for t in vtx)
        return (head + ser_string(b"") + body).hex(), hash256(head).hex()

    def sign_and_submit(self, tmpl, leader, hexblk, signhash):
        """The members' MuSig2 signature over `signhash`, then submitposblock."""
        node = self.nodes[0]
        wif_of = {p: w for w, p in self.stakers}
        members = tmpl["members"]
        self.sid = getattr(self, "sid", 0) + 1
        sid = "bp9-%d" % self.sid
        # One node holds every member's key here, and a session id is the node's:
        # one per member.
        nonces = [node.musignonce(sid + pk, wif_of[pk], members, signhash)["pubnonce"] for pk in members]
        parts = [node.musigpartialsign(sid + pk, wif_of[pk], members, nonces, signhash)["partialsig"] for pk in members]
        sig = node.musigaggregate(members, nonces, parts, signhash)["signature"]
        assert node.musigverify(members, signhash, sig)["valid"]
        return node.submitposblock(hexblk, leader, sig)

    def tpb(self, hexblk):
        try:
            self.nodes[0].testproposedblock(hexblk, True)
            return None
        except JSONRPCException as e:
            return e.error["message"]

    # ------------------------------------------------------- the window
    def window(self, name, modes, stalls=None):
        """Pay two locks at their deadline D (= R), then fill the 60 blocks from
        the first block anchored at D with `modes` ('C' certified, 'S' the leader
        alone), with Bitcoin blocks and no Sequentia block before the heights in
        `stalls`. At every height: a refund forced into a block (built, signed and
        submitted) is refused, and the claim passes the block checks."""
        stalls = stalls or {}
        assert_equal(len(modes), FLOOR_DEPTH + 1)
        self.step("%s: %s" % (name, "".join(modes)))
        node = self.nodes[0]
        b = self.btc_rpc()
        F = self.anchor_of(node.getbestblockhash())
        D = b.getblockcount() + STALL_GAP + 1
        locks = [self.new_lock(50_000_000, F, D, D) for _ in range(2)]
        self.mine(1)
        for lk in locks:
            lk["C"] = self.lock_coin_anchor(lk)
        self.btc_mine(D - 1 - b.getblockcount())
        pays = [self.pay(lk["addr"], lk["amount"]) for lk in locks]
        dh = self.btc_mine(1)[0]
        assert_equal(b.getblockheader(dh)["height"], D)
        for t in pays:
            assert t in b.getblock(dh)["tx"]
        A, B = locks
        refund = self.refund(B)
        P = node.getblockcount() + 1
        counts = {"C": 0, "S": 0}
        refused = []
        for k, mode in enumerate(modes):
            n = P + k
            if k in stalls:
                self.btc_mine(stalls[k])
            elif mode == "S" and k > 0:
                self.btc_mine(STALL_GAP)
            elif random.random() < 0.3:
                self.btc_mine(1)
            parent_anchor = self.anchor_of(node.getbestblockhash())
            tmpl, leader = self.template(mode == "C")
            assert_equal(tmpl["height"], n)
            claims = {}
            for lk, t in zip(locks, pays):
                pr = self.proof_for(t, lk["C"], dh)
                if k == 0:
                    # Nothing below P records an anchor at or above D: the proof
                    # links to the anchor P itself records.
                    a = int.from_bytes(self.split_block(tmpl["block"], tmpl["signhash"])[2][-36:-32], "little")
                    pr["record"] = payment_record(pr["height"], lk["C"], pr["header"], hashlib.sha256(pr["coinbase"]).digest(),
                                                  pr["index"], pr["pbranch"], pr["cbranch"], pr["payment"],
                                                  links=self.link_headers(pr["height"], a))
                claims[lk is A] = self.claim(lk, pr["record"], self.vout_paying(t, lk["addr"], dh))
            # The mempool, judging for this height.
            mp = node.testmempoolaccept([refund.serialize().hex()])[0]
            if k < FLOOR_DEPTH:
                assert not mp["allowed"] and mp["reject-reason"] == "anchorlock-not-reached", mp
                if k > 0:
                    assert node.testmempoolaccept([claims[False].serialize().hex()])[0]["allowed"]
            # The claim in this block passes the block checks; so does the block.
            assert_equal(self.tpb(tmpl["block"]), None)
            hexc, _ = self.with_txs(tmpl, [claims[False]])
            claim_check = self.tpb(hexc)
            if k < FLOOR_DEPTH:
                # The refund in this block: refused by the block checks, and the
                # block built, signed and submitted with it is refused too.
                hexr, shr = self.with_txs(tmpl, [refund])
                why = self.tpb(hexr)
                assert why is not None and "bad-anchorlock-not-reached" in why, why
                tip = node.getbestblockhash()
                try:
                    self.sign_and_submit(tmpl, leader, hexr, shr)
                except JSONRPCException as e:
                    assert_equal(e.error["message"], "ProcessNewBlock, block not accepted")
                assert_equal(node.getbestblockhash(), tip)
                lines = self.debug_lines(node, "ConnectBlock %s failed" % bytes.fromhex(shr)[::-1].hex())
                assert lines and "bad-anchorlock-not-reached" in lines[-1], lines
                refused.append(n)
                if k in (0, FLOOR_DEPTH - 1):
                    self.log.info("    P+%d (%s): the block checks: %s; the signed block: %s", k, mode, why,
                                  lines[-1].split("] ")[-1][:200])
                    self.results.append((self.current + " P+%d" % k, "%s | %s" % (why, lines[-1].split("] ")[-1])))
            if k == FLOOR_DEPTH - 1:
                # The window's last block mines lock A's claim.
                node.sendrawtransaction(claims[True].serialize().hex())
                tmpl, leader = self.template(mode == "C")
            if k == FLOOR_DEPTH:
                assert mp["allowed"], mp
                node.sendrawtransaction(refund.serialize().hex())
                tmpl, leader = self.template(mode == "C")
            # The same template, without the refund, is this height's block.
            res = self.sign_and_submit(tmpl, leader, tmpl["block"], tmpl["signhash"])
            assert_equal(res["height"], n)
            assert_equal(node.getbestblockhash(), res["hash"])
            anchor = self.anchor_of(res["hash"])
            if k == 0:
                assert anchor >= D and parent_anchor < D, (anchor, parent_anchor, D)
            if mode == "S":
                assert anchor - parent_anchor >= STALL_GAP
            counts[mode] += 1
            if k == 0:
                self.log.info("    P = %d anchored at %d (parent at %d), the claim in it: %s", n, anchor, parent_anchor,
                              claim_check or "passes the block checks")
                self.results.append((self.current + " P", "first block anchored at D=%d: height %d, %s; the claim in "
                                     "it: %s" % (D, n, mode, claim_check or "passes")))
            else:
                assert_equal(claim_check, None)
            if k == FLOOR_DEPTH - 1:
                assert claims[True].hash in node.getblock(res["hash"])["tx"]
            if k == FLOOR_DEPTH:
                assert refund.hash in node.getblock(res["hash"])["tx"]
        assert_equal(refused, list(range(P, P + FLOOR_DEPTH)))
        self.log.info("    window %d..%d: %d certified and %d leader-only blocks, stalls %s; the refund refused in a "
                      "signed block at each of the 60, mined at P+60; lock A's claim mined at P+59",
                      P, P + FLOOR_DEPTH, counts["C"], counts["S"], stalls)
        self.results.append((self.current, "refund refused (bad-anchorlock-not-reached) in a signed, submitted block at "
                             "each of P..P+59; claim passes the block checks at P+1..P+59 and is mined at P+59; refund "
                             "mined at P+60; %d certified, %d leader-only, stalls %s" % (counts["C"], counts["S"], stalls)))

    def leader_alone_needs_the_gap(self):
        """The control for the leader-only blocks: built the same way, one is
        refused by the block checks while the anchor has not advanced three
        Bitcoin blocks, for the quorum, so the blocks the windows accept are
        below the quorum."""
        self.step("W0 a block certified by the leader alone, without the anchor gap")
        node = self.nodes[0]
        wifs = [w for w, _ in self.stakers]
        pubs = [p for _, p in self.stakers]
        seed = bytes.fromhex(node.getposschedule()["seed"])[::-1].hex()
        whys = set()
        for i in range(len(wifs)):
            try:
                node.getposblocktemplate(wifs[i], [{"pubkey": pubs[i], "vrfproof": node.vrfprove(wifs[i], seed)["proof"]}])
                raise AssertionError("a leader-only template without the anchor gap")
            except JSONRPCException as e:
                whys.add(e.error["message"])
        assert_equal(whys, {"CreateNewBlock: TestBlockValidity failed: bad-posvrf-agg-quorum, fewer named committee "
                            "members than the certification quorum"})
        self.log.info("    refused: %s", whys.pop())
        self.results.append((self.current, "refused: bad-posvrf-agg-quorum, fewer named committee members than the "
                             "certification quorum"))
        self.btc_mine(STALL_GAP)
        tmpl, leader = self.template(False)
        res = self.sign_and_submit(tmpl, leader, tmpl["block"], tmpl["signhash"])
        assert_equal(node.getbestblockhash(), res["hash"])
        self.log.info("    after three Bitcoin blocks the same construction is accepted")

    def run_test(self):
        try:
            self.results = []
            self.setup_chain_state()
            self.leader_alone_needs_the_gap()
            random.seed(9)
            self.window("W1 every block certified by the leader alone", ["S"] * (FLOOR_DEPTH + 1))
            self.window("W2 certified and leader-only blocks in turn", ["C", "S"] * 30 + ["C"])
            mix = [random.choice("CS") for _ in range(FLOOR_DEPTH + 1)]
            self.window("W3 a random mix, with stalls", ["S"] + mix[1:], stalls={1: 20, 30: 40, 59: 12})
        finally:
            self.write_results()
            self.stop_bitcoind()

if __name__ == '__main__':
    BtcPaymentProofPosTest().main()
