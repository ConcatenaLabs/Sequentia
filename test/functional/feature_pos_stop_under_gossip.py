#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Committee members stop cleanly while proof-of-stake gossip is in flight.

A node that stops tears down its block producer while its message handler may
still be delivering a proposal, a share or a certificate to it, and while its
peers keep sending more. Nothing the gossip, the producer thread or the message
handler holds may outlive its owner: the daemon must exit with status 0 and,
in a sanitizer build, write nothing to stderr.

Seven hosts share a committee of fourteen keys (quorum eight) and produce a
block every second. Each cycle stops some hosts at once, at a random point of a
slot, and starts them again: three of them (the other four hold the quorum, so
shares keep flowing while the three shut down), and every fourth cycle all
seven. The peers of every stopped host see it disconnect mid-round, which
exercises the same lifetimes from the other side.

One host, which every cycle stops, holds each share it receives for half a
second before handling it (-posdebugsharedelayms), so its message handler is
almost always inside the producer when the stop comes: the case a heavily
loaded node meets by chance. The other six hold twelve keys, above the quorum,
so the chain does not wait for it.

Run it under a build configured with --with-sanitizers=address to find a use
after free; without one it still catches a crash at exit. --cycles sets the
number of cycles.
"""

import os
import random
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

SANITIZER_MARKERS = ("AddressSanitizer", "LeakSanitizer", "runtime error:", "ThreadSanitizer")


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


HOSTS = 7
COMMITTEE = 14
SLOW = HOSTS - 1  # the host whose message handler is held inside the producer
SHARE_DELAY_MS = 500


class PosStopUnderGossipTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--cycles", dest="cycles", type=int, default=8,
                            help="number of stop/start cycles (default: %(default)s)")

    def set_test_params(self):
        self.num_nodes = HOSTS
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(COMMITTEE)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=%d" % COMMITTEE,
            "-posslotinterval=1", "-con_max_block_sig_size=40000",
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [
            common + ["-posproducer"] + ["-posproducerkey=%s" % wif for wif, _ in self.stakers[i::HOSTS]]
            for i in range(HOSTS)
        ]
        self.extra_args[SLOW].append("-posdebugsharedelayms=%d" % SHARE_DELAY_MS)

    def setup_network(self):
        self.setup_nodes()
        for a in range(HOSTS):
            for b in range(a + 1, HOSTS):
                self.connect_nodes(a, b)

    def sanitizer_output(self, i):
        """Everything node i has written to stderr in any of its runs."""
        out = []
        stderr_dir = self.nodes[i].stderr_dir
        for name in sorted(os.listdir(stderr_dir)):
            with open(os.path.join(stderr_dir, name), encoding='utf-8', errors='replace') as f:
                text = f.read().strip()
            if text:
                out.append(text)
        return "\n".join(out)

    def stop_at_once(self, idx):
        """Ask every host in idx to stop, then wait for each: a host still
        running gossips at the ones shutting down. Each must exit with status 0
        and an empty stderr."""
        for i in idx:
            self.nodes[i].stop()
        for i in idx:
            self.nodes[i].wait_until_stopped()  # asserts exit status 0
            text = self.sanitizer_output(i)
            assert not any(m in text for m in SANITIZER_MARKERS), \
                "node%d wrote a sanitizer report while stopping:\n%s" % (i, text)
            assert text == "", "node%d wrote to stderr while stopping:\n%s" % (i, text)
        self.shutdowns += len(idx)

    def height(self):
        return max(n.getblockcount() for n in self.nodes[:SLOW] if n.running)

    def run_test(self):
        self.shutdowns = 0
        self.log.info("The committee certifies a few blocks")
        self.wait_until(lambda: self.height() >= 3, timeout=180)

        cycles = self.options.cycles
        self.log.info("%d cycles of stopping hosts mid-round and starting them again" % cycles)
        for c in range(cycles):
            idx = list(range(HOSTS)) if c % 4 == 3 else [SLOW] + random.sample(range(SLOW), 2)
            h0 = self.height()
            # Land the stop anywhere in a slot: on a proposal, a share or a
            # certificate being handled, or between them.
            time.sleep(random.uniform(0.0, 1.0))
            self.stop_at_once(idx)
            for i in idx:
                self.start_node(i)
            for i in idx:
                for j in range(HOSTS):
                    if j != i and (j not in idx or j < i):
                        self.connect_nodes(i, j)
            # The chain must be moving again before the next stop, so the next
            # one lands on live gossip too.
            self.wait_until(lambda: self.height() >= h0 + 2, timeout=120)
            self.log.info("  cycle %d: stopped %s, height %d -> %d, %d clean shutdowns so far" %
                          (c, idx, h0, self.height(), self.shutdowns))

        self.log.info("The hosts without the delay converge on one chain")
        fast = self.nodes[:SLOW]
        target = self.height() + 2
        self.wait_until(lambda: all(n.getblockcount() >= target for n in fast), timeout=180)
        h = min(n.getblockcount() for n in fast) - 1
        assert all(n.getblockhash(h) == fast[0].getblockhash(h) for n in fast)

        self.log.info("Every host stops cleanly at the end, all at once")
        self.stop_at_once(list(range(HOSTS)))
        self.log.info("%d clean shutdowns in all" % self.shutdowns)


if __name__ == '__main__':
    PosStopUnderGossipTest().main()
