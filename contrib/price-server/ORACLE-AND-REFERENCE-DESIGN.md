# Oracle integration & reference-unit design (salvaged from `rates/`)

This captures the useful design from an earlier (2023, Gerbil/Scheme) price-server
prototype that lived in `rates/` before it was removed. The canonical price server
is now the Python one in this directory (`price_server.py` + the `contrib/sequentia/`
helpers). None of the Gerbil code is kept; these are the ideas worth carrying
forward as we wire real oracles and the user-chosen reference currency.

## 1. Oracle source registry

The prototype aggregated quotes from several real providers. When we replace the
mock API with real feeds, these are the providers, their key-acquisition URLs,
free-tier limits, rate limits (`refractory_period`, seconds between queries), and
symbol formats it used:

| Provider | get-key URL | free-tier limit / suggested refractory | symbol format |
|---|---|---|---|
| blockchain.info | (no key) | 300s | `BTC` |
| cex.io | (no key) | 120s | `BTC:USD`, `ETH:USD` (pair) |
| coinapi.io | https://www.coinapi.io/get-free-api-key | 120s | `BTC`, `ETH` |
| coinlayer.com | https://coinlayer.com/product | 100 calls/mo → 28800s (8h); http only | `BTC`, `ETH` |
| coinmarketcap.com | https://coinmarketcap.com/api/pricing/ | free 7200s (2h); $40/mo → 30s | `BTC`, `ETH` |
| financialmodelingprep.com | https://site.financialmodelingprep.com/developer/docs/pricing | 250 calls/day → 600s | `BTCUSD`, `ETHUSD`; `asset_pairs` list |
| polygon.io | https://polygon.io/dashboard/stocks | (stocks) | `AAPL`, `TSLA` |

Notes carried forward:
- **API keys are secret.** Keep the services/keys config OUT of the shared assets
  config and out of git (the prototype split `rates-services-config.json` (secret,
  per-operator keys + refractory) from `rates-assets-config.json` (shareable)).
- **Per-source rate limiting** via a `refractory_period` (min seconds between
  queries to that provider) + an on-disk cache of last-query-time/last-value, so a
  restart doesn't blow the free-tier budget.
- **Aggregation = median** across the oracles configured for an asset (robust to a
  single bad/locked feed). `min_sources` / `max_source_spread` would land with it;
  neither exists in the Python server yet, which reads a single source.
- The provider list shows the market spans **crypto, fiat, and equities** — the
  reference-currency/asset model below must stay that general.

## 2. Reference-unit model: RFU / RFA / fudge_factor / decimals

This is the key idea for the user's "pick any reference currency" feature. The
reference denomination need NOT be a Sequentia asset — it can be USD, BTC, etc.

**Settled since:** it must never BE one either. The shipped server states the
reference unit as an abstract conversion factor
(`api_units_per_reference_unit`), and the mode that named an on-chain token as
the reference was removed. A reference unit that is a token stops being a
denomination and becomes a privileged asset wearing one: every other rate turns
into a quote against that token's fortunes. In Sequentia the Sequence token
(ticker `SEQ`) has equal standing with every issued asset, so no asset is the
unit of account. Read `nAsset: false` below as the only shape a future RFU may
take; a unit that happens to equal one token today is expressed as that token's
price in the feed's numeraire, which floats as soon as the price moves.

Two things the prototype ran together have to be kept apart when reading the rest
of this section, because the template below shows only the second:

1. **The RFU row** is a pure unit: `nAsset: false`, a `decimals` setting, and a
   constant. It names no asset and nothing on chain is pinned by it.
2. **A constant-priced token** is an ordinary asset that happens to have a fixed
   quote. It keeps its `nAsset`, and it is *not* the reference unit. The
   prototype's USD row is this, not the RFU: an on-chain, cents-denominated USD
   token whose oracle is `constant`, which is why it still carries an `nAsset`.

The prototype blurred them by saying the `constant` oracle "pegs the RFU". It
does not: it prices an asset. Only shape 1 defines the unit. In the shipped
server shape 1 is `api_units_per_reference_unit` and shape 2 is a manual price
(entered in reference units, conferring no privilege), and there is no key that
names a token as the unit.

- **RFU** — Reference Fee Unit: the chosen reference currency (e.g. 1 USD, 1 BTC).
- **RFA** — Reference Fee Atom: the smallest unit fees are accounted in. `decimals`
  on the RFU sets how many RFA per RFU (e.g. RFU=USD with `decimals: 9` ⇒ 1 RFU = 1e9 RFA).
- Per **asset** config: `nAsset` (on-chain asset id, or false for a pure unit like
  the RFU itself), `decimals` (the asset's on-chain atoms per whole unit, default 8
  like 1 BTC = 1e8 sat), `fudge_factor` (default 1), and `oracles` (a map of
  provider → that provider's symbol for this asset; the special `constant` oracle
  reports a fixed number). A `constant` oracle on a row with an `nAsset` prices
  that token and nothing else; the RFU is the `nAsset: false` row, per the two
  shapes above.

`fudge_factor` is a per-asset fee-pricing lever:
- `1.03` makes the asset ~3% **cheaper** to use for fees (node overvalues it →
  user pays fees at a discount) — an incentive.
- `0.97` puts a ~3% **premium** on it (disincentive / volatility cover).

Two outputs (mirrors our two concerns — display vs fees):
- **getrates(asset)** → value of one *whole unit* of the asset in RFU. Ignores
  `fudge_factor` and `decimals`. This is the **display/reference-currency**
  number (what the UI shows when a user values things in their chosen RFU).
- **getfeeexchangerates(asset)** → value of one *atom* of the asset in RFA, with
  `fudge_factor` and `decimals` applied. This is what feeds the node's fee-asset
  whitelist (atoms-of-asset per reference unit).

Worked example from the prototype (RFU = USD, `decimals: 9`; USD token uses cents,
`decimals: 2`, `fudge_factor: 1.03`): `getrates` returns `1`, while
`getfeeexchangerates` returns `10300000` (1 cent = 0.01 USD = 1e7 RFA, ×1.03).

## 3. Per-asset config schema (template)

```json
{
  "_RFU": { "nAsset": false, "decimals": 9, "oracles": { "constant": 1 } },
  "USD": { "nAsset": "00..0099", "decimals": 2, "fudge_factor": 1.03,
           "oracles": { "constant": 1 } },
  "BTC": { "nAsset": "00..0001", "decimals": 8,
           "oracles": { "blockchain.info": "BTC", "cex.io": "BTC:USD",
                        "coinapi.io": "BTC", "coinmarketcap.com": "BTC" } },
  "AAPL":{ "nAsset": "00..0101", "decimals": 6, "oracles": { "polygon.io": "AAPL" } }
}
```

The first row is the RFU (shape 1): `nAsset: false`, so it is a unit and not an
asset. The `USD` row is shape 2, an on-chain USD token quoted at a constant; its
`nAsset` is correct and does not make it the reference unit. The two coincide at
1:1 here only because the prototype chose that constant, and the coincidence
confers nothing: the row is scaled, `fudge_factor`-ed and admitted exactly like
`BTC` or `AAPL`.

Also stale in the prototype's favour: `min_sources` and `max_source_spread` are
named elsewhere in this file as things the Python server already has. It does not
have them. It reads ONE market source, so multi-source aggregation, the median,
and the spread check are all still unbuilt, and a config setting either key is
ignored rather than honoured.

## How this maps to our stack
- The Python price server already has the admission thresholds the Gerbil one
  lacked (`min_market_cap`, `min_volume_24h`, `max_change_factor`,
  `max_volatility`); fold in the **median aggregation**, **per-source refractory
  caching**, `max_source_spread`, and this **oracle registry** when wiring real
  feeds (it reads one source today, so the spread check has nothing to compare).
- The **RFU/fudge/decimals** model is the basis for the user-chosen reference
  currency across the node GUI / explorer / SWK (value anything in the user's RFU
  via `getrates`-style data), and for per-asset fee incentives via `fudge_factor`.
- The node consumes only the final per-asset rate (atoms-of-asset per reference
  unit) via `setfeeexchangerates` (`persist=false` for automated pushes); all of the above lives in
  the sidecar.
