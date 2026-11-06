#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The autonomous producer must RESUME after a parent-chain reorg rolls the tip back.

Every Sequentia block anchors to a Bitcoin (parent-chain) block. When the parent
reorganizes BELOW the height a Sequentia block anchored to, validation invalidates
that block (its anchor left the parent's best chain) and the Sequentia tip rolls
BACKWARD to the last anchor-canonical height. The committee must then resume
producing on the rolled-back tip.

The regression this guards against: the gossip producer tracked the height it had
already proposed/run a round for with monotonic high-water marks
(`m_proposed_height` / `m_round_height`). After a backward tip move those marks sat
ABOVE the now-current height, so no leader would ever propose at that height again
and receivers rejected any lower-height proposal — a PERMANENT stall, even though a
block extending the rolled-back tip with a fresh anchor is fully consensus-valid.
This was observed on the live testnet: a Bitcoin testnet4 reorg rolled the tip back
and the whole 100-node committee froze. The fix resets the round state on a
non-forward tip change (pos_producer.cpp PosProducer::Step).

Topology mirrors feature_pos_autonomous_escaping_stall.py: node0 = parent
("Bitcoin"); node1 = the founder PoS node (sole genesis staker, -posproducer);
node2 = a non-staking PoS peer providing gossip connectivity.

The two anchor watchers poll at different rates on purpose: the peer every
second, the founder never within the test (FOUNDER_POLL), so the peer always
sees the parent reorg first. Meanwhile the founder, which reads the parent tip
live when it picks an anchor, certifies a block on top of the block the peer has
just invalidated, and relays it. The founder notices the reorg only when it is
restarted with a short poll, whose first pass drops the orphaned block. That is honest: its watcher simply has not ticked
yet. The peer must reject the block without treating the founder as
misbehaving. It used to score it 100 ("invalid header via cmpctblock") and
drop the connection, leaving the founder with no peers. A producer without
peers never proposes, so the chain never resumed.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


SEED_STAKE = 1000000000      # atoms in the founder's genesis staking output
STAKE_CSV = 15               # height-based CSV (>= posunbonding 10 * slot 1)
COMMITTEE = 3                # quorum 2 -> the lone founder is sub-quorum (escaping stall)
PARENT_BLOCK_SECONDS = 600   # parent-chain block spacing (one Bitcoin interval)
PARENT_BLOCKS_PER_ROUND = 4  # >= POS_ESCAPING_STALL_ANCHOR_GAP (3)
FOUNDER_POLL = 3600          # founder's -anchorpollinterval: no tick within the test


class PosParentReorgRecoveryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.founder_wif, self.founder_pub = make_key()
        self.peer_wif, _ = make_key()     # not a registered staker; connectivity only

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
        self.parentgenesis = self.nodes[0].getblockhash(0)
        self.parent_time = self.nodes[0].getblockheader(self.parentgenesis)['time']
        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, chain)

        consensus = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % COMMITTEE, "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-con_blocksubsidy=5000000000",
            "-con_genesis_stake=%s:%d:%d" % (self.founder_pub, SEED_STAKE, STAKE_CSV),
            "-con_connect_genesis_outputs=1", "-initialfreecoins=500000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        founder_args = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                    "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        self.founder_args = founder_args
        founder_args = founder_args + ["-anchorpollinterval=%d" % FOUNDER_POLL]
        peer_args = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
                                 "-anchorpollinterval=1",
                                 "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        self.add_nodes(1, [founder_args], chain=[chain])
        self.start_node(1)
        self.add_nodes(1, [peer_args], chain=[chain])
        self.start_node(2)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def advance_parent(self, blocks):
        """Mine `blocks` parent blocks, PARENT_BLOCK_SECONDS apart.

        One block per call with the parent's mocktime stepped in between: a
        single multi-block generate stamps them all with the same time, which
        leaves median-time-past standing still and starves the escaping-stall
        real-time evidence (-posescapestallmtpgap, 600 s), so the sub-quorum
        founder could never produce past its first block.
        """
        parent = self.nodes[0]
        addr = parent.getnewaddress()
        for _ in range(blocks):
            self.parent_time += PARENT_BLOCK_SECONDS
            parent.setmocktime(self.parent_time)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def run_test(self):
        parent, founder, peer = self.nodes

        info = founder.getstakerinfo()
        assert_equal(info.get(self.founder_pub), SEED_STAKE)
        assert_equal(founder.getblockcount(), 0)

        # Climb to height 3 via the autonomous producer. Each sub-quorum block needs
        # the parent anchor to advance >= the escaping-stall gap, so we advance the
        # parent per block. block1 anchors to parent ~4, block2 ~8, block3 ~12.
        for target in (1, 2, 3):
            self.advance_parent(PARENT_BLOCKS_PER_ROUND)
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=90)
            assert_equal(founder.getblockcount(), target)

        h3_old = founder.getblockhash(3)
        # The peer holds block 3 before the parent reorgs, so it is the peer's
        # watcher that drops it, not a header refused on arrival.
        self.wait_until(lambda: peer.getbestblockhash() == h3_old, timeout=60)
        parent_h = parent.getblockcount()
        self.log.info("climbed to height 3 (block3=%s); parent at height %d" % (h3_old[:16], parent_h))

        # Reorg the parent BELOW block 3's anchor but above block 2's. block2
        # anchored near parent height 8, block3 near 12. Invalidating the parent at
        # height 9 orphans 9..parent_h (block 3's anchor) while leaving 8 (block 2's
        # anchor) canonical, then we mine a strictly longer competing branch so the
        # parent's best chain replaces the orphaned blocks.
        fork_at = 9
        bad = parent.getblockhash(fork_at)

        def block3_replaced():
            """True once the founder's active chain no longer holds the old block 3."""
            try:
                return founder.getblockhash(3) != h3_old
            except JSONRPCException:  # height 3 not reached (tip rolled back to 2)
                return True

        with peer.assert_debug_log(expected_msgs=[], unexpected_msgs=["Misbehaving"]):
            parent.invalidateblock(bad)
            assert_equal(parent.getblockcount(), fork_at - 1)
            # One block on the new branch before waiting. Without it the parent
            # tip can be exactly the one the peer's watcher saw before the climb's
            # last blocks (8 -> 12 -> 8 between two ticks): the watcher then sees no
            # move and keeps the verdict "anchor 12 is canonical" cached while
            # validating block 3. A real parent never returns to an earlier tip.
            # Height 9 is still too low for the founder to anchor block 4.
            self.advance_parent(1)
            # The peer's watcher drops block 3 within a second; the founder's has
            # not ticked yet.
            self.wait_until(lambda: peer.getblockcount() == 2, timeout=30)
            # Mine a competing branch taller than the old one (old tip was parent_h),
            # at the same Bitcoin cadence so its median-time-past keeps advancing.
            self.advance_parent((parent_h - fork_at) + 5)
            assert parent.getblockcount() > parent_h
            self.log.info("parent reorged: new best height %d, old block-3 anchor orphaned" % parent.getblockcount())

            # Still blind to the reorg, the founder extends the orphaned block 3
            # with a fresh anchor and relays it to the peer, which has already
            # invalidated block 3.
            self.wait_until(lambda: founder.getblockcount() >= 4, timeout=60)
            assert_equal(founder.getblockhash(3), h3_old)
            h4_stale = founder.getblockhash(4)
            refused = "AcceptBlockHeader: %s prev block invalid" % h4_stale

            def peer_refused():
                with open(peer.debug_log_path, encoding="utf-8") as f:
                    return refused in f.read()
            self.wait_until(peer_refused, timeout=30)
            assert_equal(peer.getblockcount(), 2)
            self.log.info("founder extended the orphaned block 3 and relayed it; the peer refused it")
            assert_equal(len(founder.getpeerinfo()), 1)

            # The founder's anchor watcher invalidates block 3 (orphaned anchor) on
            # its first pass after a restart with a short poll, and rolls the
            # Sequentia tip back. The producer may rebuild height 3 at once (the
            # new parent branch already gives it the anchor gap), so wait for
            # block 3 to leave the active chain rather than for height 2.
            self.restart_node(1, extra_args=self.founder_args + ["-anchorpollinterval=1"])
            self.connect_nodes(1, 2)
            self.wait_until(block3_replaced, timeout=60)
            self.log.info("Sequentia tip rolled back past the orphaned block 3")

        # The peer did not drop the founder for relaying on a block only the peer
        # had already seen orphaned. Without peers the producer never proposes.
        assert_equal(len(founder.getpeerinfo()), 1)

        # THE REGRESSION: the autonomous producer must RESUME and rebuild past the
        # rolled-back height on a fresh anchor. Without the round-state reset it is
        # stuck at 2 forever and this times out.
        self.wait_until(lambda: founder.getblockcount() >= 3, timeout=120)
        h3_new = founder.getblockhash(3)
        assert h3_new != h3_old, "height 3 must be a NEW block built after the reorg, not the orphaned one"

        # The peer follows the rebuilt chain (no fork).
        self.wait_until(lambda: peer.getblockcount() >= 3, timeout=60)
        assert_equal(founder.getblockhash(3), peer.getblockhash(3))
        self.log.info("Autonomous producer resumed after the parent reorg; rebuilt to height %d (no fork)"
                      % founder.getblockcount())


if __name__ == '__main__':
    PosParentReorgRecoveryTest().main()
