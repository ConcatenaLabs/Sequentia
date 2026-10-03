#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The finalized point is kept however long the committee stalls, and only
Bitcoin moves it back.

A quorum-certified block becomes final once its observation window has passed
(-posfinalitydelayms). Once final it stays final: no later pass re-opens its
window, however many blocks the chain has grown since, and a restart finds it
final again at once. Then:

 - a rival branch that forks below it is refused (bad-fork-prior-to-pos-final),
   even when it is longer and its only quorum block is its own block at that
   height;
 - a parent-chain (Bitcoin) reorg that orphans the finalized block's anchor
   still takes it away: the anchor watcher invalidates it, its ancestors stay
   final, and the node follows the branch Bitcoin leaves standing;
 - a node without the watcher (-validateanchor=0) imposes no finality gate:
   holding the same finalized block, it follows that branch by most work as
   soon as the branch is the longer one.

Both happen after a restart, which restores the finalized point from disk.

Here the committee stalls after quorum block Q: more than a hundred
escaping-stall blocks (one member, allowed once the parent chain has moved on)
follow it, and the finalized point stays at Q through all of them.

A pass that finds nothing to finalize does not walk the whole chain: a
leader-only chain (node3, which never has a quorum block) grows by 150 blocks
and no pass examines more than a hundred blocks (-debug=bench reports any that
does).

Nodes: 0 parent chain; 1 S, aggregate committee of 3 (quorum 2),
-validateanchor; 2 S2, isolated after height 2, builds the rival branch;
3 a standalone leader-only chain; 4 F, following S with -validateanchor=0.
"""
import os
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port
from test_framework.key import ECKey
from test_framework.address import byte_to_base58
from test_framework.authproxy import JSONRPCException

PARENT_BLOCK_SECONDS = 600
STRETCH = 102


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosFinalityKeptTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 5
        self.stakers = [make_staker() for _ in range(3)]
        self.leader_only = make_staker()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        chain = "elementsregtest"
        self.add_nodes(1, [["-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0), "-validatepegin=0",
                            "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                            "-signblockscript=51"]], chain=[chain])
        self.start_node(0)
        pg = self.nodes[0].getblockhash(0)
        self.parent_time = self.nodes[0].getblockheader(pg)['time']
        u, p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        common = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1", "-poscommitteesize=3", "-posslotinterval=1",
            "-con_blocksubsidy=5000000000", "-con_bitcoin_anchor=1", "-validateanchor=1",
            "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % u, "-mainchainrpcpassword=%s" % p,
            "-parentgenesisblockhash=%s" % pg,
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        for i in (1, 2):
            self.add_nodes(1, [common + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i)]], chain=[chain])
            self.start_node(i)
        self.add_nodes(1, [[
            "-port=%d" % p2p_port(3), "-rpcport=%d" % rpc_port(3),
            "-con_pos=1", "-posvrf=1", "-posslotinterval=1", "-signblockscript=51",
            "-con_blocksubsidy=0", "-anyonecanspendaremine=1", "-validatepegin=0",
            "-staker=%s:1" % self.leader_only[1], "-debug=bench",
        ]], chain=[chain])
        self.start_node(3)
        no_watcher = [a if a != "-validateanchor=1" else "-validateanchor=0" for a in common]
        self.add_nodes(1, [no_watcher + ["-port=%d" % p2p_port(4), "-rpcport=%d" % rpc_port(4)]], chain=[chain])
        self.start_node(4)
        self.connect_nodes(1, 2)
        self.connect_nodes(1, 4)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.paddr = self.nodes[0].getnewaddress()

    def advance_parent(self, n):
        for _ in range(n):
            self.parent_time += PARENT_BLOCK_SECONDS
            self.nodes[0].setmocktime(self.parent_time)
            self.generatetoaddress(self.nodes[0], 1, self.paddr, sync_fun=self.no_op)

    def produce(self, node, quorum):
        wifs = [w for w, _ in self.stakers]
        last = None
        for _ in range(60):
            for i, leader in enumerate(wifs):
                committee = [w for j, w in enumerate(wifs) if j != i] if quorum else []
                try:
                    return node.generateposblock(leader, committee)
                except JSONRPCException as e:
                    last = e
            time.sleep(0.5)
        raise last

    def fin(self, node):
        return node.getposfinality()["finalized_height"]

    def block_at(self, node, height):
        """The block at `height`, or None while the chain is shorter (mid-reorg)."""
        try:
            return node.getblockhash(height)
        except JSONRPCException:
            return None

    def run_test(self):
        parent, s, s2, lo, f = self.nodes
        self.advance_parent(12)
        for _ in range(2):
            self.produce(s, True)
        self.sync_blocks([s, s2])
        self.disconnect_nodes(1, 2)

        self.log.info("A rival quorum block at height 3 on S2, kept aside")
        r3 = self.produce(s2, True)
        r3_anchor = s2.getblockheader(r3["hash"])["anchorheight"]
        # S's block 3 anchors one parent block higher, so a parent reorg can
        # orphan its anchor and leave the rival's standing.
        self.advance_parent(1)
        for _ in range(20):
            q = self.produce(s, True)
            if s.getblockheader(q["hash"])["anchorheight"] > r3_anchor:
                break
            s.invalidateblock(q["hash"])     # the node had not seen the new parent block yet
            time.sleep(1)
        Q, q_hash = q["height"], q["hash"]
        assert q_hash != r3["hash"]
        assert_equal(Q, 3)
        self.wait_until(lambda: self.fin(s) == Q, timeout=10)
        q_anchor = s.getblockheader(q_hash)["anchorheight"]
        self.log.info("  S finalized quorum block %d (anchor %d)", Q, q_anchor)

        self.log.info("The committee stalls: %d escaping-stall blocks follow; the finalized point stays", STRETCH)
        for k in range(1, STRETCH + 1):
            self.advance_parent(3)
            assert_equal(self.produce(s, False)["countersignatures"], 1)
            if k >= STRETCH - 8:
                assert_equal(self.fin(s), Q)
        assert_equal(s.getblockcount(), Q + STRETCH)
        assert_equal(self.fin(s), Q)

        self.sync_blocks([s, f])
        self.wait_until(lambda: self.fin(f) == Q, timeout=10)

        self.log.info("After a restart it is final at once, on both")
        self.restart_node(1)
        self.restart_node(4)
        s, f = self.nodes[1], self.nodes[4]
        for node in (s, f):
            assert_equal(self.fin(node), Q)
            assert_equal(node.getposfinality()["finalized_hash"], q_hash)
        self.connect_nodes(1, 4)

        self.log.info("A longer rival branch forking at the finalized height is refused")
        target = s.getblockcount() + 1
        while s2.getblockcount() < target:
            self.advance_parent(3)
            self.produce(s2, False)
        best = s.getbestblockhash()
        with s.assert_debug_log(["bad-fork-prior-to-pos-final"]):
            for h in range(Q, s2.getblockcount() + 1):
                s.submitblock(s2.getblock(s2.getblockhash(h), 0))
        assert_equal(s.getbestblockhash(), best)
        assert_equal(s.getblockhash(Q), q_hash)
        assert_equal(self.fin(s), Q)

        self.log.info("A parent-chain reorg that orphans the finalized block's anchor still takes it away")
        # F rejoins once S has settled: on its way S briefly follows the rival
        # branch, whose blocks it then invalidates and no longer serves, and a
        # node without the watcher that learned those headers from it would wait
        # out a block download timeout before asking again.
        self.disconnect_nodes(1, 4)
        tip_height = parent.getblockcount()
        parent.invalidateblock(parent.getblockhash(q_anchor))
        self.advance_parent(tip_height - q_anchor + 2)
        # Q and everything above it are anchored at or above the orphaned parent
        # block, and the watcher invalidates them, below the finalized point.
        # S2's rival block 3 is anchored below it and stands: S, which refused
        # it while Q was final, now follows it. The rival's later blocks are
        # anchored on the orphaned parent blocks too, and go the same way.
        self.wait_until(lambda: self.block_at(s, Q) == r3["hash"] and s.getblockcount() == Q, timeout=60)
        assert_equal(s.getblock(q_hash, 1)["confirmations"], -1)
        self.wait_until(lambda: s.getposfinality()["finalized_hash"] == r3["hash"], timeout=10)
        assert_equal(self.fin(s), Q)
        self.produce(s, True)
        assert_equal(s.getblockcount(), Q + 1)

        self.log.info("Without the watcher, F follows that branch once it is the longer one")
        # F holds Q final too, but imposes no gate: Bitcoin's verdict reaches it
        # through most-work fork choice, transitively, from the nodes that watch.
        assert_equal(f.getblockhash(Q), q_hash)
        f_height = f.getblockcount()
        while s.getblockcount() <= f_height:
            self.produce(s, True)
        self.connect_nodes(1, 4)
        self.wait_until(lambda: self.block_at(f, Q) == r3["hash"] and f.getbestblockhash() == s.getbestblockhash(),
                        timeout=60)

        self.log.info("A chain with nothing to finalize is not walked whole on every pass")
        for _ in range(150):
            lo.generateposblock(self.leader_only[0])
        assert_equal(lo.getblockcount(), 150)
        assert_equal(self.fin(lo), -1)
        with open(os.path.join(lo.datadir, "elementsregtest", "debug.log"), encoding="utf-8") as f:
            assert "PoS finality: a pass examined" not in f.read()


if __name__ == '__main__':
    PosFinalityKeptTest().main()
