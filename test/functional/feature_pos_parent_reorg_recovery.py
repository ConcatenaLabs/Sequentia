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
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
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
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        founder_args = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                    "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        peer_args = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
                                 "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        self.add_nodes(1, [founder_args], chain=[chain]); self.start_node(1)
        self.add_nodes(1, [peer_args], chain=[chain]); self.start_node(2)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def run_test(self):
        parent, founder, peer = self.nodes

        info = founder.getstakerinfo()
        assert_equal(info.get(self.founder_pub), SEED_STAKE)
        assert_equal(founder.getblockcount(), 0)

        # Climb to height 3 via the autonomous producer. Each sub-quorum block needs
        # the parent anchor to advance >= the escaping-stall gap, so we advance the
        # parent per block. block1 anchors to parent ~4, block2 ~8, block3 ~12.
        for target in (1, 2, 3):
            self.generatetoaddress(parent, 4, parent.getnewaddress(), sync_fun=self.no_op)
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=90)
            assert_equal(founder.getblockcount(), target)

        h3_old = founder.getblockhash(3)
        parent_h = parent.getblockcount()
        self.log.info("climbed to height 3 (block3=%s); parent at height %d" % (h3_old[:16], parent_h))

        # Reorg the parent BELOW block 3's anchor but above block 2's. block2
        # anchored near parent height 8, block3 near 12. Invalidating the parent at
        # height 9 orphans 9..parent_h (block 3's anchor) while leaving 8 (block 2's
        # anchor) canonical, then we mine a strictly longer competing branch so the
        # parent's best chain replaces the orphaned blocks.
        fork_at = 9
        bad = parent.getblockhash(fork_at)
        parent.invalidateblock(bad)
        assert_equal(parent.getblockcount(), fork_at - 1)
        # Mine a competing branch taller than the old one (old tip was parent_h).
        self.generatetoaddress(parent, (parent_h - fork_at) + 6, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.getblockcount() > parent_h
        self.log.info("parent reorged: new best height %d, old block-3 anchor orphaned" % parent.getblockcount())

        # The founder's anchor watcher invalidates block 3 (orphaned anchor) and
        # rolls the Sequentia tip back to height 2.
        self.wait_until(lambda: founder.getblockcount() == 2, timeout=120)
        self.log.info("Sequentia tip rolled back to height 2 after the parent reorg")

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
