#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A full 100-node autonomous BLS gossip committee, end to end.

This is the headline demo: one hundred independent daemons, each holding exactly
ONE staker key (committee size 100, quorum 51), with no coordinator. Every block
is produced by whichever member's VRF slot opens first, gossiped, and certified
by a strict majority of the others' BLS shares — and every node reaches the same
tip with immediate finality.

It exercises, at the paper's target scale:
  * the full 100-member BLS certificate (witness-discounted to ~13% of weight),
  * one-key-per-node decentralization (no node can certify alone — 51 distinct
    members' shares must flood to the backer within the round),
  * coordinator-free round-robin leader election over gossip, and
  * liveness through mass failure: a quorum (51 of 100) keeps producing after a
    third of the network is killed.

Scale is configurable with POS_DEMO_NODES (default 100) so the same harness can
be smoke-run small. Slot timing is deliberately relaxed to give multi-hop gossip
room to converge across a large network on a 4-core host.
"""

import os
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

# A full 100-member committee (POS_DEMO_COMMITTEE), spread across POS_DEMO_HOSTS
# daemons. With HOSTS == COMMITTEE every node holds exactly one key (the fully
# decentralized, one-key-per-node ideal); with HOSTS < COMMITTEE each host holds
# several keys (the same 100-member certificate and quorum, fewer processes — what
# a 4-core box can actually run in real time). Both certify identical blocks.
COMMITTEE = int(os.environ.get("POS_DEMO_COMMITTEE", "100"))
HOSTS = int(os.environ.get("POS_DEMO_HOSTS", str(COMMITTEE)))
assert 1 <= HOSTS <= COMMITTEE

# The framework caps a test at MAX_NODES (12) and reserves that many ports per
# parallel test process. Lift the cap in the namespaces that enforce it. Each test
# runs in its own process, so this only affects this run; the port math stays
# inside the reserved range (p2p 11000-16000, rpc 16000-21000) for counts well
# under 5000.
import test_framework.util as _util
import test_framework.test_framework as _tf
_util.MAX_NODES = max(_util.MAX_NODES, HOSTS + 1)
_tf.MAX_NODES = _util.MAX_NODES


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


QUORUM = COMMITTEE // 2 + 1   # strict majority
HUBS = [0, 1, 2, 3][:HOSTS]   # low-diameter relay backbone (diameter <= 3)
# seconds/slot — room for multi-hop gossip at scale; raise it (POS_DEMO_SLOT) if a
# big one-key-per-node network needs more headroom on a busy machine.
SLOT = int(os.environ.get("POS_DEMO_SLOT", "2"))


class Pos100NodeNetworkTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = HOSTS
        self.setup_clean_chain = True

        self.stakers = [make_staker() for _ in range(COMMITTEE)]
        # Every node knows the full staker set (so all can validate). The COMMITTEE
        # keys are dealt round-robin across HOSTS daemons: HOSTS == COMMITTEE gives
        # one key per node; HOSTS < COMMITTEE packs several per node.
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1",
            "-poscommitteesize=%d" % COMMITTEE,
            "-posslotinterval=%d" % SLOT,
            "-con_max_block_sig_size=40000",   # 100 members * ~257B + leader sig + aggregate
            "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0",
        ]
        common += ["-staker=%s:1" % pub for _, pub in self.stakers]

        self.host_keys = [self.stakers[i::HOSTS] for i in range(HOSTS)]  # round-robin deal
        self.extra_args = []
        for i in range(HOSTS):
            keys = ["-posproducerkey=%s" % wif for wif, _ in self.host_keys[i]]
            self.extra_args.append(common + ["-posproducer"] + keys)

    def setup_network(self):
        # Start every daemon, but do NOT use the framework's default all-pairs
        # connectivity assertions — we wire a sparse, low-diameter gossip mesh.
        self.setup_nodes()

        self.log.info("Wiring %d-host gossip mesh (ring + %d-hub backbone)" % (HOSTS, len(HUBS)))
        # Ring: guarantees the graph is connected and survives hub loss.
        if HOSTS > 2:
            for i in range(HOSTS):
                self.connect_nodes(i, (i + 1) % HOSTS)
        # Hub backbone: every node attaches to one hub; hubs fully interconnect.
        # Diameter <= 3 (node -> hub -> hub -> node) so a share floods network-wide
        # well within the collection window.
        for a in HUBS:
            for b in HUBS:
                if a < b:
                    self.connect_nodes(a, b)
        for i in range(HOSTS):
            if i not in HUBS:
                self.connect_nodes(i, HUBS[i % len(HUBS)])
        self.log.info("Mesh wired; %d hosts online" % HOSTS)

    def _heights(self, nodes=None):
        nodes = nodes if nodes is not None else self.nodes
        return [n.getblockcount() for n in nodes]

    def _wait_all_reach(self, target, nodes=None, timeout=600):
        nodes = nodes if nodes is not None else self.nodes
        deadline = time.time() + timeout
        last = -1
        while time.time() < deadline:
            hs = self._heights(nodes)
            lo = min(hs)
            if lo != last:
                self.log.info("  height: min=%d max=%d (target %d)" % (lo, max(hs), target))
                last = lo
            if lo >= target:
                return
            time.sleep(2)
        raise AssertionError("network did not all reach height %d (heights=%s)" %
                             (target, self._heights(nodes)))

    def _assert_no_fork(self, nodes=None):
        """Immediate finality: at the common height every node has the same hash."""
        nodes = nodes if nodes is not None else self.nodes
        h = min(self._heights(nodes)) - 1
        assert h >= 1
        ref = nodes[0].getblockhash(h)
        for idx, n in enumerate(nodes):
            assert_equal((idx, n.getblockhash(h)), (idx, ref))
        return h, ref

    def run_test(self):
        # Confirm the mesh actually came up before we expect blocks.
        peers = [n.getpeerinfo() for n in self.nodes]
        pc = [len(p) for p in peers]
        self.log.info("Peer connectivity: min=%d avg=%.1f max=%d" %
                      (min(pc), sum(pc) / len(pc), max(pc)))

        per = COMMITTEE // HOSTS
        shape = "one key per node" if HOSTS == COMMITTEE else "~%d keys/host across %d hosts" % (per, HOSTS)
        self.log.info("=== %d-member committee (quorum %d), %s, no coordinator ===" %
                      (COMMITTEE, QUORUM, shape))
        self.log.info("Producing first blocks over gossip...")
        self._wait_all_reach(3, timeout=600)

        h, ref = self._assert_no_fork()
        block_hex = self.nodes[0].getblockhash(h)
        full = self.nodes[0].getblock(block_hex, 0)
        blk = self.nodes[0].getblock(block_hex)
        self.log.info("Immediate finality: all %d hosts agree on tip %d (%s)" % (HOSTS, h, ref[:16]))
        self.log.info("Certified block: %d bytes on the wire, weight %d (cert is x1-discounted)" %
                      (len(full) // 2, blk["weight"]))
        assert len(full) // 2 > 120 * COMMITTEE, \
            "a %d-member certificate should scale with the committee" % COMMITTEE

        # Liveness under mass failure: kill a third of the hosts. Round-robin
        # dealing means that removes ~a third of the keys, so a quorum survives and
        # production must continue without a fork.
        kill = HOSTS // 3
        if kill >= 1:
            survivor_keys = sum(len(self.host_keys[i]) for i in range(HOSTS - kill))
            assert survivor_keys >= QUORUM, "test misconfigured: survivors would lack a quorum"
            self.log.info("=== Killing %d of %d hosts; %d keys (>= quorum %d) survive ===" %
                          (kill, HOSTS, survivor_keys, QUORUM))
            # Kill from the tail so the hub backbone (low indices) stays intact.
            for i in range(HOSTS - kill, HOSTS):
                self.stop_node(i)
            survivors = [self.nodes[i] for i in range(HOSTS - kill)]

            start = min(self._heights(survivors))
            self._wait_all_reach(start + 3, nodes=survivors, timeout=600)
            self._assert_no_fork(nodes=survivors)
            self.log.info("Survivors advanced to height %d with no fork after losing %d hosts" %
                          (min(self._heights(survivors)), kill))
        self.log.info("=== %d-member autonomous committee: PASS ===" % COMMITTEE)


if __name__ == '__main__':
    Pos100NodeNetworkTest().main()
