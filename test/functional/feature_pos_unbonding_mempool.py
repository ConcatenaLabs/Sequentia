#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Two-step unbonding and the mempool: nothing the rule refuses stays resident.

The mempool admits a transaction for the next block. Two events make a resident
transaction invalid for the next block without spending any of its inputs, and
a producer whose template carries one cannot make a block at all:

 - the activation height. A staking output spent straight to an address is
   valid in every block below -posunbondheight; admitted one block before the
   last of them and not mined in it, it would be carried into the first block
   under the rule. Connecting the block before that height evicts it, with
   its descendants.
 - a parent-chain (Bitcoin) reorg. When it orphans the anchor of the block that
   confirmed an unbonding output, that block is disconnected and the unbonding
   transaction goes back to the mempool; a claim of it waiting there now
   spends an output created in the mempool, which no block may contain. The
   reorg evicts it.

In both cases the chain keeps advancing, which is what this test asserts last.

Topology: node0 = parent chain; node1 = V, wallet and producer;
node2 = W, a second producer that never sees the old-form withdrawal;
node3 = a standalone committee chain with the debug categories off, where a
leader alone cannot assemble a block: the failure must reach the default log.
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_raises_rpc_error, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

UNBONDING = 5      # staking-script CSV, in Sequentia blocks
DEPTH = 6          # parent-chain blocks an unbonding output must wait
H = 20             # -posunbondheight
COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosUnbondingMempoolTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
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
        for i in (1, 2):
            self.add_nodes(1, [[
                "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
                "-con_pos=1", "-posvrf=1", "-posunbonding=%d" % UNBONDING, "-posslotinterval=1",
                "-posunbondheight=%d" % H, "-posunbonddepth=%d" % DEPTH,
                "-signblockscript=51", "-initialfreecoins=1000000000000",
                "-anyonecanspendaremine=1", "-con_blocksubsidy=0", "-con_connect_genesis_outputs=1",
                "-staker=%s:%d" % (self.a_pub, COIN), "-validatepegin=0", "-txindex=1",
                "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
                "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
                "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
                "-parentgenesisblockhash=%s" % parentgenesis, "-par=1",
            ]], chain=[chain])
            self.start_node(i)
        self.connect_nodes(1, 2)
        self.committee = [make_staker() for _ in range(3)]
        self.add_nodes(1, [[
            "-port=%d" % p2p_port(3), "-rpcport=%d" % rpc_port(3),
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1", "-poscommitteesize=3", "-posslotinterval=1",
            "-signblockscript=51", "-con_blocksubsidy=0", "-anyonecanspendaremine=1", "-validatepegin=0",
            "-debug=none",
        ] + ["-staker=%s:1" % pub for _, pub in self.committee]], chain=[chain])
        self.start_node(3)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.parent_addr = self.nodes[0].getnewaddress()

    def parent(self, n):
        self.generatetoaddress(self.nodes[0], n, self.parent_addr, sync_fun=self.no_op)

    def anchor(self, blockhash=None):
        v = self.nodes[1]
        return v.getblockheader(blockhash or v.getbestblockhash())['anchorheight']

    def mine(self, node):
        return node.generateposblock(self.a_wif)["hash"]

    def debug_log(self, node):
        with open(os.path.join(node.datadir, "elementsregtest", "debug.log"), encoding="utf-8") as f:
            return f.read()

    def run_test(self):
        v, w = self.nodes[1], self.nodes[2]
        self.parent(1)
        self.mine(v)
        if not v.listwallets():
            v.createwallet(wallet_name="", descriptors=False)
        v.rescanblockchain(0)

        pubs = [v.getaddressinfo(v.getnewaddress())["pubkey"] for _ in range(3)]
        for pub in pubs:
            v.registerstake(pub, 100)
        self.mine(v)
        while v.getblockcount() < H - 2:
            self.mine(v)
        self.sync_blocks([v, w])
        assert_equal(v.listunbonding()["active"], False)

        self.log.info("Tip H-2: withdrawstake builds a one-step withdrawal, valid for block H-1")
        self.disconnect_nodes(1, 2)
        res = v.withdrawstake(pubs[0])
        assert "unbonding" not in res, res
        T = res["txid"]
        # A child spending the withdrawn coins, so the eviction must take descendants.
        out = [o for o in v.getrawtransaction(T, True)["vout"] if o["scriptPubKey"].get("address") == res["destination"]][0]
        child_raw = v.createrawtransaction([{"txid": T, "vout": out["n"]}],
                                           [{v.getnewaddress(): out["value"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        child = v.sendrawtransaction(v.signrawtransactionwithwallet(child_raw)["hex"])
        assert T in v.getrawmempool() and child in v.getrawmempool()
        rawT = v.getrawtransaction(T)

        self.log.info("W makes block H-1 without it; V connects that block and evicts it with its child")
        self.mine(w)
        self.connect_nodes(1, 2)
        self.sync_blocks([v, w])
        assert_equal(v.getblockcount(), H - 1)
        assert T not in v.getrawmempool()
        assert child not in v.getrawmempool()
        log = self.debug_log(v)
        assert "Evicting %s from the mempool: bad-unbond-required from height %d" % (T, H) in log
        assert_equal(v.testmempoolaccept([rawT])[0]["reject-reason"], "bad-unbond-required")

        self.log.info("V makes block H and the chain advances through the activation height")
        self.mine(v)
        self.mine(v)
        self.sync_blocks([v, w])
        assert_equal(v.getblockcount(), H + 1)
        assert_equal(v.listunbonding()["active"], True)

        self.log.info("Abandoned, the withdrawal gives way to a two-step one")
        v.abandontransaction(T)
        res = v.withdrawstake(pubs[0])
        assert_equal(res["unbonding"], True)
        self.mine(v)
        assert pubs[0] not in v.getstakerinfo()

        self.log.info("A claim waiting in the mempool when a parent-chain reorg disconnects its unbonding")
        self.disconnect_nodes(1, 2)
        self.parent(1)
        U = v.withdrawstake(pubs[1])["txid"]
        ub = self.mine(v)
        a_u = self.anchor(ub)
        assert_equal(v.gettransaction(U)["blockhash"], ub)
        self.parent(a_u + DEPTH - self.nodes[0].getblockcount())
        self.mine(v)
        claim = v.claimunbonded()
        C = claim["txid"]
        assert C in v.getrawmempool()
        assert U in [i["txid"] for i in v.getrawtransaction(C, True)["vin"]]
        n0 = self.nodes[0]
        n0.invalidateblock(n0.getblockhash(a_u))
        self.generatetoaddress(n0, DEPTH + 3, n0.getnewaddress(), sync_fun=self.no_op)
        self.wait_until(lambda: v.gettransaction(U)["confirmations"] == 0, timeout=30)
        assert U in v.getrawmempool()
        assert C not in v.getrawmempool()
        assert "Evicting %s from the mempool after a reorg: bad-unbond-premature" % C in self.debug_log(v)

        self.log.info("The chain advances, confirming the unbonding again on the new anchor")
        tip = self.mine(v)
        assert_equal(v.gettransaction(U)["blockhash"], tip)
        new_anchor = self.anchor(tip)
        assert_raises_rpc_error(-26, "bad-unbond-premature", v.sendrawtransaction, v.gettransaction(C)["hex"])
        self.mine(v)
        assert C not in v.getrawmempool()

        self.log.info("Abandoned, the evicted claim frees the unbonding output, which waits again from its new anchor")
        v.abandontransaction(C)
        lu = [o for o in v.listunbonding()["outputs"] if o["txid"] == U][0]
        assert_equal(lu["unlock_at"], new_anchor + DEPTH)
        assert_equal(lu["claimable"], False)
        self.parent(new_anchor + DEPTH - self.nodes[0].getblockcount())
        self.mine(v)
        C2 = v.claimunbonded()["txid"]
        self.mine(v)
        assert_equal(v.gettransaction(C2)["confirmations"], 1)

        self.log.info("A block a producer cannot assemble is reported in the default log")
        # A standalone committee chain (node3, default logging): a leader with no
        # committee keys cannot assemble a certified block.
        c = self.nodes[3]
        wifs = [w for w, _ in self.committee]
        assert_raises_rpc_error(-1, "Block assembly failed", c.generateposblock, wifs[0], [])
        with open(os.path.join(c.datadir, "elementsregtest", "debug.log"), encoding="utf-8") as f:
            assert "PoS producer: no block at height 1, block assembly failed" in f.read()
        c.generateposblock(wifs[0], wifs[1:])
        assert_equal(c.getblockcount(), 1)


if __name__ == '__main__':
    PosUnbondingMempoolTest().main()
