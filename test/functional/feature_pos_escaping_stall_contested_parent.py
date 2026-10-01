#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A long-lived parent-chain fork must not freeze the escaping-stall relaxation.

When Bitcoin has rival branches, the producer anchors to the last block those
branches share (-anchoravoidcontested), but only while the latest block is
itself anchored on that shared ground. Once the latest block is anchored above
a rival's fork point, the chain is already committed to the active branch, and
the producer keeps following it for as long as it remains the parent chain's
best chain.

The shape under test is the one the testnet hit on 2026-10-01: a committee
below quorum, so the chain advances only by escaping-stall blocks, and a rival
parent branch that forks just below the latest anchor and keeps pace with the
active tip. Backing the anchor down to the fork point would leave it below the
parent block's anchor, so the anchor would never move again, and an
escaping-stall block, which needs the anchor to advance, could never be made.

Topology: node0 = parent ("Bitcoin"); node1 = the lone staker, below quorum,
running -posproducer; node2 = a non-staking PoS peer for gossip connectivity;
node3 = a second parent node that mines the rival branch, whose blocks are
handed to node0 with submitblock. Both parent branches have the same length, so
node0 keeps its own branch active and reports the other as a competing tip.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_greater_than_or_equal, get_auth_cookie, get_datadir_path,
    rpc_port, p2p_port,
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
PARENT_BLOCK_SECONDS = 600   # parent-chain block spacing (one Bitcoin interval)
PARENT_BLOCKS_PER_ROUND = 4  # >= POS_ESCAPING_STALL_ANCHOR_GAP (3)


class PosEscapingStallContestedParentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.founder_wif, self.founder_pub = make_key()
        self.peer_wif, _ = make_key()     # not a registered staker; connectivity only

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []
        chain = "elementsregtest"

        def parent_args(i):
            return [
                "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
                "-validatepegin=0", "-initialfreecoins=0",
                "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1", "-signblockscript=51",
            ]

        self.add_nodes(1, [parent_args(0)], chain=[chain])
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
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-anchoravoidcontested=1", "-anchorcontestwindow=2",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        founder_args = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                                    "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        peer_args = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
                                 "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        self.add_nodes(1, [founder_args], chain=[chain])
        self.start_node(1)
        self.add_nodes(1, [peer_args], chain=[chain])
        self.start_node(2)
        self.connect_nodes(1, 2)

        # The rival parent node. It shares node0's history until the fork, then
        # mines its own branch.
        self.add_nodes(1, [parent_args(3)], chain=[chain])
        self.start_node(3)
        self.connect_nodes(0, 3)

        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.nodes[3].createwallet(wallet_name="w", descriptors=True)

    def step_time(self, *parents):
        self.parent_time += PARENT_BLOCK_SECONDS
        for p in parents:
            p.setmocktime(self.parent_time)

    def mine_active(self, blocks):
        """Mine `blocks` blocks on node0, PARENT_BLOCK_SECONDS apart.

        One block per call with the mocktime stepped in between, so the parent's
        median-time-past advances and the escaping-stall time evidence holds.
        """
        parent, rival = self.nodes[0], self.nodes[3]
        addr = parent.getnewaddress()
        for _ in range(blocks):
            self.step_time(parent, rival)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def mine_rival_to(self, height):
        """Extend node3's branch to `height` and hand each block to node0."""
        parent, rival = self.nodes[0], self.nodes[3]
        addr = rival.getnewaddress()
        while rival.getblockcount() < height:
            rival.setmocktime(self.parent_time)
            blockhash = self.generatetoaddress(rival, 1, addr, sync_fun=self.no_op)[0]
            parent.submitblock(rival.getblock(blockhash, 0))

    def rival_tip(self):
        return [t for t in self.nodes[0].getchaintips() if t['status'] != 'active']

    def run_test(self):
        parent, founder, peer, rival = self.nodes

        # Block 1: an ordinary escaping-stall block, with no fork in sight.
        self.mine_active(PARENT_BLOCKS_PER_ROUND)
        self.wait_until(lambda: rival.getblockcount() == parent.getblockcount(), timeout=30)
        self.wait_until(lambda: founder.getblockcount() >= 1, timeout=90)

        # Split the parent nodes. Everything up to here is common ground.
        self.disconnect_nodes(0, 3)
        fork_point = parent.getblockcount()
        self.log.info("Parent chain forks at height %d" % fork_point)

        # Block 2 anchors on node0's branch, above the fork point. Nothing is
        # contested yet: the rival branch does not exist.
        self.mine_active(PARENT_BLOCKS_PER_ROUND)
        self.wait_until(lambda: founder.getblockcount() >= 2, timeout=90)
        anchor2 = founder.getblockheader(founder.getblockhash(2))['anchorheight']
        assert_greater_than_or_equal(anchor2, fork_point + 1)
        self.log.info("Block 2 anchored at parent height %d, above the fork point" % anchor2)

        # Now the rival appears: a branch from the fork point, as long as the
        # active one. node0 keeps its own branch and reports the rival as a
        # live contest at every height above the fork point.
        self.mine_rival_to(parent.getblockcount())
        tips = self.rival_tip()
        assert_equal(len(tips), 1)
        assert_equal(tips[0]['height'] - tips[0]['branchlen'], fork_point)
        active_branch = parent.getblockhash(anchor2)

        # Both branches keep pace for several rounds. The committee is still
        # below quorum, so only an escaping-stall block can extend the chain, and
        # that needs the anchor to advance. Backing it down to the fork point
        # would pin it at block 2's anchor forever.
        for _ in range(PARENT_BLOCKS_PER_ROUND):
            self.mine_active(1)
            self.mine_rival_to(parent.getblockcount())
        assert_equal(parent.getblockhash(anchor2), active_branch)
        tips = self.rival_tip()
        assert_equal(len(tips), 1)
        assert_equal(tips[0]['height'], parent.getblockcount())

        self.wait_until(lambda: founder.getblockcount() >= 3, timeout=90)
        header3 = founder.getblockheader(founder.getblockhash(3))
        assert_greater_than_or_equal(header3['anchorheight'], anchor2 + 3)
        # It followed the branch it was committed to.
        assert_equal(parent.getblockhash(header3['anchorheight']), header3['anchorhash'])
        self.log.info("Block 3 anchored at parent height %d on the committed branch, with the rival still live"
                      % header3['anchorheight'])

        self.wait_until(lambda: peer.getblockcount() >= 3, timeout=60)
        assert_equal(founder.getblockhash(3), peer.getblockhash(3))


if __name__ == '__main__':
    PosEscapingStallContestedParentTest().main()
