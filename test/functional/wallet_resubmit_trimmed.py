#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The wallet hands back its own payments that a reorganization trimmed from the mempool.

When one call disconnects a block and connects a competing one that confirms
the same transactions, the disconnected block's transactions go back to the
mempool first. A chain of the wallet's payments pooled on top of them then has
more unconfirmed ancestors than the mempool allows (-limitancestorcount, 25) and
is removed; the competing block confirms the parents a moment later, and the
removed payments are valid again. Nobody refused them, so they are not handed
back as a lifted refusal: the wallet's recheck hands back any payment of its own
that is unconfirmed, missing from the mempool and accepted by a test accept.

Each round, on one node, with the wallet "w" chaining every payment on the last
one's change:
  1. 25 payments p, mined as X1 + X2; the same 25 mined again as Y1 + Y2, the tip.
  2. Five more payments c1..c5, pooled on top of them.
  3. invalidateblock(Y1): Y2 and Y1 go, p returns to the mempool, c1..c5 are
     trimmed for the ancestor limit, and X1 + X2 confirm p again.
The wallet's recheck runs on a 20 s timer of mock time; the test moves the clock
and the scheduler on, so "one recheck" means exactly one.

Rounds:
  - nothing done after the trim: c1..c5 are pooled again at the first recheck,
    parents first, and the trusted balance is what it was before the trim;
  - a wallet with nothing missing asks the node nothing;
  - c5 replaced by a fee bump before the trim: the bump comes back, the payment
    it replaced is never handed to the node;
  - c3 abandoned after the trim: c1 and c2 come back, c3..c5 stay out;
  - c1's input spent by a confirmed transaction after the trim: c1..c5 stay out
    and are not even tested;
  - the payer's wallet unloaded: the wallet that only received c1..c5 does not
    hand them back;
  - c1 sent by hand above the wallet's -maxtxfee: the wallet's hand-back is
    turned away, and the recheck waits its 20 s rather than retrying each tick.
"""
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

RECHECK_SECONDS = 20
# The recheck's summary, as wallet "w" writes it (each wallet sweeps on its own).
SWEEP_LINE = "[w] Mempool recheck: "


class WalletResubmitTrimmedTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # The test reads the wallet's log lines; the per-line rate limit would
        # drop them silently once the rounds have logged a megabyte. A chain
        # with fees in any asset sets no -maxtxfee by default; the last round
        # needs one.
        self.extra_args = [["-con_default_blinded_addresses=0", "-txindex=1", "-walletrbf=1",
                            "-ratelimitlogging=0", "-maxtxfee=0.1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # -- the clock -----------------------------------------------------------

    def settle(self):
        """Let every sweep already due run, so the next one waits for the clock."""
        n = self.nodes[0]
        n.mockscheduler(1)
        n.syncwithvalidationinterfacequeue()
        n.mockscheduler(1)
        n.syncwithvalidationinterfacequeue()

    def recheck(self):
        """Exactly one round of the wallet's recheck (and the follow-up round a
        hand-back schedules for the next tick)."""
        self.mocktime += RECHECK_SECONDS + 1
        self.nodes[0].setmocktime(self.mocktime)
        self.settle()

    def sweeps_since(self, offset):
        """Wallet w's recheck summaries written since a debug.log offset."""
        with open(self.nodes[0].debug_log_path, encoding="utf-8") as f:
            f.seek(offset)
            return [line.split(SWEEP_LINE, 1)[1].strip() for line in f if SWEEP_LINE in line]

    # -- the shape -----------------------------------------------------------

    def pay(self, k, **kwargs):
        return [self.w.sendtoaddress(address=self.away, amount=Decimal("0.01"),
                                     fee_asset_label="bitcoin", **kwargs) for _ in range(k)]

    def raw(self, txids):
        return [self.nodes[0].getrawtransaction(t) for t in txids]

    def block(self, txhex, out):
        return self.generateblock(self.nodes[0], out, txhex)["hash"]

    def pooled(self, txids):
        pool = set(self.nodes[0].getrawmempool())
        return [t in pool for t in txids]

    def trim_round(self, children, before_trim=None):
        """Steps 1-3 of the module docstring. Returns c1..c5 and the wallet's
        trusted balance just before the trim."""
        n = self.nodes[0]
        p1 = self.pay(13)
        x1 = self.block(self.raw(p1), "raw(51)")
        p2 = self.pay(12)
        x2 = self.block(self.raw(p2), "raw(51)")
        h1, h2 = self.raw(p1), self.raw(p2)
        n.invalidateblock(x1)
        assert_equal(sorted(n.getrawmempool()), sorted(p1 + p2))
        y1 = self.block(h1, "raw(52)")
        y2 = self.block(h2, "raw(52)")
        # X valid again but not the tip; both moves happen before the children
        # exist, so neither trims anything.
        n.reconsiderblock(x1)
        n.preciousblock(y2)
        assert_equal(n.getbestblockhash(), y2)

        c = children()
        if before_trim:
            c = before_trim(c)
        assert all(self.pooled(c)), "the children must be pooled before the trim"
        self.settle()
        before = self.w.getbalances()["mine"]["trusted"]

        n.invalidateblock(y1)  # the one call: Y2, Y1 out, then X1, X2 in
        assert_equal(n.getbestblockhash(), x2)
        assert all(self.w.gettransaction(t)["confirmations"] > 0 for t in p1 + p2)
        self.log.info("  after the one call: children pooled %s", self.pooled(c))
        assert not any(self.pooled(c)), "the trim this test is about did not happen"
        return c, before

    def close_round(self):
        """Confirm whatever is pooled, so the next round starts from a clean mempool."""
        n = self.nodes[0]
        self.generatetoaddress(n, 1, self.mine)
        assert_equal(n.getrawmempool(), [])
        self.settle()

    # -- the rounds ----------------------------------------------------------

    def run_test(self):
        n = self.nodes[0]
        n.createwallet("w")
        self.w = n.get_wallet_rpc("w")
        d = n.get_wallet_rpc(self.default_wallet_name)
        self.mine = d.getnewaddress()
        self.away = d.getnewaddress()
        self.generatetoaddress(n, 110, self.mine)
        d.sendtoaddress(address=self.w.getnewaddress(), amount=10, fee_asset_label="bitcoin")
        self.generatetoaddress(n, 1, self.mine)

        # From here the recheck runs only when the test moves the clock.
        self.mocktime = n.getblockheader(n.getbestblockhash())["time"] + 1
        n.setmocktime(self.mocktime)
        self.settle()

        self.test_trimmed_payments_come_back()
        self.test_nothing_missing_asks_nothing()
        self.test_replaced_payment_stays_out()
        self.test_abandoned_payment_stays_out()
        self.test_conflicted_payment_stays_out()
        self.test_received_payment_is_left_to_its_payer()
        self.test_failed_hand_back_waits_for_the_next_recheck()

    def test_trimmed_payments_come_back(self):
        self.log.info("Payments trimmed in a reorganization are pooled again at the first recheck")
        c, before = self.trim_round(lambda: self.pay(5))
        assert self.w.getbalances()["mine"]["trusted"] != before, "the trim left the balance whole"
        offset = self.nodes[0].debug_log_bytes()
        self.recheck()
        # The whole chain in the first recheck, parents first: each is handed
        # back before its child is asked about.
        assert_equal(self.pooled(c), [True] * 5)
        assert_equal(self.sweeps_since(offset)[0], "asked about 5 missing from the mempool, handed back 5")
        assert_equal(self.w.getbalances()["mine"]["trusted"], before)
        self.close_round()

    def test_nothing_missing_asks_nothing(self):
        self.log.info("A wallet with nothing missing asks the node nothing")
        # Pooled payments, so the wallet has unconfirmed transactions to skip.
        c = self.pay(5)
        self.settle()
        offset = self.nodes[0].debug_log_bytes()
        self.recheck()
        self.recheck()
        sweeps = self.sweeps_since(offset)
        assert len(sweeps) >= 2, sweeps
        assert all(s.startswith("asked about 0 ") for s in sweeps), sweeps
        assert_equal(self.pooled(c), [True] * 5)
        self.close_round()

    def test_replaced_payment_stays_out(self):
        self.log.info("A payment replaced by its own fee bump is never handed back")
        replaced = []

        def bump_last(c):
            bumped = self.w.bumpfee(c[-1])["txid"]
            replaced.append(c[-1])
            return c[:-1] + [bumped]

        c, before = self.trim_round(lambda: self.pay(5, replaceable=True), bump_last)
        offset = self.nodes[0].debug_log_bytes()
        self.recheck()
        self.recheck()
        assert_equal(self.pooled(c), [True] * 5)
        assert_equal(self.pooled(replaced), [False])
        assert_equal(self.w.getbalances()["mine"]["trusted"], before)
        with open(self.nodes[0].debug_log_path, encoding="utf-8") as f:
            f.seek(offset)
            log = f.read()
        for t in c:
            assert "Submitting wtx {}".format(t) in log, t
        assert "Submitting wtx {}".format(replaced[0]) not in log
        # Still refused (the bump holds its input), so it is asked about and
        # never retried in the next tick: one sweep per recheck, not a loop.
        offset = self.nodes[0].debug_log_bytes()
        self.recheck()
        assert_equal(self.sweeps_since(offset), ["asked about 1 missing from the mempool, handed back 0"])
        self.close_round()

    def test_abandoned_payment_stays_out(self):
        self.log.info("An abandoned payment stays out")
        c, _ = self.trim_round(lambda: self.pay(5))
        self.w.abandontransaction(c[2])  # c3, and with it c4 and c5
        assert all(self.w.gettransaction(t)["details"][0]["abandoned"] for t in c[2:])
        self.recheck()
        self.recheck()
        assert_equal(self.pooled(c), [True, True, False, False, False])
        self.close_round()

    def test_conflicted_payment_stays_out(self):
        self.log.info("A payment whose input a confirmed transaction spends stays out")
        n = self.nodes[0]
        c, _ = self.trim_round(lambda: self.pay(5))
        prevout = n.decoderawtransaction(self.w.gettransaction(c[0])["hex"])["vin"][0]
        spent = n.gettxout(prevout["txid"], prevout["vout"])
        amount = Decimal(spent["value"])
        fee = Decimal("0.0001")
        raw = self.w.createrawtransaction(
            [{"txid": prevout["txid"], "vout": prevout["vout"]}],
            [{self.w.getnewaddress(): amount - fee}, {"fee": fee}])
        signed = self.w.signrawtransactionwithwallet(raw)
        assert signed["complete"]
        rival = self.block([signed["hex"]], "raw(51)")
        assert rival == n.getbestblockhash()
        self.settle()
        assert all(self.w.gettransaction(t)["confirmations"] < 0 for t in c)
        offset = n.debug_log_bytes()
        self.recheck()
        self.recheck()
        assert_equal(self.pooled(c), [False] * 5)
        sweeps = self.sweeps_since(offset)
        assert sweeps and all(s.startswith("asked about 0 ") for s in sweeps), sweeps

        self.close_round()

    def test_received_payment_is_left_to_its_payer(self):
        self.log.info("A payment the wallet only received is left to its payer")
        n = self.nodes[0]
        c, _ = self.trim_round(lambda: self.pay(5))
        # The default wallet holds c1..c5 as payments received; with the payer's
        # wallet away, nothing hands them back.
        n.unloadwallet("w")
        offset = n.debug_log_bytes()
        self.recheck()
        self.recheck()
        assert_equal(self.pooled(c), [False] * 5)
        with open(n.debug_log_path, encoding="utf-8") as f:
            f.seek(offset)
            log = f.read()
        assert "[default wallet] Mempool recheck: " in log
        assert not any("Submitting wtx {}".format(t) in log for t in c)
        # The payer's wallet comes back and sends them on load, as it always has.
        n.loadwallet("w")
        self.w = n.get_wallet_rpc("w")
        self.settle()
        assert_equal(self.pooled(c), [True] * 5)
        self.close_round()

    def test_failed_hand_back_waits_for_the_next_recheck(self):
        self.log.info("A hand-back the node turns away waits for the next recheck")
        n = self.nodes[0]
        high_fee = Decimal("0.5")  # above the wallet's -maxtxfee
        first = []

        def children():
            # c1 sent by hand at a fee the wallet would never set itself: it
            # passes a test accept, and the wallet's own submission refuses it
            # for -maxtxfee every time. c2..c5 are the wallet's, on its change.
            coin = self.w.listunspent()[0]
            raw = self.w.createrawtransaction(
                [{"txid": coin["txid"], "vout": coin["vout"]}],
                [{self.w.getrawchangeaddress(): Decimal(coin["amount"]) - high_fee}, {"fee": high_fee}])
            signed = self.w.signrawtransactionwithwallet(raw)["hex"]
            first.append(signed)
            return [n.sendrawtransaction(signed, 0)] + self.pay(4)

        c, before = self.trim_round(children)
        offset = n.debug_log_bytes()
        self.recheck()
        assert_equal(self.pooled(c), [False] * 5)
        with open(n.debug_log_path, encoding="utf-8") as f:
            f.seek(offset)
            assert "Submitting wtx {}".format(c[0]) in f.read()
        # Turned away, so nothing changed and nothing is asked again on the
        # next tick: one sweep in this recheck, not one a second.
        assert_equal(self.sweeps_since(offset), ["asked about 5 missing from the mempool, handed back 0"])
        offset = n.debug_log_bytes()
        self.recheck()
        assert_equal(self.sweeps_since(offset), ["asked about 5 missing from the mempool, handed back 0"])
        # Sent again by hand, c1 brings the wallet's payments back with it.
        n.sendrawtransaction(first[0], 0)
        self.recheck()
        assert_equal(self.pooled(c), [True] * 5)
        assert_equal(self.w.getbalances()["mine"]["trusted"], before)
        self.close_round()

if __name__ == "__main__":
    WalletResubmitTrimmedTest().main()
