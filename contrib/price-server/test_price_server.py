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
  * the legacy factor KEY still works (it names a factor, so it stays);
  * the reference unit is ALWAYS an abstract factor: the pinned-token mode is
    gone, and a config still asking for it is refused rather than run under a
    silently different denomination;
  * the frame the server REPORTS is the frame it APPLIES: an unusable factor is
    refused rather than silently ignored, the admin page names the frame actually
    in force, /api/whitelist discloses it, and every decision row states the rate
    that was published.
"""

import importlib.util
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import price_server as ps  # noqa: E402

README = os.path.join(HERE, "README.md")
GENERATOR = os.path.join(os.path.dirname(HERE), "sequentia", "gen-price-config.py")


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


class ThePinnedTokenModeIsGone(unittest.TestCase):
    """The reference unit is always an abstract factor. A token pinned at 1e8
    would be a privileged anchor every other rate quotes against, which is what
    the factor exists to prevent, so the mode was removed and a config still
    asking for it is REFUSED. Ignoring the key would publish a whitelist in a
    denomination the operator never chose."""

    def test_a_config_that_still_pins_a_token_is_refused(self):
        with self.assertRaises(ps.ConfigError) as caught:
            ps.PriceServer(base_cfg(reference_asset_label="USDX"), dry_run=True)
        msg = str(caught.exception)
        self.assertIn("reference_asset_label", msg)
        self.assertIn("api_units_per_reference_unit", msg)  # what to use instead
        self.assertIn("abstract factor", msg)               # and why

    def test_the_check_is_reusable_and_passes_a_clean_config(self):
        self.assertIsNone(ps.check_config(base_cfg(api_units_per_reference_unit=0.88)))
        with self.assertRaises(ps.ConfigError):
            ps.check_config({"reference_asset_label": "SEQ"})

    def test_a_pinning_config_never_reaches_a_poll(self):
        # Refusal happens at construction, so there is no path on which a round
        # is published under a denomination the server cannot produce.
        with self.assertRaises(ps.ConfigError):
            run_poll(base_cfg(reference_asset_label="USDX"), LIVE_PRICES)

    def test_the_key_is_not_a_factor_and_is_not_read_as_one(self):
        self.assertNotIn("reference_asset_label", ps.REFERENCE_FACTOR_KEYS)

    def test_the_equivalent_factor_reproduces_the_old_denomination(self):
        # The migration the error message prescribes: the factor is the token's
        # price in the API numeraire, so the ratios are the ones the label mode
        # produced, and an 8-DECIMAL token lands on 1e8 by arithmetic rather than
        # by being pinned there. Other precisions do not land on 1e8: see
        # TheMigrationGuidanceCarriesThePrecisionTerm.
        anchor = LIVE_PRICES["EURX"]  # 1.1375, so the frame really moves
        rates, _ = run_poll(base_cfg(api_units_per_reference_unit=anchor), LIVE_PRICES)
        self.assertEqual(rates["EURX"], ps.COIN)
        for tk, price in LIVE_PRICES.items():
            # exactly what the removed mode computed: divide by the anchor's rate
            self.assertEqual(rates[tk], round(ps.scaled_rate(price, 8) * ps.COIN
                                              / ps.scaled_rate(anchor, 8)), tk)


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

    def test_the_report_and_the_rates_always_come_from_the_same_round(self):
        # The removed pinned-token mode had a "reference asset unavailable this
        # round" branch that stored THIS round's report beside the PREVIOUS
        # round's rates: API rows pre-denomination, manual rows post, and none of
        # them published. Nothing decides the frame from the data any more, so
        # step 4 always yields a map and the two are always in step.
        cfg = base_cfg(api_units_per_reference_unit=0.88, manual_prices={"MANU": 5.0})
        cfg["source"] = dict(cfg["source"], mode="except", assets=["MANU"])
        rates, rows = self.decisions(cfg, dict(LIVE_PRICES, MANU=7.0))
        self.assertEqual({r["id"]: r["rate"] for r in rows.values() if r["rate"] is not None},
                         {asset_id(tk): rate for tk, rate in rates.items()})

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

    def test_a_refused_factor_is_not_announced(self):
        with self.assertLogs(ps.log, "WARNING"):
            note = self.note(api_units_per_reference_unit=1e-9)
        self.assertIn("one USD, the market source's own quote currency", note)

    def test_the_manual_price_hint_states_one_frame_for_every_row(self):
        # There is no token to special-case any more, so the hint above the
        # manual-price rows says "reference units" and means it for every row.
        card = ps._scope_and_manual_cards(ps.PriceServer(base_cfg(api_units_per_reference_unit=0.88),
                                                         dry_run=True))
        self.assertIn("expressed in this server's <b>reference units</b>", card)
        self.assertIn("never re-denominated", card)
        self.assertNotIn("token", card)


class TheWhitelistApiDisclosesTheFrame(unittest.TestCase):
    """/api/whitelist is consumed by other operators' price servers, so it has to
    say which unit its rates are in, not only which currency the source quotes."""

    def payload(self, cfg, prices):
        _rates, srv = poll_server(cfg, prices)
        return ps._whitelist_payload(srv)

    def test_the_factor_and_the_key_in_force_are_published(self):
        p = self.payload(base_cfg(api_units_per_reference_unit=0.88), LIVE_PRICES)
        self.assertEqual(p["reference_unit"]["api_units_per_reference_unit"], 0.88)
        self.assertEqual(p["reference_unit"]["config_key"], "api_units_per_reference_unit")
        self.assertEqual(p["reference_unit"]["quote_currency"], "USD")
        self.assertEqual(p["quote_currency"], "USD")  # unchanged for older consumers

    def test_the_default_identity_is_stated_explicitly(self):
        p = self.payload(base_cfg(), LIVE_PRICES)
        self.assertEqual(p["reference_unit"]["api_units_per_reference_unit"], 1.0)
        self.assertIsNone(p["reference_unit"]["config_key"])  # nothing configured

    def test_the_legacy_factor_key_is_named_as_the_one_in_force(self):
        p = self.payload(base_cfg(reference_price_usd=0.375), LIVE_PRICES)
        self.assertEqual(p["reference_unit"]["config_key"], "reference_price_usd")

    def test_the_payload_is_json_serialisable_and_carries_the_published_rates(self):
        p = self.payload(base_cfg(api_units_per_reference_unit=0.88), LIVE_PRICES)
        round_tripped = json.loads(json.dumps(p))
        self.assertEqual(len(round_tripped["rates"]), len(LIVE_RATES))
        self.assertTrue(all(r["rate"] == round_tripped["rates"][r["id"]]
                            for r in round_tripped["decisions"]), round_tripped["decisions"])


class TheMigrationGuidanceCarriesThePrecisionTerm(unittest.TestCase):
    """Converting a pinned-token config to the equivalent factor reproduces the
    RATIOS the pinned mode published, but not its absolute rates.

    The pinned mode divided every rate by the anchor's own scaled rate, which
    carries the anchor's 10**(8 - precision) term. A factor divides by
    round(factor * 1e8), which carries no precision term. So the converted map is
    10**(8 - precision) times the old one and the anchor lands on
    1e8 * 10**(8 - precision). Guidance that says "1e8" flat is right only for an
    8-decimal anchor, and an operator following it on a 2-decimal one gets a
    whitelist 1e6 out."""

    def pinned_map(self, prices, precisions, anchor):
        """What the removed mode published: every rate over the anchor's rate."""
        ref = ps.scaled_rate(prices[anchor], precisions.get(anchor, 8))
        return {tk: max(1, round(ps.scaled_rate(p, precisions.get(tk, 8)) * ps.COIN / ref))
                for tk, p in prices.items()}

    def test_an_8_decimal_anchor_does_land_on_1e8(self):
        anchor = "EURX"
        rates, _ = run_poll(base_cfg(api_units_per_reference_unit=LIVE_PRICES[anchor]), LIVE_PRICES)
        self.assertEqual(rates[anchor], ps.COIN)
        self.assertEqual(rates, self.pinned_map(LIVE_PRICES, {}, anchor))

    def test_a_precision_2_anchor_lands_on_1e14_not_1e8(self):
        # The example the refusal message uses: a token worth 0.05, factor 0.05.
        prices = {"CENTS": 0.05, "USDX": 1.0, "GOLD": LIVE_PRICES["GOLD"]}
        precisions = {"CENTS": 2}
        rates, _ = run_poll(base_cfg(api_units_per_reference_unit=0.05), prices,
                            precisions=precisions)
        self.assertEqual(rates["CENTS"], 10 ** 14)
        self.assertNotEqual(rates["CENTS"], ps.COIN)

    def test_a_precision_2_anchor_scales_the_WHOLE_map_by_1e6(self):
        prices = {"CENTS": 0.05, "USDX": 1.0, "GOLD": LIVE_PRICES["GOLD"]}
        precisions = {"CENTS": 2}
        rates, _ = run_poll(base_cfg(api_units_per_reference_unit=0.05), prices,
                            precisions=precisions)
        pinned = self.pinned_map(prices, precisions, "CENTS")
        for tk in prices:
            # within the pinned map's own rounding, magnified by the same 1e6
            self.assertLessEqual(abs(rates[tk] - pinned[tk] * 10 ** 6), 10 ** 6, tk)

    def test_a_precision_0_anchor_lands_on_1e16(self):
        prices = {"WHOLE": 0.05, "USDX": 1.0}
        rates, _ = run_poll(base_cfg(api_units_per_reference_unit=0.05), prices,
                            precisions={"WHOLE": 0})
        self.assertEqual(rates["WHOLE"], 10 ** 16)

    def test_the_ratios_survive_the_conversion_at_any_anchor_precision(self):
        # What is economically inert is the RATIO, which is why the absolute
        # rescale above is a fee-floor problem and never a repricing.
        prices = {"CENTS": 0.05, "USDX": 1.0, "GOLD": LIVE_PRICES["GOLD"]}
        precisions = {"CENTS": 2}
        rates, _ = run_poll(base_cfg(api_units_per_reference_unit=0.05), prices,
                            precisions=precisions)
        pinned = self.pinned_map(prices, precisions, "CENTS")
        self.assertAlmostEqual(rates["GOLD"] / rates["USDX"],
                               pinned["GOLD"] / pinned["USDX"], delta=1e-3)

    def test_the_refusal_message_states_the_precision_term(self):
        with self.assertRaises(ps.ConfigError) as caught:
            ps.PriceServer(base_cfg(reference_asset_label="CENTS"), dry_run=True)
        msg = str(caught.exception)
        self.assertIn("10**(8 - p)", msg)   # the term, not a bare 1e8
        self.assertIn("1e14", msg)          # worked through on the 0.05 example
        self.assertIn("atoms * rate / 1e8", msg)  # why the fee floor moves

    def test_the_readme_does_not_promise_a_bare_1e8(self):
        with open(README) as f:
            readme = f.read()
        self.assertIn("10**(8 - p)", readme)
        self.assertNotIn("the token lands on `1e8` by arithmetic", readme)


class TheWhitelistNoteStatesTheDenomination(unittest.TestCase):
    """The /api/whitelist note is read by other operators' servers, so its rate
    formula has to be the one the code applies, for every precision in the
    registry (the live one holds precision-0 and precision-2 assets)."""

    def note(self, cfg=None, prices=None, **kw):
        _rates, srv = poll_server(cfg or base_cfg(), prices or LIVE_PRICES, **kw)
        return ps._whitelist_payload(srv)["reference_unit"]["note"]

    def test_the_note_carries_the_precision_term(self):
        note = self.note()
        self.assertIn("10**(8 - the asset's decimals)", note)
        self.assertIn("one reference unit = 1 USD", note)

    def test_the_formula_in_the_note_is_the_one_applied(self):
        # Read the note literally and recompute: scaled by 1e8 and by a further
        # 10**(8 - decimals). A 2-decimal asset at 1.0 publishes 1e14, not 1e8.
        prices = {"CENTS": 1.0, "USDX": 1.0}
        rates, _ = run_poll(base_cfg(), prices, precisions={"CENTS": 2})
        self.assertEqual(rates["CENTS"], round(1.0 * ps.COIN * 10 ** (8 - 2)))
        self.assertEqual(rates["CENTS"], 10 ** 14)
        self.assertEqual(rates["USDX"], ps.COIN)  # 8-decimal: 1e8 alone

    def test_the_note_still_states_the_factor_in_force(self):
        note = self.note(base_cfg(api_units_per_reference_unit=0.88))
        self.assertIn("one reference unit = 0.88 USD", note)

    def test_the_readme_quotes_the_note_verbatim(self):
        # The README prints a sample /api/whitelist body. If the note drifts from
        # the code, the documented API is wrong for every consumer reading it.
        with open(README) as f:
            readme = f.read()
        self.assertIn(self.note(), readme)


class TheQuoteCurrencyHintNamesTheFrame(unittest.TestCase):
    """The admin hint under "Quote currency" used to say the rates pushed to the
    nodes were expressed in it. That is true only at factor 1.0."""

    def hint(self, **cfg):
        srv = ps.PriceServer(base_cfg(**cfg), dry_run=True)
        page = ps._render_admin(srv, "csrf-token")
        start = page.index("The currency the API reports prices in")
        return page[start:page.index("</div>", start)]

    def test_the_hint_no_longer_claims_the_node_rates_are_in_the_quote_currency(self):
        self.assertNotIn("rates pushed to your nodes, are expressed in it", self.hint())

    def test_the_hint_names_reference_units_and_the_factor_that_sets_them(self):
        h = self.hint()
        self.assertIn("reference units", h)
        self.assertIn("api_units_per_reference_unit", h)

    def test_the_hint_still_scopes_prices_and_volumes_to_the_quote_currency(self):
        # Those really ARE in the quote currency; the fix must not blur that.
        self.assertIn("Every price, market cap and volume on these pages", self.hint())


class TheGeneratorEmitsTheCurrentSchema(unittest.TestCase):
    """contrib/sequentia/gen-price-config.py used to emit the PRE-REGISTRY schema
    (a hand-written `assets` list with per-asset `sources`, plus `min_sources`),
    so running it over a live config replaced a working configuration with one
    the current server does not read. It now seeds from config.example.json, the
    schema the server ships, and refuses to clobber an existing config."""

    def generator(self):
        spec = importlib.util.spec_from_file_location("gen_price_config", GENERATOR)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)  # the __main__ guard keeps main() from running
        return mod

    def generate(self, out, *extra):
        return self.generator().main(["--out", out, "--committee", "2", *extra])

    def written(self, tmp, *extra):
        out = os.path.join(tmp, "config.json")
        self.assertEqual(self.generate(out, *extra), 0)
        with open(out) as f:
            return json.load(f)

    def test_the_generated_config_is_one_the_server_accepts(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = self.written(tmp)
        srv = ps.PriceServer(cfg, dry_run=True)  # runs check_config
        self.assertEqual(srv.reference_factor(), (1.0, "api_units_per_reference_unit"))
        self.assertEqual(srv.source()["format"], "sequentia")

    def test_it_emits_the_registry_era_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = self.written(tmp)
        for key in ("source", "registry_url", "feed_aliases", "api_units_per_reference_unit"):
            self.assertIn(key, cfg)
        self.assertEqual(len(cfg["node_rpcs"]), 2)
        self.assertNotIn("node_rpc", cfg)

    def test_it_no_longer_emits_the_pre_registry_shape(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = self.written(tmp)
        self.assertNotIn("assets", cfg)               # assets are DISCOVERED
        self.assertNotIn("min_sources", cfg["default_thresholds"])
        self.assertNotIn("max_source_spread", cfg["default_thresholds"])
        self.assertFalse(any("sources" in v for v in cfg.values() if isinstance(v, dict)))

    def test_it_never_emits_a_removed_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = self.written(tmp)
        for key in ps.REMOVED_KEYS:
            self.assertNotIn(key, cfg)

    def test_every_threshold_it_sets_is_one_the_server_reads(self):
        # A key the server ignores is a rule the operator thinks is enforced.
        with tempfile.TemporaryDirectory() as tmp:
            cfg = self.written(tmp)
        known = {"require", "min_market_cap", "min_volume_24h", "max_change_factor",
                 "max_volatility", "volatility_window", "min_price", "max_price",
                 "issuer_domains"}
        self.assertLessEqual(set(cfg["default_thresholds"]), known)

    def test_it_refuses_to_overwrite_an_existing_config(self):
        # The config UI persists thresholds, manual prices and the admin password
        # hash to this same path; a rerun must not silently drop them.
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "config.json")
            self.assertEqual(self.generate(out), 0)
            with open(out) as f:
                before = f.read()
            edited = json.loads(before)
            edited["ui"]["password_hash"] = "pbkdf2$1$aa$bb"
            with open(out, "w") as f:
                json.dump(edited, f)
            with open(out) as f:
                hand_edited = f.read()
            self.assertEqual(self.generate(out), 1)  # refused
            with open(out) as f:
                self.assertEqual(f.read(), hand_edited)  # untouched
            self.assertEqual(self.generate(out, "--force"), 0)
            with open(out) as f:
                self.assertNotEqual(f.read(), hand_edited)



class AnEmptyRoundNeverEmptiesTheWhitelist(unittest.TestCase):
    """A poll that admits nothing must not be published.

    Before the node's 1:1 fallback was removed, publishing an empty map still left
    a working fee asset, so an empty round was survivable. It is not any more: an
    asset absent from the whitelist is not accepted, the policy asset included, so
    an empty map means every fed node accepts NO fee asset, RecomputeFees evicts,
    the mempools empty and relay stops.
    """

    def test_an_empty_round_keeps_the_last_good_rates(self):
        # A first, healthy round establishes a whitelist.
        cfg = base_cfg()
        rates, srv = poll_server(cfg, LIVE_PRICES)
        self.assertTrue(rates)
        good = dict(srv.last_rates)

        # The feed then returns nothing priceable at all.
        real_registry, real_prices = ps.fetch_registry, ps.fetch_prices
        ps.fetch_registry = lambda url, timeout: {
            tk.upper(): (asset_id(tk), "example.test", 8) for tk in LIVE_PRICES}
        ps.fetch_prices = lambda source, timeout: {
            tk.upper(): {"price": None, "market_cap": None, "volume_24h": None}
            for tk in LIVE_PRICES}
        try:
            out = srv.poll_once()
        finally:
            ps.fetch_registry, ps.fetch_prices = real_registry, real_prices

        self.assertEqual(out, good, "the last good whitelist must be what stays in force")
        self.assertEqual(srv.last_rates, good, "and it must not be overwritten by the empty round")

    def test_the_report_still_reflects_the_failed_round(self):
        """Refusing to publish must not hide WHY: the decisions table still updates."""
        cfg = base_cfg()
        _rates, srv = poll_server(cfg, LIVE_PRICES)
        before = srv.last_poll_ts

        real_registry, real_prices = ps.fetch_registry, ps.fetch_prices
        ps.fetch_registry = lambda url, timeout: {
            tk.upper(): (asset_id(tk), "example.test", 8) for tk in LIVE_PRICES}
        ps.fetch_prices = lambda source, timeout: {
            tk.upper(): {"price": None, "market_cap": None, "volume_24h": None}
            for tk in LIVE_PRICES}
        try:
            srv.poll_once()
        finally:
            ps.fetch_registry, ps.fetch_prices = real_registry, real_prices

        self.assertGreaterEqual(srv.last_poll_ts, before)
        self.assertTrue(all(r["rate"] is None for r in srv.last_report),
                        "every asset should be recorded as unpriced this round")


class ShutdownLeavesTheWhitelistInPlace(unittest.TestCase):
    """Stopping a monitoring sidecar must not take the network's fee policy down."""

    class _RecordingRpc:
        def __init__(self):
            self.url = "http://stub-node"
            self.calls = []

        def call(self, method, *args):
            self.calls.append((method, args))
            return None

    def _server_at_shutdown(self, **cfg_extra):
        cfg = base_cfg(node_rpcs=[], **cfg_extra)
        srv = ps.PriceServer(cfg, dry_run=False)
        rpc = self._RecordingRpc()
        srv.rpcs = [rpc]
        srv.last_rates = {"aa" * 32: 100000000}
        srv.stopping = True   # so run() skips straight to the shutdown tail
        srv.run()
        return rpc

    def test_shutdown_does_not_clear_by_default(self):
        rpc = self._server_at_shutdown()
        self.assertEqual(rpc.calls, [],
                         "clearing on shutdown would leave every fed node accepting no fee asset")

    def test_an_operator_can_still_opt_in(self):
        rpc = self._server_at_shutdown(clear_whitelist_on_shutdown=True)
        self.assertEqual([m for m, _a in rpc.calls], ["setfeeexchangerates"])
        method, args = rpc.calls[0]
        self.assertEqual(args[0], {}, "the opt-in still clears")
        self.assertIs(args[1], False, "and still without persisting, so a restart recovers")

if __name__ == "__main__":
    unittest.main(verbosity=2)
