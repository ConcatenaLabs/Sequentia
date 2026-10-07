#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Two-step unbonding: stake leaves through an unbonding output that unlocks
after a number of PARENT-CHAIN (Bitcoin) blocks.

Without it a staking output counts as stake until it is spent, and its
relative timelock runs from the block that created it, so a mature stake could
sign at one height and leave at the next. From -posunbondheight:
 - withdrawstake moves the stake into an unbonding output of the same key; the
   stake weight is gone as soon as that confirms;
 - claimunbonded refuses until the parent chain has advanced -posunbonddepth
   blocks past the anchor of the block that created the unbonding output —
   however many Sequentia blocks are produced meanwhile;
 - then the coins come back to the wallet.

The consensus rule, with every refusal forced into a block: a producer with
the rule off (P) mines each invalid spend into a block, and the node with the
rule (S, -par=1) must refuse that block for the rule's reason. A fee of exactly
1% of the stake, a claim at the depth and an unbonding paid by other coins are
accepted in blocks.

Crossing the activation height beside a 24.7.13 node (O), when that previous
release is available (PREVIOUS_RELEASES_DIR/v24.7.13/bin): below the height
the two accept each other's one-step withdrawals both ways; at the height O
mines one and V refuses the block; O, upgraded in place, stays on its own
branch until invalidateblock moves it onto V's.

Topology: node0 = parent chain ("Bitcoin"); node1 = S, the Sequentia node with
the staking wallet, rule from height 1; node2 = P, the same chain with the rule
off, fed S's blocks by submitblock; node3 = V, a second chain with the rule at
height CROSS; node4 = O, 24.7.13, beside V.
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_raises_rpc_error, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut, CTxOutAsset
from test_framework.script import CScript, PosRecordSignatureHash, SIGHASH_ALL, OP_DROP, OP_CHECKSIG

UNBONDING = 5      # staking-script CSV, in Sequentia blocks
DEPTH = 6          # parent-chain blocks an unbonding output must wait
CROSS = 20         # the activation height of the second chain (V and O)
OLD_RELEASE = 240713
FEE = 100_000
COIN = 100_000_000


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return k, byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def unbond_script(pub_hex):
    return CScript([b"SEQUNBOND", OP_DROP, bytes.fromhex(pub_hex), OP_CHECKSIG])


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    return wif, k.get_pubkey().get_bytes().hex()


class PosUnbondingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 5
        self.a_wif, self.a_pub = make_staker()   # block producer (config-layer stake)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"
        self.add_nodes(1, [[
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
        ]], chain=[chain])
        self.start_node(0)
        parentgenesis = self.nodes[0].getblockhash(0)
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        # Every node but O (24.7.13) can be told the rule's height and depth.
        common = [
            "-con_pos=1", "-posvrf=1", "-posunbonding=%d" % UNBONDING, "-posslotinterval=1",
            "-signblockscript=51", "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1", "-con_blocksubsidy=0", "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, COIN), "-validatepegin=0", "-txindex=1",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % parentgenesis, "-par=1",
        ]
        self.common = common
        depth = ["-posunbonddepth=%d" % DEPTH]
        self.args_v = common + depth + ["-posunbondheight=%d" % CROSS]
        for i, rule in ((1, ["-posunbondheight=1"]), (2, ["-posunbondheight=0", "-acceptnonstdtxn=1"]), (3, ["-posunbondheight=%d" % CROSS])):
            self.add_nodes(1, [common + depth + rule + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i)]], chain=[chain])
            self.start_node(i)
        self.have_old = False
        if self.options.prev_releases and os.path.isfile(os.path.join(self.options.previous_releases_path, "v24.7.13", "bin", "sequentiad")):
            self.add_nodes(1, [common + ["-port=%d" % p2p_port(4), "-rpcport=%d" % rpc_port(4)]], chain=[chain], versions=[OLD_RELEASE])
            self.start_node(4)
            self.connect_nodes(3, 4)
            self.have_old = True
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.parent_addr = self.nodes[0].getnewaddress()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[1].generateposblock(self.a_wif)

    def parent(self, n):
        self.generatetoaddress(self.nodes[0], n, self.parent_addr, sync_fun=self.no_op)

    def anchor(self):
        s = self.nodes[1]
        return s.getblockheader(s.getbestblockhash())['anchorheight']

    def debug_log(self, node):
        with open(os.path.join(node.datadir, "elementsregtest", "debug.log"), encoding="utf-8") as f:
            return f.read()

    def sync_p(self):
        """Give the rule-off producer P every block of S's active chain."""
        s, p = self.nodes[1], self.nodes[2]
        for h in range(1, s.getblockcount() + 1):
            bh = s.getblockhash(h)
            if p.getblockcount() >= h and p.getblockhash(h) == bh:
                continue
            assert p.submitblock(s.getblock(bh, 0)) in (None, "duplicate", "inconclusive")
        assert_equal(p.getbestblockhash(), s.getbestblockhash())

    def stake_outpoint(self, node, pub):
        for st in node.liststakeutxos():
            if st["pubkey"] == pub:
                vo = node.getrawtransaction(st["txid"], True)["vout"][st["vout"]]
                return st["txid"], st["vout"], int(Decimal(str(vo["value"])) * COIN), bytes.fromhex(vo["scriptPubKey"]["hex"])
        raise AssertionError("no stake for %s" % pub)

    def spent_value(self, txid, n):
        """The value, in atoms, of output n of txid: one this test built, or one
        a node indexes. A record spend signs it from -posrecordsv2height."""
        built = getattr(self, "built_values", {})
        if (txid, n) in built:
            return built[(txid, n)]
        for node in self.nodes:
            try:
                return int(Decimal(str(node.getrawtransaction(txid, True)["vout"][n]["value"])) * COIN)
            except Exception:
                continue
        raise AssertionError("no node knows %s" % txid)

    def sign_spend(self, ins, outs):
        """ins = [(txid, n, script, key, nSequence)]; outs = [(value, script)], the fee output last."""
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(t, 16), n), nSequence=seq) for (t, n, _, _, seq) in ins]
        asset = b"\x01" + bytes.fromhex(self.policy_asset)[::-1]
        tx.vout = [CTxOut(v, sc, CTxOutAsset(asset)) for (v, sc) in outs]
        for i, (t, n, script, key, _) in enumerate(ins):
            sighash, err = PosRecordSignatureHash(CScript(script), tx, i, SIGHASH_ALL, self.spent_value(t, n))
            assert err is None
            tx.vin[i].scriptSig = CScript([key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])])
        tx.rehash()
        self.built_values = getattr(self, "built_values", {})
        for k, (v, _) in enumerate(outs):
            self.built_values[(tx.hash, k)] = v
        return tx

    def force_into_block(self, txs, expect):
        """P mines `txs` into a block with the rule off; S must refuse that block
        for `expect`, naming the transaction that breaks the rule."""
        s, p = self.nodes[1], self.nodes[2]
        self.sync_p()
        txids = [p.sendrawtransaction(t.serialize().hex(), 0) for t in txs]
        blk = p.generateposblock(self.a_wif)["hash"]
        in_block = [t["txid"] for t in p.getblock(blk, 2)["tx"]]
        assert all(t in in_block for t in txids), "the forced transactions are not in the block"
        best = s.getbestblockhash()
        with s.assert_debug_log(["ConnectBlock: %s in tx %s" % (expect, txids[-1])]):
            assert_equal(s.submitblock(p.getblock(blk, 0)), expect)
        assert_equal(s.getbestblockhash(), best)
        self.log.info("  block %s with %s: %s", blk[:16], [t[:16] for t in txids], expect)
        p.invalidateblock(blk)
        for t in txids:
            if t in p.getrawmempool():
                p.prioritisetransaction(t, 0, -10**10)

    def run_test(self):
        s = self.nodes[1]
        self.parent(1)
        self.mine(1)
        # A legacy wallet, so the OP_TRUE free coins of genesis count as ours
        # (-anyonecanspendaremine), then a rescan to find them.
        if not s.listwallets():
            s.createwallet(wallet_name="", descriptors=False)
        s.rescanblockchain(0)

        self.log.info("Register a 100 SEQ stake and let its staking lock mature")
        pub = s.getaddressinfo(s.getnewaddress())["pubkey"]
        s.registerstake(pub, 100)
        self.mine(1)
        fund_height = s.getblockcount()
        assert_equal(s.getstakerinfo()[pub], 100 * COIN)
        while s.getblockcount() + 1 < fund_height + UNBONDING:
            self.mine(1)

        self.log.info("withdrawstake moves the stake into an unbonding output; the weight goes at once")
        res = s.withdrawstake()
        assert_equal(res["unbonding"], True)
        assert_equal(res["destination"], "unbonding")
        assert_equal(res["unbond_depth"], DEPTH)
        lu = s.listunbonding()
        assert_equal((lu["active"], lu["unbond_depth"], lu["unit"]), (True, DEPTH, "parent-chain block"))
        assert_equal(len(lu["outputs"]), 1)
        assert_equal(lu["outputs"][0]["confirmations"], 0)
        assert "unlock_at" not in lu["outputs"][0]
        assert_equal(lu["claimable"], 0)

        self.log.info("The pending withdrawal can be re-sent with a higher fee; it still goes to unbonding")
        bump = s.bumpwithdrawstakefee()
        assert_equal(bump["destination"], "unbonding")
        assert_equal(bump["replaced_txid"], res["txid"])
        lu = s.listunbonding()
        assert_equal(len(lu["outputs"]), 1)
        assert_equal(lu["outputs"][0]["txid"], bump["txid"])
        assert_equal(lu["total"], bump["amount"])
        assert_raises_rpc_error(-8, "most a stake may pay out of itself", s.bumpwithdrawstakefee, 1000000)
        self.mine(1)
        assert pub not in s.getstakerinfo()
        created_anchor = self.anchor()
        self.log.info("  unbonding output created in a block anchored to parent height %d", created_anchor)
        out = s.listunbonding()["outputs"][0]
        assert_equal((out["unlock_at"], out["remaining"], out["claimable"]), (created_anchor + DEPTH, DEPTH, False))

        self.log.info("A rescan finds the unbonding output again, although no address of the wallet is in it")
        s.removeprunedfunds(bump["txid"])
        assert_equal(s.listunbonding()["outputs"], [])
        s.rescanblockchain(0)
        assert_equal(s.listunbonding()["outputs"][0]["txid"], bump["txid"])

        self.log.info("Sequentia blocks alone do not unlock it: the wait is counted in Bitcoin blocks")
        assert_raises_rpc_error(-4, "not claimable yet", s.claimunbonded)
        self.mine(DEPTH * 3)
        assert_equal(self.anchor(), created_anchor)
        assert_raises_rpc_error(-4, "unlocks at parent-chain height %d" % (created_anchor + DEPTH), s.claimunbonded)

        self.log.info("One parent block short of the depth: still refused")
        self.parent(DEPTH - 1)
        self.mine(1)
        assert_equal(self.anchor(), created_anchor + DEPTH - 1)
        assert_raises_rpc_error(-4, "not claimable yet", s.claimunbonded)

        self.log.info("At the depth the coins can be claimed back")
        self.parent(1)
        self.mine(1)
        assert_equal(self.anchor(), created_anchor + DEPTH)
        assert_equal(s.listunbonding()["claimable"], bump["amount"])
        balance_before = s.getbalance()["bitcoin"]
        claim = s.claimunbonded()
        assert_equal(claim["claimed_outputs"], 1)
        self.mine(1)
        assert_equal(s.gettransaction(claim["txid"])["confirmations"], 1)
        assert_equal(s.getbalance()["bitcoin"], balance_before + claim["amount"])
        assert_equal(claim["amount"] + claim["fee"] + bump["fee"], Decimal(100))
        assert_equal(s.listunbonding()["outputs"], [])
        self.log.info("two-step unbonding: weight leaves at once, coins after %d Bitcoin blocks — OK", DEPTH)

        self.forced_blocks()
        if self.have_old:
            self.cross_activation_beside_old_release()
        else:
            self.log.info("Previous release 24.7.13 not available: crossing the activation height beside it is not run")

    def forced_blocks(self):
        s = self.nodes[1]
        self.log.info("Every refusal forced into a block by a producer with the rule off")
        self.policy_asset = s.dumpassetlabels()["bitcoin"]
        keys = [make_key() for _ in range(6)]
        for _, wif, _ in keys:
            s.importprivkey(wif, "", False)
        for _, _, pub in keys:
            s.registerstake(pub, 100)
        self.mine(UNBONDING + 1)
        addr_spk = bytes.fromhex(s.getaddressinfo(s.getnewaddress())["scriptPubKey"])

        def stake_in(i):
            txid, n, amt, spk = self.stake_outpoint(s, keys[i][2])
            return (txid, n, spk, keys[i][0], UNBONDING), amt

        (i0, a0) = stake_in(0)
        self.log.info(" stake straight to an address")
        self.force_into_block([self.sign_spend([i0], [(a0 - FEE, addr_spk), (FEE, b"")])], "bad-unbond-required")
        (i1, a1) = stake_in(1)
        self.log.info(" stake to ANOTHER key's unbonding output")
        self.force_into_block([self.sign_spend([i1], [(a1 - FEE, unbond_script(keys[2][2])), (FEE, b"")])], "bad-unbond-required")
        (i2, a2) = stake_in(2)
        cap = a2 // 1000 * 10
        self.log.info(" its own unbonding output, with a fee of 1%% of the stake + 1 atom (%d)", cap + 1)
        self.force_into_block([self.sign_spend([i2], [(a2 - cap - 1, unbond_script(keys[2][2])), (cap + 1, b"")])], "bad-unbond-required")
        self.log.info(" a fee of exactly 1%% (%d atoms) is accepted in a block", cap)
        ok = self.sign_spend([i2], [(a2 - cap, unbond_script(keys[2][2])), (cap, b"")])
        s.sendrawtransaction(ok.serialize().hex(), 0)
        self.mine(1)
        assert_equal(s.getrawtransaction(ok.hash, True)["confirmations"], 1)

        self.log.info(" a claim one parent block short of the depth")
        (i3, a3) = stake_in(3)
        U = self.sign_spend([i3], [(a3 - FEE, unbond_script(keys[3][2])), (FEE, b"")])
        s.sendrawtransaction(U.serialize().hex())
        self.mine(1)
        created = self.anchor()
        assert keys[3][2] not in s.getstakerinfo()
        C = self.sign_spend([(U.hash, 0, bytes(unbond_script(keys[3][2])), keys[3][0], 0xffffffff)],
                            [(a3 - 2 * FEE, addr_spk), (FEE, b"")])
        self.parent(created + DEPTH - 1 - self.nodes[0].getblockcount())
        self.mine(1)
        assert_equal(self.anchor(), created + DEPTH - 1)
        assert_equal(s.testmempoolaccept([C.serialize().hex()])[0]["reject-reason"], "bad-unbond-premature")
        self.force_into_block([C], "bad-unbond-premature")
        self.log.info(" an unbonding and its claim in the same block")
        (i4, a4) = stake_in(4)
        U2 = self.sign_spend([i4], [(a4 - FEE, unbond_script(keys[4][2])), (FEE, b"")])
        C2 = self.sign_spend([(U2.hash, 0, bytes(unbond_script(keys[4][2])), keys[4][0], 0xffffffff)],
                             [(a4 - 2 * FEE, addr_spk), (FEE, b"")])
        self.force_into_block([U2, C2], "bad-unbond-premature")
        self.log.info(" at the depth the claim is accepted in a block")
        self.parent(1)
        self.mine(1)
        assert_equal(self.anchor(), created + DEPTH)
        s.sendrawtransaction(C.serialize().hex())
        self.mine(1)
        assert_equal(s.getrawtransaction(C.hash, True)["confirmations"], 1)

    def cross_activation_beside_old_release(self):
        v, o = self.nodes[3], self.nodes[4]
        self.log.info("Crossing height %d beside %s", CROSS, o.getnetworkinfo()["subversion"])
        v.generateposblock(self.a_wif)
        self.sync_blocks([v, o])
        if not v.listwallets():
            v.createwallet(wallet_name="", descriptors=False)
        v.rescanblockchain(0)
        keys = [make_key() for _ in range(3)]
        for _, wif, pub in keys:
            v.importprivkey(wif, "", False)
            v.registerstake(pub, 100)
        while v.getblockcount() < CROSS - 4:
            v.generateposblock(self.a_wif)
        self.sync_blocks([v, o])

        self.log.info(" below the height the two accept each other's one-step withdrawals")
        r0 = v.withdrawstake(keys[0][2])
        assert "unbonding" not in r0
        self.sync_mempools([v, o])
        b_o = o.generateposblock(self.a_wif)["hash"]
        self.sync_blocks([v, o])
        assert_equal(v.getbestblockhash(), b_o)
        assert_equal(v.gettransaction(r0["txid"])["confirmations"], 1)
        r1 = v.withdrawstake(keys[1][2])
        assert "unbonding" not in r1
        b_v = v.generateposblock(self.a_wif)["hash"]
        self.sync_blocks([v, o])
        assert_equal(o.getbestblockhash(), b_v)
        while v.getblockcount() < CROSS - 1:
            v.generateposblock(self.a_wif)
        self.sync_blocks([v, o])

        self.log.info(" at the height O mines a one-step withdrawal and V refuses the block")
        self.policy_asset = v.dumpassetlabels()["bitcoin"]
        txid, n, amt, spk = self.stake_outpoint(v, keys[2][2])
        T = self.sign_spend([(txid, n, spk, keys[2][0], UNBONDING)],
                            [(amt - FEE, bytes.fromhex(v.getaddressinfo(v.getnewaddress())["scriptPubKey"])), (FEE, b"")])
        assert_equal(v.testmempoolaccept([T.serialize().hex()])[0]["reject-reason"], "bad-unbond-required")
        self.disconnect_nodes(3, 4)
        o.sendrawtransaction(T.serialize().hex())
        x = o.generateposblock(self.a_wif)["hash"]
        assert T.hash in [t["txid"] for t in o.getblock(x, 2)["tx"]]
        o.generateposblock(self.a_wif)
        b_h = v.generateposblock(self.a_wif)["hash"]
        with v.assert_debug_log(["ConnectBlock: bad-unbond-required in tx %s" % T.hash]):
            self.connect_nodes(3, 4)
            self.wait_until(lambda: any(t["hash"] == x and t["status"] == "invalid" for t in v.getchaintips()) or
                            any(t["status"] == "invalid" for t in v.getchaintips()), timeout=30)
        assert_equal(v.getbestblockhash(), b_h)
        self.wait_until(lambda: [pi["synced_headers"] for pi in v.getpeerinfo()] == [CROSS - 1], timeout=30)
        self.log.info("  V: %s; V sees O's headers synced to %d",
                      [(t["height"], t["status"]) for t in v.getchaintips()], CROSS - 1)

        self.log.info(" O, upgraded in place, stays on its own branch until invalidateblock moves it")
        self.stop_node(4)
        o.binary = v.binary
        o.args[0] = v.binary
        o.version = None
        self.start_node(4, extra_args=self.args_v + ["-port=%d" % p2p_port(4), "-rpcport=%d" % rpc_port(4)])
        o = self.nodes[4]
        assert_equal(o.getblockcount(), CROSS + 1)
        assert o.getblockhash(CROSS) == x
        self.connect_nodes(3, 4)
        o.invalidateblock(x)
        self.wait_until(lambda: o.getbestblockhash() == v.getbestblockhash(), timeout=30)
        assert_equal(o.testmempoolaccept([T.serialize().hex()])[0]["reject-reason"], "bad-unbond-required")
        o.generateposblock(self.a_wif)
        self.sync_blocks([v, o])
        assert_equal(v.getblockcount(), CROSS + 1)


if __name__ == '__main__':
    PosUnbondingTest().main()
