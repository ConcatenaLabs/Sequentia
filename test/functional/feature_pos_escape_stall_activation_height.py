#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The escaping-stall MTP rule must apply ONLY from its activation height.

Why the gate exists. The parent-chain median-time-past requirement for
sub-quorum (escaping-stall) blocks was added in response to the 2026-07-17
finality partition — after the testnet chain had already produced blocks that do
not satisfy it. Enforced from genesis, those blocks became unvalidatable: a node
syncing from scratch stopped at testnet block 1757 for ever and could never join
the network, while already-running nodes survived only because a block already in
the chainstate is never revalidated. That makes their state unreproducible: a
-reindex, a restore from backup or a disk failure and they cannot start again.

Gating the rule by height is the standard soft-fork treatment. This test pins
both sides of that boundary, because a gate that is silently ineffective in
either direction is worse than no gate: too low and the chain stays unsyncable,
too high and a real consensus rule is quietly not enforced.

  below the gate  a sub-quorum block WITHOUT the parent-chain time gap is
                  ACCEPTED (this is the history that already exists)
  at/above it     the same block is REJECTED with bad-pos-escape-stall-too-soon

The rule is also checked to be genuinely OFF at gate 0, which is the convention
shared with pos_exprace_height: 0 = not gated, a positive H = enforced from H.
A chain launched with the rule in place uses 1, never 0 (CONTRIBUTING.md).

The gate lives inside CheckEscapingStallMtpGap (anchor.cpp) rather than at its
call sites, so that a future call site cannot omit it and re-apply the rule
retroactively. This test drives it through real block production, so it covers
the function wherever it is reached from.

Topology mirrors feature_pos_autonomous_escaping_stall.py: node0 = parent
("Bitcoin"); node1 = the founder PoS node (sole genesis staker, sub-quorum by
construction); node2 = a non-staking peer providing gossip connectivity.
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
COMMITTEE = 3          # quorum 2 -> the lone founder is always sub-quorum
MTP_GAP = 600          # -posescapestallmtpgap, the rule under test
PARENT_BLOCK_SECONDS = 600   # parent-chain block spacing (one Bitcoin interval)

# The founder alone can only ever produce escaping-stall blocks, so every block
# it makes is a probe of the rule. Activating at height 3 leaves blocks 1-2
# below the gate and block 3 at it.
ACTIVATION_HEIGHT = 3


class PosEscapeStallActivationHeightTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.founder_wif, self.founder_pub = make_key()
        self.peer_wif, _ = make_key()
        self.mocktime = int(time.time())

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def base_consensus(self, rpc_u, rpc_p, activation_height):
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
            "-posescapestallmtpheight=%d" % activation_height,
            "-mainchainrpctimeout=5",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]

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
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        self.rpc_u, self.rpc_p = rpc_u, rpc_p

        consensus = self.base_consensus(rpc_u, rpc_p, ACTIVATION_HEIGHT)
        founder = consensus + ["-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
                               "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        peer = consensus + ["-port=%d" % p2p_port(2), "-rpcport=%d" % rpc_port(2),
                            "-posproducer=1", "-posproducerkey=%s" % self.peer_wif]
        self.add_nodes(1, [founder], chain=[chain])
        self.start_node(1)
        self.add_nodes(1, [peer], chain=[chain])
        self.start_node(2)
        self.connect_nodes(1, 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    # --- helpers -----------------------------------------------------------

    def bump_parent_height_only(self, nblocks=4):
        """Advance the parent's HEIGHT without advancing its median-time-past.

        Every block gets the same mock timestamp, so the anchor height gap that
        PosEscapingStallAllowed wants is satisfied while the MTP gap the rule
        wants is not. That is exactly the shape of the pre-rule history this
        gate has to keep valid.
        """
        parent = self.nodes[0]
        parent.setmocktime(self.mocktime)
        self.generatetoaddress(parent, nblocks, parent.getnewaddress(), sync_fun=self.no_op)

    def bump_parent_with_time(self, nblocks=12, step=PARENT_BLOCK_SECONDS):
        """Advance the parent so its MTP really moves (satisfies the rule).

        Median-time-past is the MEDIAN of the last 11 block times, so it tracks
        the tip five blocks back: `nblocks` blocks `step` seconds apart move it
        by only (nblocks - 5) * step, and the producer anchors a block or two
        behind the tip on top of that. A step smaller than MTP_GAP therefore
        buys far less evidence than it looks like it does — mine at a real
        Bitcoin interval, as the other escaping-stall tests do.
        """
        parent = self.nodes[0]
        addr = parent.getnewaddress()
        for _ in range(nblocks):
            self.mocktime += step
            parent.setmocktime(self.mocktime)
            self.generatetoaddress(parent, 1, addr, sync_fun=self.no_op)

    def log_since(self, node_idx, mark):
        with open(self.nodes[node_idx].debug_log_path, 'rb') as f:
            f.seek(mark)
            return f.read().decode('utf-8', errors='replace')

    # --- test --------------------------------------------------------------

    def run_test(self):
        parent, founder, peer = self.nodes

        assert_equal(founder.getstakerinfo().get(self.founder_pub), SEED_STAKE)
        assert_equal(founder.getblockcount(), 0)

        # ---- BELOW the gate: no MTP evidence, block must still be accepted --
        # Heights 1 and 2 are below ACTIVATION_HEIGHT. The parent only gains
        # height, never time, so the MTP gap is NOT satisfied. A node enforcing
        # the rule here would stall exactly as the live testnet did at 1757.
        mark = self.nodes[1].debug_log_bytes()
        for target in (1, 2):
            self.bump_parent_height_only()
            self.wait_until(lambda: founder.getblockcount() >= target, timeout=120)
        assert_equal(founder.getblockcount(), 2)

        below = self.log_since(1, mark)
        assert "bad-pos-escape-stall-too-soon" not in below, (
            "the rule was enforced BELOW its activation height - pre-rule history "
            "would be unvalidatable and the chain unsyncable from genesis")
        self.log.info("below the gate: reached height 2 with no parent-chain time gap, as required")

        # ---- AT the gate: the same block shape must now be refused ---------
        # Height 3 == ACTIVATION_HEIGHT. Still no MTP movement, so the rule now
        # has to bite and the chain must NOT advance.
        mark = self.nodes[1].debug_log_bytes()
        self.bump_parent_height_only()
        deadline = time.time() + 45
        while time.time() < deadline:
            time.sleep(3)
            if founder.getblockcount() > 2:
                raise AssertionError(
                    "height 3 was produced without the parent-chain time gap: the rule "
                    "is NOT enforced at its activation height")
        assert_equal(founder.getblockcount(), 2)
        self.log.info("at the gate: height 3 correctly withheld without the time gap")

        # ---- AT the gate, WITH evidence: must be accepted ------------------
        # Same height, same rule, but now the parent's median-time-past really
        # advances. This separates "the gate is on" from "the chain is simply
        # stuck", which the check above cannot distinguish on its own.
        self.bump_parent_with_time()
        # The evidence really is there now: the parent's MTP has moved at least
        # a full MTP_GAP past the MTP of the anchor block 2 is measured from.
        # Asserted rather than assumed, so a step too small for the median
        # window shows up as "the test did not supply evidence" instead of
        # masquerading as "the rule refuses a block it should accept".
        anchor2 = founder.getblockheader(founder.getblockhash(2))['anchorhash']
        mtp_from = parent.getblockheader(anchor2)['mediantime']
        mtp_now = parent.getblockheader(parent.getbestblockhash())['mediantime']
        assert mtp_now - mtp_from >= MTP_GAP, (
            "the test supplied only %d s of parent median-time-past movement, "
            "less than the %d s the rule requires" % (mtp_now - mtp_from, MTP_GAP))
        self.wait_until(lambda: founder.getblockcount() >= 3, timeout=180)
        self.log.info("at the gate: height 3 accepted once the time gap is real")

        # The peer follows the same chain: no fork was created either way.
        self.wait_until(lambda: peer.getblockcount() >= 3, timeout=90)
        assert_equal(founder.getblockhash(3), peer.getblockhash(3))

        # ---- gate 0 means NOT GATED (rule off), not "always on" ------------
        # The convention is shared with pos_exprace_height and is easy to invert
        # by accident; an inverted reading would exempt early mainnet blocks from
        # a rule meant to prevent finality partitions.
        self.log.info("restarting the founder with -posescapestallmtpheight=0 (rule off)")
        args0 = self.base_consensus(self.rpc_u, self.rpc_p, 0) + [
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-posproducer=1", "-posproducerkey=%s" % self.founder_wif]
        self.restart_node(1, extra_args=args0)
        founder = self.nodes[1]
        self.connect_nodes(1, 2)

        mark = self.nodes[1].debug_log_bytes()
        target = founder.getblockcount() + 1
        self.bump_parent_height_only()          # height only: no MTP evidence
        self.wait_until(lambda: founder.getblockcount() >= target, timeout=120)
        off = self.log_since(1, mark)
        assert "bad-pos-escape-stall-too-soon" not in off, (
            "-posescapestallmtpheight=0 still enforced the rule: 0 must mean NOT "
            "GATED, matching pos_exprace_height")
        self.log.info("gate 0: rule off, height %d produced with no time gap"
                      % founder.getblockcount())

        self.log.info("PASS: the escaping-stall MTP rule applies only from its "
                      "activation height, and 0 means not gated")


if __name__ == '__main__':
    PosEscapeStallActivationHeightTest().main()
