# Sequentia asset registry server

A self-hosted **Asset Registry**: the service that turns on-chain asset ids
(64 hex characters) into human information — ticker, name, decimal precision,
issuer domain — and records which entries the registrar has **verified**.

Consensus never needs it: every node already knows every asset *as a number*
from the chain itself. The registry is the naming-and-trust layer on top, and
this server lets anyone host their own instead of depending on a single
operator's.

It is three things at once:

| Role | What it means |
|---|---|
| **Mirror** | It syncs an upstream registry on a timer and keeps the **last good copy**, so consumers pointed here keep working even while the upstream is down. |
| **Registrar** | The operator adds, edits, **verifies** and removes entries from a password-protected admin UI. Local decisions override the upstream and survive every sync. |
| **API** | The merged result is served at `/registry/index.minimal.json` in the standard format, so this URL is a **drop-in replacement** anywhere a registry URL is configured. |

Stdlib-only Python 3, single file, same architecture and look as the
[price server](../price-server/README.md).

## Quick start

```bash
python3 registry_server.py --config config.json                  # localhost:8092
python3 registry_server.py --config config.json --set-password   # set the admin password, exit
python3 registry_server.py --config config.json --host 0.0.0.0   # public (requires a password)
```

A missing config file is created with defaults (upstream = the public
Sequentia testnet registry), so the first run already mirrors the network's
assets.

## The index format

`GET /registry/index.minimal.json` (also `/index.minimal.json`):

```json
{
  "<asset-id-64-hex>": ["issuer.domain", "TICKER", "Display Name", 8, 1]
}
```

The five fields are `[domain, ticker, name, precision, verified]`. `verified`
(0/1) is the **registrar's statement** that they checked the issuer really is
who the entry says — wallets and nodes only trust verified entries for
display, which is what prevents ticker spoofing (anyone can issue an asset
on-chain and *call* it "USDX"; the registry decides which USDX is shown).

### How `verified` is meant to be earned

> **This server lets the operator set `verified` by hand. That is a deliberate
> override, and it is not how a public registry should work.** Read this before
> copying the pattern.

The standard way an issuer earns the flag has nothing to do with the
registrar's opinion, and requires no trust in them:

1. The issuer commits a **contract** — name, ticker, precision, issuer domain —
   into the asset id at issuance, by way of `SHA256(canonical contract)`. See
   [asset contracts](../../doc/sequentia/asset-contracts-and-verification.md).
2. The issuer publishes a **domain proof** at
   `https://<domain>/.well-known/sequentia-asset-proof-<assetid>`, which only
   whoever controls the domain can do.
3. The registrar **re-derives** the asset id from the chain, checks the
   submitted contract hashes to the committed value, fetches the proof, and only
   then sets `verified: 1`.

That chain is cryptographic: the registrar cannot invent metadata for someone
else's asset, and a registrar that lies can be caught by anyone repeating the
same checks. The reference implementation is the
[sequentia-registry](https://github.com/GracedEternalKingCabbageMan/sequentia-registry)
server, which does exactly this and offers no manual override at all.

The manual flag here exists for the cases that chain cannot serve:

- **Legacy assets** issued before contracts existed, which commit to nothing and
  so can never pass the checks — the public testnet demo assets are exactly this.
- **Local experiments**, where re-issuing and publishing a file for every throwaway
  asset is pure friction.
- **Assets with no domain**, by choice, whose issuer is vouched for by the
  registrar directly.
- **Continuity**, when an issuer's domain has lapsed but the asset is still real.

If you are running a registry that other people point their wallets at, prefer
the domain proof and treat the manual flag as the exception you can justify.
A registry whose `verified` column means "the operator said so" is a registry
whose users are trusting the operator, not the issuer — which may be exactly
what you want for a private group, and is exactly what you do not want for a
public network.

## Who reads it

- **Sequentia Core** — `elements.conf`:

  ```ini
  assetregistryurl=http://<your-server>:8092/registry/index.minimal.json
  assetregistrypoll=300
  ```

  The node's fetcher speaks plain HTTP (no TLS, no redirects), which a
  self-hosted registry serves natively. The node merges only `verified=1`
  labels.

- **Price server** — the *Asset registry URL* field in its Market source tab.
- **Explorers / wallets** — anything that consumes the standard index format.

## Admin area — `/admin`

Same model as the price server: with **no password set** it answers only to
requests from the same machine (127.0.0.1); binding to a non-loopback address
is refused until a password exists (`--set-password`, or the Access section
while on localhost). With a password set, login is required from everywhere;
sessions are cookies, and every write is same-origin + CSRF checked.

What you can do there:

- **Add or edit an asset**: id (64 hex), ticker, name, issuer domain,
  precision (0–8) and the **verified** flag. Saving an id that exists
  overrides it; a new id becomes a local entry.
- **Remove** an entry — removing an upstream entry leaves a tombstone, so the
  next sync does **not** resurrect it. **Restore** drops the local
  change/tombstone and shows the upstream value again.
- **Upstream & sync**: the upstream URL, the sync interval, a *Sync now*
  button, and the live upstream status (reachable / down — serving the last
  good copy). Leave the upstream empty to run standalone.
- **Access**: public toggles for the page and the JSON index (both **on** by
  default — a registry exists to be read), the per-IP rate limit, the default
  table page size, and the admin password.
- **Export**: download the merged registry plus your local overrides as one
  JSON backup.

The asset tables (public and admin) are sortable by any column, with a text
filter and a rows-per-page selector.

## How the merge works

```
merged = upstream cache  ←overridden by→  local edits  −  local tombstones
```

Local state lives next to the config in `overrides.json`; the upstream
snapshot in `upstream-cache.json`. A sync only replaces the upstream snapshot
— your edits always win, and are never touched by a sync. If the upstream is
unreachable the last snapshot keeps being served (that is the mirror role).

## Public endpoints & rate limiting

| Path | What |
|---|---|
| `/` and `/public` | Human-readable table of every asset with provenance (upstream / edited here / local). |
| `/registry/index.minimal.json` | The machine index (also at `/index.minimal.json`). |

Public requests are rate-limited per client IP (default 60/min, configurable);
`429` with `Retry-After` when exceeded. Admins are never limited. Disabled
endpoints answer `403`.

## Config schema

```jsonc
{
  "registry_name": "Sequentia Asset Registry",
  "upstream_url": "https://sequentiatestnet.com/registry/index.minimal.json",
  "sync_interval_secs": 300,
  "upstream_timeout": 15,
  "ui": {
    "public_page": true,
    "public_index": true,
    "api_rate_limit_per_min": 60,
    "page_size": 50,
    "password_hash": ""          // via --set-password or the Access section, never by hand
  }
}
```

## Deploying on a server (systemd example)

```ini
[Unit]
Description=Sequentia asset registry

[Service]
Type=simple
User=sequentia
ExecStart=/usr/bin/python3 /opt/sequentia/asset-registry/registry_server.py \
    --config /data/asset-registry/config.json --port 8092 --host 0.0.0.0
Restart=on-failure
RestartSec=15
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

Order of operations: create/keep the config → `--set-password` → start →
open the firewall port. Multiple independent registries can coexist on a
network: every consumer chooses which registrar to trust via its configured
URL, exactly like choosing a DNS resolver.
