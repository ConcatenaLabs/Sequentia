#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Members with different views of Bitcoin keep one collection schedule.

node0 and node6 are two parent chains ("Bitcoin") that start identical.
nodes 1-5 are a gossip committee of five seats, one each, quorum 4. Nodes 1,
4 and 5 watch node0; nodes 2 and 3 watch node6. Nodes 4 and 5 stay stopped, so
no round at height H certifies.

Nodes 1 and 3 propose at H. The parent then moves one block and node2, started
a few seconds later, proposes with that fresher anchor: its proposal is round 0
on every member, and its stamp is later than the others. Then node6 alone
replaces its top two blocks. Nodes 2 and 3 leave every proposal at H aside for
its anchor (anchor-stale) while node1, which sees no reorganization, still
backs round 0.

Every member must keep timing the collection from round 0's stamp. A member
that timed it from another candidate's stamp once it could back none of them
(the earliest, as an earlier change did) runs its collection out later than
node1, takes node1's timely re-proposal for a second block from the same
leader, and excludes an honest member. No member may exclude anyone here.
Then the parents join on node6's branch, node4 returns, and H must certify,
with every member's tip anchored on the parent chain as it then is.

--testnet-timing runs the committee at the testnet's round length and window
(-posroundms=9450 -poswindowms=6750) and spreads the proposals by six seconds,
more than half a round, as the split must be to convict at that round length.
"""

import os
import re
import time

from feature_pos_gossip_anchor_reorg import make_staker
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import get_auth_cookie, get_datadir_path, p2p_port, rpc_port

COMMITTEE = 5
PARENT_A = 0
PARENT_B = 6
REORG_VIEW = (2, 3)


class PosGossipAnchorSplitTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--testnet-timing", dest="testnet_timing", default=False, action="store_true",
                            help="run at the testnet's round length and window")

    def set_test_params(self):
        self.num_nodes = 7
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(COMMITTEE)]

    def parent_args(self, i):
        return ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i), "-validatepegin=0",
                "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                "-signblockscript=51"]

    def setup_network(self):
        if self.options.testnet_timing:
            self.round_ms, window_ms, self.spread_s = 9450, 6750, 6.0
        else:
            self.round_ms, window_ms, self.spread_s = 700 + 35 * COMMITTEE, 500 + 25 * COMMITTEE, 1.2
        self.collection_s = (window_ms + COMMITTEE * self.round_ms) / 1000.0
        chain = "elementsregtest"
        self.chain_name = chain
        self.add_nodes(1, [self.parent_args(PARENT_A)], chain=[chain])
        self.start_node(PARENT_A)
        self.nodes[PARENT_A].createwallet(wallet_name="w", descriptors=True)
        self.parent_mine(PARENT_A, 3)
        self.add_nodes(COMMITTEE, [[] for _ in range(COMMITTEE)], chain=[chain] * COMMITTEE)
        self.add_nodes(1, [self.parent_args(PARENT_B)], chain=[chain])
        self.start_node(PARENT_B)
        self.nodes[PARENT_B].createwallet(wallet_name="w", descriptors=True)
        self.sync_parents()
        self.full = {}
        specs = None
        for parent in (PARENT_A, PARENT_B):
            user, password = get_auth_cookie(get_datadir_path(self.options.tmpdir, parent), chain)
            common = [
                "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
                "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
                "-poscommitteesize=%d" % COMMITTEE, "-posexpraceheight=1", "-posslotinterval=1",
                "-con_max_block_sig_size=8000", "-con_blocksubsidy=5000000000", "-par=1", "-debug=validation",
                "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
                # Keep the escaping stall (a sub-quorum block after a parent median-time gap) out of reach.
                "-posescapestallmtpgap=1000000", "-mainchainrpctimeout=5",
                "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(parent),
                "-mainchainrpcuser=%s" % user, "-mainchainrpcpassword=%s" % password,
                "-parentgenesisblockhash=%s" % self.nodes[parent].getblockhash(0),
                "-posroundms=%d" % self.round_ms, "-poswindowms=%d" % window_ms,
            ]
            if specs is None:
                self.start_node(1, extra_args=common + ["-staker=%s:1" % p for _, p in self.stakers] +
                                ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1)])
                specs = ["-staker=%s:1%s" % (p, self.nodes[1].getblsregistration(w)["spec"]) for w, p in self.stakers]
                self.stop_node(1)
            self.full[parent] = common + specs
        for i in range(1, COMMITTEE + 1):
            self.start_node(i, extra_args=self.args_for(i))
        self.mesh()

    def args_for(self, i):
        parent = PARENT_B if i in REORG_VIEW else PARENT_A
        return self.full[parent] + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
                                    "-posproducer=1", "-posproducerkey=%s" % self.stakers[i - 1][0]]

    def parent_mine(self, i, n):
        p = self.nodes[i]
        return self.generatetoaddress(p, n, p.getnewaddress(), sync_fun=self.no_op)

    def sync_parents(self):
        self.connect_nodes(PARENT_B, PARENT_A)
        self.sync_blocks([self.nodes[PARENT_A], self.nodes[PARENT_B]])
        self.disconnect_nodes(PARENT_B, PARENT_A)

    def mesh(self):
        up = [i for i in range(1, COMMITTEE + 1) if self.nodes[i].running]
        for a in up:
            for b in up:
                if a < b:
                    self.connect_nodes(a, b)

    def log_since(self, i, pos):
        with open(self.nodes[i].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(pos)
            return f.read()

    def exclusions(self, marks):
        found = {}
        for i, pos in marks.items():
            leaders = re.findall(r"leader ([0-9a-f]{16}) equivocated", self.log_since(i, pos))
            if leaders:
                found[i] = leaders
        return found

    def run_test(self):
        self.log.info("Round %d ms, collection %.2f s, proposals %.1f s apart" %
                      (self.round_ms, self.collection_s, self.spread_s))
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, COMMITTEE + 1)) >= 3, timeout=120)
        for i in (2, 4, 5):
            self.stop_node(i)
        self.parent_mine(PARENT_A, 2)
        self.sync_parents()
        h = self.nodes[1].getblockcount() + 1
        marks = {i: os.path.getsize(self.nodes[i].debug_log_path) for i in (1, 3)}
        for i in (1, 3):
            self.wait_until(lambda: "proposing block" in self.log_since(i, marks[i]), timeout=60)

        self.log.info("The parent moves; node2 proposes at H=%d %.1f s later, with the freshest anchor" % (h, self.spread_s))
        self.parent_mine(PARENT_A, 1)
        self.sync_parents()
        time.sleep(self.spread_s)
        marks[2] = os.path.getsize(self.nodes[2].debug_log_path)
        self.start_node(2, extra_args=self.args_for(2))
        self.mesh()
        self.wait_until(lambda: "proposing block" in self.log_since(2, marks[2]), timeout=60)
        time.sleep(2)
        assert self.nodes[1].getblockcount() == h - 1, "H certified with only three of five members"
        assert not self.exclusions(marks), self.exclusions(marks)

        self.log.info("Only nodes 2 and 3 see the parent replace its top two blocks")
        top = self.nodes[PARENT_B].getblockcount()
        split = {i: os.path.getsize(self.nodes[i].debug_log_path) for i in (1, 2, 3)}
        self.nodes[PARENT_B].invalidateblock(self.nodes[PARENT_B].getblockhash(top - 1))
        self.parent_mine(PARENT_B, 4)
        replacement = self.nodes[PARENT_B].getbestblockhash()
        time.sleep(2 * self.collection_s + 3)
        texts = {i: self.log_since(i, split[i]) for i in (1, 2, 3)}
        for i, text in texts.items():
            self.log.info("node%d: left aside %d, anchor-stale %d, restarts %d" %
                          (i, text.count("leaving aside proposal"), text.count("anchor-stale"),
                           text.count("restarting collection")))
        assert "anchor-stale" not in texts[1], "the case was not reached: node1 saw the reorganization"
        for i in REORG_VIEW:
            assert "anchor-stale" in texts[i], "the case was not reached: node%d left no proposal aside" % i
        assert all("restarting collection" in texts[i] for i in (1, 2, 3)), "a collection never ran out"
        assert self.nodes[1].getblockcount() == h - 1
        excluded = self.exclusions(marks)
        assert not excluded, ("members with different views of Bitcoin excluded honest leaders at H=%d: %s "
                              "(node: excluded leader keys)" % (h, excluded))

        self.log.info("The parents join on node6's branch and node4 returns: H certifies")
        self.sync_parents()
        assert self.nodes[PARENT_A].getbestblockhash() == replacement
        self.start_node(4, extra_args=self.args_for(4))
        self.mesh()
        t0 = time.time()
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= h,
                        timeout=max(120, 4 * self.collection_s))
        self.log.info("H certified %.1f s after a quorum shared one view" % (time.time() - t0))
        assert len({self.nodes[i].getblockhash(h) for i in range(1, 5)}) == 1, "the members disagree at H"
        for i in range(1, 5):
            header = self.nodes[i].getblockheader(self.nodes[i].getblockhash(h))
            assert header["anchorhash"] == self.nodes[PARENT_A].getblockhash(header["anchorheight"]), \
                "node%d certified H on an anchor the parent chain no longer has" % i
        excluded = self.exclusions(marks)
        assert not excluded, "honest leaders were excluded at H=%d: %s" % (h, excluded)


if __name__ == '__main__':
    PosGossipAnchorSplitTest().main()
