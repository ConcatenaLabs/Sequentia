#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A proposal claiming an anchor nobody has seen gains no rank (#286).

Committee members rank proposals by anchor freshness first, then by the
election key. A member that claimed an anchor far above the real one used to
rank first at every height: every member backed it, found the anchor
unverifiable only when about to sign, and excluded its leader. Anchor freshness
now counts only up to the parent-chain height the member knows, so the claim
ranks like an honest anchor at the tip and the election decides. A proposal
whose anchor cannot be verified yet is left aside, not excluded, and tried
again once the member's view of the parent chain moves.

Topology: node0 is the parent ("Bitcoin") chain, mined during warm-up and then
kept still; nodes 1-4 are producers with one seat each (quorum 3). Producer 1
claims an anchor 1000 blocks above the real one on every proposal.
"""
import os
import re
import shutil
import time

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_auth_cookie, get_datadir_path, p2p_port, rpc_port

PARENT_BLOCK_SECONDS = 600
PARENT_WARMUP_BLOCKS = 12
PRODUCERS = [1, 2, 3, 4]
ATTACKER = 1
BLOCKS = 40
MIN_FAKE_HEIGHTS = 10
MAX_GAP_SECONDS = 30


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosFakeAnchorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 5
        self.stakers = [make_staker() for _ in range(4)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def anchored_args(self, i):
        return [
            "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-con_max_block_sig_size=8000",
            "-con_blocksubsidy=0", "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1",
            "-acceptnonstdtxn=1", "-par=1",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % self.rpc_u, "-mainchainrpcpassword=%s" % self.rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]

    def node_args(self, i):
        args = self.anchored_args(i) + self.specs
        args += ["-posproducer=1", "-posproducerkey=%s" % self.stakers[i - 1][0]]
        if i == ATTACKER:
            args += ["-posbyzantinefakeanchor=1000"]
        return args

    def setup_network(self):
        self.nodes = []
        chain = "elementsregtest"
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
        ]
        self.add_nodes(1, [parent_args], chain=[chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)
        self.parent_time = self.nodes[0].getblockheader(self.parentgenesis)['time']
        self.rpc_u, self.rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

        self.add_nodes(4, [self.anchored_args(i) for i in PRODUCERS], chain=[chain] * 4)
        # The stakers' BLS registrations are derived by a running node.
        self.start_node(2, extra_args=self.anchored_args(2) + ["-staker=%s:1" % pub for _, pub in self.stakers])
        self.specs = ["-staker=%s:1%s" % (pub, self.nodes[2].getblsregistration(wif)["spec"])
                      for wif, pub in self.stakers]
        self.stop_node(2)
        shutil.rmtree(os.path.join(self.nodes[2].datadir, chain))

    def advance_parent(self, blocks):
        parent = self.nodes[0]
        addr = parent.getnewaddress()
        for _ in range(blocks):
            self.parent_time += PARENT_BLOCK_SECONDS
            parent.setmocktime(self.parent_time)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def log_lines(self, node, pattern):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return [m for m in (re.search(pattern, line) for line in f) if m]

    def run_test(self):
        parent = self.nodes[0]
        self.advance_parent(PARENT_WARMUP_BLOCKS)
        parent_height = parent.getblockcount()
        for i in PRODUCERS:
            self.start_node(i, extra_args=self.node_args(i))
        for a in PRODUCERS:
            for b in PRODUCERS:
                if a < b:
                    self.connect_nodes(a, b)
        producers = [self.nodes[i] for i in PRODUCERS]
        honest = [self.nodes[i] for i in PRODUCERS if i != ATTACKER]
        watcher = honest[0]

        self.log.info("The committee keeps certifying while producer 1 claims anchors 1000 blocks ahead")
        self.wait_until(lambda: watcher.getblockcount() >= 1, timeout=120)
        start = watcher.getblockcount()
        last_height, last_at, max_gap = start, time.time(), 0.0
        while last_height < start + BLOCKS:
            height = watcher.getblockcount()
            now = time.time()
            if height > last_height:
                last_height, last_at = height, now
            max_gap = max(max_gap, now - last_at)
            assert max_gap < MAX_GAP_SECONDS, "no block for %.0f s at height %d" % (max_gap, height)
            time.sleep(0.2)
        self.log.info("%d blocks, longest gap %.1f s" % (last_height - start, max_gap))

        self.log.info("No block on the chain carries the fake anchor")
        self.sync_blocks(producers)
        for h in range(1, watcher.getblockcount() + 1):
            assert watcher.getblockheader(watcher.getblockhash(h))['anchorheight'] <= parent_height

        self.log.info("The fake proposals were not ranked first at every height")
        # Keyed by the fake anchor's hash, which a member names when it tries
        # to validate the proposal.
        fakes = {m.group(2): int(m.group(1)) for m in
                 self.log_lines(self.nodes[ATTACKER],
                                r"BYZANTINE proposing block [0-9a-f]{64} at height (\d+) with fake anchor ([0-9a-f]{64})")}
        fake_heights = {h for h in fakes.values() if start < h <= last_height}
        assert len(fake_heights) >= MIN_FAKE_HEIGHTS, "producer 1 proposed at only %d heights" % len(fake_heights)
        # A member validates a proposal only when it is the one it backs. With
        # the claim ranking first, that was the fake at every height; now it is
        # only when the election puts its leader first.
        backed = {fakes[m.group(1)] for m in
                  self.log_lines(watcher, r"anchor ([0-9a-f]{64})")
                  if m.group(1) in fakes}
        backed &= fake_heights
        self.log.info("Producer 1 proposed at %d heights; the fake was backed at %d of them" %
                      (len(fake_heights), len(backed)))
        assert len(backed) * 4 <= len(fake_heights) * 3

        self.log.info("The members agree on the chain")
        h = min(n.getblockcount() for n in producers)
        assert_equal(len({n.getblockhash(h) for n in producers}), 1)


if __name__ == '__main__':
    PosFakeAnchorTest().main()
