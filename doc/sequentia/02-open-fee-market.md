# Open ("no-coin") fee market

Any asset issued on Sequentia can be offered as a transaction fee. Proposing a
fee in a given asset is permissionless; *accepting* it is not. A transaction is
included only if a block producer is willing to accept that asset **and** the
rate at which the fee is posted. Each producer independently decides which assets
it accepts and at what relative value, then builds the most valuable block it can
from the transactions paying in those assets.

**SEQ holds no privileged fee status.** It is special only as the asset that
unlocks block-production eligibility — staking (see
[`04-proof-of-stake.md`](04-proof-of-stake.md)). For fees it is just another
asset: accepted 1:1 only as the *default* an unconfigured producer uses. A
producer may re-price SEQ at any rate, refuse it, or designate a different asset
(for example a USD stablecoin) as the reference. The fee market is the design's
lowest-risk property because it is entirely node-local policy and requires **no
consensus change** ([§6](#6-why-no-consensus-change)).

## 1. Reference-unit valuation

Heterogeneous fees are made comparable by valuing each in a common abstract unit,
the **reference fee atom (rfa)** — `CValue` in `src/policy/value.h`. A producer's
acceptance and pricing live in the `ExchangeRateMap` singleton
(`src/exchangerates.{h,cpp}`), a `{CAsset → rate}` table persisted to
`<datadir>/exchangerates.json`. The substrate is described in
[`01-architecture.md`](01-architecture.md); the valuation rule is:

```
reference_value(amount, asset) = amount × rate(asset) ÷ 100000000
```

The rate is an integer scaled by `COIN` (1e8):

| rate | meaning |
|---|---|
| `100000000` (1e8) | the asset is valued **1:1** with the reference unit |
| `> 1e8` | the asset is worth **more** than the reference per atom |
| `< 1e8` | the asset is worth **less** per atom (a "cheap" asset) |
| `0` | the asset is **explicitly refused** |

An asset **absent** from the map values to `0` rfa — i.e. not accepted — so the
table *is* the producer's acceptance set. The one exception is the policy asset,
SEQ, which is valued 1:1 when unlisted; that default is overridable by listing it
with any rate (including `0` to refuse it).

A rate of `0` reads as "refuse this asset": it is a valid stored value that flows
through to the conversion as "not accepted". Setting a rate accepts any
**non-negative** value; only **negative** rates are rejected by the RPCs. A
producer can therefore drop an asset either by omitting it from the next write or
by listing it with an explicit `0`.

Mempool entries carry `nFeeAsset` and `nFeeValue` (the rfa value); the miner
(`src/node/miner.cpp`) ranks packages by rfa value, and `RecomputeFees()`
re-values the mempool whenever rates change.

## 2. Per-producer acceptance: a single whitelist

A producer keeps **one** `{asset → rate}` whitelist — the `ExchangeRateMap`
singleton. There are no static and dynamic layers and no precedence between
writers: the most recent write replaces the table (last-writer-wins), and there
is no per-asset "source" or provenance.

The table is written with `setfeeexchangerates` and read with
`getfeeexchangerates`. Writing persists the table to `exchangerates.json` and
calls `RecomputeFees()`. A price server ([§5](#5-the-dynamic-price-server)) writes
the same single table; `getfeeacceptancepolicy` returns the current acceptance
set. The operator-facing setup — listing assets, running the price server, and
constructing transactions that pay fees in a chosen asset — is in
[`05-operating-sequentia.md`](05-operating-sequentia.md).

## 3. Paying fees in an arbitrary asset

A wallet holding **zero SEQ** can transact entirely in another asset, provided a
producer prices that asset. The fee is paid in the chosen asset and the resulting
transaction's fee output is denominated in that asset, not SEQ. The wallet flow
(`assetlabel` for the asset sent, `fee_asset_label` for the fee asset) and worked
commands are in [`05-operating-sequentia.md`](05-operating-sequentia.md) §4. On-chain
stake-registration transactions and ordinary asset transfers both relay under
default policy.

## 4. Fee floors and replacement, in reference units

Every configured fee floor is denominated in the reference unit, so the mempool
and miner treat all assets uniformly. Because SEQ defaults to 1:1 with rfa, a
SEQ-atom floor equals an rfa floor out of the box; a producer that re-prices SEQ
or pegs a different asset to the reference changes that equivalence while the
floors stay rfa-denominated.

- **Mempool acceptance** (`MemPoolAccept::CheckFeeRate`) compares the
  rfa-converted modified fee against the rolling mempool minimum (itself an rfa
  aggregate) and `-minrelaytxfee`.
- **Mining** (`-blockmintxfee`) compares against the package's rfa value,
  including the discounted-CT path.
- **Replacement (RBF)** compares a replacement's fee against the conflicts it
  evicts **in reference value**, plus `-incrementalrelayfee`. A replacement may
  pay its fee in a *different* asset than the original (`bumpfee` accepts a
  `fee_asset`); it is accepted only if its rfa value genuinely exceeds the
  original's. A replacement that pays a larger *raw* amount of a cheaper asset but
  a smaller reference value is correctly rejected.
- **Child-pays-for-parent (CPFP)** works across assets: a child spending an
  unconfirmed parent contributes its rfa fee to the package's rfa rate.
- **The absurd-fee ceiling** (`-maxtxfee`, and `testmempoolaccept`'s
  `maxfeerate`) is also evaluated in reference value, so a fee paid in a
  low-per-unit-value asset is not spuriously rejected for a large raw amount.
- **Prioritisation** (`prioritisetransaction`) deltas apply in rfa and survive
  rate updates.

The operator how-tos for RBF and CPFP with asset fees are in
[`05-operating-sequentia.md`](05-operating-sequentia.md) §5.

## 5. The price server

The price server is a locally-run sidecar (`contrib/price-server/`) — a standalone
program the operator runs alongside the node. Keeping it out of the consensus
daemon isolates third-party HTTP, API keys, and JSON parsing from the node and
keeps that outbound-network surface out of `sequentiad`; the sidecar is
independently restartable and testable.

It periodically queries operator-designated external APIs (exchange endpoints,
DEX oracles) for per-asset market data, applies operator-defined **admission
thresholds** (e.g. market cap, 24h volume, volatility), computes each admitted
asset's rate from its price relative to the reference unit, and writes the
resulting `{asset → rate}` table into the node's single whitelist. It can do so
through `setfeeexchangerates` directly, or through deprecated aliases retained for
sidecar convenience (`src/rpc/exchangerates.cpp`):

| RPC | Purpose |
|---|---|
| `setdynamicfeerates {asset: rate, …}` | Deprecated alias: replaces the whitelist and `RecomputeFees()`, but does **not** persist to `exchangerates.json` (it calls `SetRates`, not the persisting path). |
| `getdynamicfeerates` | Deprecated alias: returns the current whitelist. |
| `cleardynamicfeerates` | Deprecated alias: clears the whitelist (e.g. on sidecar shutdown). |
| `getfeeacceptancepolicy` | Return the current acceptance set. |

Because the deprecated `setdynamicfeerates` path does not persist, an operator who
wants the table to survive a restart writes it with `setfeeexchangerates`, which
does persist.

The reference unit is anchored to a chosen value (for example a USD-equivalent
stablecoin) so rates are meaningful; the choice is operator policy, not consensus.
The node holds the last-set rates indefinitely — there is **no** staleness or
max-age option; keeping rates fresh (and refusing assets when a feed dies, by
writing `0` or omitting them) is the sidecar's job. The one rule the node enforces
is the **non-negative-rate** floor: a negative quote is rejected outright, while a
zero is accepted and read as "refuse this asset". Vetting source data — quorum
across feeds, guarding implausible inter-poll jumps and dust-priced rates — is the
price server's responsibility before it writes.

The reference-unit rate math lives next to `ExchangeRateMap::ConvertAmountToValue`
/ `ConvertValueToAmount` and handles the `INT64_MAX` saturation edge. Running and
configuring the price server is covered in
[`05-operating-sequentia.md`](05-operating-sequentia.md).

## 6. Why no consensus change

Fee valuation is node-local policy: it decides what a producer *chooses* to
include, not what the network considers valid. Consensus only checks that the
fees declared in a block were actually paid by its transactions, in whatever
asset. A producer's acceptance set and the price server are therefore purely a
policy, mempool, and mining concern, and can be tuned without forking the chain.
