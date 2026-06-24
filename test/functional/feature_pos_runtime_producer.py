#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Seamless staking: enable block production at runtime, with no restart.

A node that was NOT started as a producer must be able to begin producing blocks
the moment a staking key is enabled via the startposproducer RPC (no config edit,
no restart), and the choice must persist so production resumes automatically after
a restart (the GUI staking page relies on both — see src/qt/stakingpage.cpp).

Topology: node0 = parent ("Bitcoin"); node1 = a Sequentia node that holds the sole
genesis stake but is started WITHOUT -posproducer (so it is idle), then is switched
on at runtime.
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
COMMITTEE = 1


class PosRuntimeProducerTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.founder_wif, self.founder_pub = make_key()

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

        # The PoS node: holds the genesis stake but is NOT a producer (no -posproducer).
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
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
        ]
        self.add_nodes(1, [self.consensus], chain=[chain])
        self.start_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def run_test(self):
        parent, node = self.nodes
        assert_equal(node.getstakerinfo().get(self.founder_pub), SEED_STAKE)

        # Give the node a parent chain to anchor to.
        self.generatetoaddress(parent, 6, parent.getnewaddress(), sync_fun=self.no_op)

        # Not a producer yet (no -posproducer, so the producer thread never started):
        # the chain must stay at genesis.
        assert_equal(node.getblockcount(), 0)
        self.log.info("node is idle (not a producer): height 0")

        # Enable production at RUNTIME — no restart.
        res = node.startposproducer([self.founder_wif])
        assert_equal(res["producing"], True)
        assert res["keys"] >= 1
        assert_equal(res["persisted"], True)
        self.wait_until(lambda: node.getblockcount() >= 3, timeout=90)
        self.log.info("runtime enable worked: producing, height %d" % node.getblockcount())

        # Persistence: restart WITHOUT any -posproducer args. The settings.json written
        # by startposproducer must make it resume producing on its own.
        h_before = node.getblockcount()
        self.stop_node(1)
        self.start_node(1, extra_args=self.consensus)  # note: no -posproducer here
        self.wait_until(lambda: node.getblockcount() >= h_before + 2, timeout=90)
        self.log.info("after a plain restart (no producer args) it resumed: height %d" % node.getblockcount())


if __name__ == '__main__':
    PosRuntimeProducerTest().main()
