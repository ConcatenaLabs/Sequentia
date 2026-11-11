#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A recovery-set entry can outlive its recovery window.

The anchor watcher keeps every block it invalidated for an orphaned parent-chain
anchor in its recovery set, and drops an entry only once that block's branch
reconnects to the active chain. When the parent chain reorganizes back, the
watcher un-fails the block, but fork choice may still prefer the branch the
chain built meanwhile. Then the restored branch never reconnects and its entry
stays for as long as the node runs, while the chain carries on without it.

This is the steady state in which the producer's certified-sibling guard
(AnchorCertifiedSiblingPending) used to walk the whole block index on every
poll: the entry is un-failed and its anchor is canonical, so it passes every
filter, yet it sits on a branch the tip does not descend from. The cost of
that walk, and the guard's equivalence with it, are in
src/test/anchor_sibling_tests.cpp; this test shows the state arising from a
real parent-chain reorg and back, and what clears it (a restart: the block no
longer carries the watcher's failure marker, so nothing re-seeds it).

Topology: node0 = parent ("Bitcoin"); node1 = a validator holding three
unit-weight stakers at committee size 3, minting quorum-certified blocks by RPC.
"""

import time

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosAnchorRecoverySetSteadyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.stakers = [make_staker() for _ in range(3)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
        ]
        self.add_nodes(1, [parent_args], chain=[chain])
        self.start_node(0)
        parentgenesis = self.nodes[0].getblockhash(0)
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        args = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=3", "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-con_blocksubsidy=5000000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % parentgenesis,
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.add_nodes(1, [args], chain=[chain])
        self.start_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def mint_certified(self, node):
        last_err = None
        for wif, _ in self.stakers:
            members = [w for w, _ in self.stakers if w != wif]
            try:
                res = node.generateposblock(wif, members)
            except JSONRPCException as e:
                last_err = e
                continue
            assert res["countersignatures"] >= 2, "minted a sub-quorum block"
            return res["hash"]
        raise AssertionError("no staker could mint a certified block: %s" % last_err)

    def run_test(self):
        parent, node = self.nodes

        def anchor_of(height):
            return node.getblock(node.getblockhash(height))["anchorheight"]

        def mine_parent(n):
            self.generatetoaddress(parent, n, parent.getnewaddress(), sync_fun=self.no_op)

        mine_parent(6)
        self.mint_certified(node)
        self.mint_certified(node)
        keep_h = node.getblockcount()
        keep_anchor = anchor_of(keep_h)
        assert_equal(node.getanchorstatus()["recoveryset"], [])

        # Branch A: a certified block anchored above the keep-point, and two more.
        mine_parent(2)
        orphan = self.mint_certified(node)
        assert anchor_of(keep_h + 1) > keep_anchor
        for _ in range(2):
            mine_parent(1)
            self.mint_certified(node)
        a_tip = node.getbestblockhash()

        # The parent chain drops everything above the keep-point's anchor. The
        # watcher invalidates the orphaned-anchor block and remembers it.
        fork_hash = parent.getblockhash(keep_anchor + 1)
        parent.invalidateblock(fork_hash)
        self.wait_until(lambda: node.getblockcount() == keep_h, timeout=60)
        self.wait_until(lambda: node.getanchorstatus()["recoveryset"] == [orphan], timeout=30)
        self.log.info("parent rolled back; block %s at %d invalidated and in the recovery set" % (orphan[:16], keep_h + 1))

        # The chain carries on meanwhile, on ground every parent branch shares
        # (the parent's tip is the keep-point's anchor itself), and outgrows A.
        for _ in range(5):
            self.mint_certified(node)
        b_tip = node.getbestblockhash()
        assert anchor_of(node.getblockcount()) <= keep_anchor

        # The parent chain returns to A. The watcher finds the anchor canonical
        # again and un-fails the block, but fork choice keeps the longer chain,
        # so the restored branch never reconnects.
        parent.reconsiderblock(fork_hash)
        mine_parent(1)
        self.wait_until(lambda: any(t["hash"] == a_tip and t["status"] != "invalid" for t in node.getchaintips()),
                        timeout=60)
        assert_equal(node.getbestblockhash(), b_tip)

        # Steady state: the chain moves on, the entry stays.
        for _ in range(3):
            mine_parent(1)
            self.mint_certified(node)
        tip_h = node.getblockcount()
        self.wait_until(lambda: node.getanchorstatus()["anchorstatus"] == "ok", timeout=30)
        for _ in range(5):  # several watcher ticks (-anchorpollinterval=1)
            assert_equal(node.getanchorstatus()["recoveryset"], [orphan])
            time.sleep(1)
        status = {t["hash"]: t["status"] for t in node.getchaintips()}
        self.log.info("tip at %d; restored branch tip %s is %s; recovery set still %s"
                      % (tip_h, a_tip[:16], status.get(a_tip), [h[:16] for h in node.getanchorstatus()["recoveryset"]]))
        assert status.get(a_tip) != "invalid"
        assert node.getblockheader(orphan)["confirmations"] == -1

        # A restart clears it: the block no longer carries the watcher's failure
        # marker, so the start-up seeding passes it over.
        self.restart_node(1)
        assert_equal(node.getanchorstatus()["recoveryset"], [])
        assert_equal(node.getblockcount(), tip_h)
        self.log.info("after a restart the recovery set is empty")


if __name__ == '__main__':
    PosAnchorRecoverySetSteadyTest().main()
