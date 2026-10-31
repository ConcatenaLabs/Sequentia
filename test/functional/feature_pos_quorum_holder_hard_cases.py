#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Hard cases for a producer that holds a committee quorum alone.

node0 holds one staker of weight 9: 3 of 4 seats, the quorum of 4. node1 holds
three stakers of weight 1. node2 stakes nothing and watches. --case picks one:

  baseline   the chain runs; block intervals for reference
  leaderdies node1 is killed (SIGKILL) the moment it logs a proposal
  invalid    node1 proposes only consensus-invalid blocks
  partition  node0 drops every peer at a point in its own round
  stalelock  node0 drops its peers, then the block it just signed is rolled
             back (as an anchor reorg would): can it produce at that height?
  skew       node1's clock runs --skew seconds fast (needs --smallbinary to be
             a faketime wrapper)
  crash      node0 is killed (SIGKILL) the moment it certifies a block and
             restarts with no peers: does it certify a rival at that height?
"""

import os
import re
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class LogTail:
    def __init__(self, node):
        self.path = node.debug_log_path
        self.f = open(self.path, encoding='utf-8', errors='replace')
        self.f.seek(0, os.SEEK_END)
        self.buf = ""

    def wait(self, pattern, timeout=120):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.f.read()
            if chunk:
                self.buf += chunk
                lines = self.buf.split("\n")
                self.buf = lines.pop()
                for ln in lines:
                    m = rx.search(ln)
                    if m:
                        return m
            else:
                time.sleep(0.003)
        raise AssertionError("no log line matching %r in %d s" % (pattern, timeout))

    def close(self):
        self.f.close()


class PosQuorumHolderHardCasesTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--case", dest="case", default="baseline")
        parser.add_argument("--holderbinary", dest="holderbinary", default=None)
        parser.add_argument("--smallbinary", dest="smallbinary", default=None)
        parser.add_argument("--iterations", dest="iterations", type=int, default=10)
        parser.add_argument("--skew", dest="skew", type=int, default=30)
        parser.add_argument("--dial", dest="dial", action="store_true", default=False)
        parser.add_argument("--observe", dest="observe", type=int, default=90)
        parser.add_argument("--no-partition", dest="partition", action="store_false", default=True)

    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.big = make_staker()
        self.small = [make_staker() for _ in range(3)]
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=4", "-posexpraceheight=1",
            "-posslotinterval=1", "-par=1",
            "-con_max_block_sig_size=8000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        idle = self.common + ["-staker=%s:9" % self.big[1]] + ["-staker=%s:1" % p for _, p in self.small]
        self.extra_args = [list(idle) for _ in range(self.num_nodes)]

    def setup_nodes(self):
        b = self.options.bitcoind
        bins = [self.options.holderbinary or b, self.options.smallbinary or b, b]
        self.add_nodes(self.num_nodes, self.extra_args, binary=bins)
        self.start_nodes()

    def setup_network(self):
        self.setup_nodes()

    # --- helpers -----------------------------------------------------------
    def args_for(self, i, extra=()):
        if i == 0:
            return self.full + ["-posproducer", "-posproducerkey=%s" % self.big[0]] + list(extra)
        if i == 1:
            return self.full + ["-posproducer"] + ["-posproducerkey=%s" % w for w, _ in self.small] + list(extra)
        return self.full + list(extra)

    def mesh(self):
        for i in range(self.num_nodes):
            for j in range(i + 1, self.num_nodes):
                if self.nodes[i].running and self.nodes[j].running:
                    self.connect_nodes(i, j)

    def sigkill(self, i):
        n = self.nodes[i]
        n.process.kill()
        n.process.wait()
        n.running = False
        n.process = None
        n.rpc_connected = False
        n.rpc = None

    def producer_at(self, node, height):
        h = node.getblockhash(height)
        tip = node.getblockcount()
        for b in node.getposrecentblocks(tip - height + 1)["blocks"]:
            if b["height"] == height:
                return h, b["producer"]
        raise AssertionError("height %d not in recent blocks" % height)

    def who(self, pub):
        if pub == self.big[1]:
            return "node0"
        if pub in {p for _, p in self.small}:
            return "node1"
        return pub[:10]

    def certified_rivals(self):
        """Every (height, hash) known to any node that is not on node2's chain."""
        rivals = []
        for i, n in enumerate(self.nodes):
            if not n.running:
                continue
            for t in n.getchaintips():
                if t["status"] == "active":
                    continue
                hdr = n.getblockheader(t["hash"])
                rivals.append((i, t["height"], t["hash"][:16], t["status"], hdr.get("poscertified")))
        return rivals

    def run_blocks(self, count, timeout_per=15):
        n2 = self.nodes[2]
        start = n2.getblockcount()
        times = {}
        last = start
        t_end = time.time() + count * timeout_per
        while last < start + count and time.time() < t_end:
            c = n2.getblockcount()
            if c > last:
                now = time.time()
                for h in range(last + 1, c + 1):
                    times[h] = now
                last = c
            time.sleep(0.02)
        assert last >= start + count, "chain stalled at %d" % last
        rows = []
        for h in range(start + 2, start + count + 1):
            _, prod = self.producer_at(n2, h)
            rows.append((h, self.who(prod), times[h] - times[h - 1],
                         n2.getblockheader(n2.getblockhash(h))["time"] - n2.getblockheader(n2.getblockhash(h - 1))["time"]))
        return rows

    def summarize(self, rows, label):
        by = {}
        for _, w, dt, _ in rows:
            by.setdefault(w, []).append(dt)
        for w, dts in sorted(by.items()):
            dts.sort()
            self.log.info("%s: %s led %d blocks, wall interval median %.2f s max %.2f s" %
                          (label, w, len(dts), dts[len(dts) // 2], dts[-1]))

    # --- the test ------------------------------------------------------------
    def run_test(self):
        n0 = self.nodes[0]
        specs = ["-staker=%s:9%s" % (self.big[1], n0.getblsregistration(self.big[0])["spec"])]
        specs += ["-staker=%s:1%s" % (p, n0.getblsregistration(w)["spec"]) for w, p in self.small]
        self.full = self.common + specs
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.args_for(i))
        self.mesh()
        self.wait_until(lambda: self.nodes[2].getblockcount() >= 3, timeout=120)
        assert_equal(n0.getblockheader(n0.getblockhash(3))["posquorum"], 3)
        getattr(self, "case_" + self.options.case)()
        rivals = self.certified_rivals()
        self.log.info("non-active tips at the end: %s" % rivals)

    def case_baseline(self):
        rows = self.run_blocks(self.options.iterations)
        self.summarize(rows, "baseline")

    def case_leaderdies(self):
        results = []
        for it in range(self.options.iterations):
            tail = LogTail(self.nodes[1])
            m = tail.wait(r"PoS gossip: proposing block ([0-9a-f]{64}) at height (\d+)")
            self.sigkill(1)
            t_kill = time.time()
            tail.close()
            prop, h = m.group(1), int(m.group(2))
            self.wait_until(lambda: self.nodes[0].getblockcount() >= h, timeout=120)
            dt = time.time() - t_kill
            got, prod = self.producer_at(self.nodes[0], h)
            results.append((h, dt, got == prop, self.who(prod)))
            self.log.info("leaderdies %d: height %d, next block %.2f s after the kill, "
                          "dead leader's block certified: %s, led by %s" % (it, h, dt, got == prop, self.who(prod)))
            self.start_node(1, extra_args=self.args_for(1))
            self.mesh()
            self.sync_blocks()
        worst = max(r[1] for r in results)
        self.log.info("leaderdies: worst %.2f s; dead leader's block certified %d of %d" %
                      (worst, sum(1 for r in results if r[2]), len(results)))

    def case_invalid(self):
        self.restart_node(1, extra_args=self.args_for(1, ["-posbyzantineinvalid=1"]))
        self.mesh()
        rows = self.run_blocks(self.options.iterations)
        self.summarize(rows, "invalid")
        assert self.nodes[1].is_node_stopped() is False
        with open(self.nodes[1].debug_log_path, encoding='utf-8') as f:
            n_bad = sum(1 for ln in f if "BYZANTINE proposing invalid block" in ln)
        self.log.info("invalid: node1 sent %d invalid proposals" % n_bad)

    def case_partition(self):
        n0 = self.nodes[0]
        delays = [0.0, 0.2, 0.4, 0.55, 0.65, 0.8, 1.0, 1.5]
        for it in range(self.options.iterations):
            d = delays[it % len(delays)]
            tail = LogTail(n0)
            m = tail.wait(r"PoS gossip: proposing block ([0-9a-f]{64}) at height (\d+)")
            tail.close()
            prop, h = m.group(1), int(m.group(2))
            time.sleep(d)
            n0.setnetworkactive(False)
            t_cut = time.time()
            self.wait_until(lambda: n0.getblockcount() >= h, timeout=120)
            dt = time.time() - t_cut
            got, prod = self.producer_at(n0, h)
            others = [n.getblockhash(h) if n.getblockcount() >= h else None for n in self.nodes[1:]]
            self.log.info("partition %d: cut %.2f s after proposing at %d; block at %d %.2f s after the cut, "
                          "own proposal: %s, led by %s; peers hold at %d: %s" %
                          (it, d, h, h, dt, got == prop, self.who(prod), h,
                           ["same" if o == got else (o[:12] if o else "none") for o in others]))
            time.sleep(1.5)
            n0.setnetworkactive(True)
            self.mesh()
            self.sync_blocks(timeout=60)
            r = self.certified_rivals()
            if r:
                self.log.info("partition %d: rival tips %s" % (it, r))

    def case_stalelock(self):
        n0 = self.nodes[0]
        for it in range(self.options.iterations):
            # Wait for a block that node0 share-signed: any block certified while
            # it is in the round.
            h0 = n0.getblockcount()
            self.wait_until(lambda: n0.getblockcount() > h0, timeout=60)
            if self.options.partition:
                n0.setnetworkactive(False)
            tip = n0.getbestblockhash()
            t = n0.getblockcount()
            self.log.info("stalelock %d: partitioned at tip %d %s" % (it, t, tip[:16]))
            for n in self.nodes:
                n.invalidateblock(tip)
            t_roll = time.time()
            try:
                self.wait_until(lambda: n0.getblockcount() >= t, timeout=30)
                self.log.info("stalelock %d: node0 produced at %d %.2f s after the rollback (partitioned: %s)" %
                              (it, t, time.time() - t_roll, self.options.partition))
            except AssertionError:
                self.log.info("stalelock %d: node0 produced NOTHING at height %d in 30 s while partitioned" % (it, t))
            n0.setnetworkactive(True)
            t_heal = time.time()
            self.mesh()
            self.wait_until(lambda: min(n.getblockcount() for n in self.nodes) >= t + 1, timeout=120)
            self.log.info("stalelock %d: after reconnecting, height %d reached %.2f s later" %
                          (it, t + 1, time.time() - t_heal))
            for n in self.nodes:
                n.reconsiderblock(tip)
            self.sync_blocks(timeout=60)

    def case_skew(self):
        rows = self.run_blocks(self.options.iterations, timeout_per=self.options.skew + 30)
        self.summarize(rows, "skew %+ds on node1" % self.options.skew)
        for h, w, dt, ddt in rows:
            if w == "node1":
                self.log.info("skew: height %d led by node1, wall %.2f s after its parent, stamped %+d s after it" % (h, dt, ddt))

    def case_crash(self):
        n0 = self.nodes[0]
        for it in range(self.options.iterations):
            tail = LogTail(n0)
            m = tail.wait(r"(?:PoS gossip: certified|PoS producer: created) block ([0-9a-f]{64}) at height (\d+)")
            self.sigkill(0)
            tail.close()
            X, h = m.group(1), int(m.group(2))
            time.sleep(0.5)
            peers = [(n.getblockcount(), n.getblockhash(h) if n.getblockcount() >= h else None) for n in self.nodes[1:]]
            dial = []
            if self.options.dial:
                dial = ["-connect=127.0.0.1:%d" % p2p_port(1), "-connect=127.0.0.1:%d" % p2p_port(2)]
            self.start_node(0, extra_args=self.args_for(0, dial))
            after = n0.getblockcount()
            self.log.info("crash %d: killed after certifying %s at %d; restarted at tip %d; peers %s" %
                          (it, X[:16], h, after, [(c, (x[:16] if x else None)) for c, x in peers]))
            t_start = time.time()
            # Isolated (or dialling its peers from startup) for 5 s: does it
            # certify a rival at h?
            time.sleep(5)
            if n0.getblockcount() >= h:
                mine = n0.getblockhash(h)
                self.log.info("crash %d: isolated node0 holds %s at %d (%s)" %
                              (it, mine[:16], h, "SAME" if mine == X else "RIVAL"))
            self.mesh()
            t_re = time.time()
            while time.time() - t_re < self.options.observe:
                best = [n.getbestblockhash() for n in self.nodes]
                if len(set(best)) == 1:
                    break
                time.sleep(1)
            r = self.certified_rivals()
            self.log.info("crash %d: %.0f s after reconnecting, tips %s heights %s rivals %s" %
                          (it, time.time() - t_re, [b[:12] for b in best], [n.getblockcount() for n in self.nodes], r))
            if len(set(best)) != 1:
                self.log.info("crash %d: NODES DISAGREE ON THE CHAIN after %d s" % (it, self.options.observe))
                return


if __name__ == '__main__':
    PosQuorumHolderHardCasesTest().main()
