#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The anchor watcher's parent-chain RPC cost must not grow with the chain.

The watcher walks the WHOLE active chain on every tick, deliberately with no
depth floor: a Sequentia block is valid if and only if its Bitcoin anchor is on
Bitcoin's best chain, at any depth (doc/sequentia/03-bitcoin-anchoring.md §intro
and §3). That walk is not the problem and must not be weakened. What it costs
the parent chain daemon is.

The regression this test exists for: the watcher used to empty both anchor
verdict caches on EVERY parent tip change, a plain extension included. Bitcoin
extends about every ten minutes and Sequentia produces a block every thirty
seconds, so ~20 Sequentia blocks share one anchor and the number of DISTINCT
anchors on the chain grows with the number of Bitcoin blocks it spans. Emptying
the caches made the next walk re-ask the parent daemon about every one of them —
thousands of calls per Bitcoin block on a chain a few weeks old, more every day
it lives, each one a fresh TCP connection, and on testnet all pointed by default
at one operator's server. Yet nothing about that history had changed: an
extension cannot alter the best chain below the old tip.

The fix keeps the verdicts a parent move provably cannot have touched, so:

  - a tick where the parent stood still costs NO parent-chain call beyond the
    poll itself;
  - a tick where the parent appended a block costs a small constant;
  - and neither depends on how many distinct anchors the chain carries.

That last property is what this test measures, because it is the one a future
change could quietly take away. getmainchainrpcstats breaks its count down by
method, which gives both halves of the measurement exactly:

  - `getblockheader` is how an anchor is checked against the parent chain, and
    on this setup nothing else issues it, so its count IS the anchor-verification
    cost — cleanly separated from the checkpoint scanner's `getblock` and the
    block producer's `getblockcount`/`getchaintips`;
  - `getbestblockhash` is issued once per watcher tick and by nothing else, so
    its count IS the tick count, and the test can wait for exact numbers of ticks
    instead of guessing at sleeps.

The same test then reorganizes the parent for real and checks that the chain
still rolls back — a verdict kept when it should have been dropped shows up here
as a chain wedged on an orphaned anchor.

Topology:
  node0: the parent chain (stands in for Bitcoin); holds a wallet.
  node1: the Sequentia chain, anchored to node0, single genesis-config staker so
         every generateposblock block is quorum(1)-certified.
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

POLL_INTERVAL = 1        # -anchorpollinterval on the Sequentia node
BLOCKS_PER_ANCHOR = 2    # Sequentia blocks produced per parent block
ANCHORS_FIRST = 20       # distinct anchors for the first measurement
ANCHORS_MORE = 40        # anchors added before the second measurement

# Absorbing a parent-chain extension costs exactly one header read: the one that
# establishes it WAS an extension. Nothing on the chain needs re-checking. This
# bound leaves room for a stray extra read; what it does not leave room for is a
# cost that scales with ANCHORS_FIRST or ANCHORS_MORE.
MAX_EXTENSION_COST = 6
# How much the same measurement may grow when the chain carries ANCHORS_MORE
# additional distinct anchors. Zero is the honest expectation; the slack absorbs
# a tick boundary, not a per-anchor term.
MAX_COST_GROWTH = 3
REORG_DEPTH = 3   # parent blocks orphaned by the reorg at the end of the test
# A reorg costs one header read per block of depth (finding the fork point) plus
# a re-check of the anchors above the fork. Both are bounded by the reorg, not by
# the chain: that is the property being asserted.
REORG_COST_SLACK = 6


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class AnchorRpcCostTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.staker_wif, self.staker_pub = make_staker()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
            "-signblockscript=51", "-blindedaddresses=0", "-fallbackfee=0.0001",
        ]
        self.add_nodes(1, [parent_args], chain=[chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)

        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, chain)
        seq_args = [
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
            "-signblockscript=51", "-blindedaddresses=0",
            "-con_pos=1", "-posvrf=1", "-posslotinterval=1",
            "-staker=%s:1" % self.staker_pub,
            "-con_bitcoin_anchor=1", "-validateanchor=1",
            "-anchorpollinterval=%d" % POLL_INTERVAL, "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        self.add_nodes(1, [seq_args], chain=[chain])
        self.start_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    # --- measurement -------------------------------------------------------

    def stats(self):
        """(anchor checks, watcher ticks) as counted inside node1.

        On this setup `getblockheader` is issued only to judge an anchor against
        the parent chain, and `getbestblockhash` only once per watcher tick, so
        the two counts are exactly the cost and the clock. getmainchainrpcstats
        itself makes no parent-chain call, so sampling does not perturb what it
        measures."""
        m = self.nodes[1].getmainchainrpcstats()['bymethod']
        return m.get('getblockheader', 0), m.get('getbestblockhash', 0)

    def wait_ticks(self, n, timeout=180):
        """Wait for n further watcher ticks."""
        start = self.stats()[1]
        self.wait_until(lambda: self.stats()[1] >= start + n, timeout=timeout)

    def wait_quiet(self, quiet_ticks=3, timeout=300):
        """Wait until the watcher has gone `quiet_ticks` consecutive ticks
        without asking the parent chain daemon about a single anchor — i.e. the
        walk is fully served from cache and nothing is outstanding. Both the old
        and the new behaviour reach this state once a walk has completed, so
        starting every measurement from it is fair to both."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            c0, _ = self.stats()
            self.wait_ticks(quiet_ticks)
            c1, _ = self.stats()
            if c1 == c0:
                return
        raise AssertionError("the anchor walk never went quiet: it keeps re-asking the "
                             "parent chain daemon about anchors on every tick")

    def extension_cost(self, observe_ticks=4):
        """Anchor checks the watcher makes against the parent chain daemon to
        absorb ONE parent-chain extension."""
        self.wait_quiet()
        c0, _ = self.stats()
        self.generatetoaddress(self.nodes[0], 1, self.parent_addr, sync_fun=self.no_op)
        self.wait_ticks(observe_ticks)
        c1, _ = self.stats()
        return c1 - c0

    # --- chain construction ------------------------------------------------

    def build_anchors(self, count):
        """Advance the parent `count` times, producing BLOCKS_PER_ANCHOR
        Sequentia blocks against each new parent block, and assert that this
        really did create `count` distinct anchors."""
        parent, seq = self.nodes
        anchors = set()
        for _ in range(count):
            self.generatetoaddress(parent, 1, self.parent_addr, sync_fun=self.no_op)
            for _ in range(BLOCKS_PER_ANCHOR):
                seq.generateposblock(self.staker_wif)
                hdr = seq.getblockheader(seq.getbestblockhash())
                anchors.add((hdr['anchorheight'], hdr['anchorhash']))
        assert_equal(len(anchors), count)
        return anchors

    def distinct_anchors_on_chain(self):
        seq = self.nodes[1]
        anchors = set()
        for h in range(1, seq.getblockcount() + 1):
            hdr = seq.getblockheader(seq.getblockhash(h))
            anchors.add((hdr['anchorheight'], hdr['anchorhash']))
        return anchors

    # --- test --------------------------------------------------------------

    def run_test(self):
        parent, seq = self.nodes
        self.parent_addr = parent.getnewaddress()

        # Mature coins on the parent, then let the Sequentia node settle onto it.
        self.generatetoaddress(parent, 101, self.parent_addr, sync_fun=self.no_op)
        self.build_anchors(ANCHORS_FIRST)
        assert_equal(seq.getanchorstatus()['anchorstatus'], 'ok')
        small = len(self.distinct_anchors_on_chain())
        self.log.info("chain carries %d distinct anchors over %d blocks"
                      % (small, seq.getblockcount()))

        cost_small = self.extension_cost()
        self.log.info("one parent extension cost %d anchor check(s) against the parent "
                      "daemon, with %d distinct anchors on the chain" % (cost_small, small))
        assert cost_small <= MAX_EXTENSION_COST, (
            "absorbing one parent-chain extension cost %d anchor checks with only %d distinct "
            "anchors on the chain; the watcher is re-asking the parent daemon about anchors "
            "the extension cannot have changed" % (cost_small, small))

        # Same measurement on a chain carrying several times the anchors. This
        # is the whole point: the cost must be the same number, not the same
        # number per anchor.
        self.build_anchors(ANCHORS_MORE)
        big = len(self.distinct_anchors_on_chain())
        assert big >= small + ANCHORS_MORE
        self.log.info("chain now carries %d distinct anchors over %d blocks"
                      % (big, seq.getblockcount()))

        cost_big = self.extension_cost()
        self.log.info("one parent extension cost %d anchor check(s) against the parent "
                      "daemon, with %d distinct anchors on the chain" % (cost_big, big))
        assert cost_big <= cost_small + MAX_COST_GROWTH, (
            "the cost of absorbing one parent-chain extension grew from %d to %d anchor checks "
            "when the chain went from %d to %d distinct anchors: the per-tick parent-chain cost "
            "is scaling with the length of the chain again"
            % (cost_small, cost_big, small, big))
        assert cost_big <= MAX_EXTENSION_COST, (
            "absorbing one parent-chain extension cost %d anchor checks with %d distinct "
            "anchors on the chain" % (cost_big, big))

        # ------------------------------------------------------------------
        # A real parent reorganization must still roll the chain back, and must
        # still cost only what sits above the fork.
        # ------------------------------------------------------------------
        self.wait_quiet()
        seq_height = seq.getblockcount()
        parent_height = parent.getblockcount()

        # Fork the parent REORG_DEPTH blocks down. Sequentia blocks anchored at
        # or above the fork lose their anchor; those below keep it, and their
        # verdicts are the ones the watcher may legitimately keep cached.
        fork_at = parent_height - (REORG_DEPTH - 1)
        doomed = parent.getblockhash(fork_at)
        doomed_anchored = [h for h in range(1, seq_height + 1)
                           if seq.getblockheader(seq.getblockhash(h))['anchorheight'] >= fork_at]
        assert doomed_anchored, "the reorg must orphan the anchor of at least one Sequentia block"
        lowest_doomed = min(doomed_anchored)
        lowest_doomed_hash = seq.getblockhash(lowest_doomed)
        anchors_above_fork = len({a for a in self.distinct_anchors_on_chain() if a[0] >= fork_at})

        c0, _ = self.stats()
        parent.invalidateblock(doomed)
        assert_equal(parent.getblockcount(), fork_at - 1)
        # A strictly longer competing branch, mined to a fresh address so its
        # blocks cannot hash-collide with the ones just invalidated.
        self.generatetoaddress(parent, 6, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.getblockcount() > parent_height
        assert_equal(parent.getblockheader(doomed)['confirmations'], -1)

        # Correctness first: every block from the lowest doomed one up must go.
        self.wait_until(lambda: seq.getblockcount() == lowest_doomed - 1, timeout=180)
        assert_equal(seq.getblockheader(lowest_doomed_hash)['confirmations'], -1)
        self.log.info("parent reorg rolled the Sequentia tip back from %d to %d"
                      % (seq_height, seq.getblockcount()))

        self.wait_quiet()
        c1, _ = self.stats()
        reorg_cost = c1 - c0
        # Finding the fork point costs one header read per block of reorg depth,
        # and the anchors above the fork have to be re-judged. Everything below
        # the fork is untouched parent-chain history and must NOT be re-asked —
        # and below the fork is nearly the whole chain.
        budget = REORG_DEPTH + anchors_above_fork + REORG_COST_SLACK
        self.log.info("absorbing a %d-block parent reorg cost %d anchor check(s) (budget %d) "
                      "with %d distinct anchors on the chain, %d of them above the fork"
                      % (REORG_DEPTH, reorg_cost, budget, big, anchors_above_fork))
        assert reorg_cost <= budget, (
            "absorbing a %d-block parent reorg cost %d anchor checks with %d distinct anchors "
            "on the chain (%d above the fork): the fork point is not being used to limit what "
            "gets re-checked" % (REORG_DEPTH, reorg_cost, big, anchors_above_fork))

        # Production resumes on the parent's new best chain.
        for _ in range(3):
            seq.generateposblock(self.staker_wif)
        assert_equal(seq.getblockcount(), lowest_doomed + 2)
        assert_equal(seq.getanchorstatus()['anchorstatus'], 'ok')
        self.wait_quiet()
        self.log.info("chain rebuilt on the parent's new best chain; watcher quiet again")


if __name__ == '__main__':
    AnchorRpcCostTest().main()
