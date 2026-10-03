#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""PoS same-height fork choice among NOT-YET-FINAL blocks (whitepaper §3.8).

Sequentia's ordering of the three rules is fixed (Theoretical Paper §5/§6/§11,
and doc/sequentia/04-proof-of-stake.md):

    Bitcoin anchoring  >  checkpoints  >  immediate finality

so the comparator this test covers is the LAST word only where neither
candidate is final yet:

  * A quorum-certified block is FINAL against every Sequentia-internal
    competitor once its observation window has passed, including a sibling
    whose certificate names MORE members. That case is feature_pos_finality.py,
    and the answer there is "no reorg".
  * The only thing that may undo a final block is BITCOIN. That case is
    feature_pos_finalized_anchor_reorg.py (and, at depth,
    feature_pos_deep_anchor_reorg.py).
  * Between blocks that are NOT final, CBlockIndexWorkComparator decides:
    a certified block (its certificate names at least the quorum) beats an
    uncertified one, then the lower leader VRF score wins, then the lower
    block hash, regardless of arrival order. The number of members a
    certificate names is never a key: in the BLS forms one block can reach two
    nodes with certificates of different sizes (feature_pos_split_equivocation
    covers that), so ranking by it could order the same two blocks oppositely
    on two nodes.

Committee size 5 ⇒ quorum 3. Height-2 siblings are built on a shared
quorum-certified height-1 parent, all by the same leader, so their VRF scores
are equal and the hash breaks ties: two sub-quorum siblings, certified by the
leader alone (1) and by the leader plus a member (2), which the escaping stall
permits once the Bitcoin anchor has advanced; and a certified sibling (3). The
weak sibling is rebuilt (on a fresh anchor) until its hash is the lower of the
two, so that ranking by count would pick the other, and the certified sibling
until its hash is the highest, so that only certification can make it win. Each pair is exposed in
both arrival orders, and the last once more after a restart, when whether a
block is certified is measured again from the stored countersignature count.

Topology mirrors feature_pos_escaping_stall.py: node0 is the parent
("Bitcoin") chain, node1 is the anchored PoS chain. The escaping stall demands
real parent-chain time as well as height, so parent blocks are mined at a
realistic Bitcoin cadence with setmocktime instead of switching the rule off.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, get_auth_cookie, get_datadir_path, rpc_port, p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

COMMITTEE_SIZE = 5           # quorum = 3, so 1 and 2 countersignatures are both sub-quorum
PARENT_BLOCK_SECONDS = 600   # parent-chain block spacing (one Bitcoin interval)
# Median-time-past is the median of the last 11 block times, so it only tracks
# the spacing one-for-one once that window is full: pre-grow past it before
# measuring any gap.
PARENT_WARMUP_BLOCKS = 12


def hash_order(h):
    """The order of uint256::operator<, which the comparator's hash key uses:
    the stored bytes, least significant first, i.e. the displayed hex reversed."""
    return bytes.fromhex(h)[::-1]


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosForkChoiceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.stakers = [make_staker() for _ in range(COMMITTEE_SIZE)]

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
        self.parent_time = self.nodes[0].getblockheader(self.parentgenesis)['time']

        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, parent_chain)
        anchored_args = [
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-validatepegin=0", "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1",
            "-poscommitteesize=%d" % COMMITTEE_SIZE, "-posslotinterval=1",
            "-con_blocksubsidy=5000000000",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1",
            "-anchorminconf=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.add_nodes(1, [anchored_args], chain=[parent_chain])
        self.start_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def advance_parent(self, blocks):
        """Mine `blocks` parent blocks, PARENT_BLOCK_SECONDS apart.

        One block per call with the parent's mocktime stepped in between: a
        single multi-block generate stamps them all with the same time, which
        leaves median-time-past standing still and starves the escaping-stall
        real-time evidence.
        """
        parent = self.nodes[0]
        addr = parent.getnewaddress()
        for _ in range(blocks):
            self.parent_time += PARENT_BLOCK_SECONDS
            parent.setmocktime(self.parent_time)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def sibling(self, node, leader, members, parent_hash, better_than=None, worse_than=None):
        """Build a height-2 sibling on `parent_hash` (the tip must be the parent),
        rebuilding it on a fresh anchor until its hash compares as asked."""
        for _ in range(30):
            res = node.generateposblock(leader, members)
            h = res['hash']
            assert_equal(node.getblockheader(h)['previousblockhash'], parent_hash)
            if (better_than is None or hash_order(h) < hash_order(better_than)) and \
               (worse_than is None or hash_order(h) > hash_order(worse_than)):
                return res
            node.invalidateblock(h)
            self.advance_parent(1)
            self.wait_until(lambda: node.getbestblockhash() == parent_hash)
        raise AssertionError("no sibling with the wanted hash order in 30 attempts")

    def run_test(self):
        node = self.nodes[1]
        wifs = [w for w, _ in self.stakers]
        leader = wifs[0]

        # Parent chain deep enough to anchor to and to fill the MTP window.
        self.advance_parent(PARENT_WARMUP_BLOCKS)

        # --- Shared height-1 parent, quorum-certified (leader + 2 members = 3). ---
        res_p = node.generateposblock(leader, wifs[1:3])
        assert_equal(res_p['countersignatures'], 3)
        parent_hash = res_p['hash']
        hdr_p = node.getblockheader(parent_hash)
        assert_equal(hdr_p['posquorum'], 3)
        assert_equal(hdr_p['poscertified'], True)   # this block IS final

        # --- Open the escaping stall: +3 anchor blocks, 600 s apart, so
        # sub-quorum height-2 blocks become acceptable on this parent. ---
        self.advance_parent(3)

        self.log.info("Two uncertified siblings: the lower hash wins, not the larger certificate")
        res_s = node.generateposblock(leader, wifs[1:2])
        strong = res_s['hash']
        assert_equal(res_s['countersignatures'], 2)
        assert_equal(node.getblockheader(strong)['poscertified'], False)
        node.invalidateblock(strong)
        assert_equal(node.getbestblockhash(), parent_hash)
        res_w = self.sibling(node, leader, [], parent_hash, better_than=strong)
        weak = res_w['hash']
        assert_equal(res_w['countersignatures'], 1)
        assert_equal(node.getblockheader(weak)['poscertified'], False)
        assert_equal(node.getbestblockhash(), weak)
        # Weak is the tip; the 2-member sibling arrives: the lower hash stays.
        node.reconsiderblock(strong)
        assert_equal(node.getbestblockhash(), weak)
        # And the other arrival order: strong the tip first, weak arrives.
        node.invalidateblock(weak)
        assert_equal(node.getbestblockhash(), strong)
        node.reconsiderblock(weak)
        assert_equal(node.getbestblockhash(), weak)
        assert_equal(node.getblockhash(1), parent_hash)

        self.log.info("A certified sibling beats both, though its hash is the highest")
        node.invalidateblock(weak)
        node.invalidateblock(strong)
        assert_equal(node.getbestblockhash(), parent_hash)
        res_c = self.sibling(node, leader, wifs[1:3], parent_hash, worse_than=max(weak, strong, key=hash_order))
        cert = res_c['hash']
        assert_equal(res_c['countersignatures'], 3)
        assert_equal(node.getblockheader(cert)['poscertified'], True)
        # Inside the certified block's observation window the comparator, not
        # the finality gate, decides.
        node.reconsiderblock(weak)
        node.reconsiderblock(strong)
        assert_equal(node.getbestblockhash(), cert)
        # The other arrival order: an uncertified tip, the certified block arrives.
        node.invalidateblock(cert)
        assert_equal(node.getbestblockhash(), weak)
        node.reconsiderblock(cert)
        assert_equal(node.getbestblockhash(), cert)

        # No reorg crossed the finalized point: the quorum-certified height-1
        # parent is still on the chain, untouched.
        assert_equal(node.getblockhash(1), parent_hash)

        self.log.info("After a restart the keys are measured again: the certified sibling still wins")
        self.restart_node(1)
        node = self.nodes[1]
        assert_equal(node.getbestblockhash(), cert)
        node.invalidateblock(cert)
        assert_equal(node.getbestblockhash(), weak)
        node.reconsiderblock(cert)
        assert_equal(node.getbestblockhash(), cert)


if __name__ == '__main__':
    PosForkChoiceTest().main()
