#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Lost blocks (doc 03 §3, "Transactions of a lost block"), attacked.

A transaction T is certified in block L, Bitcoin then replaces L's anchor, and
the seller who signed T has a conflicting spend R built into a block. Every
node that holds L (the evidence) refuses that block, whoever certified it.

A real bitcoind is the parent chain. Six stakers, one seat each (quorum 4).
Blocks are made only through generateposblock, so that each case names who
leads and who signs: node 0 is the seller (-posdebugignorelost, the rule off,
so it builds what honest nodes refuse), nodes 1-4 are honest full nodes, node
5 is the node that is offline, restarted, or syncs later. Every node runs
-par=1; each refusal is asserted by the node's own reason.
"""

import json
import os
import shutil
import subprocess
import time
import urllib.parse
from decimal import Decimal

from test_framework.address import byte_to_base58
from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.key import ECKey
from test_framework.messages import tx_from_hex
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, p2p_port, rpc_port

BITCOIND = os.environ.get("PARENT_BITCOIND", "/usr/local/bin/bitcoind")
CASES = os.environ.get("LOST_CASES", "a,b,c,f,g").split(",")
RESULTS = os.environ.get("LOST_RESULTS", "")
BASE_BIN = os.environ.get("LOST_BASE_BIN", "")
W = int(os.environ.get("LOST_WINDOW", "6"))
N = 6
SELLER = 0
LATE = 5
HONEST = [1, 2, 3, 4]
ALL_KEYS = list(range(6))
CHAIN = "elementsregtest"


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class LostBlocksTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = N
        self.stakers = [make_key() for _ in range(N)]
        self.results = {}
        self.base_nodes = set(int(x) for x in os.environ.get("LOST_BASE_NODES", "").split(",") if x)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if not os.path.exists(BITCOIND):
            raise SkipTest("needs Bitcoin Core's bitcoind (set PARENT_BITCOIND)")

    # ------------------------------------------------------------- bitcoind
    def btc(self, wallet=None):
        url = "http://%s:%s@127.0.0.1:%d" % (urllib.parse.quote(self.btc_user), urllib.parse.quote(self.btc_pass), self.btc_port)
        return AuthServiceProxy(url + ("/wallet/" + wallet if wallet else ""), timeout=120)

    def btc_mine(self, n):
        self.btc_addr = self.btc("miner").getnewaddress()
        return self.btc().generatetoaddress(n, self.btc_addr)

    def stop_bitcoind(self):
        if getattr(self, "btc_proc", None) and self.btc_proc.poll() is None:
            try:
                self.btc().stop()
                self.btc_proc.wait(timeout=60)
            except Exception:
                self.btc_proc.terminate()
                self.btc_proc.wait(timeout=60)

    # ------------------------------------------------------------- network
    def node_args(self, i):
        a = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=%d" % (N + 1), "-posslotinterval=1", "-poshardeningheight=0", "-posexpraceheight=1",
            "-con_max_block_sig_size=8000", "-con_blocksubsidy=0", "-initialfreecoins=1000000000000",
            "-con_connect_genesis_outputs=1", "-acceptnonstdtxn=1", "-par=1", "-fallbackfee=0.0001",
            "-blindedaddresses=0", "-con_default_blinded_addresses=0",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-posescapestallmtpgap=0",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % self.btc_port,
            "-mainchainrpcuser=%s" % self.btc_user, "-mainchainrpcpassword=%s" % self.btc_pass,
            "-parentgenesisblockhash=%s" % self.btc_genesis, "-debug=mempool", "-debug=validation", "-txindex=1",
            "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
        ] + self.specs
        if i not in self.base_nodes:
            a.append("-poslostwindow=%d" % W)
        if i == SELLER:
            a += ["-posdebugignorelost=1", "-minrelaytxfee=0", "-blockmintxfee=0"]
        return a

    def setup_network(self):
        try:
            self._setup_network()
        except BaseException:
            self.stop_bitcoind()
            raise

    def _setup_network(self):
        self.btc_port = rpc_port(N + 2)
        btcdir = os.path.join(self.options.tmpdir, "btc")
        os.makedirs(btcdir)
        self.btc_proc = subprocess.Popen([BITCOIND, "-datadir=" + btcdir, "-regtest", "-port=%d" % p2p_port(N + 2),
                                          "-rpcport=%d" % self.btc_port, "-listen=0", "-server", "-fallbackfee=0.0001",
                                          "-printtoconsole=0"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cookie = os.path.join(btcdir, "regtest", ".cookie")
        self.wait_until(lambda: os.path.exists(cookie), timeout=60)
        self.btc_user, self.btc_pass = open(cookie).read().strip().split(":", 1)

        def up():
            try:
                return self.btc().getblockcount() >= 0
            except Exception:
                return False
        self.wait_until(up, timeout=60)
        self.btc().createwallet("miner")
        self.btc_genesis = self.btc().getblockhash(0)
        self.btc_mine(5)
        self.specs = ["-staker=%s:1" % pub for (_, pub) in self.stakers]
        binaries = [BASE_BIN if (i in self.base_nodes and BASE_BIN) else self.options.bitcoind for i in range(N)]
        self.add_nodes(N, [self.node_args(i) for i in range(N)], chain=[CHAIN] * N, binary=binaries)
        self.nodes[1].start()
        self.nodes[1].wait_for_rpc_connection()
        self.specs = ["-staker=%s:1%s" % (pub, self.nodes[1].getblsregistration(wif)["spec"]) for (wif, pub) in self.stakers]
        self.stop_node(1)
        shutil.rmtree(os.path.join(self.nodes[1].datadir, CHAIN))
        for i in range(N):
            self.nodes[i].extra_args = self.node_args(i)
            self.start_node(i, extra_args=self.node_args(i))
        self.mesh(range(N))
        for i in range(N):
            self.nodes[i].createwallet(wallet_name="w", descriptors=True)
        self.bootstrap()

    def mesh(self, idx):
        idx = list(idx)
        for k, i in enumerate(idx):
            for j in idx[k + 1:]:
                self.connect_nodes(i, j)

    def disconnect_all(self, i):
        n = self.nodes[i]
        n.setnetworkactive(False)
        self.wait_until(lambda: n.getconnectioncount() == 0, timeout=30)
        n.setnetworkactive(True)

    # --------------------------------------------------------------- chain
    def tip(self, node):
        return node.getblockheader(node.getbestblockhash())

    def sync_tips(self, idx, timeout=60):
        self.wait_until(lambda: len({self.nodes[i].getbestblockhash() for i in idx}) == 1, timeout=timeout)

    def produce(self, node_i, signers, leaders=None, txs_must=(), allow_fail=False):
        """A block made on node_i, led by one of `leaders` (the first whose slot is open), countersigned by `signers`."""
        node = self.nodes[node_i]
        leaders = leaders if leaders is not None else signers
        if txs_must:
            self.wait_until(lambda: all(t in node.getrawmempool() for t in txs_must), timeout=30)
        if len(set(signers) | set(leaders[:1])) < 4:
            # Below the quorum: an escaping-stall block needs Bitcoin 3 blocks above the parent's anchor.
            need = self.tip(node)["anchorheight"] + 3
            if self.btc().getblockcount() < need:
                self.btc_mine(need - self.btc().getblockcount())
            time.sleep(2)
        last = None
        for _ in range(80):
            for k in leaders:
                try:
                    r = node.generateposblock(self.stakers[k][0], [self.stakers[j][0] for j in signers if j != k])
                    b = node.getblock(r["hash"])
                    for t in txs_must:
                        assert t in b["tx"], "block lacks %s" % t
                    r["block"] = b
                    return r
                except JSONRPCException as e:
                    last = e.error["message"]
            time.sleep(0.25)
        if allow_fail:
            return {"error": last}
        raise AssertionError("no block on node %d: %s" % (node_i, last))

    def block_of(self, txid, node):
        t = node.getblockcount()
        for h in range(t, max(0, t - 300), -1):
            if txid in node.getblock(node.getblockhash(h))["tx"]:
                return h
        return None

    def log_has(self, i, text, since=0):
        with open(self.nodes[i].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(since)
            return [line.strip() for line in f if text in line]

    def log_size(self, i):
        return os.path.getsize(self.nodes[i].debug_log_path)

    def record(self, case, key, value):
        self.results.setdefault(case, {})[key] = value
        self.log.info("  [%s] %s: %s", case, key, value)

    def lost(self, i):
        return {b["hash"]: b for b in self.nodes[i].getlostblocks()["blocks"] if b["lost"]}

    def wait_lost(self, h, idx, timeout=60):
        self.wait_until(lambda: all(h in self.lost(i) for i in idx), timeout=timeout)

    def status_of(self, node, h):
        for t in node.getchaintips():
            if t["hash"] == h:
                return t["status"]
        try:
            hdr = node.getblockheader(h)
            return "in active chain" if hdr["confirmations"] > 0 else "known, not a tip"
        except JSONRPCException:
            return "unknown"

    # ----------------------------------------------------------- the coins
    def bootstrap(self):
        """Split the genesis anyone-can-spend coin into coins of the seller's."""
        a = self.nodes[SELLER]
        g = a.getblock(a.getblockhash(0), 2)
        coin = None
        for tx in g["tx"]:
            for vo in tx["vout"]:
                if vo["scriptPubKey"]["hex"] == "51" and vo.get("value", 0) > 0:
                    coin = (tx["txid"], vo["n"], Decimal(vo["value"]))
        fee = Decimal("0.001")
        outs = [{a.getnewaddress(): Decimal("100")} for _ in range(40)]
        outs.append({a.getnewaddress(): coin[2] - 40 * Decimal("100") - fee})
        outs.append({"fee": fee})
        raw = a.createrawtransaction([{"txid": coin[0], "vout": coin[1]}], outs)
        txid = a.sendrawtransaction(raw)
        self.produce(SELLER, ALL_KEYS, txs_must=[txid])
        self.sync_tips(range(N))

    def fund(self, amount=Decimal("1"), online=None):
        a = self.nodes[SELLER]
        txid = a.sendtoaddress(address=a.getnewaddress(), amount=amount, fee_asset_label="bitcoin")
        self.produce(SELLER, ALL_KEYS, txs_must=[txid])
        self.sync_tips(online if online is not None else range(N))
        vout = [o for o in a.getrawtransaction(txid, 1)["vout"] if o["value"] == amount][0]["n"]
        return {"txid": txid, "vout": vout, "amount": amount}

    def sync_mempools_of(self, idx, txid, timeout=30):
        self.wait_until(lambda: all(txid in self.nodes[i].getrawmempool() for i in idx), timeout=timeout)

    def spend(self, coin, to_addr, fee):
        a = self.nodes[SELLER]
        raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"], "sequence": 0xfffffffd}],
                                     [{to_addr: coin["amount"] - fee}, {"fee": fee}])
        return a.signrawtransactionwithwallet(raw)["hex"]

    def bitcoin_reorg(self, depth):
        b = self.btc()
        tip = b.getblockcount()
        self.btc_orig_first = b.getblockhash(tip - depth + 1)
        b.invalidateblock(self.btc_orig_first)
        self.btc_new = self.btc_mine(depth + 1)
        return tip - depth

    # ----------------------------------------------------------- the gap
    def gap(self, case, online=None):
        """T certified in L (anchored at Bitcoin's tip, a certified child above it), Bitcoin replaces L's
        anchor 1 deep, R in the seller's mempool. Returns the state."""
        online = online if online is not None else list(range(N))
        honest_on = [i for i in HONEST if i in online]
        a = self.nodes[SELLER]
        coin = self.fund(online=online)
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()
        self.btc_mine(1)
        btc_tip = self.btc().getblockcount()
        self.nodes[1].sendrawtransaction(t_hex)
        L = self.produce(1, ALL_KEYS, txs_must=[t_id])
        assert_equal(L["block"]["anchorheight"], btc_tip)
        self.produce(1, ALL_KEYS)   # a certified child: T is certified and buried one block
        self.sync_tips(online)
        hdr = self.nodes[1].getblockheader(L["hash"])
        assert hdr["poscertified"]
        self.record(case, "T", "%s in L %s at height %d, anchor %d, certified" % (t_id, L["hash"][:16], L["height"], btc_tip))
        sizes = [self.log_size(i) for i in range(N)]
        t0 = time.time()
        fork = self.bitcoin_reorg(1)
        left = {}
        while len(left) < len(online) and time.time() - t0 < 60:
            for i in online:
                if i not in left and self.nodes[i].getblockcount() < L["height"]:
                    left[i] = round(time.time() - t0, 2)
            time.sleep(0.05)
        assert_equal(len(left), len(online))
        follow = max(left.values())
        self.record(case, "seconds each node took to leave L", left)
        honest_on = [i for i in honest_on if i not in self.base_nodes]
        self.wait_lost(L["hash"], honest_on)
        self.record(case, "reorg", "Bitcoin 1 deep (fork %d); every online node left L in %.1f s; L lost on %s" % (fork, follow, honest_on))
        # R: the seller's mempool takes it (RBF over T), an honest mempool refuses it with the reason.
        a.sendrawtransaction(r_hex)
        assert r_id in a.getrawmempool()
        try:
            self.nodes[honest_on[0]].sendrawtransaction(r_hex)
            raise AssertionError("honest mempool took R")
        except JSONRPCException as e:
            assert "txn-lost-conflict" in e.error["message"], e.error
            self.record(case, "honest mempool refuses R", e.error["message"])
        return dict(t_id=t_id, r_id=r_id, t_hex=t_hex, r_hex=r_hex, L=L["hash"], L_h=L["height"], sizes=sizes, coin=coin)

    def respend_block(self, case, st, signers):
        """The seller builds and broadcasts X carrying R on its own tip."""
        a = self.nodes[SELLER]
        x = self.produce(SELLER, signers, leaders=[SELLER] + [k for k in signers if k != SELLER], txs_must=[st["r_id"]])
        xh = a.getblockheader(x["hash"])
        self.record(case, "X", "%s at height %d, %d countersignature(s), certified on the seller's node: %s, carries R" %
                    (x["hash"][:16], x["height"], x["countersignatures"], xh["poscertified"]))
        return x

    def assert_refused(self, case, idx, x_hash, since, timeout=30):
        """Each node in idx refuses X by the rule's reason (its log), X is invalid there, not its tip."""
        def ok():
            return all(self.log_has(i, "Lost blocks: refusing block %s" % x_hash, since[i]) for i in idx)
        self.wait_until(ok, timeout=timeout)
        for i in idx:
            n = self.nodes[i]
            assert n.getbestblockhash() != x_hash
            st = self.status_of(n, x_hash)
            assert st == "invalid", (i, st)
        line = self.log_has(idx[0], "Lost blocks: refusing block %s" % x_hash, since[idx[0]])[0]
        self.record(case, "refusal (node %d log)" % idx[0], line.split(" ", 1)[1][:260])

    def forced(self, case, node_i, block_hex):
        r = self.nodes[node_i].submitblock(block_hex)
        self.record(case, "submitblock of X on node %d" % node_i, r)
        return r

    # ---------------------------------------------------------------- cases
    def case_a(self):
        self.log.info("(a) the seller leads every block; the committee is honest")
        st = self.gap("a")
        a = self.nodes[SELLER]
        x = self.respend_block("a", st, [SELLER])
        self.assert_refused("a", HONEST + [LATE], x["hash"], st["sizes"])
        # Forced: X handed to an honest node directly.
        self.forced("a", 2, a.getblock(x["hash"], 0))
        # A child of X: refused as the child of an invalid block.
        y = self.produce(SELLER, [SELLER])
        time.sleep(3)
        for i in HONEST:
            assert self.nodes[i].getbestblockhash() not in (x["hash"], y["hash"])
        self.record("a", "honest tips while only the seller produces", "stay at height %d (the fork point); T in honest mempools: %s" %
                    (self.nodes[1].getblockcount(), all(st["t_id"] in self.nodes[i].getrawmempool() for i in HONEST)))
        # An honest producer gets a turn: T is mined, R never.
        h = self.produce(1, [1, 2, 3, 4, 5], leaders=[1, 2, 3, 4, 5], txs_must=[st["t_id"]])
        self.sync_tips(HONEST + [LATE])
        n = self.nodes[2]
        assert self.block_of(st["t_id"], n) is not None and self.block_of(st["r_id"], n) is None
        self.record("a", "seller's peers after X and its child", "%d of 5 (no ban): %s" % (
            len(a.getpeerinfo()), [p.get("banscore", 0) for p in self.nodes[1].getpeerinfo()]))
        assert_equal(len(a.getpeerinfo()), 5)
        self.record("a", "outcome", "honest block %s (%d signer(s)) mines T at height %d on every honest node; R nowhere" %
                    (h["hash"][:16], h["countersignatures"] + 1, h["height"]))
        self.realign()

    def case_b(self):
        self.log.info("(b) a dishonest quorum certifies the respend")
        st = self.gap("b")
        a = self.nodes[SELLER]
        x = self.respend_block("b", st, ALL_KEYS)
        assert a.getblockheader(x["hash"])["poscertified"]
        self.assert_refused("b", HONEST + [LATE], x["hash"], st["sizes"])
        self.forced("b", 3, a.getblock(x["hash"], 0))
        self.record("b", "observer (node 2) chain tips", [(t["height"], t["hash"][:16], t["status"]) for t in self.nodes[2].getchaintips()][:4])
        self.record("b", "buyer's node holds", "T in mempool: %s; L lost: %s" %
                    (st["t_id"] in self.nodes[1].getrawmempool(), st["L"] in self.lost(1)))
        # The quorum refuses to certify honest blocks: an honest block escaping the stall, signed by one member.
        h = self.produce(1, [1], leaders=[1], txs_must=[st["t_id"]])
        self.sync_tips(HONEST + [LATE])
        self.record("b", "honest escaping-stall block", "%s mines T at height %d, %d signer(s), certified: %s" %
                    (h["hash"][:16], h["height"], h["countersignatures"] + 1, self.nodes[2].getblockheader(h["hash"])["poscertified"]))
        st["h"] = h
        if "b2" in CASES:
            self.case_b2(st, x)
        self.realign()

    def case_b2(self, st, x):
        """After the window: the quorum respends again, anchored more than W above L."""
        self.log.info("(b2) the same quorum after the window")
        a = self.nodes[SELLER]
        lh = self.nodes[1].getlostblocks()
        l_anchor = [b for b in lh["blocks"] if b["hash"] == st["L"]][0]["anchorheight"]
        self.btc_mine(W + 1)
        target = self.btc().getblockcount()
        self.wait_until(lambda: self.tip(a)["anchorheight"] <= target)
        sizes = [self.log_size(i) for i in range(N)]
        # The seller rebuilds on the fork point: its own tip may be X; make a sibling on X's parent.
        a.invalidateblock(x["hash"])
        if st.get("h") and a.getbestblockhash() == st["h"]["hash"]:
            a.invalidateblock(st["h"]["hash"])
        a.sendrawtransaction(st["r_hex"]) if st["r_id"] not in a.getrawmempool() else None
        x2 = self.produce(SELLER, ALL_KEYS, leaders=[SELLER] + ALL_KEYS[1:], txs_must=[st["r_id"]])
        x2b = self.produce(SELLER, ALL_KEYS)
        self.record("b2", "X2", "%s anchored %d (L's anchor %d + W %d = %d), certified" %
                    (x2["hash"][:16], x2["block"]["anchorheight"], l_anchor, W, l_anchor + W))
        time.sleep(5)
        outcome = {}
        for i in HONEST:
            n = self.nodes[i]
            outcome[i] = "R mined" if self.block_of(st["r_id"], n) is not None else ("T mined" if self.block_of(st["t_id"], n) is not None else "neither")
        self.record("b2", "honest nodes after the window", outcome)

    def realign(self):
        """Bring the seller (and the late node) back onto the honest chain for the next case."""
        n1 = self.nodes[1]
        for i in (SELLER, LATE):
            a = self.nodes[i]
            h, child = a.getbestblockhash(), None
            while not self.in_chain(n1, h):
                child, h = h, a.getblockheader(h)["previousblockhash"]
            if child:
                a.invalidateblock(child)
            self.wait_until(lambda: a.getbestblockhash() == n1.getbestblockhash(), timeout=60)
        self.sync_tips(range(N))

    def in_chain(self, node, h):
        try:
            return node.getblockheader(h)["confirmations"] > 0
        except JSONRPCException:
            return False

    def case_c(self):
        """Nodes without the lost block."""
        self.log.info("(c) a node offline through the reorganization, and one resyncing from nothing")
        # c1: node 5 offline through the reorganization (it holds L on disk).
        self.stop_node(LATE)
        online = [0, 1, 2, 3, 4]
        st = self.gap("c1", online=online)
        x = self.respend_block("c1", st, ALL_KEYS)
        self.assert_refused("c1", HONEST, x["hash"], st["sizes"])
        # Node 5 restarts and meets the seller first, alone.
        self.start_node(LATE, extra_args=self.node_args(LATE))
        self.connect_nodes(LATE, SELLER)
        time.sleep(4)
        self.record("c1", "node 5 restarted, seller its only peer", "tip %s, X %s, L lost here: %s" %
                    (self.nodes[LATE].getbestblockhash()[:16], self.status_of(self.nodes[LATE], x["hash"]), st["L"] in self.lost(LATE)))
        self.connect_nodes(LATE, 1)
        self.wait_until(lambda: st["L"] in self.lost(LATE), timeout=30)
        self.record("c1", "node 5 with an honest peer", "X %s, L lost: %s (forward-only: an accepted block stays; it rejoins by reconciliation, case b1)" %
                    (self.status_of(self.nodes[LATE], x["hash"]), st["L"] in self.lost(LATE)))
        h = self.produce(1, [1], leaders=[1], txs_must=[st["t_id"]])
        self.nodes[LATE].invalidateblock(x["hash"])  # the harness, not the rule, moves it on
        self.connect_nodes(LATE, 2)
        self.sync_tips(HONEST + [LATE])
        self.record("c1", "outcome", "node 5 on the honest chain with T at height %s" % self.block_of(st["t_id"], self.nodes[LATE]))
        # c2: node 5 wiped and resynced, the seller its only peer (inside the window): it follows X.
        self.stop_node(LATE)
        shutil.rmtree(os.path.join(self.nodes[LATE].datadir, CHAIN))
        self.start_node(LATE, extra_args=self.node_args(LATE))
        self.nodes[LATE].createwallet(wallet_name="w", descriptors=True)
        self.connect_nodes(LATE, SELLER)
        self.wait_until(lambda: self.nodes[LATE].getblockcount() >= self.nodes[SELLER].getblockcount(), timeout=60)
        late_tip = self.nodes[LATE].getbestblockhash()
        on_x = self.in_chain(self.nodes[LATE], x["hash"])
        self.record("c2", "resynced from nothing, the seller its only peer (inside W)", "tip %s, X in its chain: %s, R mined: %s" %
                    (late_tip[:16], on_x, self.block_of(st["r_id"], self.nodes[LATE]) is not None))
        t0 = time.time()
        self.connect_nodes(LATE, 3)
        if on_x:
            self.wait_until(lambda: st["L"] in self.lost(LATE), timeout=60)
            self.nodes[LATE].invalidateblock(x["hash"])  # the harness, not the rule, moves it on (case b1)
        self.wait_until(lambda: self.nodes[LATE].getbestblockhash() == self.nodes[3].getbestblockhash(), timeout=60)
        self.record("c2", "after an honest peer", "on the honest chain in %.1f s; X %s; T at height %s" %
                    (time.time() - t0, self.status_of(self.nodes[LATE], x["hash"]), self.block_of(st["t_id"], self.nodes[LATE])))
        st["x"] = x
        st["h"] = h
        self.c_state = st
        if "c3" in CASES:
            self.case_c3(st)
        self.mesh(range(N))
        self.realign()

    def case_c3(self, st):
        """After the window: node 5 resynced from nothing, from the seller and an honest node."""
        self.log.info("(c3) a node resyncing from nothing after the window")
        a = self.nodes[SELLER]
        # The seller extends X's branch so that it is the longer one.
        for _ in range(3):
            self.produce(SELLER, ALL_KEYS)
        self.btc_mine(2 * W + 2)
        target = self.btc().getblockcount()
        # The honest chain moves on, anchored past the keep window: the evidence expires everywhere.
        self.produce(1, ALL_KEYS)
        self.sync_tips(HONEST + [LATE])
        self.wait_until(lambda: all(not self.nodes[i].getlostblocks()["blocks"] for i in HONEST), timeout=30)
        time.sleep(5)
        self.record("c3", "seller's chain", "height %d, X in it: %s" % (a.getblockcount(), self.in_chain(a, st["x"]["hash"])))
        self.record("c3", "evidence after 2W+2 Bitcoin blocks", {i: len(self.nodes[i].getlostblocks()["blocks"]) for i in HONEST})
        self.record("c3", "honest tips", {i: (self.nodes[i].getblockcount(), self.in_chain(self.nodes[i], st["x"]["hash"])) for i in HONEST})
        self.stop_node(LATE)
        shutil.rmtree(os.path.join(self.nodes[LATE].datadir, CHAIN))
        self.start_node(LATE, extra_args=self.node_args(LATE))
        self.connect_nodes(LATE, SELLER)
        self.connect_nodes(LATE, 2)
        time.sleep(15)
        n = self.nodes[LATE]
        self.record("c3", "resynced after W from the seller and node 2", "tip height %d, X in its chain: %s, same tip as node 2: %s" %
                    (n.getblockcount(), self.in_chain(n, st["x"]["hash"]), n.getbestblockhash() == self.nodes[2].getbestblockhash()))

    def case_f(self):
        """Late reveal: a dishonest quorum certifies S in secret on a Bitcoin block it then makes stale.
        Forward-only: nothing accepted is undone; S binds only candidates for the tip, until the lapse."""
        self.log.info("(f) late reveal by a dishonest quorum")
        a = self.nodes[SELLER]
        coin = self.fund()
        coin2 = self.fund()
        merchant = self.nodes[2].getnewaddress()
        m_hex = self.spend(coin, merchant, Decimal("0.0001"))
        s_hex = self.spend(coin, a.getnewaddress(), Decimal("0.0002"))
        s2_hex = self.spend(coin2, a.getnewaddress(), Decimal("0.0002"))
        q_hex = self.spend(coin2, self.nodes[3].getnewaddress(), Decimal("0.0003"))
        m_id, s_id, s2_id, q_id = [tx_from_hex(h).rehash() for h in (m_hex, s_hex, s2_hex, q_hex)]
        fpoint = self.nodes[1].getbestblockhash()
        # The merchant is paid in a certified block H1, anchored at Q, buried under certified blocks.
        self.nodes[1].sendrawtransaction(m_hex)
        h1 = self.produce(1, ALL_KEYS, txs_must=[m_id])
        q = h1["block"]["anchorheight"]
        self.sync_tips(range(N))
        # The quorum, in secret: Bitcoin block A at Q+1 is mined; S on the fork point anchored at A.
        self.disconnect_all(SELLER)
        a.setnetworkactive(False)
        a_hash = self.btc_mine(1)[0]
        a.invalidateblock(h1["hash"])
        assert_equal(a.getbestblockhash(), fpoint)
        a.sendrawtransaction(s_hex)
        a.sendrawtransaction(s2_hex)
        s = self.produce(SELLER, ALL_KEYS, leaders=ALL_KEYS, txs_must=[s_id, s2_id])
        assert_equal(s["block"]["anchorheight"], q + 1)
        # A goes stale (a natural reorganization, or a block the quorum mined and withheld).
        self.btc().invalidateblock(a_hash)
        self.btc_mine(2)
        self.wait_until(lambda: a.getbestblockhash() != s["hash"], timeout=60)
        self.wait_until(lambda: s["hash"] in [b["hash"] for b in a.getlostblocks()["blocks"]], timeout=30)
        ev = a.getlostblock(s["hash"])
        self.record("f", "S", "%s at height %d anchored %d (stale), certified, spends the merchant's coin and coin2 to the seller; %d Bitcoin header(s) carried" %
                    (s["hash"][:16], s["height"], q + 1, len(ev["bitcoinheaders"])))
        # The honest chain goes on: certified blocks above H1.
        for _ in range(4):
            self.produce(1, ALL_KEYS)
        self.sync_tips(HONEST + [LATE])
        before = self.nodes[2].getblockcount()
        best_before = {i: self.nodes[i].getbestblockhash() for i in HONEST + [LATE]}
        sizes = [self.log_size(i) for i in range(N)]
        self.record("f", "honest chain", "H1 (merchant paid) at height %d, %d certified blocks above it, tip anchor %d" %
                    (h1["height"], before - h1["height"], self.tip(self.nodes[2])["anchorheight"]))
        # The reveal, at one honest node.
        r = self.nodes[1].submitlostblock(ev["block"], ev["bitcoinheaders"])
        self.record("f", "reveal at node 1", repr(r))
        self.wait_until(lambda: all(s["hash"] in self.lost(i) for i in HONEST), timeout=60)
        time.sleep(5)
        for i in HONEST + [LATE]:
            assert self.in_chain(self.nodes[i], h1["hash"]), i
            assert_equal(self.nodes[i].getbestblockhash(), best_before[i])
        self.record("f", "no rollback", "S held as lost on nodes %s; H1 (%d deep) and every block above it stay on every honest node; tips unchanged" %
                    (HONEST, before - h1["height"] + 1))
        assert not any(self.log_has(i, "breaks the rule:", sizes[i]) for i in HONEST)
        # What S can still freeze: coin2, unspent, which S's own transaction spends.
        try:
            self.nodes[1].sendrawtransaction(q_hex)
            raise AssertionError("honest mempool took Q")
        except JSONRPCException as e:
            assert "txn-lost-conflict" in e.error["message"], e.error
            self.record("f", "honest mempool refuses Q (coin2 elsewhere)", e.error["message"])
        a.reconsiderblock(h1["hash"])
        a.setnetworkactive(True)
        self.mesh(range(N))
        self.wait_until(lambda: a.getbestblockhash() == self.nodes[1].getbestblockhash(), timeout=60)
        sizes = [self.log_size(i) for i in range(N)]
        a.sendrawtransaction(q_hex)
        xq = self.produce(SELLER, ALL_KEYS, leaders=[SELLER], txs_must=[q_id])
        self.assert_refused("f", HONEST, xq["hash"], sizes)
        # The lapse: Bitcoin passes anchor(S) + W; the refusal ends, the block is judged again and taken.
        need = q + 1 + W + 1
        if self.btc().getblockcount() < need:
            self.btc_mine(need - self.btc().getblockcount())
        t0 = time.time()
        self.wait_until(lambda: all(self.in_chain(self.nodes[i], xq["hash"]) for i in HONEST), timeout=60)
        self.record("f", "lapse", "Bitcoin at %d > anchor(S)+W = %d: every honest node took the block with Q in %.1f s; S dropped: %s" %
                    (self.btc().getblockcount(), q + 1 + W, time.time() - t0, all(s["hash"] not in self.lost(i) for i in HONEST)))
        self.realign()

    def sub(self, node, *args):
        try:
            return repr(node.submitlostblock(*args))
        except JSONRPCException as e:
            return "RPC error: " + e.error["message"]

    def case_g(self):
        """Evidence that is not."""
        self.log.info("(g) evidence that is not")
        n = self.nodes[1]
        a = self.nodes[SELLER]
        # g1: a certified block on the active chain.
        tip_hex = n.getblock(n.getbestblockhash(), 0)
        self.record("g", "a block of the active chain", self.sub(n, tip_hex))
        # g2: a certified sibling whose anchor is on Bitcoin's best chain.
        self.disconnect_all(SELLER)
        a.setnetworkactive(False)
        top = a.getbestblockhash()
        a.invalidateblock(top)
        sib = self.produce(SELLER, ALL_KEYS)
        sib_hex = a.getblock(sib["hash"], 0)
        self.record("g", "a certified sibling anchored on Bitcoin's best chain", self.sub(n, sib_hex))
        # g3: a block below the quorum on a stale anchor (one signer), parent on the active chain.
        a.reconsiderblock(top)
        a.invalidateblock(sib["hash"])
        self.wait_until(lambda: a.getbestblockhash() == top, timeout=30)
        a_hash = self.btc_mine(1)[0]
        self.wait_until(lambda: self.tip(a)["anchorheight"] <= self.btc().getblockcount())
        one = self.produce(SELLER, [SELLER], leaders=[SELLER])
        self.btc().invalidateblock(a_hash)
        self.btc_mine(2)
        self.wait_until(lambda: one["hash"] in [b["hash"] for b in a.getlostblocks()["blocks"]], timeout=30)
        ev = a.getlostblock(one["hash"])
        self.record("g", "one signer on a stale anchor", self.sub(n, ev["block"], ev["bitcoinheaders"]) +
                    "; lost: %s" % (one["hash"] in self.lost(1)))
        # g4: a mutated block (the certificate's block hash changes).
        bad = ev["block"][:-2] + ("00" if ev["block"][-2:] != "00" else "01")
        self.record("g", "a block with a changed last byte", self.sub(n, bad))
        # g5: an unknown parent: a block of a branch this node never had (its own parent withheld).
        # g6: flood cost: 200 junk blocks.
        t0 = time.time()
        for k in range(200):
            self.sub(n, bad)
        self.record("g", "200 junk items", "%.2f ms each (RPC round trip included)" % ((time.time() - t0) * 1000 / 200))
        # g7: the cost of judging real evidence: S-like blocks are judged once; time one fresh judgement on node 4.
        t0 = time.time()
        r = self.sub(self.nodes[4], ev["block"], ev["bitcoinheaders"])
        self.record("g", "one genuine item judged on node 4", "%s in %.1f ms (RPC round trip included)" % (r, (time.time() - t0) * 1000))
        a.setnetworkactive(True)
        self.mesh(range(N))
        self.realign()

    def case_d(self):
        """Nodes without the rule (LOST_BASE_NODES run the base binary)."""
        self.log.info("(d) a network with nodes that do not run the rule: %s", sorted(self.base_nodes))
        st = self.gap("d", online=list(range(N)))
        rule = [i for i in HONEST + [LATE] if i not in self.base_nodes]
        base = sorted(self.base_nodes)
        x = self.respend_block("d", st, ALL_KEYS)
        self.assert_refused("d", rule, x["hash"], st["sizes"])
        self.wait_until(lambda: all(self.nodes[i].getbestblockhash() == x["hash"] for i in base), timeout=30)
        h = self.produce(1, [1], leaders=[1], txs_must=[st["t_id"]])
        time.sleep(5)
        self.record("d", "after an honest uncertified block with T", {i: ("T" if self.block_of(st["t_id"], self.nodes[i]) is not None else "R" if self.block_of(st["r_id"], self.nodes[i]) is not None else "-") for i in range(1, N)})
        self.record("d", "split", "rule nodes %s refuse X; base nodes %s are on X" % (rule, base))

    def case_e(self):
        """Bitcoin there and back, then to a third branch."""
        self.log.info("(e) Bitcoin there and back, and to a third branch")
        st = self.gap("e")
        b = self.btc()
        orig, new = self.btc_orig_first, self.btc_new
        # Back: the original branch returns, longer.
        b.invalidateblock(new[0])
        b.reconsiderblock(orig)
        self.btc_mine(3)
        self.wait_until(lambda: all(self.in_chain(self.nodes[i], st["L"]) for i in HONEST), timeout=60)
        self.wait_until(lambda: all(st["L"] not in self.lost(i) for i in HONEST), timeout=30)
        self.record("e", "back", "L reconnected on every honest node; no longer lost; T in the chain: %s; spends bound on node 1: %d" %
                    (self.block_of(st["t_id"], self.nodes[1]) is not None, self.nodes[1].getlostblocks()["boundoutpoints"]))
        # A third branch: L lost again.
        b.invalidateblock(orig)
        self.btc_mine(6)
        self.wait_until(lambda: all(not self.in_chain(self.nodes[i], st["L"]) for i in HONEST), timeout=60)
        self.wait_lost(st["L"], HONEST)
        self.record("e", "third branch", "L lost again on every honest node")
        a = self.nodes[SELLER]
        if st["r_id"] not in a.getrawmempool():
            a.sendrawtransaction(st["r_hex"])
        sizes = [self.log_size(i) for i in range(N)]
        x = self.respend_block("e", st, ALL_KEYS)
        self.assert_refused("e", HONEST, x["hash"], sizes)
        h = self.produce(1, [1], leaders=[1], txs_must=[st["t_id"]])
        self.sync_tips(HONEST + [LATE])
        self.record("e", "outcome", "T mined again at height %d" % h["height"])
        self.realign()

    def run_test(self):
        try:
            for c in CASES:
                if c in ("b2", "c3"):
                    continue
                getattr(self, "case_" + c)()
        finally:
            if RESULTS:
                with open(RESULTS, "w") as f:
                    json.dump(self.results, f, indent=1)
            self.stop_bitcoind()


if __name__ == '__main__':
    LostBlocksTest().main()
