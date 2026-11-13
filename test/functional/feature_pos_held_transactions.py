#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The committee keeps certified transactions (doc 04, "Held transactions"), attacked.

A transaction T is certified, Bitcoin then takes its anchor away, and the
seller who signed it tries to have a conflicting spend R certified instead.
A member that ran the anchor watcher through the disconnect holds T: it does
not sign a proposal that carries R or builds on it (arm one), it sets aside a
sub-quorum block that carries R so that it can sign a sibling (arm two), and
its mempool refuses R.

A real bitcoind is the parent chain. Six members, one seat each (quorum 4):
node 0 is the attacker, with the rule off (-posdebugignoreheld) and 3 of the
8 stake units, so that it leads round 0 about 3 times in 8; nodes 1-5 are honest and run
the rule unless a case turns it off ("unaware"). Every node runs -par=1 and
the gossip producer. To give the attacker the first word after the reorg,
every node is taken off the network before Bitcoin reorganizes and put back
once the attacker's mempool holds R; a block the attacker builds alone in the
meantime (the escaping-stall cases) is built with generateposblock.

Each refusal is asserted by the member's own reason (its debug log, or the RPC
error), never by the absence of a block alone.
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
from test_framework.messages import COIN, tx_from_hex
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, p2p_port, rpc_port

BITCOIND = os.environ.get("PARENT_BITCOIND", "/usr/local/bin/bitcoind")
CASES = os.environ.get("CR1_CASES", "a,e,d,c,g,f,b,cc,i,rC,rA,rB,rD,b15,bb,dd,fs,share").split(",")
RESULTS = os.environ.get("CR1_RESULTS", "")
# The testnet's settings (CR1_TESTNET=1): 9.45 s rounds and a 6.75 s collection
# window (the formulas at the committee cap of 250), so a liveness bound of
# 120.15 s, and a 600 s Bitcoin median-time-past gap for a block that escapes a
# stall, with Bitcoin's blocks 600 s apart. Otherwise the harness's: the
# formulas at this committee (7 seats: 945 ms, 675 ms, a 12.0 s bound), no gap.
TESTNET = os.environ.get("CR1_TESTNET", "") == "1"
ROUND_MS = 9450 if TESTNET else 700 + 35 * 7
WINDOW_MS = 6750 if TESTNET else 500 + 25 * 7
BOUND_S = (12 * ROUND_MS + WINDOW_MS) / 1000.0
BACKSTOP_S = 2 * BOUND_S
N = 6
ATTACKER = 0
CHAIN = "elementsregtest"


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class HeldTransactionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = N
        self.stakers = [make_key() for _ in range(N)]
        self.results = {}

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if not os.path.exists(BITCOIND):
            raise SkipTest("needs Bitcoin Core's bitcoind (set PARENT_BITCOIND)")

    # ------------------------------------------------------------- bitcoind
    def btc(self, wallet=None):
        url = "http://%s:%s@127.0.0.1:%d" % (urllib.parse.quote(self.btc_user), urllib.parse.quote(self.btc_pass), self.btc_port)
        return AuthServiceProxy(url + ("/wallet/" + wallet if wallet else ""), timeout=120)

    def btc_mine(self, n):
        # A fresh address each time: a block mined in the same second to the same
        # address as one just invalidated would be that block again.
        self.btc_addr = self.btc("miner").getnewaddress()
        if not TESTNET:
            return self.btc().generatetoaddress(n, self.btc_addr)
        # Bitcoin's blocks 600 s apart, so that its median time past moves as on
        # the live network and the 600 s gap binds as it does there.
        hashes = []
        for _ in range(n):
            self.btc_time = max(getattr(self, "btc_time", int(time.time())), int(time.time())) + 600
            self.btc().setmocktime(self.btc_time)
            hashes += self.btc().generatetoaddress(1, self.btc_addr)
        return hashes

    def stop_bitcoind(self):
        if getattr(self, "btc_proc", None) and self.btc_proc.poll() is None:
            try:
                self.btc().stop()
                self.btc_proc.wait(timeout=60)
            except Exception:
                self.btc_proc.terminate()
                self.btc_proc.wait(timeout=60)

    # ------------------------------------------------------------- network
    def node_args(self, i, aware=True):
        a = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=%d" % (N + 1), "-posslotinterval=1", "-poshardeningheight=0", "-posexpraceheight=1",
            "-con_max_block_sig_size=8000", "-con_blocksubsidy=0", "-initialfreecoins=1000000000000",
            "-con_connect_genesis_outputs=1", "-acceptnonstdtxn=1", "-par=1", "-fallbackfee=0.0001",
            "-blindedaddresses=0", "-con_default_blinded_addresses=0",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-posescapestallmtpgap=%d" % (600 if TESTNET else 0),
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % self.btc_port,
            "-mainchainrpcuser=%s" % self.btc_user, "-mainchainrpcpassword=%s" % self.btc_pass,
            "-parentgenesisblockhash=%s" % self.btc_genesis, "-debug=mempool", "-txindex=1",
            "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
        ] + self.specs + ["-posproducer=1", "-posproducerkey=%s" % self.stakers[i][0]]
        if TESTNET:
            a += ["-posroundms=%d" % ROUND_MS, "-poswindowms=%d" % WINDOW_MS]
        a += getattr(self, "extra_node_args", {}).get(i, [])
        if i == ATTACKER or not aware:
            a.append("-posdebugignoreheld=1")
        if i == ATTACKER:
            # The attacker mines what honest policy refuses (the F4 case).
            a += ["-minrelaytxfee=0", "-blockmintxfee=0"]
        return a

    def setup_network(self, split=False):
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
        self.btc_addr = self.btc("miner").getnewaddress()
        self.btc_genesis = self.btc().getblockhash(0)
        self.btc_mine(5)

        # The stakers' BLS registrations come from a node that knows them.
        # The attacker holds 3 of 8 stake units (it leads round 0 about 3 times in 8) and one seat of six.
        weights = [3 if i == ATTACKER else 1 for i in range(N)]
        self.specs = ["-staker=%s:%d" % (pub, w) for (_, pub), w in zip(self.stakers, weights)]
        self.add_nodes(N, [self.node_args(i) for i in range(N)], chain=[CHAIN] * N)
        self.nodes[0].start(extra_args=[a for a in self.node_args(0) if not a.startswith("-posproducer")])
        self.nodes[0].wait_for_rpc_connection()
        self.specs = ["-staker=%s:%d%s" % (pub, w, self.nodes[0].getblsregistration(wif)["spec"])
                      for (wif, pub), w in zip(self.stakers, weights)]
        self.stop_node(0)
        shutil.rmtree(os.path.join(self.nodes[0].datadir, CHAIN))
        self.aware = [i != ATTACKER for i in range(N)]
        for i in range(N):
            self.nodes[i].extra_args = self.node_args(i)
            self.start_node(i, extra_args=self.node_args(i))
        self.mesh()
        for i in range(N):
            self.nodes[i].createwallet(wallet_name="w", descriptors=True)
        self.bootstrap()

    def bootstrap(self):
        """Split the genesis anyone-can-spend coin into coins of the attacker's (the seller's)."""
        a = self.nodes[ATTACKER]
        g = a.getblock(a.getblockhash(0), 2)
        coin = None
        for tx in g["tx"]:
            for vo in tx["vout"]:
                if vo["scriptPubKey"]["hex"] == "51" and vo.get("value", 0) > 0:
                    coin = (tx["txid"], vo["n"], Decimal(vo["value"]))
        assert coin
        fee = Decimal("0.001")
        outs = [{a.getnewaddress(): Decimal("100")} for _ in range(40)]
        outs.append({a.getnewaddress(): coin[2] - 40 * Decimal("100") - fee})
        outs.append({"fee": fee})
        raw = a.createrawtransaction([{"txid": coin[0], "vout": coin[1]}], outs)
        txid = a.sendrawtransaction(raw)
        self.wait_mined(txid, a)

    def mesh(self):
        for i in range(N):
            for j in range(i + 1, N):
                self.connect_nodes(i, j)

    def isolate(self):
        for n in self.nodes:
            n.setnetworkactive(False)
        self.wait_until(lambda: all(n.getconnectioncount() == 0 for n in self.nodes), timeout=30)

    def rejoin(self):
        for n in self.nodes:
            n.setnetworkactive(True)
        self.mesh()

    def set_aware(self, aware):
        """Restart the honest nodes whose setting changes: True runs the rule, False is unaware."""
        changed = [i for i in range(1, N) if self.aware[i] != aware[i]]
        for i in changed:
            self.stop_node(i)
        for i in changed:
            self.aware[i] = aware[i]
            self.nodes[i].extra_args = self.node_args(i, aware[i])
            self.start_node(i, extra_args=self.nodes[i].extra_args)
            self.nodes[i].loadwallet("w")
        if changed:
            self.mesh()
            self.sync_all_tips()

    # --------------------------------------------------------------- chain
    def tip(self, node):
        return node.getblockheader(node.getbestblockhash())

    def sync_all_tips(self, timeout=120):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=timeout)

    def wait_certified_above(self, height, node=None, timeout=120):
        node = node or self.nodes[1]

        def ok():
            t = node.getblockcount()
            if t <= height:
                return False
            return any(node.getblockheader(node.getblockhash(h))["poscertified"] for h in range(height + 1, t + 1))
        self.wait_until(ok, timeout=timeout)

    def block_of(self, txid, node):
        """Height of the active-chain block that holds txid, or None."""
        t = node.getblockcount()
        for h in range(t, max(0, t - 400), -1):
            b = node.getblock(node.getblockhash(h))
            if txid in b["tx"]:
                return h
        return None

    def wait_mined(self, txid, node, timeout=120):
        self.wait_until(lambda: self.block_of(txid, node) is not None, timeout=timeout)
        return self.block_of(txid, node)

    def log_has(self, i, text, since=0):
        with open(self.nodes[i].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(since)
            return [line.strip() for line in f if text in line]

    def log_size(self, i):
        return os.path.getsize(self.nodes[i].debug_log_path)

    def record(self, case, key, value):
        self.results.setdefault(case, {})[key] = value
        self.log.info("  [%s] %s: %s", case, key, value)

    # ----------------------------------------------------------- the coins
    def fund(self, amount=Decimal("1")):
        """A fresh coin of the attacker's (the seller's), confirmed."""
        a = self.nodes[ATTACKER]
        txid = a.sendtoaddress(address=a.getnewaddress(), amount=amount, fee_asset_label="bitcoin")
        self.wait_mined(txid, a)
        vout = [o for o in a.getrawtransaction(txid, 1)["vout"] if o["value"] == amount][0]["n"]
        return {"txid": txid, "vout": vout, "amount": amount}

    def spend(self, coin, to_addr, fee, locktime=0):
        """Sign (do not broadcast) a spend of coin paying to_addr, RBF-signalling."""
        a = self.nodes[ATTACKER]
        raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"], "sequence": 0xfffffffd}],
                                     [{to_addr: coin["amount"] - fee}, {"fee": fee}], locktime)
        return a.signrawtransactionwithwallet(raw)["hex"]

    # ----------------------------------------------------------- the attack
    def certify(self, txhex):
        """Broadcast and wait until the tx sits in a certified block anchored at Bitcoin's tip, with a
        certified block above it. Returns (txid, height, anchor)."""
        a = self.nodes[ATTACKER]
        txid = a.sendrawtransaction(txhex)
        h = self.wait_mined(txid, self.nodes[1])
        self.wait_certified_above(h)
        self.sync_all_tips()
        hdr = self.nodes[1].getblockheader(self.nodes[1].getblockhash(h))
        return txid, h, hdr["anchorheight"]

    def bitcoin_reorg(self, depth):
        """Replace Bitcoin's top `depth` blocks with depth+1 new ones."""
        b = self.btc()
        tip = b.getblockcount()
        self.btc_orig_first = b.getblockhash(tip - depth + 1)
        b.invalidateblock(self.btc_orig_first)
        self.btc_new_first = self.btc_mine(depth + 1)[0]
        return tip - depth   # the fork height

    def wait_held(self, txids, nodes):
        self.wait_until(lambda: all(set(txids) <= set(self.nodes[i].getheldtxs()["txids"]) for i in nodes), timeout=60)

    def attack(self, case, depth, escape_block=False):
        """The common attack: T certified, Bitcoin reorganized `depth` deep, R ready on the attacker.
        Returns a dict of what happened."""
        a = self.nodes[ATTACKER]
        buyer = self.nodes[1].getnewaddress()
        coin = self.fund()
        t_hex = self.spend(coin, buyer, Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        # Bitcoin's tip moves first so T's block anchors above the blocks we keep.
        self.btc_mine(depth)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        t_id, t_h, t_anchor = self.certify(t_hex)
        r_id = tx_from_hex(r_hex).rehash()
        self.record(case, "T", "%s at height %d, anchor %d, certified" % (t_id, t_h, t_anchor))
        aware = [i for i in range(1, N) if self.aware[i]]
        sizes = [self.log_size(i) for i in range(N)]

        self.isolate()
        t0 = time.time()
        fork = self.bitcoin_reorg(depth)
        btc_tip = self.btc().getblockcount()
        # Every node follows Bitcoin, the rule or not.
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork for n in self.nodes), timeout=60)
        follow = time.time() - t0
        self.record(case, "reorg", "Bitcoin %d deep (fork %d, new tip %d); every node disconnected T's block in %.1fs" %
                    (depth, fork, btc_tip, follow))
        self.wait_held([t_id], aware)
        self.record(case, "held", "T held on aware nodes %s" % aware)
        # R in the attacker's mempool (it replaces T there: the attacker runs no rule).
        a.sendrawtransaction(r_hex)
        assert r_id in a.getrawmempool()
        # An unaware member's mempool takes R as any honest mempool does (higher fee, RBF).
        for i in range(1, N):
            if not self.aware[i]:
                self.nodes[i].sendrawtransaction(r_hex)
                assert r_id in self.nodes[i].getrawmempool()
        # The mempool of an aware member refuses R, with the reason.
        if aware:
            try:
                self.nodes[aware[0]].sendrawtransaction(r_hex)
                raise AssertionError("aware mempool took R")
            except JSONRPCException as e:
                assert "txn-held-conflict" in e.error["message"], e.error
                self.record(case, "mempool refusal", e.error["message"])
        x_hash = None
        if escape_block:
            # The attacker certifies alone: a single-signer block escaping the stall, carrying R.
            x = self.escape_block(a)
            x_hash = x["hash"]
            xb = a.getblock(x_hash)
            assert r_id in xb["tx"]
            self.record(case, "X", "%s at height %d, %d member(s), anchor %d, carries R" %
                        (x_hash, xb["height"], x["countersignatures"], xb["anchorheight"]))
        self.rejoin()
        return dict(t_id=t_id, r_id=r_id, t_h=t_h, fork=fork, x_hash=x_hash, sizes=sizes, aware=aware)

    def outcome(self, case, st, timeout=180):
        """Wait for T or R to be mined under a certified block on every node; report which."""
        def settled():
            n = self.nodes[1]
            for txid in (st["t_id"], st["r_id"]):
                h = self.block_of(txid, n)
                if h is not None and any(n.getblockheader(n.getblockhash(k))["poscertified"]
                                         for k in range(h, n.getblockcount() + 1)):
                    return True
            return False
        self.wait_until(settled, timeout=timeout)
        self.sync_all_tips()
        n = self.nodes[1]
        th, rh = self.block_of(st["t_id"], n), self.block_of(st["r_id"], n)
        won = "T" if th is not None else "R"
        self.record(case, "outcome", "%s mined at height %s on every node" % (won, th if th is not None else rh))
        refusals = []
        for i in st["aware"]:
            refusals += self.log_has(i, "not signing", st["sizes"][i])
        if refusals:
            self.record(case, "member refusals", len(refusals))
            self.record(case, "refusal example", refusals[0].split(" ", 1)[1])
        return won

    # ---------------------------------------------------------------- cases
    def case_a(self):
        self.log.info("(a) one producer, Bitcoin 1 deep: the members refuse R, T is mined again")
        # The attacker's proposal is weighed only when a round backs it, which the draw and the
        # share-lock's grace decide; repeat until it is, and count the tries.
        for k in range(8):
            case = "a" if k == 0 else "a.%d" % (k + 1)
            st = self.attack(case, 1)
            assert_equal(self.outcome(case, st), "T")
            if self.results[case].get("member refusals", 0) > 0:
                self.record("a", "tries until the attacker's proposal was weighed", k + 1)
                return st
        raise AssertionError("no round backed the attacker's proposal in 8 tries")

    def case_b(self):
        self.log.info("(b) members running the rule: 2, 4 and 3 of 6 (quorum 4)")
        for n_aware in (2, 4, 3):
            self.set_aware([False] + [i <= n_aware for i in range(1, N)])
            # With fewer than a quorum aware, R needs a round that backs an R-carrying proposal,
            # which depends on the draw; try several reorganizations and count.
            attempts = 5 if n_aware == 2 else (2 if n_aware == 4 else 3)
            wins = []
            for k in range(attempts):
                case = "b%d.%d" % (n_aware, k + 1)
                st = self.attack(case, 1)
                try:
                    won = self.outcome(case, st, timeout=240)
                except AssertionError:
                    won = "neither"
                    self.record(case, "outcome", "neither T nor R certified within 240s")
                if won == "R":
                    rel = []
                    for i in st["aware"]:
                        rel += self.log_has(i, "is respent by", st["sizes"][i])
                    self.record(case, "aware members released T", len(rel) > 0)
                wins.append(won)
                if n_aware == 2 and won == "R":
                    break
            self.record("b%d" % n_aware, "outcomes", wins)
            if n_aware == 2:
                assert "R" in wins, "R never certified with 2 aware members: %s" % wins
            if n_aware == 4:
                assert "R" not in wins, wins
        self.set_aware([False] + [True] * (N - 1))

    def case_c(self):
        for depth in (2, 3, 10):
            case = "c%d" % depth
            self.log.info("(%s) Bitcoin %d deep, the respend in a single-signer escaping-stall block", case, depth)
            st = self.attack(case, depth, escape_block=True)
            won = self.outcome(case, st)
            parked = []
            for i in st["aware"]:
                parked += self.log_has(i, "setting aside sub-quorum block", st["sizes"][i])
            self.record(case, "set aside by", len(parked))
            assert_equal(won, "T")
            for n in self.nodes:
                assert st["x_hash"] != n.getblockhash(n.getblockheader(st["x_hash"])["height"])

    def case_c_control(self):
        self.log.info("(c-control) the same at 2 deep with every member unaware: R is certified")
        self.set_aware([False] * N)
        st = self.attack("c-control", 2, escape_block=True)
        assert_equal(self.outcome("c-control", st), "R")
        self.set_aware([False] + [True] * (N - 1))

    def case_d(self):
        self.log.info("(d) a member restarted between the disconnect and the vote, and a node synced afresh")
        a = self.nodes[ATTACKER]
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        self.btc_mine(1)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        t_id, t_h, _ = self.certify(t_hex)
        sizes = [self.log_size(i) for i in range(N)]
        self.isolate()
        self.bitcoin_reorg(1)
        self.wait_held([t_id], [1, 2, 3, 4, 5])
        for i in (1, 2):    # two members restart, which with the attacker leaves only 3 that never stopped
            self.restart_node(i, extra_args=self.nodes[i].extra_args)
            self.nodes[i].setnetworkactive(False)
            self.nodes[i].loadwallet("w")
        assert t_id in self.nodes[1].getheldtxs()["txids"]
        self.record("d", "after restart", "T still held on nodes 1 and 2 (read back from heldtxs.dat)")
        try:
            self.nodes[1].sendrawtransaction(r_hex)
            raise AssertionError("restarted member took R")
        except JSONRPCException as e:
            assert "txn-held-conflict" in e.error["message"]
            self.record("d", "restarted mempool refusal", e.error["message"])
        a.sendrawtransaction(r_hex)
        self.rejoin()
        st = dict(t_id=t_id, r_id=tx_from_hex(r_hex).rehash(), sizes=sizes, aware=[1, 2, 3, 4, 5])
        assert_equal(self.outcome("d", st), "T")

    def case_e(self, st):
        self.log.info("(e) Bitcoin returns to the original branch")
        b = self.btc()
        # The branch T's first certified block anchored to comes back, and grows longer.
        b.invalidateblock(self.btc_new_first)
        b.reconsiderblock(self.btc_orig_first)
        self.btc_mine(2)
        t0 = time.time()
        self.wait_until(lambda: all(st["t_h"] <= n.getblockcount() and
                                    n.getblockhash(st["t_h"]) and st["t_id"] in n.getblock(n.getblockhash(st["t_h"]))["tx"]
                                    for n in self.nodes), timeout=120)
        self.record("e", "original branch", "T's original block is back at height %d on every node in %.1fs" %
                    (st["t_h"], time.time() - t0))
        self.wait_until(lambda: st["t_id"] not in self.nodes[1].getheldtxs()["txids"], timeout=60)
        self.record("e", "held", "T released on return: %s" % self.nodes[1].getheldtxs())

    def case_f(self):
        self.log.info("(f) a held transaction not valid yet on the new chain (lock time), valid again later")
        a = self.nodes[ATTACKER]
        coin = self.fund()
        self.btc_mine(1)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        # let blocks pile up on this anchor so the disconnect drops the chain well below the lock time
        h_now = self.nodes[1].getblockcount()
        self.wait_until(lambda: self.nodes[1].getblockcount() >= h_now + 15, timeout=120)
        lock = self.nodes[1].getblockcount()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"), locktime=lock)
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        t_id, t_h, _ = self.certify(t_hex)
        self.isolate()
        fork = self.bitcoin_reorg(1)
        self.wait_held([t_id], [1, 2, 3, 4, 5])
        new_h = self.nodes[1].getblockcount()
        self.record("f", "lock time", "T locked to height %d, chain back at %d after the disconnect" % (lock, new_h))
        try:
            self.nodes[1].sendrawtransaction(t_hex)
        except JSONRPCException as e:
            self.record("f", "T on the new chain now", e.error["message"])
        try:
            self.nodes[1].sendrawtransaction(r_hex)
            raise AssertionError("aware member took R")
        except JSONRPCException as e:
            assert "txn-held-conflict" in e.error["message"]
            self.record("f", "R meanwhile", e.error["message"])
        a.sendrawtransaction(r_hex)
        self.rejoin()
        st = dict(t_id=t_id, r_id=tx_from_hex(r_hex).rehash(), sizes=[self.log_size(i) for i in range(N)], aware=[1, 2, 3, 4, 5])
        assert_equal(self.outcome("f", st, timeout=300), "T")

    def case_g(self):
        self.log.info("(g) a chain of held transactions; a conflict already mined in a sub-quorum block")
        a = self.nodes[ATTACKER]
        coin = self.fund()
        buyer = self.nodes[1]
        t1_hex = self.spend(coin, a.getnewaddress(), Decimal("0.0001"))
        t1 = tx_from_hex(t1_hex)
        t1_id = t1.rehash()
        out = [o for o in a.decoderawtransaction(t1_hex)["vout"] if o["scriptPubKey"].get("address")][0]
        c2 = {"txid": t1_id, "vout": out["n"], "amount": out["value"]}
        t2_hex = None
        self.btc_mine(2)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        a.sendrawtransaction(t1_hex)
        t2_hex = self.spend(c2, buyer.getnewaddress(), Decimal("0.0001"))
        r2_hex = self.spend(c2, a.getnewaddress(), Decimal("0.001"))   # respends T2's input (T1's output)
        t2_id, t2_h, _ = self.certify(t2_hex)
        self.isolate()
        self.bitcoin_reorg(2)
        self.wait_held([t1_id, t2_id], [1, 2, 3, 4, 5])
        self.record("g", "held chain", "T1 %s and its child T2 %s held" % (t1_id, t2_id))
        a.sendrawtransaction(r2_hex)
        x = self.escape_block(a)
        self.record("g", "X", "%s carries R2, a respend of T1's output" % x["hash"])
        self.rejoin()
        st = dict(t_id=t2_id, r_id=tx_from_hex(r2_hex).rehash(), sizes=[self.log_size(i) for i in range(N)], aware=[1, 2, 3, 4, 5])
        assert_equal(self.outcome("g", st), "T")

    def case_i(self):
        self.log.info("(i) the cost of filling the record after a deep disconnect of full blocks")
        a = self.nodes[ATTACKER]
        self.btc_mine(1)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        n_tx = int(os.environ.get("CR1_FILL_TXS", "1500"))
        coins = []
        for _ in range((n_tx + 399) // 400 + 1):
            addrs = {a.getnewaddress(): Decimal("0.01") for _ in range(400)}
            txid = a.sendmany(dummy="", amounts=addrs, fee_asset="bitcoin")
            for o in a.getrawtransaction(txid, 1)["vout"]:
                if o["value"] == Decimal("0.01"):
                    coins.append({"txid": txid, "vout": o["n"], "amount": Decimal("0.01")})
        self.wait_until(lambda: a.getmempoolinfo()["size"] == 0, timeout=300)
        self.wait_certified_above(self.nodes[1].getblockcount() - 1)
        coins = [c for c in coins if a.gettxout(c["txid"], c["vout"])]
        n_tx = min(n_tx, len(coins))
        sizes = [self.log_size(i) for i in range(N)]
        h_before = self.nodes[1].getblockcount()
        dest = a.getnewaddress()
        for c in coins[:n_tx]:
            a.sendrawtransaction(self.spend(c, dest, Decimal("0.00001")))
        self.wait_until(lambda: a.getmempoolinfo()["size"] == 0, timeout=600)
        self.wait_certified_above(self.nodes[1].getblockcount() - 1)
        n1 = self.nodes[1]
        sizes_vb = [n1.getblock(n1.getblockhash(h))["size"] for h in range(h_before + 1, n1.getblockcount() + 1)]
        self.record("i", "blocks", "%d spends in blocks of %s bytes" % (n_tx, sizes_vb))
        self.isolate()
        t0 = time.time()
        self.bitcoin_reorg(1)
        self.wait_until(lambda: len(self.nodes[1].getheldtxs()["txids"]) >= n_tx, timeout=300)
        lines = self.log_has(1, "Held transactions: holding", sizes[1])
        self.record("i", "fill", lines[-1].split(" ", 1)[1] if lines else "no fill line")
        dis = self.log_has(1, "Parent chain reorganization detected", sizes[1])
        self.record("i", "disconnect lines", len(dis))
        self.rejoin()
        self.wait_until(lambda: len(self.nodes[1].getheldtxs()["txids"]) == 0, timeout=600)
        self.record("i", "released", "every held transaction mined again and released in %.1fs" % (time.time() - t0))

    # ------------------------------------------------------- review RV56 cases
    def prepare_escape(self, case, depth=2):
        """T certified; everyone isolated; Bitcoin reorganized `depth` deep; R in the attacker's mempool
        (and the unaware members'); X = a 1-signer escaping-stall block carrying R, built by the attacker alone."""
        a = self.nodes[ATTACKER]
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        self.btc_mine(depth)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        t_id, t_h, _ = self.certify(t_hex)
        sizes = [self.log_size(i) for i in range(N)]
        self.isolate()
        fork = self.bitcoin_reorg(depth)
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork for n in self.nodes), timeout=60)
        aware = [i for i in range(1, N) if self.aware[i]]
        self.wait_held([t_id], aware)
        a.sendrawtransaction(r_hex)
        for i in range(1, N):
            if not self.aware[i]:
                self.nodes[i].sendrawtransaction(r_hex)
        x = self.escape_block(a)
        r_id = tx_from_hex(r_hex).rehash()
        xb = a.getblock(x["hash"])
        assert r_id in xb["tx"]
        self.record(case, "X", "%s at height %d, %d member(s), carries R" % (x["hash"], xb["height"], x["countersignatures"]))
        return dict(t_id=t_id, r_id=r_id, x_hash=x["hash"], x_h=xb["height"], x_hex=a.getblock(x["hash"], 0),
                    sizes=sizes, aware=aware)

    def on_certified_x(self, node, st):
        try:
            if node.getblockcount() < st["x_h"] or node.getblockhash(st["x_h"]) != st["x_hash"]:
                return False
        except JSONRPCException:
            return False
        return any(node.getblockheader(node.getblockhash(h))["poscertified"]
                   for h in range(st["x_h"] + 1, node.getblockcount() + 1))

    def tip_status(self, node, h):
        for t in node.getchaintips():
            if t["hash"] == h:
                return t["status"]
        return "not a tip"

    def connect_set(self, idx):
        for i in idx:
            self.nodes[i].setnetworkactive(True)
        for k, i in enumerate(idx):
            for j in idx[k + 1:]:
                self.connect_nodes(i, j)

    def case_rA(self):
        """RV56 (A), F1 and F2: aware {1,2}; the unaware quorum {0,3,4,5} certifies on the set-aside block.
        Node 2 restarts without the rule, node 1 with it. Both must follow the certified chain on X,
        and nobody may be punished for relaying it."""
        self.log.info("(rA) a set-aside block that an unaware quorum builds on; restarts with and without the rule")
        self.set_aware([False, True, True, False, False, False])
        st = self.prepare_escape("rA")
        for i in (1, 2):
            self.nodes[i].submitblock(st["x_hex"])
        self.wait_until(lambda: all(st["x_hash"] in self.nodes[i].getheldtxs()["parked"] for i in (1, 2)), timeout=60)
        self.record("rA", "set aside", "X kept out of fork choice on nodes 1 and 2")
        # Node 2 restarts without the rule; node 1 restarts with it.
        self.stop_node(2)
        self.aware[2] = False
        self.nodes[2].extra_args = self.node_args(2, aware=False)
        self.start_node(2, extra_args=self.nodes[2].extra_args)
        self.nodes[2].setnetworkactive(False)
        self.nodes[2].loadwallet("w")
        self.restart_node(1, extra_args=self.nodes[1].extra_args)
        self.nodes[1].setnetworkactive(False)
        self.nodes[1].loadwallet("w")
        s2 = self.tip_status(self.nodes[2], st["x_hash"])
        self.record("rA", "node 2 after a restart without the rule", "X status '%s', tip %s X" %
                    (s2, "is" if self.nodes[2].getbestblockhash() == st["x_hash"] else "is not"))
        assert s2 != "invalid", "X stayed invalid across a restart without the rule"
        info1 = self.nodes[1].getheldtxs()
        # Only the held transactions are on disk; a set-aside is memory only, so X is parked here
        # only if the restarted producer set it aside again from the record.
        self.record("rA", "node 1 after a restart with the rule", "T held (read from the record): %s; X set aside again since the restart: %s" %
                    (st["t_id"] in info1["txids"], st["x_hash"] in info1["parked"]))
        assert st["t_id"] in info1["txids"]
        # The unaware quorum builds on X and certifies.
        self.connect_set([0, 3, 4, 5])
        self.wait_until(lambda: self.on_certified_x(self.nodes[3], st), timeout=120)
        sz = [self.log_size(i) for i in range(N)]
        t0 = time.time()
        self.rejoin()
        self.wait_until(lambda: self.on_certified_x(self.nodes[2], st), timeout=120)
        self.record("rA", "node 2 (rule off after the set-aside)", "follows the certified chain on X %.1fs after rejoining" % (time.time() - t0))
        self.wait_until(lambda: self.on_certified_x(self.nodes[1], st), timeout=120)
        self.record("rA", "node 1 (rule on)", "follows the certified chain on X %.1fs after rejoining" % (time.time() - t0))
        for i in (1, 2):
            bad = self.log_has(i, "bad-prevblk", sz[i]) + self.log_has(i, "Misbehaving", sz[i])
            self.record("rA", "node %d punishments" % i, len(bad))
            assert not bad, bad[0]
        self.set_aware([False] + [True] * (N - 1))

    def case_rB(self):
        """RV56 (B), F3: 3 aware {1,2,3}, 3 unaware {0,4,5}, quorum 4, X a 1-signer escaping-stall block with R.
        Certification must resume within the liveness bound."""
        self.log.info("(rB) mixed 3/3 with an escaping-stall respend: the chain resumes within the bound")
        bound = BOUND_S
        self.set_aware([False, True, True, True, False, False])
        st = self.prepare_escape("rB")
        t0 = time.time()
        self.rejoin()
        n4 = self.nodes[4]

        def certified_above():
            t = n4.getblockcount()
            return any(n4.getblockheader(n4.getblockhash(h))["poscertified"] for h in range(st["x_h"], t + 1))
        self.wait_until(certified_above, timeout=180)
        waited = time.time() - t0
        self.sync_all_tips()
        won = "T" if self.block_of(st["t_id"], n4) is not None else ("R" if self.block_of(st["r_id"], n4) is not None else "neither")
        rel = []
        for i in (1, 2, 3):
            rel += self.log_has(i, "the liveness bound", st["sizes"][i])
        self.record("rB", "first certified block", "%.1fs after rejoining (bound %.1fs); %s mined; bound releases logged: %d" %
                    (waited, bound, won, len(rel)))
        assert waited < bound + 30, "certification paused %.0fs" % waited
        self.set_aware([False] + [True] * (N - 1))

    def case_rC(self):
        """RV56 (C) / RV57 (C), F4 and N1: T0 pays a 1-atom fee, which no honest mempool takes. Held anyway,
        whatever its fee: the respend is refused and the next block an aware member leads mines T0 from
        the record, so the seller's fee buys no release and the coin is not frozen."""
        self.log.info("(rC) a held transaction honest mempools refuse (a 1-atom fee) is mined from the record")
        a = self.nodes[ATTACKER]
        coin = self.fund()
        t0_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.00000001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        self.record("rC", "honest mempool on T0", self.nodes[1].testmempoolaccept([t0_hex])[0].get("reject-reason"))
        self.btc_mine(1)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        t0_id, t0_h, _ = self.certify(t0_hex)
        self.record("rC", "T0", "1-atom fee %s certified at height %d" % (t0_id, t0_h))
        sizes = [self.log_size(i) for i in range(N)]
        self.isolate()
        fork = self.bitcoin_reorg(1)
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork for n in self.nodes), timeout=60)
        self.wait_held([t0_id], [1, 2, 3, 4, 5])
        time.sleep(5)
        info = self.nodes[1].getheldtxs()
        assert t0_id in info["txids"], "T0 released: %s" % self.log_has(1, "%s released" % t0_id, sizes[1])
        # Held whatever the mempool makes of it: the reorganization's own readmission may keep T0 in
        # node 1's mempool although a fresh broadcast is refused (above). Neither releases it.
        self.record("rC", "T0 after 5 s", "still held on every aware member; in node 1's mempool (readmitted by the reorganization): %s" %
                    (t0_id in self.nodes[1].getrawmempool()))
        try:
            self.nodes[1].sendrawtransaction(r_hex)
            raise AssertionError("aware member took the respend")
        except JSONRPCException as e:
            assert "txn-held-conflict" in e.error["message"], e.error
            self.record("rC", "respend on an aware member", e.error["message"])
        # The attacker offers R (its mempool drops T0 for it).
        a.prioritisetransaction(txid=t0_id, fee_delta=-100000000)
        a.sendrawtransaction(r_hex)
        t_join = time.time()
        h_join = self.nodes[1].getblockcount()
        self.rejoin()
        st = dict(t_id=t0_id, r_id=tx_from_hex(r_hex).rehash(), sizes=sizes, aware=[1, 2, 3, 4, 5])
        won = self.outcome("rC", st)
        assert_equal(won, "T")
        n1 = self.nodes[1]
        h = self.block_of(t0_id, n1)
        self.record("rC", "T0 mined again", "at height %d, %d block(s) after the rejoin, %.1fs" % (h, h - h_join, time.time() - t_join))
        mined = []
        for i in range(1, N):
            mined += self.log_has(i, "mined from the record", sizes[i])
        self.record("rC", "record lines", len(mined))
        if mined:
            self.record("rC", "record line", mined[0].split(" ", 1)[1])
        self.wait_until(lambda: t0_id not in n1.getheldtxs()["txids"], timeout=60)

    def case_rD(self):
        """RV57 (D), N2: the attacker's escaping-stall block X (Bitcoin-consistent) is set aside on member 1,
        whose own sibling S then loses its anchor. Member 1 is isolated, so it never proposes or signs; the
        backstop, counted from the set-aside, must return it to X's branch. Node 5 (rule on, no producer)
        follows X at once."""
        self.log.info("(rD) the backstop: a set-aside ends within %.1fs of itself, whatever the member does", BACKSTOP_S)
        a = self.nodes[ATTACKER]
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        self.btc_mine(2)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        t_id, t_h, _ = self.certify(t_hex)
        sizes = [self.log_size(i) for i in range(N)]
        self.isolate()
        fork = self.bitcoin_reorg(2)
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork for n in self.nodes), timeout=60)
        self.wait_held([t_id], [1, 5])
        p_hash = self.nodes[1].getbestblockhash()
        self.stop_node(5)
        args5 = [x for x in self.node_args(5, aware=True) if not x.startswith("-posproducer")]
        self.nodes[5].extra_args = args5
        self.start_node(5, extra_args=args5)
        self.nodes[5].setnetworkactive(False)
        a.sendrawtransaction(r_hex)
        x = self.escape_block(a)
        x_hash, x_hex = x["hash"], a.getblock(x["hash"], 0)
        x_anchor = a.getblockheader(x_hash)["anchorheight"]
        self.btc_mine(1)
        b1 = self.btc().getblockcount()
        self.nodes[1].submitblock(x_hex)
        self.wait_until(lambda: x_hash in self.nodes[1].getheldtxs()["parked"], timeout=30)
        t_aside = time.time()
        if self.nodes[1].getbestblockhash() in (p_hash, x_hash):
            self.escape_block(self.nodes[1], 1)
        s_hash = self.nodes[1].getbestblockhash()
        s_hdr = self.nodes[1].getblockheader(s_hash)
        self.record("rD", "setup", "P %s; X %s anchor %d, set aside on node 1; node 1's sibling S %s anchor %d, %.1fs after the set-aside" %
                    (p_hash[:16], x_hash[:16], x_anchor, s_hash[:16], s_hdr["anchorheight"], time.time() - t_aside))
        assert s_hdr["anchorheight"] == b1 > x_anchor
        self.nodes[5].submitblock(x_hex)
        self.nodes[5].submitblock(self.nodes[1].getblock(s_hash, 0))
        # Bitcoin takes S's anchor away and keeps X's.
        b = self.btc()
        b.invalidateblock(b.getblockhash(b1))
        self.btc_mine(2)
        t_re = time.time()
        samples = {1: [], 5: []}
        follow = {1: None, 5: None}
        while time.time() - t_re < BACKSTOP_S + 60 and (follow[1] is None or follow[5] is None):
            for i in (1, 5):
                n = self.nodes[i]
                tip = n.getbestblockhash()
                hdr = n.getblockheader(tip)
                try:
                    ok = self.btc().getblockheader(hdr["anchorhash"])["confirmations"] > 0
                except Exception:
                    ok = False
                on_x = hdr["height"] >= x["height"] and n.getblockhash(x["height"]) == x_hash
                tag = "X-branch" if on_x else ("P" if tip == p_hash else ("S" if tip == s_hash else "other"))
                if not samples[i] or samples[i][-1][1:] != (tag, ok):
                    samples[i].append((round(time.time() - t_re, 1), tag, ok))
                if on_x and follow[i] is None:
                    follow[i] = time.time()
            time.sleep(0.25)
        for i in (1, 5):
            self.record("rD", "node %d tips after the Bitcoin reorg (s, tip, anchor canonical)" % i, samples[i])
        assert follow[1] is not None, "node 1 never returned to X's branch"
        since_aside = follow[1] - t_aside
        self.record("rD", "node 1 on X's branch", "%.1fs after the set-aside (backstop %.1fs), %.1fs after the Bitcoin reorg" %
                    (since_aside, BACKSTOP_S, follow[1] - t_re))
        self.record("rD", "node 5 on X's branch", "%.1fs after the Bitcoin reorg" % (follow[5] - t_re))
        lines = self.log_has(1, "the backstop", sizes[1])
        self.record("rD", "node 1 backstop line", lines[0].split(" ", 1)[1] if lines else "none")
        assert since_aside < BACKSTOP_S + 3, "node 1 was kept off X's branch %.1fs" % since_aside
        # No sample of either node on a block whose anchor Bitcoin replaced, beyond the watcher's tick.
        for i in (1, 5):
            stale = [smp for smp in samples[i] if not smp[2]]
            self.record("rD", "node %d stale-anchor samples" % i, stale)
        self.stop_node(5)
        self.nodes[5].extra_args = self.node_args(5, aware=True)
        self.start_node(5, extra_args=self.nodes[5].extra_args)
        self.nodes[5].loadwallet("w")
        self.rejoin()
        self.sync_all_tips(timeout=BACKSTOP_S + 120)

    def escape_block(self, node, signer=ATTACKER):
        """A single-signer escaping-stall block on `node`'s tip. At the testnet's settings Bitcoin must first
        move its median time past 600 s beyond the parent's anchor: mine until the block is allowed."""
        for _ in range(12):
            try:
                return node.generateposblock(self.stakers[signer][0], [])
            except JSONRPCException as e:
                if not TESTNET:
                    raise
                self.log.info("  escaping-stall block not allowed yet (%s); one more Bitcoin block", e.error["message"][:120])
                self.btc_mine(1)
                target = self.btc().getblockcount()
                self.wait_until(lambda: node.getblockheader(node.getbestblockhash())["anchorheight"] <= target, timeout=30)
                time.sleep(2)
        raise AssertionError("no escaping-stall block allowed")

    def case_b15(self):
        self.log.info("(b15) members running the rule: 1 and 5 of 6 (quorum 4)")
        for n_aware, attempts in ((1, 4), (5, 3)):
            self.set_aware([False] + [i <= n_aware for i in range(1, N)])
            wins = []
            for k in range(attempts):
                case = "b%d.%d" % (n_aware, k + 1)
                st = self.attack(case, 1)
                try:
                    won = self.outcome(case, st, timeout=240)
                except AssertionError:
                    won = "neither"
                    self.record(case, "outcome", "neither T nor R certified within 240s")
                wins.append(won)
                if n_aware == 1 and won == "R":
                    break
            self.record("b%d" % n_aware, "outcomes", wins)
            if n_aware == 5:
                assert "R" not in wins, wins
        self.set_aware([False] + [True] * (N - 1))

    def case_bb(self):
        """Reorganizations back to back: T certified; Bitcoin reorganizes 1 deep; while T is held, before any
        vote, Bitcoin reorganizes again, 2 deep from its new tip, taking the blocks built since away too.
        The record must keep T across both, and T must be mined, not R."""
        self.log.info("(bb) two Bitcoin reorganizations back to back")
        a = self.nodes[ATTACKER]
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        self.btc_mine(1)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        t_id, t_h, _ = self.certify(t_hex)
        sizes = [self.log_size(i) for i in range(N)]
        self.isolate()
        fork1 = self.bitcoin_reorg(1)
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork1 for n in self.nodes), timeout=60)
        self.wait_held([t_id], [1, 2, 3, 4, 5])
        # The attacker certifies nothing alone at 1 deep; it does put R first.
        a.sendrawtransaction(r_hex)
        fork2 = self.bitcoin_reorg(2)
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork2 for n in self.nodes), timeout=60)
        held = [t_id in self.nodes[i].getheldtxs()["txids"] for i in range(1, N)]
        self.record("bb", "after the second reorganization", "fork %d then %d; T held on members 1-5: %s" % (fork1, fork2, held))
        assert all(held)
        x = self.escape_block(a)
        self.record("bb", "X", "%s carries R (single signer)" % x["hash"])
        self.rejoin()
        st = dict(t_id=t_id, r_id=tx_from_hex(r_hex).rehash(), sizes=sizes, aware=[1, 2, 3, 4, 5])
        assert_equal(self.outcome("bb", st), "T")
        # Phase 2: T, mined again and certified above, is disconnected again at once by a further
        # Bitcoin reorganization that takes the new anchor away. The second record holds it afresh.
        n1 = self.nodes[1]
        h2 = self.block_of(t_id, n1)
        self.wait_certified_above(h2)
        self.wait_until(lambda: self.tip(n1)["anchorheight"] == self.btc().getblockcount(), timeout=60)
        self.btc_mine(1)
        self.wait_until(lambda: n1.getblockheader(n1.getblockhash(h2))["anchorheight"] >= 0 and
                        self.tip(n1)["anchorheight"] == self.btc().getblockcount(), timeout=60)
        a_t = n1.getblockheader(n1.getblockhash(h2))["anchorheight"]
        sizes = [self.log_size(i) for i in range(N)]
        self.isolate()
        depth = self.btc().getblockcount() - a_t + 1
        fork3 = self.bitcoin_reorg(depth)
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork3 for n in self.nodes), timeout=60)
        self.wait_held([t_id], [1, 2, 3, 4, 5])
        self.record("bb", "phase 2", "T mined again at height %d (anchor %d) and certified; Bitcoin reorganized %d deep; T held again on members 1-5" %
                    (h2, a_t, depth))
        try:
            a.sendrawtransaction(r_hex)
        except JSONRPCException as e:
            self.record("bb", "attacker's mempool on R", e.error["message"])
        if depth >= 2:
            x2 = self.escape_block(a)
            self.record("bb", "X2", "%s carries R: %s" % (x2["hash"], st["r_id"] in a.getblock(x2["hash"])["tx"]))
        self.rejoin()
        st["sizes"] = sizes
        assert_equal(self.outcome("bb2", st), "T")

    def cert_heights(self, node, lo):
        t = node.getblockcount()
        return [h for h in range(lo + 1, t + 1) if node.getblockheader(node.getblockhash(h))["poscertified"]]

    def case_dd(self):
        """RV59 (S1): a hold whose clocks started before a second, deeper Bitcoin reorganization.
        T (lock time at the tip, as anti-fee-sniping sets it) is certified, then T1 one Bitcoin block
        later. Bitcoin goes 1 deep: T1 is held and the members decline the attacker's R1, so their hold
        begins, marked at the highest certified block. A short partition starts their bound. Then Bitcoin
        goes deeper, taking T's block too, which lowers the chain below the mark; T is held and not minable
        until the new branch passes its lock time. Certified blocks of the new branch must end the hold, so
        no bound or backstop fires while they keep coming, R stays refused, and T is mined at its lock time."""
        self.log.info("(dd) a hold begun before a second, deeper Bitcoin reorganization")
        k_blocks = int(os.environ.get("CR1_DD_K", "20" if TESTNET else "40"))
        iso_wait = float(os.environ.get("CR1_DD_ISO_WAIT", "3"))
        a = self.nodes[ATTACKER]
        n1 = self.nodes[1]
        aware = [i for i in range(1, N) if self.aware[i]]
        for k in range(4):
            case = "dd" if k == 0 else "dd.%d" % (k + 1)
            buyer = n1.getnewaddress()
            # Each coin is locked in the attacker's wallet once funded, so that the next
            # funding cannot spend it before T and T1 (signed later) do.
            coin_t = self.fund()
            a.lockunspent(False, [{"txid": coin_t["txid"], "vout": coin_t["vout"]}])
            coin_t1 = self.fund()
            a.lockunspent(False, [{"txid": coin_t1["txid"], "vout": coin_t1["vout"]}])
            # 1. Bitcoin block A, k_blocks blocks anchored at it, T near the end with lock time = tip.
            self.btc_mine(1)
            A = self.btc().getblockcount()
            self.wait_until(lambda: self.tip(n1)["anchorheight"] == A, timeout=120)
            h_a = n1.getblockcount()
            while h_a > 0 and n1.getblockheader(n1.getblockhash(h_a - 1))["anchorheight"] >= A:
                h_a -= 1
            self.wait_until(lambda: n1.getblockcount() >= h_a + k_blocks, timeout=60 + 30 * k_blocks)
            lock = n1.getblockcount()
            t_hex = self.spend(coin_t, buyer, Decimal("0.0001"), locktime=lock)
            r_hex = self.spend(coin_t, a.getnewaddress(), Decimal("0.001"))
            t_id, t_h, _ = self.certify(t_hex)
            r_id = tx_from_hex(r_hex).rehash()
            self.record(case, "T", "%s lock time %d, certified at height %d; first block anchored at A=%d is %d" % (t_id, lock, t_h, A, h_a))
            # 2. Bitcoin block B, T1 anchored at it.
            self.btc_mine(1)
            B = self.btc().getblockcount()
            self.wait_until(lambda: self.tip(n1)["anchorheight"] == B, timeout=120)
            t1_hex = self.spend(coin_t1, buyer, Decimal("0.0001"))
            r1_hex = self.spend(coin_t1, a.getnewaddress(), Decimal("0.001"))
            t1_id, t1_h, _ = self.certify(t1_hex)
            self.record(case, "T1", "%s certified at height %d" % (t1_id, t1_h))
            sizes = [self.log_size(i) for i in range(N)]
            # 3. Bitcoin 1 deep: T1 held, R1 on the attacker; rejoin until a member declines, then isolate.
            self.isolate()
            fork1 = self.bitcoin_reorg(1)
            self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork1 for n in self.nodes), timeout=60)
            self.wait_held([t1_id], aware)
            a.sendrawtransaction(r1_hex)
            self.rejoin()
            t_rejoin = time.time()
            declined = False
            while time.time() - t_rejoin < 60 + 6 * ROUND_MS / 1000.0:
                if any(self.nodes[i].getheldtxs()["held_for_ms"] >= 0 for i in aware):
                    declined = True
                    break
                if t1_id not in n1.getheldtxs()["txids"]:
                    break
                time.sleep(0.05)
            if not declined:
                self.record(case, "decline", "no member declined R1 before T1 settled; again")
                self.rejoin()
                self.sync_all_tips()
                continue
            self.isolate()
            time.sleep(iso_wait)
            mark = max(self.cert_heights(n1, 0) or [0])
            clocks = {i: (self.nodes[i].getheldtxs()["hold_ms"], self.nodes[i].getheldtxs()["held_for_ms"]) for i in aware}
            self.record(case, "before the second reorganization", "node 1's highest certified height %d; (bound, backstop) clocks in ms %s" % (mark, clocks))
            # 4. Bitcoin deeper: A goes, T's block with it.
            fork2 = self.bitcoin_reorg(B - A + 2 if B > A else 3)
            self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork2 for n in self.nodes), timeout=60)
            f2 = n1.getblockcount()
            self.wait_held([t_id], aware)
            assert f2 < mark, "the second reorganization did not lower the chain below the hold's mark"
            a.sendrawtransaction(r_hex)
            self.record(case, "second reorganization", "Bitcoin fork %d; node 1 back at height %d (T's lock time %d, hold mark %d); T held on %s" %
                        (fork2, f2, lock, mark, aware))
            sizes2 = [self.log_size(i) for i in range(N)]
            self.rejoin()
            t_rj2 = time.time()
            # 5. Watch until T or R is certified.
            won = None
            samples = []
            while won is None and time.time() - t_rj2 < 240 + 40 * ROUND_MS / 1000.0 * (lock - f2) / 10.0:
                ch = self.cert_heights(n1, f2)
                inf = n1.getheldtxs()
                samples.append((round(time.time() - t_rj2, 1), n1.getblockcount(), len(ch), inf["hold_ms"], inf["held_for_ms"]))
                for txid, name in ((t_id, "T"), (r_id, "R")):
                    h = self.block_of(txid, n1)
                    if h is not None and any(n1.getblockheader(n1.getblockhash(j))["poscertified"] for j in range(h, n1.getblockcount() + 1)):
                        won = (name, h)
                time.sleep(1)
            for smp in samples[::max(1, len(samples) // 8)] + samples[-1:]:
                self.log.info("    t=%5.1fs height %d, certified above %d: %d; node 1 hold_ms %d, held_for_ms %d" %
                              (smp[0], smp[1], f2, smp[2], smp[3], smp[4]))
            releases = []
            for i in aware:
                releases += self.log_has(i, "(the liveness bound)", sizes2[i]) + self.log_has(i, "(the backstop,", sizes2[i])
            self.record(case, "outcome", "%s certified at height %s, %.1fs after the rejoin" % (won[0], won[1], time.time() - t_rj2)
                        if won else "neither")
            self.record(case, "bound or backstop releases after the second reorganization", len(releases))
            assert won is not None and won[0] == "T", "R won or nothing settled: %s; releases %s" % (won, releases[:3])
            assert won[1] > lock, "T mined below its lock time"
            assert not releases, releases[0]
            refusals = []
            for i in aware:
                refusals += self.log_has(i, "respends an input of held transaction %s" % t_id, sizes2[i])
            self.record(case, "refusals of R after the second reorganization", len(refusals))
            self.sync_all_tips()
            self.wait_until(lambda: not n1.getheldtxs()["txids"], timeout=120)
            return
        raise AssertionError("no member declined R1 in 4 tries")

    def case_fs(self):
        """A node synced afresh between the disconnect and the vote: member 5's data directory is wiped after
        the disconnect and it syncs from the network. It is unaware (it never saw T certified); the other
        four aware members hold a quorum, so R must still not be certified."""
        self.log.info("(fs) a member synced afresh between the disconnect and the vote")
        a = self.nodes[ATTACKER]
        coin = self.fund()
        t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
        r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
        self.btc_mine(1)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        t_id, t_h, _ = self.certify(t_hex)
        sizes = [self.log_size(i) for i in range(N)]
        self.isolate()
        fork = self.bitcoin_reorg(1)
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork for n in self.nodes), timeout=60)
        self.wait_held([t_id], [1, 2, 3, 4, 5])
        self.stop_node(5)
        shutil.rmtree(os.path.join(self.nodes[5].datadir, CHAIN))
        self.start_node(5, extra_args=self.node_args(5))
        self.nodes[5].setnetworkactive(False)
        self.nodes[5].createwallet(wallet_name="w", descriptors=True)
        a.sendrawtransaction(r_hex)
        self.rejoin()
        self.wait_until(lambda: self.nodes[5].getblockcount() >= self.nodes[1].getblockcount() - 1, timeout=180)
        info5 = self.nodes[5].getheldtxs()
        self.record("fs", "node 5 after the fresh sync", "holds %d transaction(s); T held: %s" % (len(info5["txids"]), t_id in info5["txids"]))
        sizes[5] = 0
        st = dict(t_id=t_id, r_id=tx_from_hex(r_hex).rehash(), sizes=sizes, aware=[1, 2, 3, 4])
        assert_equal(self.outcome("fs", st), "T")

    def case_share(self):
        """N1's share: after a disconnect of blocks full of transactions no honest mempool takes (1-atom fees),
        aware leaders mine them from the record, oldest first, never in more than half of a block, and
        fee-paying transactions still get the rest. The honest nodes run small blocks (-blockmaxweight)
        so that the record outgrows several of them."""
        self.log.info("(share) many held 1-atom transactions: re-mined at most %d%% of each block", 50)
        maxw = 60000
        self.extra_node_args = {i: ["-blockmaxweight=%d" % maxw] for i in range(1, N)}
        changed = list(range(1, N))
        for i in changed:
            self.stop_node(i)
        for i in changed:
            self.nodes[i].extra_args = self.node_args(i, self.aware[i])
            self.start_node(i, extra_args=self.nodes[i].extra_args)
            self.nodes[i].loadwallet("w")
        self.mesh()
        self.sync_all_tips()
        a = self.nodes[ATTACKER]
        # An honest wallet with coins, to send fee-paying transactions while the record is re-mined.
        payer = self.nodes[2]
        fund_id = a.sendtoaddress(address=payer.getnewaddress(), amount=Decimal("5"), fee_asset_label="bitcoin")
        self.wait_mined(fund_id, self.nodes[1])
        self.wait_certified_above(self.nodes[1].getblockcount() - 1)
        self.btc_mine(1)
        self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
        # Two parents: one of 120 outputs, heavier than the share (the first always goes in), one of 80.
        # Each spends one chosen coin, so that its weight is that of its outputs whatever the attacker's
        # wallet holds by now: a parent that wallet coin selection made heavier than an honest block
        # could never be mined again by an honest producer.
        coins = []
        parents = []
        for n_out in (120, 80):
            coin = self.fund(Decimal("0.01") * (n_out + 1))
            fee = Decimal("0.005")
            outs = [{a.getnewaddress(): Decimal("0.01")} for _ in range(n_out)]
            raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],
                                         outs + [{a.getnewaddress(): Decimal("0.01") - fee}, {"fee": fee}])
            txid = a.sendrawtransaction(a.signrawtransactionwithwallet(raw)["hex"])
            parents.append(txid)
            coins += [{"txid": txid, "vout": o["n"], "amount": Decimal("0.01")} for o in a.getrawtransaction(txid, 1)["vout"]
                      if o["value"] == Decimal("0.01")]
        for txid in parents:
            self.wait_mined(txid, self.nodes[1])
        self.wait_certified_above(self.nodes[1].getblockcount() - 1)
        pw = [a.getrawtransaction(t, 1)["weight"] for t in parents]
        self.record("share", "parents", "weights %s against a share of %d in blocks of %d" % (pw, maxw // 2, maxw))
        assert maxw // 2 < pw[0] < maxw and pw[1] < maxw, "the parents do not fit the case: %s" % pw
        dest = a.getnewaddress()
        h_before = self.nodes[1].getblockcount()
        ids = []
        for c in coins:
            ids.append(a.sendrawtransaction(self.spend(c, dest, Decimal("0.00000001"))))
        ids_children = list(ids)
        self.wait_until(lambda: a.getmempoolinfo()["size"] == 0, timeout=600)
        self.wait_certified_above(self.nodes[1].getblockcount() - 1)
        self.sync_all_tips()
        n1 = self.nodes[1]
        carriers = [h for h in range(h_before + 1, n1.getblockcount() + 1) if set(ids) & set(n1.getblock(n1.getblockhash(h))["tx"])]
        self.record("share", "setup", "%d 1-atom spends certified in blocks %s (the attacker's, the only producer that takes them)" % (len(ids), carriers))
        sizes = [self.log_size(i) for i in range(N)]
        self.isolate()
        fork = self.bitcoin_reorg(1)
        self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork for n in self.nodes), timeout=60)
        self.wait_until(lambda: len(set(ids) & set(n1.getheldtxs()["txids"])) == len(ids), timeout=120)
        h0 = n1.getblockcount()
        # The attacker's mempool holds the same transactions, and a block it leads would mine them all from
        # there: take it offline, so that every block measured is one an aware member assembled.
        self.stop_node(ATTACKER)
        # Fee-paying traffic from an honest wallet, queued before the members meet again.
        paid = []
        for _ in range(15):
            paid.append(payer.sendtoaddress(address=payer.getnewaddress(), amount=Decimal("0.001"), fee_asset_label="bitcoin"))
        # In every honest mempool already, so that whoever leads has them.
        for txid in paid:
            hexed = payer.gettransaction(txid)["hex"]
            for i in (1, 3, 4, 5):
                self.nodes[i].sendrawtransaction(hexed)
        self.connect_set(list(range(1, N)))
        t0 = time.time()
        while (len(set(ids) & set(n1.getheldtxs()["txids"])) > 0 or n1.getmempoolinfo()["size"] > 0) and time.time() - t0 < 600:
            time.sleep(1)
        left = len(set(ids) & set(n1.getheldtxs()["txids"]))
        self.wait_until(lambda: len({self.nodes[i].getbestblockhash() for i in range(1, N)}) == 1, timeout=120)
        rows = []
        worst = 0.0
        for h in range(h0 + 1, n1.getblockcount() + 1):
            b = n1.getblock(n1.getblockhash(h), 2)
            held_w = sum(t["weight"] for t in b["tx"][1:] if t["txid"] in ids or t["txid"] in parents)
            n_held = sum(1 for t in b["tx"][1:] if t["txid"] in ids or t["txid"] in parents)
            n_paid = sum(1 for t in b["tx"][1:] if t["txid"] in paid)
            honest = self.stakers[ATTACKER][1] not in b.get("signblock_challenge", "")
            if n_held or n_paid:
                rows.append((h, "honest" if honest else "attacker", n_held, held_w, n_paid, b["weight"]))
                # The oldest held transaction always goes in, however large; any more only within half.
                if honest and n_held > 1:
                    worst = max(worst, held_w / maxw)
        self.record("share", "blocks after the rejoin (height, leader, held txs, their weight, paid txs, block weight)", rows)
        self.record("share", "largest held share in an honest block with more than one", "%.3f of %d" % (worst, maxw))
        self.record("share", "left held", left)
        assert left == 0
        assert worst <= 0.5, worst
        assert any(r[1] == "honest" and r[2] > 0 for r in rows), "no honest block mined a held transaction"
        assert any(r[1] == "honest" and r[2] > 0 and r[4] > 0 for r in rows), "no honest block carried held and paid transactions together"
        self.extra_node_args = {}
        self.start_node(ATTACKER, extra_args=self.node_args(ATTACKER))
        self.nodes[ATTACKER].loadwallet("w")
        for i in changed:
            self.stop_node(i)
        for i in changed:
            self.nodes[i].extra_args = self.node_args(i, self.aware[i])
            self.start_node(i, extra_args=self.nodes[i].extra_args)
            self.nodes[i].loadwallet("w")
        self.mesh()
        self.sync_all_tips()

    def run_test(self):
        try:
            # Wait for the committee to certify blocks on its own.
            self.wait_certified_above(0, timeout=180)
            self.log.info("committee running at height %d", self.nodes[1].getblockcount())
            st = None
            if "a" in CASES:
                st = self.case_a()
            if "e" in CASES and st:
                self.case_e(st)
            if "d" in CASES:
                self.case_d()
            if "c" in CASES:
                self.case_c()
            if "g" in CASES:
                self.case_g()
            if "f" in CASES:
                self.case_f()
            if "b" in CASES:
                self.case_b()
            if "cc" in CASES:
                self.case_c_control()
            if "i" in CASES:
                self.case_i()
            if "rC" in CASES:
                self.case_rC()
            if "rA" in CASES:
                self.case_rA()
            if "rB" in CASES:
                self.case_rB()
            if "rD" in CASES:
                self.case_rD()
            if "b15" in CASES:
                self.case_b15()
            if "bb" in CASES:
                self.case_bb()
            if "dd" in CASES:
                self.case_dd()
            if "fs" in CASES:
                self.case_fs()
            if "share" in CASES:
                self.case_share()
        finally:
            if RESULTS:
                with open(RESULTS, "w") as f:
                    json.dump(self.results, f, indent=1)
            self.stop_bitcoind()


if __name__ == '__main__':
    HeldTransactionsTest().main()
