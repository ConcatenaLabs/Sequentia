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

Mempool entries carry `nFeeAsset` and `nFeeValue` (the rfa value); the miner
(`src/node/miner.cpp`) ranks packages by rfa value, and `RecomputeFees()`
re-values the mempool whenever rates change.

## 2. Per-producer acceptance: static and dynamic layers

A producer configures acceptance two ways, and the effective table is their
merge (an explicit static entry takes precedence over a dynamic one):

- **Static whitelist** — a fixed `{asset → rate}` table set with
  `setfeeexchangerates` and read with `getfeeexchangerates`. Writing it persists
  the table and calls `RecomputeFees()`.
- **Dynamic whitelist** — a layer maintained by a locally-run **price server**
  ([§5](#5-the-dynamic-price-server)) that admits assets and computes rates from
  live market data, pushed in via `setdynamicfeerates`.

`getfeeacceptancepolicy` returns the effective acceptance set with per-asset
provenance (static or dynamic). The operator-facing setup — listing assets,
running the price server, and constructing transactions that pay fees in a chosen
asset — is in [`05-operating-sequentia.md`](05-operating-sequentia.md).

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

## 5. The dynamic price server

The dynamic layer is a locally-run sidecar (`contrib/price-server/`) — a
standalone program the operator runs alongside the node. Keeping it out of the
consensus daemon isolates third-party HTTP, API keys, and JSON parsing from the
node and keeps that outbound-network surface out of `sequentiad`; the sidecar is
independently restartable and testable.

It periodically queries operator-designated external APIs (exchange endpoints,
DEX oracles) for per-asset market data, applies operator-defined **admission
thresholds** (e.g. market cap, 24h volume, volatility), computes each admitted
asset's rate from its price relative to the reference unit, and pushes the
resulting `{asset → rate}` table into the node. It drives the node through RPCs
registered alongside `setfeeexchangerates` (`src/rpc/exchangerates.cpp`):

| RPC | Purpose |
|---|---|
| `setdynamicfeerates {asset: rate, …}` | Replace the dynamic layer, persist, `RecomputeFees()`. |
| `getdynamicfeerates` | Return the dynamic layer with metadata (source, age). |
| `getfeeacceptancepolicy` | Return the effective set (static ∪ dynamic) with provenance. |
| `cleardynamicfeerates` | Drop all dynamic entries (e.g. on sidecar shutdown). |

The reference unit is anchored to a chosen value (for example a USD-equivalent
stablecoin) so rates are meaningful; the choice is operator policy, not
consensus. Safety rules keep a producer from mining against bad data:

- **Staleness fail-safe** — a dynamic rate older than `-dynfeeratemaxage` is
  dropped, so a dead price server fails closed to "not accepted" rather than
  honouring stale rates.
- **Sanity clamps** — implausible inter-poll jumps and near-zero rates (which
  would let dust buy blockspace) are rejected.
- **Source quorum** — agreement across multiple sources before admitting an
  asset blunts a single compromised API.
- **Operator override** — a statically pinned asset is never overridden by the
  dynamic layer.

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
