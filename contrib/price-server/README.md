# Sequentia price server

A small, stdlib-only Python sidecar that keeps a Sequentia node's **fee-asset
whitelist** up to date, and (optionally) republishes its prices to the world.

Each poll it:

1. **discovers** the asset universe from the network's Asset Registry
   (ticker → asset id, issuer domain) — you never list assets by hand;
2. **fetches prices** for those assets from one market-data API;
3. applies your **admission rules** to decide which assets are allowed to pay
   transaction fees, and at what rate;
4. **publishes** the result to your node(s) via the `setfeeexchangerates` RPC
   with `persist=false`.

The node itself keeps a single whitelist and is unaware of all this — running
this sidecar is what makes the whitelist "dynamic". Stop the sidecar and the
whitelist just stops changing (each node keeps the last set it received; on a
node restart it falls back to the operator's static file). Nothing here touches
consensus: it only decides which assets your node accepts fees in.

No dependencies — just Python 3. Run the node with `-con_any_asset_fees=1`.

## Quick start

```bash
python3 price_server.py --config config.json                 # poll forever
python3 price_server.py --config config.json --once          # one poll, print, exit
python3 price_server.py --config config.json --dry-run       # decide + log, don't publish
python3 price_server.py --config config.json --ui-port 8090  # + web UI on http://127.0.0.1:8090
python3 price_server.py --config config.json --set-password  # set the admin password, then exit
```

Copy `config.example.json` to `config.json` as a starting point; its defaults
point at the public Sequentia testnet demo feed, so a fresh install works out
of the box. From the desktop node GUI, **Settings → Price server** launches the
bundled copy with a UI on localhost — no setup at all.

## The web UI: one port, two audiences

### Admin area — `/admin`

Everything is configurable here; the config file never needs hand-editing.

| Tab | What it controls |
|---|---|
| **Status** | Live decisions per asset (price, market cap, volume, admitted/rejected **and why**), last push result per node. |
| **Market source** | The market-data API URL, its **quote currency** (see below), API format, the Asset Registry URL, the source **scope** and **manual prices** (see below), poll interval, publisher name. |
| **Admission rules** | The criteria (market cap, volume, price-jump, volatility, issuer allow-list), ALL/ANY combination, always-admit / always-reject exceptions. |
| **Nodes** | The node RPCs that receive the whitelist — as many as you like (host, port, user+password or cookie file). |
| **Access** | The public toggles, the rate limit, the default table page size, and the admin password. |

The asset tables (public page and admin Status) are **sortable** — click any
column header — with a **text filter** and a **rows-per-page selector**, so
they stay usable with hundreds of assets. The default page size is
configurable (Access tab, `ui.page_size`; 0 = show all).

**How admin access works:**

- **No password set** → the admin area answers **only to requests from the
  same machine** (127.0.0.1). This is the desktop-GUI flow: launch, browser
  opens, configure — zero setup. The server **refuses to bind** to a
  non-loopback address in this state (`--ui-allow-remote` overrides, at your
  own risk).
- **Password set** → login is required from everywhere (localhost included).
  Sessions are cookies (HttpOnly, 12 h sliding); config writes are additionally
  protected by a same-origin check and a CSRF token that rotates on restart.
- Set or change the password from the **Access** tab, or from the CLI:
  `python3 price_server.py --config config.json --set-password`
  (restart the server afterwards). The config stores only a PBKDF2 hash
  (`pbkdf2$200000$salt$hash`), never the password.

### Public area — off by default, per-toggle in the Access tab

| Toggle | What it opens |
|---|---|
| **Public read-only price page** | `GET /` (and `/public`) — a human-readable page: every asset's price, market cap, volume and admission decision with its reason. Never shows the config or your nodes. |
| **Public JSON API** | `GET /api/prices` and `GET /api/whitelist` (below). |

Both default to **off**: a fresh install exposes nothing. Turn them on to let
anyone who can reach the port consult your prices — including other operators
who want to use your server as *their* market source.

## JSON API

No authentication needed once enabled (it is public by definition). Disabled
endpoints answer `403 {"error": "disabled by the operator"}`.

### `GET /api/prices`

Every asset keyed by ticker, in the **Sequentia combined format** — exactly
what the *Market source* tab of any price server expects. So chaining servers
is one config field: point your `source.url` at another operator's
`/api/prices` and you consume their prices with your own admission rules.

```json
{
  "SEQ":  {"price": 0.0471, "market_cap": 4520800274.4, "volume_24h": 382294309.1},
  "USDX": {"price": 1.0545, "market_cap": 3000375933.4, "volume_24h": 18521685.6},
  "_meta": {"quote_currency": "USD", "updated": 1784034446.66,
            "publisher": "alberto-vps", "format": "sequentia"}
}
```

(`_meta` is informational; consumers of the Sequentia format ignore entries
without a `price` field.)

### `GET /api/whitelist`

The rates as pushed to the nodes, plus the per-asset decisions:

```json
{
  "updated": 1784034446.66,
  "quote_currency": "USD",
  "rates":     {"<asset-id-64-hex>": 5238188, "...": 100000000},
  "decisions": [{"ticker": "SEQ", "id": "<asset-id>", "domain": "sequentia.io",
                 "price": 0.0524, "rate": 5238188, "status": "admitted"}]
}
```

A rate is the value of **one whole unit of the asset, in the quote currency,
scaled by 1e8** — so `rate / 1e8` is simply the asset's price in (usually) USD.

### Rate limiting

Public requests are limited **per client IP** — default **30 requests/minute**,
configurable in the Access tab (`ui.api_rate_limit_per_min`). Over the limit
the server answers `429` with a `Retry-After` header. Logged-in admins are
never limited. The limiter protects the host (typically a small VPS), not the
data — the data is public once you enable the toggles. Put a real reverse
proxy in front if you expect serious traffic.

## Quote currency (there is no "reference asset" to configure)

Prices are expressed in whatever currency the market-data API quotes —
normally plain **USD**. It does **not** need to exist as an on-chain asset. Set
it once in the Market source tab; every price, market cap and volume shown,
and every rate pushed to the nodes, is in that unit. (A legacy
`reference_asset_label` config key can re-denominate rates against an on-chain
asset; it is advanced, off by default, and not exposed in the UI — see
`ORACLE-AND-REFERENCE-DESIGN.md`.)

## Market-source scope & manual prices

By default the market source prices **every** asset discovered from the
registry. In the *Market source* tab you can scope it:

- **All assets** — the standard behaviour.
- **All except the listed ones** — the listed assets are not priced from the
  market source.
- **Only the listed ones** — everything else is not priced from it.

Entries match by registry ticker, feed key (alias) or 64-hex asset id.

An asset left **without a market price** — because it is out of scope, or the
feed simply doesn't quote it — can carry a **manual price**: a fixed value per
unit in the quote currency (e.g. `0.5` = 1 unit is worth 0.5 USD). A manually
priced asset is admitted at that fixed rate, bypassing the market criteria —
the operator setting a price by hand *is* the decision — but an
**always-reject** exception still wins. Without a market price *and* without a
manual price, the asset is skipped (not whitelisted).

## Admission rule engine

An asset is admitted to the whitelist if it passes the configured criteria,
combined per `require`:

- `"require": "all"` — every ticked criterion must pass (default).
- `"require": "any"` — at least one passes.

**Criteria (all optional — untick/omit to disable):**

| key | meaning |
|---|---|
| `min_market_cap` | reject below this market cap (in the quote currency) |
| `min_volume_24h` | reject below this 24h volume — a liquidity floor |
| `max_change_factor` | reject a single-poll price jump larger than this factor (guards against a bad print) |
| `max_volatility` | reject if the rolling stddev of log-returns over `volatility_window` polls exceeds this |
| `min_price` / `max_price` | sanity floor/cap on the unit price |
| `issuer_domains` | admit assets whose registry-verified issuer is in the list |

**Exceptions** (override the criteria; reject wins): `always_admit`,
`always_reject` — each a list of tickers or 64-hex asset ids.

Always-on sanity regardless of rules: a positive price is required, and a rate
that rounds to 0 or exceeds the node's parse limit is dropped per-asset (one
absurd quote can never poison the rest of the batch).

## Config schema (outline)

Everything below is editable from the UI; the UI reads and writes the same
file passed to `--config` (atomically). Download a backup any time from
**Access → Download the current config** — it includes node RPC credentials,
keep it private.

```jsonc
{
  "poll_interval_secs": 60,
  "source_timeout": 15,
  "source_name": "my-price-server",        // publisher name shown on the public page / _meta

  "node_rpcs": [                            // every node that receives the whitelist
    {"name": "local", "host": "127.0.0.1", "port": 7041,
     "user": "rpcuser", "password": "rpcpassword"}   // or "cookie": "/path/.cookie"
  ],

  "registry_url": "http://159.195.15.140/registry/index.minimal.json",

  "source": {
    "url": "http://159.195.15.140/prices",  // or another operator's /api/prices
    "quote_currency": "USD",
    "format": "sequentia",                  // "custom" adds price_path / market_cap_path / volume_24h_path
    "mode": "all",                          // "all" | "except" | "only" (scope, see above)
    "assets": []                            // tickers/ids the mode refers to
  },

  "manual_prices": {},                      // {"TICKER": price-in-quote} for assets without a market source
  "feed_aliases": {"TSEQ": "SEQ"},          // registry ticker -> feed key, when they differ

  "default_thresholds": {"require": "all"}, // admission rules (see above)
  "exceptions": {"always_admit": [], "always_reject": []},

  "ui": {
    "public_status": false,                 // read-only price page at /
    "public_api": false,                    // /api/prices + /api/whitelist
    "api_rate_limit_per_min": 30,           // per client IP; admins exempt
    "page_size": 50,                        // default table rows per page (0 = all)
    "password_hash": ""                     // set via --set-password or the Access tab, never by hand
  }
}
```

## Deploying on a server (systemd example)

```ini
[Unit]
Description=Sequentia price server
After=sequentia.service

[Service]
Type=simple
User=sequentia
ExecStart=/usr/bin/python3 /opt/sequentia/price-server/price_server.py \
    --config /data/price-server/config.json --ui-port 8090 --ui-host 0.0.0.0
Restart=on-failure
RestartSec=15
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

Order of operations for a public deployment:

1. create the config (start from `config.example.json`; `chmod 600` — it holds
   node RPC credentials);
2. `--set-password` (the server refuses `--ui-host 0.0.0.0` without one);
3. start the service, log in at `/admin`, enable the public toggles you want;
4. open the port on your firewall.

## Inspecting state on the node

```sh
elements-cli getfeeexchangerates            # the whitelist (asset -> rate)
elements-cli getfeeacceptancepolicy         # the whitelist as {asset: {rate}}
elements-cli setfeeexchangerates '{...}'    # operator manual set (persists to exchangerates.json)
elements-cli setfeeexchangerates '{}'       # empty the whitelist
```

## Windows installer bundle

The Windows installer ships this sidecar with a self-contained interpreter so
the node GUI's **Settings → Price server** launcher works with no Python
install. That interpreter (the official embeddable CPython) is **not** vendored
in git — run `./fetch-embeddable-python.sh` once before `make deploy` to
download and verify it into `./python/` (git-ignored). `price_server.py` is
stdlib-only, so the stock embeddable distribution suffices.

## Security model, in one table

| Surface | Who can reach it | Notes |
|---|---|---|
| `/admin` + all POSTs | Admin only | password (PBKDF2) + session cookie; same-origin + CSRF on writes; loopback-only when no password is set |
| `/`, `/public` | Everyone, **if** `public_status` | read-only; never shows config or nodes |
| `/api/prices`, `/api/whitelist` | Everyone, **if** `public_api` | rate-limited per IP |
| Node RPC credentials | Never served | live only in the config file (600) and in RAM |
| Failed logins | — | logged with the client IP; each attempt costs a flat delay |

For wiring real market-data oracles (providers, keys, rate limits) and the
RFU/fudge reference-unit model, see `ORACLE-AND-REFERENCE-DESIGN.md`.
