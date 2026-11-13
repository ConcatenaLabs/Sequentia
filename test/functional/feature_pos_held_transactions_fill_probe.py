#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Held transactions: the autonomous leader during the record's fill window (KR8, F2).

The members' anchor watchers wait 20 s (-posdebugheldfillms) between
disconnecting T's certified block and recording T. R, a respend of T, is put
in every mempool inside that window. No member may build, sign or gossip a
proposal until its record is filled, as no member signs a share then.
"""

import os
import time
from decimal import Decimal

from feature_pos_held_transactions import HeldTransactionsTest, N, tx_from_hex


class HeldTransactionsFillProbe(HeldTransactionsTest):
    def set_test_params(self):
        super().set_test_params()

    def setup_network(self):
        self.extra_node_args = {i: ["-posdebugheldfillms=20000"] for i in range(1, N)}
        super().setup_network()

    def run_test(self):
        try:
            self.wait_certified_above(0, timeout=180)
            a = self.nodes[0]
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
            # Every node has disconnected T's block (the members are now inside their 20 s fill).
            self.wait_until(lambda: all(self.tip(n)["anchorheight"] <= fork for n in self.nodes), timeout=15)
            assert all(not self.nodes[i].getheldtxs()["txids"] for i in range(1, N)), "a record filled early"
            sizes = [self.log_size(i) for i in range(N)]
            for i in range(N):
                self.nodes[i].sendrawtransaction(r_hex)
                assert r_id in self.nodes[i].getrawmempool()
            proposals = []
            deadline = time.time() + 15
            while not proposals and time.time() < deadline:
                for i in range(1, N):
                    proposals += [(i, line) for line in self.log_has(i, "PoS gossip: proposing block", sizes[i])]
                time.sleep(0.1)
            self.record("fill", "leader", "R %s was in every mempool; T %s disconnected; autonomous proposals during the 20 s fill: %s" %
                        (r_id, t_id, proposals))
            assert not proposals, "an autonomous leader proposed while the held record was filling"
        finally:
            if os.environ.get("CR1_RESULTS"):
                import json
                with open(os.environ["CR1_RESULTS"], "w") as f:
                    json.dump(self.results, f, indent=1)
            self.stop_bitcoind()


if __name__ == "__main__":
    HeldTransactionsFillProbe().main()
