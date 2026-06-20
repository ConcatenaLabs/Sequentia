#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The AUTONOMOUS producer must self-bootstrap a sub-quorum founder via escaping stall.

feature_pos_escaping_stall.py covers escaping stall through the manual
`generateposblock` RPC. This covers the autonomous gossip path (`-posproducer`
with BLS): a lone genesis founder is below the committee quorum, so it is routed
into the gossip round — which must honor the escaping-stall relaxation (sub-quorum,
down to one member, once the Bitcoin anchor advanced >= the gap) and certify the
block ITSELF, with no coordinator and no manual RPC. Without that relaxation in
the gossip assembler the founder is stranded (can never reach a full quorum of
shares alone) and the chain cannot start under the BLS default — the regression
this guards against.

Topology: node0 = parent ("Bitcoin"); node1 = the founder PoS node (the sole
genesis staker, running -posproducer); node2 = a non-staking PoS peer that only
provides gossip connectivity (the producer proposes only when it has peers).
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
COMMITTEE = 3                # quorum 2 -> the lone founder is sub-quorum


class PosAutonomousEscapingStallTest(BitcoinTestFramework):
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

        # Shared consensus: a single genesis staker (the founder), committee 3
        # (quorum 2), autonomous BLS. Both PoS nodes use this verbatim so they
        # derive the same genesis; only the producer key differs.
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

        # Only the founder is a registered staker (from genesis); committee is 3,
        # so quorum is 2 and the founder alone is sub-quorum.
        info = founder.getstakerinfo()
        assert_equal(info.get(self.founder_pub), SEED_STAKE)
        assert_equal(len(info), 1)
        assert_equal(founder.getblockcount(), 0)

        # Advance the parent past the escaping-stall gap and let the AUTONOMOUS
        # producer (no generateposblock) certify the first blocks itself. Each
        # sub-quorum block needs another gap, so we advance the parent per block.
        for target in (1, 2, 3):
            self.generatetoaddress(parent, 4, parent.getnewaddress(), sync_fun=self.no_op)
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=90)
            assert_equal(founder.getblockcount(), target)

        # The non-staking peer follows the same chain (no fork): it accepts the
        # autonomously-produced escaping-stall blocks.
        self.wait_until(lambda: peer.getblockcount() >= 3, timeout=60)
        assert_equal(founder.getblockhash(3), peer.getblockhash(3))
        self.log.info("Autonomous producer self-bootstrapped a sub-quorum founder via escaping stall to height %d"
                      % founder.getblockcount())


if __name__ == '__main__':
    PosAutonomousEscapingStallTest().main()
