#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Held transactions: no member signs over a departure, even before its record is on disk.

The record takes what a certified block carried in the same hold of cs_main
that disconnects it; the file is written afterwards, outside every lock. Here
the members write their files only 20 s after the departure
(-posdebugheldfillms). Inside that delay every member already holds T, refuses
R (a respend of T) from its mempool, and signs no block that carries it, while
the attacker, which runs without the rule, offers R from its own mempool.
"""

import os
import struct
import time
from decimal import Decimal

from feature_pos_held_transactions import ATTACKER, CHAIN, HeldTransactionsTest, N, tx_from_hex
from test_framework.authproxy import JSONRPCException

WRITE_DELAY_MS = 20000


class HeldTransactionsDepartureProbe(HeldTransactionsTest):
    def set_test_params(self):
        super().set_test_params()

    def setup_network(self):
        self.extra_node_args = {i: ["-posdebugheldfillms=%d" % WRITE_DELAY_MS] for i in range(1, N)}
        super().setup_network()

    def held_on_disk(self, i):
        p = os.path.join(self.nodes[i].datadir, CHAIN, "heldtxs.dat")
        if not os.path.exists(p):
            return 0
        with open(p, "rb") as f:
            data = f.read(12)
        return struct.unpack("<IQ", data)[1] if len(data) == 12 else 0

    def file_mtime(self, i):
        p = os.path.join(self.nodes[i].datadir, CHAIN, "heldtxs.dat")
        return os.stat(p).st_mtime_ns if os.path.exists(p) else None

    def run_test(self):
        try:
            self.wait_certified_above(0, timeout=180)
            a = self.nodes[ATTACKER]
            members = [i for i in range(N) if i != ATTACKER]
            coin = self.fund()
            t_hex = self.spend(coin, self.nodes[1].getnewaddress(), Decimal("0.0001"))
            r_hex = self.spend(coin, a.getnewaddress(), Decimal("0.001"))
            self.btc_mine(1)
            self.wait_until(lambda: self.tip(self.nodes[1])["anchorheight"] == self.btc().getblockcount(), timeout=60)
            t_id, _, _ = self.certify(t_hex)
            r_id = tx_from_hex(r_hex).rehash()
            sizes = [self.log_size(i) for i in range(N)]
            fork = self.bitcoin_reorg(1)
            self.wait_until(lambda: self.log_has(1, "Parent chain reorganization detected", sizes[1]), timeout=30)
            t0 = time.time()
            # Every node has disconnected T's block; every member holds T, its file not yet written.
            self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork for n in self.nodes), timeout=15)
            mtime = {i: self.file_mtime(i) for i in members}
            held = {i: t_id in self.nodes[i].getheldtxs()["txids"] for i in members}
            on_disk = {i: self.held_on_disk(i) for i in members}
            self.record("departure", "held", "%.2f s after the reorganization was logged: T %s held %s; held on disk %s" %
                        (time.time() - t0, t_id, held, on_disk))
            assert all(held.values()), "a member does not hold T once its block left"
            assert not any(on_disk.values()), "a member wrote its record before the delay ended"
            sizes = [self.log_size(i) for i in range(N)]
            refusals = {}
            for i in members:
                try:
                    self.nodes[i].sendrawtransaction(r_hex)
                    refusals[i] = "accepted"
                except JSONRPCException as e:
                    refusals[i] = e.error["message"]
            self.record("departure", "mempools", "R %s: %s" % (r_id, refusals))
            assert all("txn-held-conflict" in m for m in refusals.values()), refusals
            a.sendrawtransaction(r_hex)
            assert r_id in a.getrawmempool()
            # The attacker leads about 3 rounds in 8 and proposes R; no member signs it.
            deadline = t0 + WRITE_DELAY_MS / 1000.0 - 2
            while time.time() < deadline:
                time.sleep(0.25)
            declines = {i: len([line for line in self.log_has(i, "not signing", sizes[i]) if r_id in line]) for i in members}
            proposals = {i: len(self.log_has(i, "PoS gossip: proposing block", sizes[i])) for i in members}
            proposals[ATTACKER] = len(self.log_has(ATTACKER, "PoS gossip: proposing block", sizes[ATTACKER]))
            mined = [i for i in range(N) if self.block_of(r_id, self.nodes[i]) is not None]
            t_again = {i: self.block_of(t_id, self.nodes[i]) for i in range(N)}
            on_disk_end = {i: self.held_on_disk(i) for i in members}
            self.record("departure", "window", "%.1f s: R mined on nodes %s; T mined again at heights %s; members' declines naming R %s; proposals %s; held on disk %s" %
                        (time.time() - t0, mined, t_again, declines, proposals, on_disk_end))
            assert not mined, "a block carrying R was accepted while the members held T"
            # The file is written once the delay ends (holding T, or nothing if T was mined again meanwhile).
            self.wait_until(lambda: all(self.file_mtime(i) != mtime[i] for i in members), timeout=60)
            self.record("departure", "written", "every member's file written %.1f s after the reorganization was logged: %s held" %
                        (time.time() - t0, {i: self.held_on_disk(i) for i in members}))
        finally:
            if os.environ.get("CR1_RESULTS"):
                import json
                with open(os.environ["CR1_RESULTS"], "w") as f:
                    json.dump(self.results, f, indent=1)
            self.stop_bitcoind()


if __name__ == "__main__":
    HeldTransactionsDepartureProbe().main()
