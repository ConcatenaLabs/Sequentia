#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests for the price server's reference-unit denomination.

Run:  python3 contrib/price-server/test_price_server.py
 or:  cd contrib/price-server && python3 -m unittest test_price_server -v

Stdlib only, no node and no network: the registry and the market feed are
stubbed, and the server runs in dry-run mode so nothing is published anywhere.

What is pinned here:
  * the LIVE published rate map, byte for byte, as a golden (a refactor that
    moves a rate would change fee acceptance across the network);
  * a MANUAL price is entered in reference units and must NOT be translated by
    the reference-unit conversion factor, while an API-sourced price MUST be;
  * the identity factor (1.0, the default) changes nothing at all;
  * the legacy config key still works, and so does the legacy pinned-token mode,
    including when the pinned token is the one priced by hand;
  * the frame the server REPORTS is the frame it APPLIES: an unusable factor is
    refused rather than silently ignored, the admin page names the mode actually
    in force, and every decision row states the rate that was published.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import price_server as ps  # noqa: E402


# The rates the live testnet box publishes today, keyed by ticker. The reference
# unit there is one USD (conversion factor 1.0), so each rate is simply the
# asset's USD price scaled by 1e8.
LIVE_RATES = {
    "EURX": 113750000,
    "BITCOIN": 37702136,
    "SILVR": 5890600000,
    "SBTC": 6434050000000,
    "OILX": 8931000000,
    "GOLD": 407080000000,
    "USDX": 100000000,
}
LIVE_PRICES = {tk: rate / ps.COIN for tk, rate in LIVE_RATES.items()}


def asset_id(ticker):
    """A deterministic 64-hex id per ticker, as the registry hands out."""
    return (ticker.lower().encode().hex() * 32)[:64]


def poll_server(cfg, prices, tickers=None, precisions=None):
    """Run one poll with a stubbed registry and market feed. Returns
    (rates_by_ticker, server); the rates are exactly what the server would have
    pushed to setfeeexchangerates, and the server carries last_report."""
    tickers = list(tickers if tickers is not None else prices)
    precisions = precisions or {}
    registry = {tk.upper(): (asset_id(tk), "example.test", precisions.get(tk, 8))
                for tk in tickers}
    feed = {tk.upper(): {"price": p, "market_cap": None, "volume_24h": None}
            for tk, p in prices.items()}
    ticker_of_id = {aid: tk for tk, (aid, _d, _p) in registry.items()}

    real_registry, real_prices = ps.fetch_registry, ps.fetch_prices
    ps.fetch_registry = lambda url, timeout: dict(registry)
    ps.fetch_prices = lambda source, timeout: dict(feed)
    try:
        srv = ps.PriceServer(dict(cfg), dry_run=True)
        rates = srv.poll_once()
    finally:
        ps.fetch_registry, ps.fetch_prices = real_registry, real_prices
    return {ticker_of_id[aid]: r for aid, r in rates.items()}, srv


def run_poll(cfg, prices, tickers=None, precisions=None):
    """poll_server(), reduced to (rates_by_ticker, {ticker: status})."""
    rates, srv = poll_server(cfg, prices, tickers, precisions)
    return rates, {r["ticker"]: r["status"] for r in srv.last_report}


def base_cfg(**extra):
    cfg = {
        "source": {"url": "http://stub/prices", "quote_currency": "USD", "format": "sequentia"},
        "registry_url": "http://stub/registry",
        "feed_aliases": {},
        "default_thresholds": {"require": "all"},
        "exceptions": {},
    }
    cfg.update(extra)
    return cfg


class LiveRatesUnchanged(unittest.TestCase):
    """The published numbers must not move."""

    def test_golden_live_map(self):
        rates, statuses = run_poll(base_cfg(api_units_per_reference_unit=1.0), LIVE_PRICES)
        self.assertEqual(rates, LIVE_RATES)
        self.assertTrue(all(s == "admitted" for s in statuses.values()), statuses)

    def test_golden_live_map_under_the_legacy_key(self):
        rates, _ = run_poll(base_cfg(reference_price_usd=1.0), LIVE_PRICES)
        self.assertEqual(rates, LIVE_RATES)

    def test_golden_live_map_with_no_factor_configured(self):
        # No key at all: the default factor is the identity, so the same map.
        rates, _ = run_poll(base_cfg(), LIVE_PRICES)
        self.assertEqual(rates, LIVE_RATES)


class ManualPricesAreAlreadyInReferenceUnits(unittest.TestCase):
    """A hand-entered price is in reference units and must not be translated."""

    def scoped_out(self, **extra):
        """A config where MANU is excluded from the market source, so its price
        can only come from the manual list."""
        cfg = base_cfg(**extra)
        cfg["source"] = dict(cfg["source"], mode="except", assets=["MANU"])
        return cfg

    def test_not_double_translated_under_a_non_identity_factor(self):
        cfg = self.scoped_out(api_units_per_reference_unit=0.88, manual_prices={"MANU": 100.0})
        rates, statuses = run_poll(cfg, {"MANU": 7.0, "USDX": 1.0})
        self.assertEqual(statuses["MANU"], "admitted (manual price)")
        # 100 reference units, published as entered.
        self.assertEqual(rates["MANU"], 100 * ps.COIN)
        # NOT the double-translated value the old code produced.
        self.assertNotEqual(rates["MANU"], 11363636364)
        # The API-sourced asset in the same round IS translated: 1 USD / 0.88.
        self.assertEqual(rates["USDX"], round(1.0 * ps.COIN * ps.COIN / round(0.88 * ps.COIN)))

    def test_not_double_translated_under_the_legacy_key(self):
        cfg = self.scoped_out(reference_price_usd=0.88, manual_prices={"MANU": 100.0})
        rates, _ = run_poll(cfg, {"MANU": 7.0})
        self.assertEqual(rates["MANU"], 100 * ps.COIN)

    def test_identity_factor_changes_nothing(self):
        cfg = self.scoped_out(api_units_per_reference_unit=1.0, manual_prices={"MANU": 100.0})
        rates, _ = run_poll(cfg, dict(LIVE_PRICES, MANU=7.0))
        expected = dict(LIVE_RATES)
        expected["MANU"] = 100 * ps.COIN
        self.assertEqual(rates, expected)

    def test_manual_price_carries_the_asset_denomination(self):
        # precision 2: the rate still has to carry 10**(8-2), and only that.
        cfg = self.scoped_out(api_units_per_reference_unit=0.88, manual_prices={"MANU": 100.0})
        rates, _ = run_poll(cfg, {"MANU": 7.0}, precisions={"MANU": 2})
        self.assertEqual(rates["MANU"], ps.scaled_rate(100.0, 2))

    def test_range_check_runs_in_the_reference_frame(self):
        # The check bounds the rate that actually gets published, so it is the
        # operator's own number that is judged, not an API-numeraire shadow of it.
        top = ps.MAX_RATE / ps.COIN  # the largest manual price that still fits
        cfg = self.scoped_out(api_units_per_reference_unit=0.88,
                              manual_prices={"OK": top, "TOOBIG": top * 10})
        rates, statuses = run_poll(cfg | {"source": {"url": "http://stub/prices",
                                                     "quote_currency": "USD",
                                                     "format": "sequentia",
                                                     "mode": "only", "assets": []}},
                                   {}, tickers=["OK", "TOOBIG"])
        self.assertEqual(statuses["OK"], "admitted (manual price)")
        self.assertEqual(rates["OK"], ps.MAX_RATE)
        self.assertEqual(statuses["TOOBIG"], "skipped: manual rate out of range")
        self.assertNotIn("TOOBIG", rates)

    def test_always_reject_still_wins_over_a_manual_price(self):
        cfg = self.scoped_out(api_units_per_reference_unit=0.88, manual_prices={"MANU": 100.0})
        cfg["exceptions"] = {"always_reject": ["MANU"]}
        rates, statuses = run_poll(cfg, {"MANU": 7.0})
        self.assertEqual(statuses["MANU"], "rejected: always_reject")
        self.assertNotIn("MANU", rates)

    def test_the_api_price_wins_while_the_source_covers_the_asset(self):
        # Unchanged precedence: the manual list is the FALLBACK for an asset the
        # source does not price. Once the source covers it and quotes it, the
        # API price is used (and translated), and the manual entry sits idle.
        cfg = base_cfg(api_units_per_reference_unit=0.88, manual_prices={"USDX": 100.0})
        rates, statuses = run_poll(cfg, {"USDX": 1.0})
        self.assertEqual(statuses["USDX"], "admitted")
        self.assertEqual(rates["USDX"], round(1.0 * ps.COIN * ps.COIN / round(0.88 * ps.COIN)))


class ApiPricesTranslate(unittest.TestCase):
    """The factor is what re-expresses API prices, and it is inert on ratios."""

    def test_translation_is_the_price_divided_by_the_factor(self):
        rates, _ = run_poll(base_cfg(api_units_per_reference_unit=0.88), LIVE_PRICES)
        ref = round(0.88 * ps.COIN)
        for tk, price in LIVE_PRICES.items():
            self.assertEqual(rates[tk], round(ps.scaled_rate(price, 8) * ps.COIN / ref), tk)

    def test_relative_values_survive_any_factor(self):
        # The economic content of the whitelist is ratios; every factor gives the
        # same ones, to within the rounding of the published integers (a big
        # factor shrinks every rate, so the rounding slack grows with it).
        want = LIVE_PRICES["GOLD"] / LIVE_PRICES["USDX"]
        for factor in (1.0, 0.88, 0.375, 1234.5):
            rates, _ = run_poll(base_cfg(api_units_per_reference_unit=factor), LIVE_PRICES)
            got = rates["GOLD"] / rates["USDX"]
            slack = want * (0.5 / rates["GOLD"] + 0.5 / rates["USDX"])
            self.assertAlmostEqual(got, want, delta=slack, msg=factor)


class ReferenceFactorAccessor(unittest.TestCase):

    def factor(self, cfg):
        return ps.PriceServer(base_cfg(**cfg), dry_run=True).reference_factor()

    def test_default_is_the_identity(self):
        self.assertEqual(self.factor({}), (1.0, None))

    def test_new_key(self):
        self.assertEqual(self.factor({"api_units_per_reference_unit": 0.88}),
                         (0.88, "api_units_per_reference_unit"))

    def test_legacy_key_is_read_as_a_fallback(self):
        self.assertEqual(self.factor({"reference_price_usd": 0.375}),
                         (0.375, "reference_price_usd"))

    def test_the_new_key_takes_precedence_over_the_legacy_one(self):
        self.assertEqual(self.factor({"api_units_per_reference_unit": 2.0,
                                      "reference_price_usd": 0.375}),
                         (2.0, "api_units_per_reference_unit"))

    def test_junk_falls_back(self):
        with self.assertLogs(ps.log, "WARNING"):
            self.assertEqual(self.factor({"api_units_per_reference_unit": "not a number",
                                          "reference_price_usd": 0.5}),
                             (0.5, "reference_price_usd"))
        with self.assertLogs(ps.log, "WARNING"):
            self.assertEqual(self.factor({"api_units_per_reference_unit": 0}), (1.0, None))

    def test_a_factor_too_small_to_apply_falls_back(self):
        # At or below half a unit in the last place the divisor round(factor *
        # 1e8) is 0, so the factor cannot be applied at all. Refusing it here is
        # what keeps the reported frame and the applied frame the same frame.
        for tiny in (1e-9, 5e-9):  # 5e-9 included: round(0.5) is 0, not 1
            with self.assertLogs(ps.log, "WARNING"):
                self.assertEqual(self.factor({"api_units_per_reference_unit": tiny}), (1.0, None), tiny)
        self.assertEqual(self.factor({"api_units_per_reference_unit": 5.1e-9}),
                         (5.1e-9, "api_units_per_reference_unit"))

    def test_a_too_small_factor_still_falls_through_to_the_legacy_key(self):
        with self.assertLogs(ps.log, "WARNING"):
            self.assertEqual(self.factor({"api_units_per_reference_unit": 1e-9,
                                          "reference_price_usd": 0.5}),
                             (0.5, "reference_price_usd"))

    def test_an_unusable_factor_is_not_applied_silently(self):
        # The whole point: the rates published under a refused factor are the
        # identity ones the UI will now describe, not an untranslated map hiding
        # behind a frame nobody applied.
        with self.assertLogs(ps.log, "WARNING"):
            rates, _ = run_poll(base_cfg(api_units_per_reference_unit=1e-9), LIVE_PRICES)
        self.assertEqual(rates, LIVE_RATES)

    def test_the_smallest_usable_factor_is_applied(self):
        rates, _ = run_poll(base_cfg(api_units_per_reference_unit=5.1e-9), {"USDX": 1.0})
        self.assertEqual(rates["USDX"], round(ps.COIN * ps.COIN / round(5.1e-9 * ps.COIN)))


class LegacyReferenceAssetLabel(unittest.TestCase):
    """The pinned-token mode is untouched, except that manual prices no longer
    get dragged through it either."""

    def test_the_labelled_token_is_pinned_to_coin(self):
        rates, _ = run_poll(base_cfg(reference_asset_label="USDX"), LIVE_PRICES)
        self.assertEqual(rates["USDX"], ps.COIN)
        self.assertEqual(rates["GOLD"], round(ps.scaled_rate(LIVE_PRICES["GOLD"], 8) * ps.COIN
                                              / ps.scaled_rate(LIVE_PRICES["USDX"], 8)))

    def test_a_manual_price_is_published_as_entered(self):
        cfg = base_cfg(reference_asset_label="GOLD", manual_prices={"MANU": 100.0})
        cfg["source"] = dict(cfg["source"], mode="except", assets=["MANU"])
        rates, _ = run_poll(cfg, dict(LIVE_PRICES, MANU=7.0))
        self.assertEqual(rates["MANU"], 100 * ps.COIN)

    def anchored_on_a_manual_price(self):
        """The anchor named by the label is the one asset the API does not price,
        so the operator prices it by hand. This is the config the split broke."""
        cfg = base_cfg(reference_asset_label="ANCHOR", manual_prices={"ANCHOR": 2.0})
        cfg["source"] = dict(cfg["source"], mode="except", assets=["ANCHOR"])
        return cfg

    def test_a_manually_priced_anchor_still_publishes(self):
        # The regression: the anchor lived in the manual map, _denominate could
        # not see it, and the server published NOTHING every poll.
        rates, statuses = run_poll(self.anchored_on_a_manual_price(),
                                   {"ANCHOR": 9.0, "USDX": 1.0, "GOLD": LIVE_PRICES["GOLD"]})
        self.assertEqual(statuses["ANCHOR"], "admitted (manual price)")
        self.assertEqual(len(rates), 3, rates)
        # The label declares the anchor to BE the reference unit, so it is 1e8,
        # and the hand-entered 2.0 is what the market-priced assets divide by.
        self.assertEqual(rates["ANCHOR"], ps.COIN)
        self.assertEqual(rates["USDX"], round(ps.scaled_rate(1.0, 8) * ps.COIN / ps.scaled_rate(2.0, 8)))
        self.assertEqual(rates["GOLD"], round(ps.scaled_rate(LIVE_PRICES["GOLD"], 8) * ps.COIN
                                              / ps.scaled_rate(2.0, 8)))

    def test_an_api_priced_anchor_wins_over_a_manual_entry_for_it(self):
        # Resolution order: api_rates first. A stale manual entry for an asset the
        # feed does price is never consulted, exactly as when no label is set.
        cfg = base_cfg(reference_asset_label="USDX", manual_prices={"USDX": 2.0})
        rates, statuses = run_poll(cfg, LIVE_PRICES)
        self.assertEqual(statuses["USDX"], "admitted")
        self.assertEqual(rates["USDX"], ps.COIN)

    def test_every_other_manual_price_keeps_meaning_anchor_units(self):
        cfg = self.anchored_on_a_manual_price()
        cfg["source"] = dict(cfg["source"], assets=["ANCHOR", "MANU"])
        cfg["manual_prices"] = {"ANCHOR": 2.0, "MANU": 100.0}
        rates, _ = run_poll(cfg, {"ANCHOR": 9.0, "USDX": 1.0, "MANU": 7.0})
        self.assertEqual(rates["MANU"], 100 * ps.COIN)  # 100 anchor tokens, as typed
        self.assertEqual(rates["ANCHOR"], ps.COIN)

    def test_an_anchor_with_no_price_at_all_still_retains_last_good(self):
        cfg = base_cfg(reference_asset_label="NOSUCH")
        with self.assertLogs(ps.log, "WARNING"):
            rates, _ = run_poll(cfg, LIVE_PRICES)
        self.assertEqual(rates, {})


class TheReportedFrameIsThePublishedFrame(unittest.TestCase):
    """What the server SAYS it did has to be what it did."""

    def decisions(self, cfg, prices, **kw):
        rates, srv = poll_server(cfg, prices, **kw)
        return rates, {r["ticker"]: r for r in srv.last_report}

    def test_every_decision_row_states_the_published_rate(self):
        # Before: API rows carried the pre-denomination rate and manual rows the
        # published one, so at factor 0.5 an API dollar and a manual one read
        # identically in /api/whitelist while differing in the rates map.
        cfg = base_cfg(api_units_per_reference_unit=0.5, manual_prices={"MANU": 1.0})
        cfg["source"] = dict(cfg["source"], mode="except", assets=["MANU"])
        rates, rows = self.decisions(cfg, {"USDX": 1.0, "MANU": 3.0})
        self.assertEqual(rates["USDX"], 2 * ps.COIN)
        self.assertEqual(rates["MANU"], 1 * ps.COIN)
        self.assertEqual(rows["USDX"]["rate"], rates["USDX"])
        self.assertEqual(rows["MANU"]["rate"], rates["MANU"])
        self.assertNotEqual(rows["USDX"]["rate"], rows["MANU"]["rate"])

    def test_rows_state_the_unit_their_price_is_in(self):
        cfg = base_cfg(api_units_per_reference_unit=0.5, manual_prices={"MANU": 1.0})
        cfg["source"] = dict(cfg["source"], mode="except", assets=["MANU"])
        _rates, rows = self.decisions(cfg, {"USDX": 1.0, "MANU": 3.0})
        self.assertEqual(rows["USDX"]["price_unit"], "USD")
        self.assertEqual(rows["MANU"]["price_unit"], "reference units")

    def test_a_rate_lost_to_the_clamp_is_not_reported_as_admitted(self):
        # A factor small enough to inflate a rate past MAX_RATE drops it at the
        # clamp; the row must not keep claiming a rate that was never published.
        factor = 1e-7
        cfg = base_cfg(api_units_per_reference_unit=factor)
        with self.assertLogs(ps.log, "WARNING"):
            rates, rows = self.decisions(cfg, {"GOLD": LIVE_PRICES["GOLD"]})
        self.assertEqual(rates, {})
        self.assertIsNone(rows["GOLD"]["rate"])
        self.assertTrue(rows["GOLD"]["status"].startswith("skipped:"), rows["GOLD"]["status"])


class TheAdminPageNamesTheModeInForce(unittest.TestCase):
    """The reference-unit note has to describe the frame actually applied."""

    def note(self, **cfg):
        card = ps._scope_and_manual_cards(ps.PriceServer(base_cfg(**cfg), dry_run=True))
        start = card.index("Reference unit:")
        return card[start:card.index(" Used only when", start)]

    def test_the_identity_is_named_only_when_it_is_in_force(self):
        self.assertIn("one USD, the market source's own quote currency", self.note())

    def test_a_factor_is_spelled_out(self):
        note = self.note(api_units_per_reference_unit=0.88)
        self.assertIn("0.88 USD", note)
        self.assertIn("api_units_per_reference_unit", note)

    def test_the_legacy_label_is_named_as_the_reference_unit(self):
        # The bug: reference_factor() returns (1.0, None) here, so the note used
        # to announce the USD identity while _denominate pinned the token.
        note = self.note(reference_asset_label="TSEQ")
        self.assertIn("one TSEQ token", note)
        self.assertNotIn("the identity", note)

    def test_a_configured_factor_outranks_the_label_in_the_note_too(self):
        note = self.note(api_units_per_reference_unit=0.88, reference_asset_label="TSEQ")
        self.assertIn("0.88 USD", note)
        self.assertNotIn("TSEQ", note)

    def test_a_refused_factor_is_not_announced(self):
        with self.assertLogs(ps.log, "WARNING"):
            note = self.note(api_units_per_reference_unit=1e-9)
        self.assertIn("one USD, the market source's own quote currency", note)


if __name__ == "__main__":
    unittest.main(verbosity=2)
