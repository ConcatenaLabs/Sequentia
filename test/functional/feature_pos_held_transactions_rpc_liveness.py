#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Held transactions on a member that signs only through the RPCs (KR8, F1).

A MuSig2 committee chain (-posaggcommittee) whose nodes run no autonomous
producer: every block is made by RPC. Node 0 holds a certified transaction T
that a Bitcoin reorganization disconnected; node 1 runs without the rule
(-posdebugignoreheld) and is the attacker. While the record fills (widened
with -posdebugheldfillms) every signing RPC refuses with held-filling; then
each refuses a block carrying R, a respend of T, or built on the
single-signer block X that carries it. No block is certified afterwards, so
the backstop, counted from node 0's first refusal and applied by its anchor
watcher, must release T: the liveness bound and the backstop bind a member
whatever way it signs. A real bitcoind is the parent chain.
"""

import os
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
CHAIN = "elementsregtest"
FILL_MS = 8000


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class HeldTransactionsRpcLivenessTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.stakers = [make_key() for _ in range(3)]
        self.results = {}

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if not os.path.exists(BITCOIND):
            raise SkipTest("needs Bitcoin Core's bitcoind (set PARENT_BITCOIND)")

    def btc(self, wallet=None):
        url = "http://%s:%s@127.0.0.1:%d" % (urllib.parse.quote(self.btc_user), urllib.parse.quote(self.btc_pass), self.btc_port)
        return AuthServiceProxy(url + ("/wallet/" + wallet if wallet else ""), timeout=120)

    def btc_mine(self, n):
        return self.btc().generatetoaddress(n, self.btc("miner").getnewaddress())

    def node_args(self, i):
        a = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1", "-poscommitteesize=3", "-posslotinterval=1",
            "-con_blocksubsidy=0", "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1",
            "-acceptnonstdtxn=1", "-par=1", "-fallbackfee=0.0001", "-blindedaddresses=0",
            "-con_default_blinded_addresses=0", "-con_bitcoin_anchor=1", "-validateanchor=1",
            "-anchorpollinterval=1", "-anchorminconf=1", "-posescapestallmtpgap=0",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % self.btc_port,
            "-mainchainrpcuser=%s" % self.btc_user, "-mainchainrpcpassword=%s" % self.btc_pass,
            "-parentgenesisblockhash=%s" % self.btc_genesis, "-txindex=1", "-debug=mempool",
            "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        a.append("-posdebugheldfillms=%d" % FILL_MS if i == 0 else "-posdebugignoreheld=1")
        return a

    def setup_network(self):
        self.btc_port = rpc_port(5)
        btcdir = os.path.join(self.options.tmpdir, "btc")
        os.makedirs(btcdir)
        self.btc_proc = subprocess.Popen([BITCOIND, "-datadir=" + btcdir, "-regtest", "-port=%d" % p2p_port(5),
                                          "-rpcport=%d" % self.btc_port, "-listen=0", "-server", "-printtoconsole=0"],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
        self.add_nodes(2, [self.node_args(i) for i in range(2)], chain=[CHAIN] * 2)
        for i in range(2):
            self.start_node(i, extra_args=self.node_args(i))
            self.nodes[i].createwallet(wallet_name="w", descriptors=True)
        self.connect_nodes(0, 1)

    def stop_bitcoind(self):
        if getattr(self, "btc_proc", None) and self.btc_proc.poll() is None:
            try:
                self.btc().stop()
                self.btc_proc.wait(timeout=60)
            except Exception:
                self.btc_proc.terminate()
                self.btc_proc.wait(timeout=60)

    def record(self, key, value):
        self.results[key] = value
        self.log.info("  %s: %s", key, value)

    def refused(self, label, want, fn):
        try:
            r = fn()
        except JSONRPCException as e:
            msg = e.error["message"]
            self.record(label, msg[:300])
            assert want in msg, (label, msg)
            return msg
        raise AssertionError("%s accepted: %s" % (label, str(r)[:200]))

    # ------------------------------------------------------------ blocks
    def wifs(self):
        return [w for w, _ in self.stakers]

    def certified_block(self, node):
        r = node.generateposblock(self.stakers[0][0], self.wifs()[1:])
        assert r["countersignatures"] >= 2
        return r["hash"]

    def template(self, node, leader=0):
        seed = bytes.fromhex(node.getposschedule()["seed"])[::-1].hex()
        members = [{"pubkey": pub, "vrfproof": node.vrfprove(wif, seed)["proof"]} for wif, pub in self.stakers]
        return node.getposblocktemplate(self.stakers[leader][0], members)

    def musig(self, node, tmpl, tag, block_hex=None):
        """Every member's nonce and partial signature on `node`; returns the aggregate, or raises."""
        msg = tmpl["signhash"]
        wif_for = {pub: wif for wif, pub in self.stakers}
        nonces = [node.musignonce("%s%d" % (tag, k), wif_for[pk], tmpl["members"], msg)["pubnonce"]
                  for k, pk in enumerate(tmpl["members"])]
        partials = []
        for k, pk in enumerate(tmpl["members"]):
            args = ["%s%d" % (tag, k), wif_for[pk], tmpl["members"], nonces, msg]
            if block_hex is not None:
                args.append(block_hex)
            partials.append(node.musigpartialsign(*args)["partialsig"])
        return node.musigaggregate(tmpl["members"], nonces, partials, msg)["signature"]

    def anchored_at_tip(self, node):
        return node.getblockheader(node.getbestblockhash())["anchorheight"] == self.btc().getblockcount()

    def run_test(self):
        try:
            self.body()
        finally:
            out = os.environ.get("CR1_RESULTS", "")
            if out:
                import json
                with open(out, "w") as f:
                    json.dump(self.results, f, indent=1)
            self.stop_bitcoind()

    def body(self):
        m, att = self.nodes
        # Coins of the attacker's (the seller's), from the genesis anyone-can-spend output.
        g = m.getblock(m.getblockhash(0), 2)
        coin = [(tx["txid"], vo["n"], Decimal(vo["value"])) for tx in g["tx"] for vo in tx["vout"]
                if vo["scriptPubKey"]["hex"] == "51" and vo.get("value", 0) > 0][0]
        addr = att.getnewaddress()
        raw = m.createrawtransaction([{"txid": coin[0], "vout": coin[1]}],
                                     [{addr: Decimal("100")}, {att.getnewaddress(): coin[2] - Decimal("100.001")}, {"fee": Decimal("0.001")}])
        fund = m.sendrawtransaction(raw)
        self.certified_block(m)
        self.sync_blocks()
        vout = [o for o in m.getrawtransaction(fund, 1)["vout"] if o["value"] == Decimal("100")][0]["n"]

        def spend(to, fee):
            r = att.createrawtransaction([{"txid": fund, "vout": vout, "sequence": 0xfffffffd}],
                                         [{to: Decimal("100") - fee}, {"fee": fee}])
            return att.signrawtransactionwithwallet(r)["hex"]
        t_hex = spend(m.getnewaddress(), Decimal("0.0001"))
        r_hex = spend(att.getnewaddress(), Decimal("0.001"))
        t_id, r_id = tx_from_hex(t_hex).rehash(), tx_from_hex(r_hex).rehash()

        # T certified, anchored at a Bitcoin block the reorganization below takes away.
        self.btc_mine(2)
        self.wait_until(lambda: self.certified_block(m) and self.anchored_at_tip(m), timeout=60)
        m.sendrawtransaction(t_hex)
        b_t = self.certified_block(m)
        self.certified_block(m)
        self.sync_blocks()
        assert t_id in m.getblock(b_t)["tx"]
        self.record("T", "%s certified in %s" % (t_id, b_t))
        # A template for the fill-window probe of submitposblock (leader key 0).
        pre = self.template(att)

        # Bitcoin reorganizes 2 deep. Node 0 then sleeps FILL_MS before filling its record.
        log0 = m.debug_log_path
        size0 = os.path.getsize(log0)
        b = self.btc()
        tip = b.getblockcount()
        b.invalidateblock(b.getblockhash(tip - 1))
        self.btc_mine(3)

        def reorg_seen():
            with open(log0, encoding="utf-8", errors="replace") as f:
                f.seek(size0)
                return "Parent chain reorganization detected" in f.read()
        self.wait_until(reorg_seen, timeout=30)
        t_seen = time.time()
        self.refused("fill: generateposblock", "held-filling", lambda: m.generateposblock(self.stakers[0][0], self.wifs()[1:]))
        self.refused("fill: getposblocktemplate", "held-filling", lambda: self.template(m))
        self.refused("fill: musigpartialsign", "held-filling",
                     lambda: m.musigpartialsign("f", self.stakers[1][0], pre["members"], ["00" * 66] * 3, pre["signhash"], pre["block"]))
        self.refused("fill: submitposblock", "held-filling", lambda: m.submitposblock(pre["block"], self.stakers[0][0], "00" * 64))
        self.record("fill probes", "all four refused %.1fs after the reorganization was logged (window %d ms)" % (time.time() - t_seen, FILL_MS))
        assert time.time() - t_seen < FILL_MS / 1000.0
        self.wait_until(lambda: t_id in m.getheldtxs()["txids"], timeout=60)
        self.sync_blocks()
        p_hash = m.getbestblockhash()
        self.record("held", "node 0 holds T; both nodes on P %s (height %d)" % (p_hash, m.getblockcount()))

        # A block carrying R, assembled by the attacker, which runs no rule.
        att.prioritisetransaction(txid=t_id, fee_delta=-100000000)
        att.sendrawtransaction(r_hex)
        d = self.template(att)
        dblk = d["block"]
        self.refused("direct: musigpartialsign of a block carrying R", "held-conflict: transaction %s respends an input of held transaction %s" % (r_id, t_id),
                     lambda: self.musig(m, d, "d", dblk))
        d_sig = self.musig(att, d, "dA")
        self.refused("direct: submitposblock of a block carrying R", "held-conflict: transaction %s respends" % r_id,
                     lambda: m.submitposblock(dblk, self.stakers[0][0], d_sig))
        self.refused("direct: musigpartialsign without the block", "pass the block to sign",
                     lambda: self.musig(m, d, "d2"))

        # X: a single-signer escaping-stall block carrying R, the holding node's tip (no producer: no set-aside).
        x = att.generateposblock(self.stakers[0][0], [])
        self.sync_blocks()
        assert_equal(m.getbestblockhash(), x["hash"])
        assert r_id in m.getblock(x["hash"])["tx"]
        self.record("X", "%s, %d signer(s), carries R, node 0's tip" % (x["hash"], x["countersignatures"]))
        anc = "ancestor %s" % x["hash"]
        self.refused("ancestor: generateposblock", anc, lambda: m.generateposblock(self.stakers[0][0], self.wifs()[1:]))
        self.refused("ancestor: getposblocktemplate", anc, lambda: self.template(m))
        e = self.template(att)
        self.refused("ancestor: musigpartialsign", anc, lambda: self.musig(m, e, "e", e["block"]))
        e_sig = self.musig(att, e, "eA")
        self.refused("ancestor: submitposblock", anc, lambda: m.submitposblock(e["block"], self.stakers[0][0], e_sig))

        # Neither node runs the autonomous producer. The specification's backstop
        # is 20.47 s for this three-seat committee. No new block is certified.
        backstop = 2 * (12 * (700 + 35 * 3) + (500 + 25 * 3)) / 1000.0
        time.sleep(backstop + 5)
        info = m.getheldtxs()
        self.record("RPC-only backstop", "after %.2f s: %s" % (backstop + 5, info))
        assert t_id not in info["txids"], "RPC-only member kept T beyond the backstop"
        # Released by the backstop's clock, applied by the anchor watcher (no producer runs here).
        with open(log0, encoding="utf-8", errors="replace") as f:
            f.seek(size0)
            rel = [line.strip() for line in f if "(the backstop," in line]
        self.record("RPC-only release", rel[0].split(" ", 1)[1] if rel else "none")
        assert rel and "[anchorwatch]" in rel[0], rel


if __name__ == '__main__':
    HeldTransactionsRpcLivenessTest().main()
