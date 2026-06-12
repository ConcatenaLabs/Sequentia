# Challenge 1 — Open ("no-coin") fee market

**Goal.** Any asset issued on Sequentia can be offered as a transaction fee.
Each block producer independently decides which assets it accepts and their
relative values, then builds the most valuable block from transactions paying in
those assets. Two configuration modes: a **static whitelist**, and a **dynamic
whitelist** maintained by a local **price server**.

## A. What already exists (static whitelist) — keep

The Sequentia fork already implements the static path end-to-end. Summarised
here so the dynamic work bolts on cleanly; see doc 01 §3 for detail.

- `ExchangeRateMap` singleton: `{CAsset → scaled rate}`, scale `COIN`, persisted
  to `<datadir>/exchangerates.json`.
- `CValue` reference-fee-atom (rfa) unit; mempool stores `nFeeAsset`+`nFeeValue`;
  miner ranks by rfa; `RecomputeFees()` re-values the mempool on rate change.
- RPCs `getfeeexchangerates` (read) and `setfeeexchangerates` (write + persist +
  `RecomputeFees`).
- Acceptance semantics: **an asset absent from the map values to 0 rfa**, so the
  whitelist *is* the producer's acceptance set. A producer rejects an asset by
  simply not listing it (or, below, by the price server not admitting it).

### Small gaps to close in the static path

1. **`blockmintxfee` / min-relay floors in rfa.** Confirm every fee floor
   (`-minrelaytxfee`, `-blockmintxfee`, `incrementalrelayfee`) is consistently
   compared in rfa for any-asset txs (audit `src/policy/policy.cpp`,
   `src/validation.cpp`, `src/node/miner.cpp`). Document the canonical rfa floor.
2. **Producer acceptance vs. relay.** Today rate map = both the node's relay
   policy *and* the producer's acceptance set. We keep that for the PoC but note
   the distinction for later (a node might relay assets it won't itself mine).

## B. New work — the dynamic price server

### B.1 Responsibilities

A locally-run component that:

1. Periodically queries operator-designated **external APIs** (CEX REST
   endpoints, DEX oracles) for per-asset market data: price (vs. the rfa
   reference), market cap, 24h volume, volatility, etc.
2. Applies operator-defined **admission thresholds** to decide which assets
   belong on the dynamic whitelist (e.g. "admit if mcap > $50M **and** 24h vol >
   $1M **and** 30d volatility < 80%").
3. Computes each admitted asset's **rate** (atoms-per-rfa, scaled by `COIN`) from
   its price relative to the reference unit.
4. Pushes the resulting `{asset → rate}` into the node, which merges it with the
   static entries, persists, and calls `RecomputeFees()`.

### B.2 Where it runs — design choice

Two viable shapes; the PoC adopts **(1)** for isolation and operator safety, with
a clean interface so **(2)** remains possible later.

1. **Out-of-process sidecar** (recommended for PoC). A standalone program
   (Python or Rust) that the operator runs alongside `sequentiad`. It owns all
   outbound HTTP to exchanges, applies thresholds, and feeds the node through a
   new authenticated RPC. *Pros:* keeps third-party HTTP, API keys and parsing
   out of the consensus daemon; independently restartable; testable in isolation;
   no new outbound-network attack surface in `sequentiad`. *Cons:* an extra
   process to run.
2. **In-process price-feed thread.** A scheduler thread inside `sequentiad`
   (mirroring how `mainchainrpc` reaches out). *Pros:* single binary. *Cons:*
   pulls HTTP clients, JSON-from-the-internet parsing and credentials into the
   node; larger attack surface; harder to sandbox.

### B.3 Node-side interface (new RPCs)

The sidecar drives the node through new RPCs registered next to the existing
`exchangerates` category (`src/rpc/exchangerates.cpp`):

| RPC | Purpose |
|---|---|
| `setdynamicfeerates {asset: rate, …}` | Replace the **dynamic** layer of the rate map (distinct from the static layer set by `setfeeexchangerates`), persist, `RecomputeFees()`. |
| `getdynamicfeerates` | Return the current dynamic layer with metadata (source, age). |
| `getfeeacceptancepolicy` | Return effective acceptance set = static ∪ dynamic, with provenance per asset. |
| `cleardynamicfeerates` | Drop stale dynamic entries (e.g. on sidecar shutdown / staleness). |

Internally this means **layering** the `ExchangeRateMap`: a `static` map and a
`dynamic` map with a defined precedence (proposed: explicit static entry wins;
otherwise dynamic; effective map = merge). This avoids the price server clobbering
a value the operator pinned by hand. Add a per-entry `source` + `updated_at` so
staleness can be enforced (a dynamic rate older than `-dynfeerate-maxage` is
dropped, failing safe to "not accepted").

### B.4 Sidecar internals (`contrib/price-server/`)

```
price-server/
  config.toml          # API endpoints, asset map, thresholds, poll interval,
                        # node RPC creds, reference-unit definition
  sources/             # one adapter per API (binance, coingecko, dex-oracle, …)
  thresholds.py        # admission predicate over the collected metrics
  rate.py              # price -> scaled rate (atoms per rfa)
  publisher.py         # calls setdynamicfeerates / cleardynamicfeerates
  main.py              # poll loop + staleness + structured logging
```

Config sketch:

```toml
poll_interval_secs = 60
reference = { asset = "USDT", label = "rfa-usd" }   # rfa pegged to 1 USD-equiv

[node_rpc]
host = "127.0.0.1"; port = 18776; cookie = "~/.sequentia/.cookie"

[[asset]]
label   = "USDT"
id      = "b2e15d0d...f0f23"
sources = ["binance:USDTUSD", "coingecko:tether"]
[asset.thresholds]
min_market_cap_usd = 50_000_000
min_volume_24h_usd = 1_000_000
max_volatility_30d = 0.80
```

### B.5 Rate computation & the reference unit

The rfa unit needs an anchor so rates are meaningful. PoC choice: define rfa as a
**USD-equivalent** atom (e.g. rfa pegged to a chosen stablecoin/oracle). Then for
asset `A` priced `price_A` (reference-per-whole-unit) with `d` decimals:

```
rate_A (scaled, atoms-per-rfa) = round( exchange_rate_scale / (price_A in rfa per atom) )
```

The exact formula and rounding/saturation live next to
`ExchangeRateMap::ConvertAmountToValue` and must be unit-tested for the
`INT64_MAX` saturation edge already handled there. The reference choice is an
operator policy, **not** consensus — different producers may use different
references; consensus only validates that *included* fees were really paid.

### B.6 Safety / sanity rules

- **Staleness fail-safe:** dynamic rate older than max-age ⇒ dropped ⇒ asset not
  accepted (never silently keep mining a dead price).
- **Sanity clamps:** reject implausible jumps (e.g. > Nx between polls) and
  near-zero rates that would let dust pay for blockspace.
- **Source quorum:** require agreement across ≥ k sources before admitting, to
  blunt a single compromised/oddball API.
- **Operator override:** a statically-pinned asset is never overridden by the
  dynamic layer.

### B.7 Why this needs **no consensus change**

Fee valuation is **node-local policy**: it decides what a producer *chooses* to
include, not what the network considers valid. Consensus only checks that the
fees declared in a block were actually paid by its transactions (in whatever
asset). So the price server is purely a policy/mempool/mining concern and is safe
to iterate on without forking the chain — a key reason challenge 1 is the
low-risk starting point.

## C. Test strategy

- Unit: rate math + saturation (`ConvertAmountToValue/ValueToAmount`); layered
  map precedence; staleness drop; threshold predicate.
- Functional (`test/functional/`): two-asset chain; submit txs paying fees in
  asset B; `setdynamicfeerates` admits B; assert miner includes B-fee txs ranked
  by rfa; assert dropping B's rate evicts/deprioritises them via `RecomputeFees`.
- Sidecar: mock API server; assert admission/withdrawal across threshold
  crossings; assert node RPC calls.
</content>
