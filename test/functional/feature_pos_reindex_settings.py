#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A node judges the blocks it stored by the settings it is configured with.

The loadblk thread connects every stored block that a -reindex,
-reindex-chainstate or -loadblock brings back, and it starts while the node is
still being set up. Every setting that decides whether such a block is
accepted must be in place before it, or the node judges its own history by
defaults it is not configured with, refuses blocks it accepted before, and
discards their bodies.

-posescapestallmtpgap shows it. A custom chain run with the gap at 0 accepts
sub-quorum (escaping-stall) blocks whose parent chain's median-time-past has
not moved; the default, 600 s, refuses each of them with
bad-pos-escape-stall-too-soon. The founder below produces such blocks with
the gap at 0, then restarts with -reindex-chainstate and the same
configuration, and must reconnect every one of them.

Topology as in feature_pos_escape_stall_activation_height.py: node0 = the
parent chain ("Bitcoin"), node1 = the founder (sole genesis staker, so every
block it makes is sub-quorum), node2 = a non-staking peer for gossip.
"""

import time

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
COMMITTEE = 3          # quorum 2: the lone founder is always sub-quorum
MTP_GAP = 0            # the configuration under test; the default is 600
HEIGHT = 3             # blocks produced before the restart


class PosReindexSettingsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.founder_wif, self.founder_pub = make_key()
        self.peer_wif, _ = make_key()
        self.mocktime = int(time.time())

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def consensus(self):
        return [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % COMMITTEE, "-posslotinterval=1",
            "-con_max_block_sig_size=4000",
            "-con_blocksubsidy=5000000000",
            "-con_genesis_stake=%s:%d:%d" % (self.founder_pub, SEED_STAKE, STAKE_CSV),
            "-con_connect_genesis_outputs=1", "-initialfreecoins=500000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1",
            "-anchorpollinterval=1", "-anchorminconf=1",
            "-posescapestallmtpgap=%d" % MTP_GAP,
            "-posescapestallmtpheight=1",
            "-mainchainrpctimeout=5",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % self.rpc_u, "-mainchainrpcpassword=%s" % self.rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]

    def founder_args(self):
        return self.consensus() + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                   "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"
        self.chain_name = chain
        parent_args = [
            "-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
            "-validatepegin=0", "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
            "-signblockscript=51", "-mocktime=%d" % self.mocktime,
        ]
        self.add_nodes(1, [parent_args], chain=[chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)
        self.rpc_u, self.rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        peer = self.consensus() + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
                                   "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        self.add_nodes(1, [self.founder_args()], chain=[chain])
        self.start_node(1)
        self.add_nodes(1, [peer], chain=[chain])
        self.start_node(2)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def bump_parent_height_only(self, nblocks=4):
        """Parent blocks that all carry the same time: height for PosEscapingStallAllowed, no MTP movement."""
        parent = self.nodes[0]
        parent.setmocktime(self.mocktime)
        self.generatetoaddress(parent, nblocks, parent.getnewaddress(), sync_fun=self.no_op)

    def log_since(self, node_idx, mark):
        with open(self.nodes[node_idx].debug_log_path, 'rb') as f:
            f.seek(mark)
            return f.read().decode('utf-8', errors='replace')

    def run_test(self):
        parent, founder, peer = self.nodes
        assert_equal(founder.getblockcount(), 0)

        # Sub-quorum blocks with no parent-chain time gap, accepted at the configured gap of 0.
        mark = founder.debug_log_bytes()
        for target in range(1, HEIGHT + 1):
            self.bump_parent_height_only()
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=120)
        tip = founder.getblockhash(HEIGHT)
        assert "bad-pos-escape-stall-too-soon" not in self.log_since(1, mark)
        for h in range(1, HEIGHT + 1):
            b = founder.getblockheader(founder.getblockhash(h))
            p = founder.getblockheader(b["previousblockhash"])
            mtp = lambda a: parent.getblockheader(a)["mediantime"]
            # Block 1's parent is the genesis block, which has no anchor to compare with.
            assert h == 1 or mtp(b["anchorhash"]) - mtp(p["anchorhash"]) < 600, "block %d has a real time gap" % h
        self.log.info("the founder produced %d sub-quorum blocks with no parent-chain time gap", HEIGHT)

        # No peer to fetch the blocks from again: the founder has only what it stored.
        self.stop_node(2)
        self.stop_node(1)
        args = [a for a in self.founder_args() if not a.startswith("-posproducer")]
        self.start_node(1, extra_args=args + ["-reindex-chainstate"])
        founder = self.nodes[1]
        self.wait_until(lambda: founder.getblockcount() >= HEIGHT or
                        "bad-pos-escape-stall-too-soon" in self.log_since(1, 0), timeout=120)
        time.sleep(2)
        log = self.log_since(1, 0)
        refused = [x for x in log.splitlines() if "bad-pos-escape-stall-too-soon" in x]
        for x in refused[:3]:
            self.log.info("  founder: %s", x[:200])
        assert not refused, "the founder judged a stored block by the default gap, not the one it is configured with"
        assert_equal(founder.getblockcount(), HEIGHT)
        assert_equal(founder.getbestblockhash(), tip)
        self.log.info("after -reindex-chainstate: every stored block reconnected, tip %s at height %d", tip[:16], HEIGHT)


if __name__ == '__main__':
    PosReindexSettingsTest().main()
