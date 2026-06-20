#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A PoS node must re-validate its chain on reload (the genesis staker is seeded early).

The stake registry is a pure function of the UTXO set, rebuilt by a scan at init.
But that scan runs AFTER the chainstate is loaded/activated, and chain (re)activation
validates PoS blocks via CheckPosStakeRules — whose first block's leader is the
genesis staker. On any reload that re-validates blocks DURING load — a node restarted
with the coins tip behind the block index, or started with -reindex-chainstate — the
first PoS block was checked against an EMPTY registry and rejected
("bad-posvrf-leader-not-staker"), marking the whole chain invalid and stranding the
node at genesis. This was observed on the live testnet: after a restart every node
got stuck at height 0. The fix seeds the genesis staker into the registry before the
chain is loaded (init.cpp -> SeedGenesisStake), so re-validation sees it.

This test reproduces the reload via -reindex-chainstate (wipe the chainstate, rebuild
it from the on-disk blocks), which re-validates every block from genesis. Without the
fix the node is stranded at genesis; with it the node rebuilds to the chain tip.

Topology mirrors feature_pos_autonomous_escaping_stall.py: node0 = parent
("Bitcoin"); node1 = the founder PoS node (sole genesis staker, -posproducer);
node2 = a non-staking PoS peer for gossip connectivity.
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


class PosReloadRegistryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.founder_wif, self.founder_pub = make_key()
        self.peer_wif, _ = make_key()

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

        self.consensus = [
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
        founder_args = self.consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                         "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        peer_args = self.consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
                                      "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        self.add_nodes(1, [founder_args], chain=[chain]); self.start_node(1)
        self.add_nodes(1, [peer_args], chain=[chain]); self.start_node(2)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def run_test(self):
        parent, founder, peer = self.nodes

        info = founder.getstakerinfo()
        assert_equal(info.get(self.founder_pub), SEED_STAKE)

        # Climb to height 3 via the autonomous producer (escaping stall; advance the
        # parent per block to satisfy the anchor gap).
        for target in (1, 2, 3):
            self.generatetoaddress(parent, 4, parent.getnewaddress(), sync_fun=self.no_op)
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=90)
        h3 = founder.getblockhash(3)
        self.log.info("founder climbed to height 3 (%s)" % h3[:16])

        # Restart the founder with -reindex-chainstate: wipe the chainstate and
        # rebuild it by re-validating every block from genesis on load. THE
        # REGRESSION: without the early genesis-staker seed, block 1 is validated
        # against an empty registry, rejected "leader-not-staker", and the node is
        # stranded at genesis. With the fix it rebuilds to the tip.
        self.restart_node(1, extra_args=self.consensus + [
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-posproducer=1", "-posproducerkey=%s" % self.founder_wif,
            "-reindex-chainstate",
        ])
        self.connect_nodes(1, 2)

        # The reindexed node must reconstruct the chain to (at least) the same height,
        # with the same block 3 — i.e. it re-validated the PoS blocks, not stalled.
        self.wait_until(lambda: founder.getblockcount() >= 3, timeout=120)
        assert_equal(founder.getblockhash(3), h3)
        self.log.info("founder re-validated its chain to height %d after -reindex-chainstate"
                      % founder.getblockcount())


if __name__ == '__main__':
    PosReloadRegistryTest().main()
