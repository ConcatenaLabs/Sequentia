#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests escaping-stall sub-threshold certification (whitepaper §3.8).

An aggregate-committee PoS chain normally needs a strict-majority quorum of the
sortitioned committee to countersign each block. If the committee stalls, the
chain must not freeze forever: once the Bitcoin anchor has advanced at least
POS_ESCAPING_STALL_ANCHOR_GAP (3) blocks past the last certified block's anchor
(the "h+3" rule), a block may be certified by fewer than quorum. This is
self-limiting — a +3 anchor jump requires Bitcoin to genuinely have produced
3 blocks, which a healthy fast chain never lets happen.

Topology mirrors feature_bitcoin_anchoring: node0 is the parent ("Bitcoin")
chain; node1 is the anchored PoS chain (VRF + aggregate committee).

Covers:
 - a full-quorum block is accepted normally
 - a sub-quorum (single-member) block is REJECTED when the anchor has not
   advanced the required gap
 - the same sub-quorum block is ACCEPTED once the parent chain has advanced
   >= the gap (escaping stall)
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_raises_rpc_error,
    get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

COMMITTEE_SIZE = 3   # quorum = 2


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosEscapingStallTest(BitcoinTestFramework):
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
            "-validatepegin=0", "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1",
            "-poscommitteesize=%d" % COMMITTEE_SIZE, "-posslotinterval=1",
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
        leader = wifs[0]
        committee = wifs[1:]  # the other members

        # Grow the parent chain so the anchored node has something to anchor to.
        self.generatetoaddress(parent, 6, parent.getnewaddress(), sync_fun=self.no_op)

        def anchor_of(blockhash):
            return node.getblockheader(blockhash)['anchorheight']

        # --- A full-quorum block (leader + both members) is accepted. ---
        res = node.generateposblock(leader, committee)
        assert_equal(res['height'], 1)
        assert_equal(res['countersignatures'], COMMITTEE_SIZE)
        base_anchor = anchor_of(res['hash'])

        # --- A sub-quorum block now (no parent advance) is rejected: only the
        # leader signs (1 < quorum 2) and the anchor has not moved. ---
        assert_raises_rpc_error(-1, "", node.generateposblock, leader, [])
        assert_equal(node.getblockcount(), 1)

        # --- Advance the parent chain by the escaping-stall gap, then the same
        # single-member block IS accepted (the chain has stalled). Block
        # assembly queries the parent height live, so no poll wait is needed. ---
        self.generatetoaddress(parent, 3, parent.getnewaddress(), sync_fun=self.no_op)
        assert_equal(parent.getblockcount(), 9)

        res2 = node.generateposblock(leader, [])
        assert_equal(res2['height'], 2)
        assert_equal(res2['countersignatures'], 1)  # sub-threshold
        assert anchor_of(res2['hash']) >= base_anchor + 3

        # --- Back to normal: a full-quorum block is accepted again, and a fresh
        # sub-quorum attempt (anchor gap reset to 0) is rejected once more. ---
        res3 = node.generateposblock(leader, committee)
        assert_equal(res3['height'], 3)
        assert_equal(res3['countersignatures'], COMMITTEE_SIZE)
        assert_raises_rpc_error(-1, "", node.generateposblock, leader, [])
        assert_equal(node.getblockcount(), 3)


if __name__ == '__main__':
    PosEscapingStallTest().main()
