#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests Bitcoin checkpoints against PoS long-range attacks (paper §11).

A Sequentia block hash committed into the parent chain ("SEQCKPT" OP_RETURN)
and buried -poscheckpointdepth confirmations deep becomes *finalized* on every
node that has it on its active chain: forks below it are rejected even if
longer/validly signed. Checkpoints never force a chain — a node only locks in
history it already validated — so bogus checkpoints are harmless.

Topology:
  node0: the parent chain (stands in for Bitcoin), holds a wallet to embed
         checkpoint commitments.
  node1: the honest PoS chain (anchored to node0), observes the checkpoint.
  node2: the "long-range attacker": same staker keys, isolated, builds a
         LONGER competing branch from before the checkpoint.

The attack must fail on node1 (it keeps its finalized chain despite the longer
branch) — exactly what an attacker with unbonded keys could otherwise do.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    get_auth_cookie,
    get_datadir_path,
    rpc_port,
    p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

CHECKPOINT_DEPTH = 2


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosCheckpointsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.staker_wif, self.staker_pub = make_staker()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []

        # Parent chain ("Bitcoin") with a funded wallet for checkpoint txs
        parent_chain = "elementsregtest"
        parent_args = [
            "-port=" + str(p2p_port(0)),
            "-rpcport=" + str(rpc_port(0)),
            "-validatepegin=0",
            "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-blindedaddresses=0",
            "-fallbackfee=0.0001",
        ]
        self.add_nodes(1, [parent_args], chain=[parent_chain])
        self.start_node(0)
        parent_genesis = self.nodes[0].getblockhash(0)

        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, parent_chain)
        pos_args_common = [
            "-validatepegin=0",
            "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-con_pos=1",
            "-posvrf=1",
            "-posslotinterval=1",
            "-staker=%s:1" % self.staker_pub,
            "-con_bitcoin_anchor=1",
            "-validateanchor=1",
            "-anchorpollinterval=1",
            "-poscheckpointdepth=%d" % CHECKPOINT_DEPTH,
            "-mainchainrpchost=127.0.0.1",
            "-mainchainrpcport=%s" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u,
            "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % parent_genesis,
        ]
        for n in (1, 2):
            args = ["-port=" + str(p2p_port(n)), "-rpcport=" + str(rpc_port(n))] + pos_args_common
            self.add_nodes(1, [args], chain=["elementsregtest"])
            self.start_node(n)

        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        # node1 and node2 start disconnected: node2 will build the attack branch.

    def wait_for_finality(self, node, height, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if node.getcheckpointinfo()['finalized_height'] == height:
                return
            time.sleep(0.25)
        raise AssertionError("checkpoint did not finalize within %ds: %s" %
                             (timeout, node.getcheckpointinfo()))

    def run_test(self):
        parent, honest, attacker = self.nodes[0], self.nodes[1], self.nodes[2]

        # Fund the parent wallet and give the PoS chains a parent tip to anchor
        addr = parent.getnewaddress()
        self.generatetoaddress(parent, 101, addr, sync_fun=self.no_op)

        # --- Build the honest chain and checkpoint its tip into the parent ---
        for _ in range(4):
            honest.generateposblock(self.staker_wif)
        assert_equal(honest.getblockcount(), 4)
        ckpt = honest.getcheckpointpayload()
        assert_equal(ckpt['height'], 4)

        # Commit the payload in a parent-chain transaction (a `data` output)
        raw = parent.createrawtransaction([], [{"data": ckpt['payload']}])
        funded = parent.fundrawtransaction(raw)['hex']
        signed = parent.signrawtransactionwithwallet(funded)['hex']
        parent.sendrawtransaction(signed)
        # Bury it CHECKPOINT_DEPTH deep
        self.generatetoaddress(parent, CHECKPOINT_DEPTH, addr, sync_fun=self.no_op)

        # The honest node's watcher observes and finalizes the checkpoint
        self.wait_for_finality(honest, 4)
        info = honest.getcheckpointinfo()
        assert_equal(info['finalized_hash'], ckpt['blockhash'])
        assert_equal(len(info['checkpoints']), 1)

        # Extend the honest chain a little past the checkpoint
        honest.generateposblock(self.staker_wif)
        honest_tip = honest.getbestblockhash()
        assert_equal(honest.getblockcount(), 5)

        # --- The long-range attack: the attacker (same staker keys — e.g.
        # acquired after unbonding) builds a LONGER chain from genesis ---
        for _ in range(8):
            attacker.generateposblock(self.staker_wif)
        assert_equal(attacker.getblockcount(), 8)
        assert attacker.getbestblockhash() != honest_tip

        # Without checkpoints, longer wins in signed-block chains. Connect the
        # attacker to the honest node: the honest node must REJECT the longer
        # branch because it forks below the finalized block.
        self.connect_nodes(1, 2)
        time.sleep(3)  # give headers/blocks time to (fail to) propagate
        assert_equal(honest.getbestblockhash(), honest_tip)
        assert_equal(honest.getblockcount(), 5)
        # The finality point is intact
        assert_equal(honest.getcheckpointinfo()['finalized_hash'], ckpt['blockhash'])

        # The honest chain keeps producing on top of its finalized history
        honest.generateposblock(self.staker_wif)
        assert_equal(honest.getblockcount(), 6)
        assert_equal(honest.getblockhash(4), ckpt['blockhash'])

        # --- The long-range-fork ALARM: the attacker node also watches the
        # parent chain and sees the buried checkpoint committing a block it
        # does NOT have at a height its chain has passed. It cannot finalize
        # it (checkpoints never replace validated history), but it must
        # surface the conflict — this is what tells a freshly-synced node it
        # may be on the losing side of a long-range fork. ---
        deadline = time.time() + 30
        while time.time() < deadline:
            ainfo = attacker.getcheckpointinfo()
            if len(ainfo.get('conflicts', [])) == 1:
                break
            time.sleep(0.25)
        ainfo = attacker.getcheckpointinfo()
        assert_equal(len(ainfo['conflicts']), 1)
        assert_equal(ainfo['conflicts'][0]['blockhash'], ckpt['blockhash'])
        assert_equal(ainfo['conflicts'][0]['height'], 4)
        assert_equal(ainfo['finalized_height'], -1)
        # The honest node, by contrast, has no conflicts.
        assert_equal(honest.getcheckpointinfo()['conflicts'], [])


if __name__ == '__main__':
    PosCheckpointsTest().main()
