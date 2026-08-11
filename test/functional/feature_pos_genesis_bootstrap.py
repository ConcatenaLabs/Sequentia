#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the genesis-seeded PoS bootstrap (whitepaper launch model, doc 13).

A Sequentia chain starts from a genesis that distributes the pre-mined supply
to the founder, with a CSV-locked *staking* output among those genesis outputs.
That single genesis staking output — with NO `-staker` config layer — must make
the founder a registered staker, so the founder is the sole eligible leader and
can certify the first blocks (here with a size-1 committee; on the real chain,
slowly via escaping-stall until others stake). This exercises the config-driven
form via `-con_genesis_stake=<pubkey>:<atoms>:<csv>` on a custom PoS chain.

Topology mirrors feature_pos_escaping_stall: node0 is the parent ("Bitcoin")
chain; node1 is the anchored PoS chain.

Covers:
 - the genesis-seeded founder appears in the registry with no -staker config
 - the founder (registered only via genesis stake) can produce blocks
 - a key that is NOT a genesis staker cannot lead (the registry truly gates)
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_raises_rpc_error,
    get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


SEED_STAKE = 1000000000           # atoms placed in the genesis staking output
STAKE_CSV = 15                    # height-based CSV (>= posunbonding 10 * slot 1)


class PosGenesisBootstrapTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.founder_wif, self.founder_pub = make_key()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []
        parent_chain = "elementsregtest"
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
            "-signblockscript=51",
        ]
        self.add_nodes(1, [parent_args], chain=[parent_chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)

        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, parent_chain)
        # PoS chain bootstrapped from a genesis staking output ONLY (no -staker).
        anchored_args = [
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1",
            "-poscommitteesize=1", "-posslotinterval=1",
            "-con_blocksubsidy=5000000000",
            "-con_genesis_stake=%s:%d:%d" % (self.founder_pub, SEED_STAKE, STAKE_CSV),
            "-con_connect_genesis_outputs=1",   # enter genesis outputs into the UTXO set
            "-initialfreecoins=500000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1",
            "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        self.add_nodes(1, [anchored_args], chain=[parent_chain])
        self.start_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def run_test(self):
        parent, node = self.nodes

        # The founder is registered purely from the genesis staking output —
        # there is no -staker config on this node.
        info = node.getstakerinfo()
        assert_equal(info.get(self.founder_pub), SEED_STAKE)
        assert_equal(len(info), 1)

        # The genesis-seeded founder is the sole staker, so it bootstraps the
        # chain via escaping-stall (sub-quorum: leader only, 0 countersignatures),
        # exactly the whitepaper's slow-start launch — with NO -staker config.
        # Each sub-threshold block needs the Bitcoin anchor to advance >= the gap,
        # so we advance the parent before each one.
        for expected_height in (1, 2, 3):
            self.generatetoaddress(parent, 4, parent.getnewaddress(), sync_fun=self.no_op)
            res = node.generateposblock(self.founder_wif, [])
            assert_equal(res['height'], expected_height)
            assert_equal(res['countersignatures'], 0)   # sub-threshold (single staker)
            assert_equal(node.getblockcount(), expected_height)

        # A key that is NOT a genesis staker cannot lead — proving the registry
        # genuinely gates leadership (the bootstrap isn't "anyone can sign").
        outsider_wif, _ = make_key()
        assert_raises_rpc_error(-5, "", node.generateposblock, outsider_wif, [])
        assert_equal(node.getblockcount(), 3)


if __name__ == '__main__':
    PosGenesisBootstrapTest().main()
