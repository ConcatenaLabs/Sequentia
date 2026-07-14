# Sequentia price server

A small, stdlib-only Python sidecar that keeps a node's **fee-asset whitelist**
up to date. It polls market-data sources for each configured asset, decides which
assets to admit (and at what rate) via a configurable **admission rule engine**,
and pushes the result to one or more nodes' `setfeeexchangerates` RPC (with `persist=false`).

The node itself keeps a **single** whitelist and is unaware of all this — running
this sidecar is simply what makes the whitelist "dynamic". Stop the sidecar and
the whitelist just stops changing (the node holds whatever was last set). The
sidecar owns the admission thresholds and the add/remove logic.

## Run it

```bash
python3 price_server.py --config config.json                 # poll forever
python3 price_server.py --config config.json --once          # one poll, print, exit
python3 price_server.py --config config.json --dry-run       # decide + log, don't publish
python3 price_server.py --config config.json --ui-port 8089  # + web UI on http://127.0.0.1:8089
python3 price_server.py --config config.json --set-password  # set the admin password, then exit
```

No dependencies — just Python 3. Run the node with `-con_any_asset_fees=1`.

## Windows installer bundle

The Windows installer ships this sidecar with a self-contained interpreter so
the node GUI's **Settings → Price server** launcher works with no Python
install. That interpreter (the official embeddable CPython) is **not** vendored
in git — run `./fetch-embeddable-python.sh` once before `make deploy` to
download and verify it into `./python/` (git-ignored). `price_server.py` is
stdlib-only, so the stock embeddable distribution suffices.

## Web UI & API

`--ui-port <port>` serves a web UI with two separate audiences:

**Admin area** (`/admin`) — every setting, no JSON editing: the market source
(API URL, quote currency, format), the admission rules and exceptions, the
**nodes** that receive the whitelist, polling, and access control. With no
password set the admin area answers only to `127.0.0.1` (the desktop-GUI
flow). Set a password (Access tab, or `--set-password` on the CLI) to log in
from anywhere the UI is reachable; sessions are cookie-based, config writes
keep the same-origin + CSRF protection (the token rotates on restart).

**Public area** (both **off by default**, enabled per toggle in the Access tab):

- `/` — a read-only price page: per-asset price, market cap, volume and the
  admission decision with its reason. Never shows the config or your nodes.
- `GET /api/prices` — every asset keyed by ticker in the Sequentia combined
  format, so **another operator can point their price server at yours** as its
  market source.
- `GET /api/whitelist` — the published rates plus per-asset decisions.

Public requests are rate-limited per client IP (default 30/min, configurable):
the typical host is a small VPS, and the limiter protects the box, not the
data. Logged-in admins are never limited.

All prices, caps and volumes — and the rates pushed to the nodes — are
expressed in the source's **quote currency** (usually plain USD; it does not
need to exist as an on-chain asset).

Binding a non-loopback `--ui-host` is refused until an admin password is set
(`--ui-allow-remote` overrides, at your own risk).

## Admission rule engine

Each asset is admitted to the whitelist if it passes the configured criteria,
combined per `require`:

- `"require": "all"` — every configured criterion must pass (default).
- `"require": "any"` — at least one passes.

**Criteria (all optional — omit a key to disable it):**

| key | meaning |
|---|---|
| `min_market_cap` | reject below this market cap |
| `min_volume_24h` | reject below this 24h volume |
| `max_source_spread` | reject if sources disagree by more than this (relative) |
| `max_change_factor` | reject a single-poll jump larger than this factor |
| `max_volatility` | reject if the rolling stddev of log-returns over `volatility_window` polls exceeds this |
| `issuer_domains` / `issuer_pubkeys` | admit assets whose (registry-verified) issuer is in the list |
| `min_sources` | quorum of sources required (always enforced) |

**Exceptions** (override the criteria; reject wins): `always_admit`, `always_reject`
— each a list of asset ids or labels.

Always-on sanity: a source quorum and a positive price/rate are required
regardless of mode.

Example — admit if market cap ≥ 50M **or** issued by `sequentia.io`, except a banned asset:
```json
"default_thresholds": {
  "require": "any",
  "min_market_cap": 50000000,
  "issuer_domains": ["sequentia.io"],
  "always_reject": ["<assetid-or-label>"]
}
```

## Config schema (outline)

```jsonc
{
  "node_rpcs": [{"url": "http://127.0.0.1:18443/", "user": "...", "password": "..."}],
  "poll_interval_secs": 60,
  "reference_asset_label": "SEQ",          // re-express every rate against this asset (it lands at 1e8)
  "default_thresholds": { ...admission rules above... },
  "assets": [
    {"label": "USDX", "id": "<64-hex>",
     "issuer_domain": "sequentia.io",       // optional, for the issuer criterion
     "sources": [{"kind": "jsonapi", "url": "...", "price_path": "price",
                  "market_cap_path": "market_cap", "volume_24h_path": "volume_24h"}],
     "thresholds": { ... }}                  // optional per-asset overrides of default_thresholds
  ]
}
```

## Rate semantics

The node values fees in **reference fee atoms**: an asset's rate is *the value of one whole asset unit expressed in the
reference*, scaled by 1e8 (e.g. if GOLD is worth 1000 SEQ, rate(GOLD) = 1000 × 1e8). The
operator picks the reference (e.g. SEQ or USD) by pricing all sources in it;
`reference_asset_label` then re-expresses every rate against that asset, which
lands at exactly `100000000`.

## Inspecting state on the node

```sh
elements-cli getfeeexchangerates            # the whitelist (asset -> rate)
elements-cli getfeeacceptancepolicy         # the whitelist as {asset: {rate}}
elements-cli setfeeexchangerates '{...}'    # operator manual set (persists to exchangerates.json)
elements-cli setfeeexchangerates '{}'       # empty the whitelist
```

For wiring real market-data oracles (providers, keys, rate limits) and the
RFU/fudge reference-unit model, see `ORACLE-AND-REFERENCE-DESIGN.md`.
