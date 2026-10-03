#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Two equivocating committee members can split a majority-quorum committee;
the finality observation window heals it.

Committee of 4, quorum 3, so any two quorums overlap in exactly two members.
Two of the four staking keys are "Byzantine": each runs on TWO nodes, one in
each half of the network, so each key signs whatever block its half backs —
exactly an attacker that signs both of two competing blocks. The two honest
members are put on different halves and briefly disconnected, which stands in
for the timing split an attacker provokes with a late proposal (paper §6.2):
each half certifies a different block at the same height with one honest
signature plus the two double-signers. The halves are reconnected as soon as
both certificates exist, so every node sees both within a second.

The same scenario runs on two independent networks:
  - finality on connection (-posfinalitydelayms=0, the old rule): each half
    keeps the block it certified first, the finality gate refuses the other,
    and both branches keep growing — a permanent split;
  - the default observation window (3 s): both certificates meet inside the
    window, the deterministic comparator picks the same block everywhere, and
    the network converges on one chain.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

GROUP = 6          # H1, H2, Z1a, Z2a, Z1b, Z2b
H1, H2, Z1A, Z2A, Z1B, Z2B = range(GROUP)


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosSplitEquivocationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2 * GROUP
        self.setup_clean_chain = True
        # Four staking keys: honest H1, H2 and Byzantine Z1, Z2. Equal weights,
        # committee 4, quorum 3.
        self.stakers = [make_staker() for _ in range(4)]
        key_of = {H1: 0, H2: 1, Z1A: 2, Z1B: 2, Z2A: 3, Z2B: 3}
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-posblockspacing=5", "-posblockspacingheight=1",
            "-con_max_block_sig_size=4000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = []
        for group, delay in ((0, 0), (1, 3000)):
            for role in range(GROUP):
                wif = self.stakers[key_of[role]][0]
                self.extra_args.append(common + [
                    "-posproducer", "-posproducerkey=%s" % wif,
                    "-posfinalitydelayms=%d" % delay,
                ])

    def node(self, group, role):
        return self.nodes[group * GROUP + role]

    def setup_network(self):
        self.setup_nodes()
        for g in (0, 1):
            o = g * GROUP
            for a, b in ((H1, Z1A), (H1, Z2A), (Z1A, Z2A),      # half A
                         (H2, Z1B), (H2, Z2B), (Z1B, Z2B),      # half B
                         (H1, H2)):                             # the bridge
                self.connect_nodes(o + a, o + b)

    def provoke_split(self, g):
        """Partition the honest members, let each half certify a block at the
        same height, reconnect at once. Returns that height."""
        h1, h2 = self.node(g, H1), self.node(g, H2)
        # When a Byzantine key holds the best VRF, its two copies propose the
        # same block and both halves back it: no split. Like the attacker, just
        # try again at the next height.
        for attempt in range(1, 21):
            self.wait_until(lambda: h1.getblockcount() >= 3 and
                            h1.getbestblockhash() == h2.getbestblockhash(), timeout=180)
            self.disconnect_nodes(g * GROUP + H1, g * GROUP + H2)
            base = max(h1.getblockcount(), h2.getblockcount())
            target = base + 1
            self.wait_until(lambda: h1.getblockcount() >= target and h2.getblockcount() >= target, timeout=120)
            # Read both certified blocks BEFORE reconnecting: with the
            # observation window a split heals within a second of contact.
            a, b = h1.getblockhash(target), h2.getblockhash(target)
            if a != b:
                for n, name in ((h1, "H1"), (h2, "H2")):
                    hdr = n.getblockheader(n.getblockhash(target))
                    self.log.info("  %s certified %s at height %d with %s countersignatures",
                                  name, hdr["hash"][:16], target, hdr.get("poscountersigs"))
            self.connect_nodes(g * GROUP + H1, g * GROUP + H2)
            if a != b:
                self.log.info("  split obtained at attempt %d", attempt)
                break
            self.log.info("  attempt %d: both halves certified the same block, retrying", attempt)
        else:
            raise AssertionError("no split obtained in 20 attempts")
        return target

    def run_test(self):
        self.log.info("Network 1: finality on connection (old rule)")
        h_old = self.provoke_split(0)
        n1, n2 = self.node(0, H1), self.node(0, H2)
        self.wait_until(lambda: n1.getblockcount() >= h_old + 3 and n2.getblockcount() >= h_old + 3, timeout=180)
        assert n1.getblockhash(h_old) != n2.getblockhash(h_old)
        self.log.info("  split persists: both branches grew to %d and %d blocks, still different at height %d",
                      n1.getblockcount(), n2.getblockcount(), h_old)

        self.log.info("Network 2: 3-second finality observation window")
        h_new = self.provoke_split(1)
        m1, m2 = self.node(1, H1), self.node(1, H2)
        t0 = time.time()
        self.wait_until(lambda: m1.getblockhash(h_new) == m2.getblockhash(h_new), timeout=30)
        self.log.info("  converged on %s at height %d after %.1f s",
                      m1.getblockhash(h_new)[:16], h_new, time.time() - t0)
        target = h_new + 3
        self.wait_until(lambda: all(self.node(1, r).getblockcount() >= target for r in range(GROUP)), timeout=180)
        tips = {self.node(1, r).getblockhash(target) for r in range(GROUP)}
        assert len(tips) == 1, "network 2 did not stay on one chain"
        self.log.info("  all six nodes on one chain at height %d", target)


if __name__ == '__main__':
    PosSplitEquivocationTest().main()
