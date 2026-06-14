#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests anchor-freshness fork choice (whitepaper §3.7/§3.8 — real-time swaps).

So that cross-chain atomic swaps need no extra reorg-protection timelocks, the
Sequentia tip must track Bitcoin's tip: among equally-certified same-height
blocks the chain prefers the one referencing the FRESHER (higher) Bitcoin
anchor. Here we build two full-committee blocks on the same parent — one
anchored to an older Bitcoin block (and seen first), one to a newer Bitcoin
block — and verify the chain selects the fresher-anchored one even though it
arrived later (so anchor-freshness overrides first-seen).

Topology mirrors feature_pos_escaping_stall: node0 is the parent ("Bitcoin")
chain; node1 is the anchored PoS chain.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_greater_than,
    get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosAnchorFreshnessTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.stakers = [make_staker() for _ in range(3)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []
        parent_chain = "elementsregtest"
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
            "-signblockscript=51",
        ]
        self.add_nodes(1, [parent_args], chain=[parent_chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)

        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, parent_chain)
        anchored_args = [
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1",
            "-poscommitteesize=3", "-posslotinterval=1",
            "-con_blocksubsidy=5000000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1",
            "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.add_nodes(1, [anchored_args], chain=[parent_chain])
        self.start_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def run_test(self):
        parent, node = self.nodes
        wifs = [w for w, _ in self.stakers]
        leader, committee = wifs[0], wifs[1:]

        self.generatetoaddress(parent, 6, parent.getnewaddress(), sync_fun=self.no_op)

        # Parent SEQ block (height 1).
        node.generateposblock(leader, committee)
        assert_equal(node.getblockcount(), 1)
        h1 = node.getbestblockhash()

        # B_old: height 2, full committee, anchored to the current Bitcoin tip.
        res_old = node.generateposblock(leader, committee)
        b_old = res_old['hash']
        assert_equal(res_old['countersignatures'], 3)
        anchor_old = node.getblockheader(b_old)['anchorheight']

        # Bury B_old, advance Bitcoin, and build a competing height-2 block
        # B_new anchored to the NEWER Bitcoin block. B_old was seen first.
        node.invalidateblock(b_old)
        assert_equal(node.getbestblockhash(), h1)
        self.generatetoaddress(parent, 1, parent.getnewaddress(), sync_fun=self.no_op)

        res_new = node.generateposblock(leader, committee)
        b_new = res_new['hash']
        assert_equal(res_new['countersignatures'], 3)
        anchor_new = node.getblockheader(b_new)['anchorheight']
        assert_greater_than(anchor_new, anchor_old)   # B_new references a fresher BTC block
        assert_equal(node.getblockheader(b_new)['previousblockhash'], h1)

        # Reconsider B_old (earlier-seen, same height, same countersigs, OLDER
        # anchor). First-seen alone would pick B_old; anchor-freshness must keep
        # the chain on the fresher-anchored B_new.
        node.reconsiderblock(b_old)
        assert_equal(node.getbestblockhash(), b_new)
        assert_equal(node.getblockcount(), 2)


if __name__ == '__main__':
    PosAnchorFreshnessTest().main()
