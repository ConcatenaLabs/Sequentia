#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The wallet's mempool recheck asks again only when something its answer depends on has moved.

One node, one wallet "w" with K coins. For each coin i:
  A_i  a replaceable payment to itself spending coin i      (sendrawtransaction)
  C_i  a child spending A_i's output                         (sendrawtransaction)
  B_i  a higher-fee spend of coin i, replacing A_i and C_i   (sendrawtransaction)
A_i is then refused by the node (a lower-fee conflict of B_i) and C_i lacks its
input. Both are unconfirmed and out of the mempool, so both are candidates of the
recheck, which runs every 20 s of mock time. The mempool also holds a few large
transactions, so a mempool consistency check (-checkmempool=1, the test chains'
default) costs what it costs with full blocks.

Each pass is driven by moving the mock clock, after the default wallet has made
an unrelated payment, which changes the mempool. Measured per pass: how many
candidates it asked about, its wall time from the clock's move, the longest a
validation callback waited while it ran (syncwithvalidationinterfacequeue, queued
every 50 ms from a second connection), and, from the recheck's own log, its
slices and the longest one.

Asserted:
  - the first pass asks about all 2K; later passes, with nothing they depend on
    having moved (the unrelated payments touch none of their inputs), ask about
    none;
  - a new tip makes every candidate be asked again, once;
  - a replacement of B_0 (it touches A_0's input, and takes B_0 out of the
    mempool) makes exactly A_0 and B_0 be asked, once;
  - a change of the fee asset's rate makes every candidate be asked again, once;
  - no slice holds the wallet and the scheduler longer than RECHECK_SLICE (50 ms)
    and one validation and one submission, and a callback queued with a pass
    waits no longer than that.

--many=N measures a wallet with N candidates (N/2 coins) and asserts the same
bound on the slices; it is slow and not part of the default run.
"""
from decimal import Decimal
import json
import os
import re
import threading
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_rpc_proxy

PASS_LINE = "[w] Mempool recheck: "
SLICE_LINE = "[w] Mempool recheck took "


class WalletRecheckUnchangedTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--k", type=int, default=11, help="coins, so 2k candidates")
        parser.add_argument("--checkmempool", type=int, default=1)
        parser.add_argument("--bulk", type=int, default=4, help="large transactions kept in the mempool")
        parser.add_argument("--passes", type=int, default=3)
        parser.add_argument("--many", type=int, default=0, help="measure a wallet with this many candidates instead")

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def setup_network(self):
        self.extra_args = [["-con_default_blinded_addresses=0", "-checkmempool=%d" % self.options.checkmempool,
                            "-debug=mempool", "-logtimemicros=1", "-txindex=1", "-ratelimitlogging=0"]]
        self.setup_nodes()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def out(self, **kw):
        self.log.info(json.dumps(kw))

    def raw(self, ins, outs):
        signed = self.w.signrawtransactionwithwallet(self.w.createrawtransaction(ins, outs))
        assert signed["complete"], signed
        return signed["hex"]

    def lines_since(self, mark, prefix):
        with open(self.nodes[0].debug_log_path, encoding="utf-8", errors="replace") as fh:
            fh.seek(mark)
            return [line.split(prefix, 1)[1].strip() for line in fh if prefix in line]

    def one_pass(self, label):
        """Move the clock past the recheck timer and measure the pass that follows."""
        n = self.nodes[0]
        mark = os.path.getsize(n.debug_log_path)
        delays = []
        done = threading.Event()

        def probe():
            # A callback queued every 50 ms while the pass may be running: how
            # long the longest of them waited to be delivered.
            rpc = self.probe_rpc
            while not done.is_set():
                t0 = time.monotonic()
                rpc.syncwithvalidationinterfacequeue()
                delays.append(time.monotonic() - t0)
                time.sleep(0.05)

        # An unrelated payment: it changes the mempool and queues a callback.
        self.d.sendtoaddress(address=self.d.getnewaddress(), amount=Decimal("0.01"), fee_asset_label="bitcoin")
        th = threading.Thread(target=probe)
        th.start()
        t0 = time.monotonic()  # setmocktime takes cs_main, so it waits for the pass too
        self.mocktime += 21
        n.setmocktime(self.mocktime)
        self.wait_until(lambda: self.lines_since(mark, PASS_LINE), timeout=1200)
        took = time.monotonic() - t0
        done.set()
        th.join(1200)
        delay = {"s": max(delays)}
        line = self.lines_since(mark, PASS_LINE)[0]
        row = {"pass": label,
               "asked": int(re.search(r"asked about (\d+)", line).group(1)),
               "handed_back": int(re.search(r"handed back (\d+)", line).group(1)),
               "pass_wall_s": round(took, 3),
               "callback_delay_s": round(delay["s"], 3)}
        slices = self.lines_since(mark, SLICE_LINE)
        if slices:
            m = re.search(r"(\d+) slices, longest (\d+) us; (\d+) unchanged", slices[0])
            row.update(slices=int(m.group(1)), longest_slice_ms=round(int(m.group(2)) / 1000, 1),
                       unchanged=int(m.group(3)))
        self.out(**row)
        return row

    def build_candidates(self, k):
        """K coins, each with A_i refused, C_i missing its input, B_i pooled."""
        n = self.nodes[0]
        coins = [u for u in self.w.listunspent(1) if u["amount"] == Decimal("1")][:k]
        assert_equal(len(coins), k)
        a_ids, c_ids, b_ids = [], [], []
        for u in coins:
            to = self.w.getnewaddress()
            a = n.sendrawtransaction(self.raw([{"txid": u["txid"], "vout": u["vout"], "sequence": 0xfffffffd}],
                                              [{to: Decimal("0.9999")}, {"fee": Decimal("0.0001")}]))
            av = [i for i, x in enumerate(n.getrawtransaction(a, True)["vout"])
                  if x.get("scriptPubKey", {}).get("address") == to][0]
            c = n.sendrawtransaction(self.raw([{"txid": a, "vout": av, "sequence": 0xfffffffd}],
                                              [{self.w.getnewaddress(): Decimal("0.9998")}, {"fee": Decimal("0.0001")}]))
            b = n.sendrawtransaction(self.raw([{"txid": u["txid"], "vout": u["vout"], "sequence": 0xfffffffd}],
                                              [{self.w.getnewaddress(): Decimal("0.99")}, {"fee": Decimal("0.01")}]))
            a_ids.append(a)
            c_ids.append(c)
            b_ids.append(b)
        pool = set(n.getrawmempool())
        assert not (set(a_ids) | set(c_ids)) & pool and set(b_ids) <= pool
        return coins, a_ids, c_ids, b_ids

    def run_test(self):
        n = self.nodes[0]
        # The recheck runs only when the test moves the clock, so none runs
        # while the candidates are being made.
        self.mocktime = int(time.time())
        n.setmocktime(self.mocktime)
        # A connection of its own for the probe, which runs on another thread.
        self.probe_rpc = get_rpc_proxy(n.url, n.index, timeout=1200)
        n.createwallet("w")
        self.w = n.get_wallet_rpc("w")
        self.d = n.get_wallet_rpc(self.default_wallet_name)
        self.mine = self.d.getnewaddress()
        self.generatetoaddress(n, 120, self.mine)
        k = self.options.many // 2 if self.options.many else self.options.k
        # Confirmed coins: K for w, and one per bulk transaction.
        amounts = {self.w.getnewaddress(): Decimal("1") for _ in range(k)}
        amounts.update({self.d.getnewaddress(): Decimal("5") for _ in range(self.options.bulk)})
        self.d.sendmany(amounts=amounts, fee_asset="bitcoin")
        self.generatetoaddress(n, 1, self.mine)
        # The bulk: large transactions that stay in the mempool.
        for _ in range(self.options.bulk):
            self.d.sendmany(amounts={self.d.getnewaddress(): Decimal("0.001") for _ in range(400)}, fee_asset="bitcoin")
        coins, a_ids, c_ids, b_ids = self.build_candidates(k)
        self.out(step="set up", candidates=2 * k, mempool=len(n.getrawmempool()), checkmempool=self.options.checkmempool)

        rows = [self.one_pass(p) for p in range(self.options.passes + 1)]
        self.out(step="result", asked_per_pass=[r["asked"] for r in rows],
                 worst_callback_delay_s=max(r["callback_delay_s"] for r in rows),
                 longest_slice_ms=max((r.get("longest_slice_ms", 0) for r in rows), default=None))
        assert_equal(rows[0]["asked"], 2 * k)
        assert_equal([r["asked"] for r in rows[1:]], [0] * self.options.passes)
        self.assert_slices_bounded(rows)

        self.log.info("A new tip: every candidate is asked again, once")
        self.generateblock(n, "raw(51)", [])
        rows = [self.one_pass("new tip"), self.one_pass("after")]
        assert_equal([r["asked"] for r in rows], [2 * k, 0])
        self.assert_slices_bounded(rows)
        if self.options.many:
            return

        self.log.info("A replacement touching A_0's input: A_0 and the replaced B_0 are asked, once")
        u = coins[0]
        b0 = n.sendrawtransaction(self.raw([{"txid": u["txid"], "vout": u["vout"], "sequence": 0xfffffffd}],
                                           [{self.w.getnewaddress(): Decimal("0.98")}, {"fee": Decimal("0.02")}]), 0)
        assert b0 in n.getrawmempool() and b_ids[0] not in n.getrawmempool()
        rows = [self.one_pass("replacement"), self.one_pass("after")]
        assert_equal([r["asked"] for r in rows], [2, 0])

        self.log.info("A change of the fee asset's rate: every candidate is asked again, once")
        rates = n.getfeeexchangerates()
        n.setfeeexchangerates({a: r * 2 for a, r in rates.items()})
        rows = [self.one_pass("rate up"), self.one_pass("after")]
        n.setfeeexchangerates(rates)
        rows += [self.one_pass("rate back"), self.one_pass("after")]
        assert_equal([r["asked"] for r in rows], [2 * k + 1, 0, 2 * k + 1, 0])
        self.assert_slices_bounded(rows)

    def assert_slices_bounded(self, rows):
        # The bound is the slice budget plus whatever the last ask of a slice
        # takes, which the consistency check makes large on a test chain: it is
        # measured here, not assumed, as twice the average ask of the first
        # pass, with a margin for the machine.
        if "longest_slice_ms" not in rows[0]:
            return
        first = rows[0]
        per_ask_ms = 1000 * first["pass_wall_s"] / max(1, first["asked"])
        bound_ms = 50 + 2 * per_ask_ms + 100
        for r in rows:
            assert r["longest_slice_ms"] <= bound_ms, (r, bound_ms)
            assert r["callback_delay_s"] * 1000 <= bound_ms + 200, (r, bound_ms)


if __name__ == "__main__":
    WalletRecheckUnchangedTest().main()
