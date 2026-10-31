#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Block shares on a committee shaped like the public testnet.

One key holds about 52% of all stake and, because a quarter of the stake has no
BLS registration, about 69% of the seated stake: a quorum of seats by itself at
every height. Seven small keys on four other hosts hold the rest of the seated
stake; one more key (25% of all stake, no BLS registration) runs on no host.

The test lets the chain run and reports how many blocks each key led against
its share of the stake that is being run. With --holderbinary and
--smallbinary it runs a mixed fleet and checks that every node ends on the same
chain with no rival certified block anywhere.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


BIG_W = 52
NOBLS_W = 25
SMALL_W = [[4, 4], [3, 3], [3, 3], [3]]   # per host (node1..node4)


class PosQuorumHolderFairnessTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--holderbinary", dest="holderbinary", default=None)
        parser.add_argument("--smallbinary", dest="smallbinary", default=None)
        parser.add_argument("--blocks", dest="blocks", type=int, default=210)

    def set_test_params(self):
        self.num_nodes = 5
        self.setup_clean_chain = True
        self.big = make_staker()
        self.nobls = make_staker()
        self.small = [[make_staker() for _ in ws] for ws in SMALL_W]
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=8", "-posexpraceheight=1",
            "-posslotinterval=1", "-par=1",
            "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        idle = self.common + ["-staker=%s:%d" % (self.big[1], BIG_W), "-staker=%s:%d" % (self.nobls[1], NOBLS_W)]
        for host, ws in zip(self.small, SMALL_W):
            idle += ["-staker=%s:%d" % (p, w) for (_, p), w in zip(host, ws)]
        self.extra_args = [list(idle) for _ in range(self.num_nodes)]

    def setup_nodes(self):
        b = self.options.bitcoind
        bins = [self.options.holderbinary or b] + [self.options.smallbinary or b] * 4
        self.add_nodes(self.num_nodes, self.extra_args, binary=bins)
        self.start_nodes()

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        n0 = self.nodes[0]
        reg = lambda wif: n0.getblsregistration(wif)["spec"]
        specs = ["-staker=%s:%d%s" % (self.big[1], BIG_W, reg(self.big[0])),
                 "-staker=%s:%d" % (self.nobls[1], NOBLS_W)]
        for host, ws in zip(self.small, SMALL_W):
            specs += ["-staker=%s:%d%s" % (p, w, reg(wif)) for (wif, p), w in zip(host, ws)]
        full = self.common + specs

        self.restart_node(0, extra_args=full + ["-posproducer", "-posproducerkey=%s" % self.big[0]])
        for i, host in enumerate(self.small, start=1):
            self.restart_node(i, extra_args=full + ["-posproducer"] + ["-posproducerkey=%s" % w for w, _ in host])
        for i in range(self.num_nodes):
            for j in range(i + 1, self.num_nodes):
                self.connect_nodes(i, j)

        self.wait_until(lambda: n0.getblockcount() >= 2, timeout=120)
        hdr = n0.getblockheader(n0.getblockhash(2))
        self.log.info("height 2: posquorum %s" % hdr.get("posquorum"))
        assert_equal(hdr["posquorum"], 5)   # 8 seats, quorum 5; the big key holds 5 or 6

        start_h = n0.getblockcount()
        target = start_h + self.options.blocks
        t0 = time.time()
        self.wait_until(lambda: min(n.getblockcount() for n in self.nodes) >= target, timeout=self.options.blocks * 12)
        elapsed = time.time() - t0
        self.sync_blocks(timeout=120)

        # Same chain everywhere, and no rival tip anywhere.
        best = {n.getbestblockhash() for n in self.nodes}
        assert_equal(len(best), 1)
        for i, n in enumerate(self.nodes):
            tips = n.getchaintips()
            others = [t for t in tips if t["status"] != "active"]
            self.log.info("node%d chain tips besides the active one: %s" % (i, others))
            assert_equal(others, [])

        recent = n0.getposrecentblocks(self.options.blocks)["blocks"]
        assert_equal(len(recent), self.options.blocks)
        counts = {}
        for b in recent:
            counts[b["producer"]] = counts.get(b["producer"], 0) + 1
            assert n0.getblockheader(b["hash"])["poscertified"]
        live_w = BIG_W + sum(sum(ws) for ws in SMALL_W)
        N = self.options.blocks
        self.log.info("%d blocks in %.0f s (%.2f s/block)" % (N, elapsed, elapsed / N))
        rows = [("big (node0)", self.big[1], BIG_W)]
        for i, (host, ws) in enumerate(zip(self.small, SMALL_W), start=1):
            for (_, p), w in zip(host, ws):
                rows.append(("small node%d" % i, p, w))
        rows.append(("no-BLS, unrun", self.nobls[1], NOBLS_W))
        chi2 = 0.0
        for label, p, w in rows:
            got = counts.get(p, 0)
            exp = N * w / live_w if p != self.nobls[1] else 0
            if exp > 0:
                chi2 += (got - exp) ** 2 / exp
            self.log.info("  %-14s weight %2d  led %3d  expected %5.1f" % (label, w, got, exp))
        small_total = sum(counts.get(p, 0) for host in self.small for _, p in host)
        self.log.info("small keys together led %d of %d (expected %.1f); chi-square %.2f on %d dof" %
                      (small_total, N, N * (live_w - BIG_W) / live_w, chi2, len(rows) - 2))
        self.small_total = small_total


if __name__ == '__main__':
    PosQuorumHolderFairnessTest().main()
