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
  single bad/locked feed). Pairs nicely with the Python server's existing
  `min_sources` / `max_source_spread`.
- The provider list shows the market spans **crypto, fiat, and equities** — the
  reference-currency/asset model below must stay that general.

## 2. Reference-unit model: RFU / RFA / fudge_factor / decimals

This is the key idea for the user's "pick any reference currency" feature. The
reference denomination need NOT be a Sequentia asset — it can be USD, BTC, etc.

- **RFU** — Reference Fee Unit: the chosen reference currency (e.g. 1 USD, 1 BTC).
- **RFA** — Reference Fee Atom: the smallest unit fees are accounted in. `decimals`
  on the RFU sets how many RFA per RFU (e.g. RFU=USD with `decimals: 9` ⇒ 1 RFU = 1e9 RFA).
- Per **asset** config: `nAsset` (on-chain asset id, or false for a pure unit like
  the RFU itself), `decimals` (the asset's on-chain atoms per whole unit, default 8
  like 1 BTC = 1e8 sat), `fudge_factor` (default 1), and `oracles` (a map of
  provider → that provider's symbol for this asset; the special `constant` oracle
  reports a fixed number, used to peg the RFU, e.g. USD = 1).

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
  "USD": { "nAsset": "00..0099", "decimals": 2, "fudge_factor": 1.03,
           "oracles": { "constant": 1 } },
  "BTC": { "nAsset": "00..0001", "decimals": 8,
           "oracles": { "blockchain.info": "BTC", "cex.io": "BTC:USD",
                        "coinapi.io": "BTC", "coinmarketcap.com": "BTC" } },
  "AAPL":{ "nAsset": "00..0101", "decimals": 6, "oracles": { "polygon.io": "AAPL" } }
}
```

## How this maps to our stack
- The Python price server already has the admission thresholds the Gerbil one
  lacked (`min_market_cap`, `min_volume_24h`, `max_source_spread`,
  `max_change_factor`); fold in the **median aggregation**, **per-source refractory
  caching**, and this **oracle registry** when wiring real feeds.
- The **RFU/fudge/decimals** model is the basis for the user-chosen reference
  currency across the node GUI / explorer / SWK (value anything in the user's RFU
  via `getrates`-style data), and for per-asset fee incentives via `fudge_factor`.
- The node consumes only the final per-asset rate (atoms-of-asset per reference
  unit) via `setfeeexchangerates`/`setdynamicfeerates`; all of the above lives in
  the sidecar.
