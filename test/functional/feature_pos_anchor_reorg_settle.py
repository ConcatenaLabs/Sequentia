#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A block whose parent (Bitcoin) anchor is reorged away but SETTLED stays syncable.

Every Sequentia block commits an anchor to a parent-chain (Bitcoin) block, and
validation requires that anchor to be on the parent's best chain. On a reorg-prone
parent (Bitcoin testnet4), a block that is already buried and committee-certified
can have its anchor reorganized away. The strict check made such a block
permanently unsyncable for NEW nodes (the anchor never returns to the parent's best
chain), so the chain's own buried history became unreplayable after a parent reorg
-- new participants could not join. Observed on the live testnet: after a testnet4
reorg, a wiped/new node could not sync past the orphaned-anchor block.

The fix (CheckMainchainAnchor, POS_ANCHOR_REORG_SETTLE_DEPTH): once the parent has
built its best chain well past an orphaned anchor's height, the reorg is SETTLED and
the anchor -- a real block that was canonical when the Sequentia block was produced
and certified -- is accepted. Recent (unsettled) orphaned anchors stay STALE so the
anchor watcher still rolls back the tip and the producer re-anchors fresh.

This test climbs a chain, deeply reorgs the parent so every Sequentia block's anchor
is orphaned AND buried past the settle depth, then checks (a) the producing node
keeps the chain and (b) a fresh node syncs the full chain from scratch.
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


SEED_STAKE = 1000000000
STAKE_CSV = 15
COMMITTEE = 3
SETTLE_DEPTH = 6   # must match POS_ANCHOR_REORG_SETTLE_DEPTH


class PosAnchorReorgSettleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4   # 0=parent, 1=founder, 2=peer, 3=fresh syncer
        self.founder_wif, self.founder_pub = make_key()
        self.peer_wif, _ = make_key()
        self.fresh_wif, _ = make_key()

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
            "-con_max_block_sig_size=4000", "-con_blocksubsidy=5000000000",
            "-con_genesis_stake=%s:%d:%d" % (self.founder_pub, SEED_STAKE, STAKE_CSV),
            "-con_connect_genesis_outputs=1", "-initialfreecoins=500000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=2", "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        founder_args = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                    "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        peer_args = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
                                 "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        fresh_args = consensus + ["-port=%d" % p2p_port(3), "-rpcport=%d" % rpc_port(3)]
        self.add_nodes(1, [founder_args], chain=[chain]); self.start_node(1)
        self.add_nodes(1, [peer_args], chain=[chain]); self.start_node(2)
        self.add_nodes(1, [fresh_args], chain=[chain]); self.start_node(3)  # fresh: left UNCONNECTED
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def run_test(self):
        parent, founder, peer, fresh = self.nodes

        # Climb to height 3 (escaping stall; advance the parent per block).
        for target in (1, 2, 3):
            self.generatetoaddress(parent, 4, parent.getnewaddress(), sync_fun=self.no_op)
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=90)
        h3 = founder.getblockhash(3)
        parent_h = parent.getblockcount()
        assert_equal(fresh.getblockcount(), 0)   # fresh node has no peers yet
        self.log.info("climbed to height 3; parent at %d" % parent_h)

        # Deeply reorg the parent: orphan it from height 3 (below block 1's anchor)
        # and rebuild a strictly longer branch that buries every Sequentia anchor
        # well past the settle depth, so all anchors are orphaned-but-settled.
        bad = parent.getblockhash(3)
        parent.invalidateblock(bad)
        self.generatetoaddress(parent, (parent_h - 2) + SETTLE_DEPTH + 6, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.getblockcount() >= parent_h + SETTLE_DEPTH
        self.log.info("parent deeply reorged to height %d (all SEQ anchors orphaned + settled)" % parent.getblockcount())

        # The producing node must KEEP its committee-certified chain: the orphaned
        # anchors are settled, so they are accepted (or invalidated then promptly
        # reconsidered). Without the fix it rolls back to genesis and stays there.
        self.wait_until(lambda: founder.getblockcount() >= 3, timeout=120)
        assert_equal(founder.getblockhash(3), h3)
        self.log.info("producing node kept its chain through the settled parent reorg")

        # A FRESH node now joins and must sync the FULL chain from genesis, including
        # the buried blocks whose anchors were reorged away. Without the fix it is
        # stranded below the first orphaned-anchor block.
        self.connect_nodes(3, 1)
        self.wait_until(lambda: fresh.getblockcount() >= 3, timeout=120)
        assert_equal(fresh.getblockhash(3), h3)
        self.log.info("fresh node synced the full chain (height %d) past orphaned-but-settled anchors"
                      % fresh.getblockcount())


if __name__ == '__main__':
    PosAnchorReorgSettleTest().main()
