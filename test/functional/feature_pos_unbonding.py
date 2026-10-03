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

Topology: node0 = parent chain ("Bitcoin"); node1 = the Sequentia node, which
produces blocks with generateposblock and holds the staking wallet.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_raises_rpc_error, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

UNBONDING = 5      # staking-script CSV, in Sequentia blocks
DEPTH = 6          # parent-chain blocks an unbonding output must wait
COIN = 100_000_000


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    return wif, k.get_pubkey().get_bytes().hex()


class PosUnbondingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
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
        self.add_nodes(1, [[
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-con_pos=1", "-posvrf=1", "-posunbonding=%d" % UNBONDING, "-posslotinterval=1",
            "-posunbondheight=1", "-posunbonddepth=%d" % DEPTH,
            "-signblockscript=51", "-initialfreecoins=1000000000000",
            "-anyonecanspendaremine=1", "-con_blocksubsidy=0", "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, COIN), "-validatepegin=0", "-txindex=1",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % parentgenesis,
        ]], chain=[chain])
        self.start_node(1)
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
        self.mine(1)
        assert pub not in s.getstakerinfo()
        created_anchor = self.anchor()
        self.log.info("  unbonding output created in a block anchored to parent height %d", created_anchor)

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
        balance_before = s.getbalance()["bitcoin"]
        claim = s.claimunbonded()
        assert_equal(claim["claimed_outputs"], 1)
        self.mine(1)
        assert_equal(s.gettransaction(claim["txid"])["confirmations"], 1)
        assert_equal(s.getbalance()["bitcoin"], balance_before + claim["amount"])
        assert_equal(claim["amount"] + claim["fee"] + res["fee"], Decimal(100))
        self.log.info("two-step unbonding: weight leaves at once, coins after %d Bitcoin blocks — OK", DEPTH)


if __name__ == '__main__':
    PosUnbondingTest().main()
