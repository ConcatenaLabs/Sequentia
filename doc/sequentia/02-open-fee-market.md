# Open ("no-coin") fee market

Any asset issued on Sequentia can be offered as a transaction fee. Proposing a
fee in a given asset is permissionless; *accepting* it is not. A transaction is
included only if a block producer is willing to accept that asset **and** the
rate at which the fee is posted. Each producer independently decides which assets
it accepts and at what relative value, then builds the most valuable block it can
from the transactions paying in those assets.

**SEQ holds no privileged fee status.** It is special only as the asset that
unlocks block-production eligibility - staking (see
[`04-proof-of-stake.md`](04-proof-of-stake.md)). For fees it is just another
asset: an unconfigured producer starts with SEQ seeded at 1:1, and from there a
producer may re-price SEQ at any rate, refuse it, or drop it and price other
assets instead. The reference unit stays an abstract factor throughout: no asset
is ever *defined* to be the reference, and none is valued without being listed.
The fee market is the design's lowest-risk property because it is entirely
node-local policy and requires **no consensus change**
([§6](#6-why-no-consensus-change)).

## 1. Reference-unit valuation

Heterogeneous fees are made comparable by valuing each in a common abstract unit,
the **reference fee atom (rfa)** - `CValue` in `src/policy/value.h`. A producer's
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

An asset **absent** from the map values to `0` rfa - i.e. not accepted - so the
table *is* the producer's acceptance set, with **no exceptions**: the policy
asset, SEQ, is unlisted-means-refused like everything else. What a never
configured node accepts comes from a **seed**, not from a special case: the map
is constructed holding SEQ at `1e8` (`ExchangeRateMap::ResetToBootstrapRates`),
so fees work out of the box, and any write that replaces the table replaces the
seed along with it.

A rate of `0` reads as "refuse this asset": it is a valid stored value that flows
through to the conversion as "not accepted". Setting a rate accepts any
**non-negative** value; only **negative** rates are rejected by the RPCs. A
producer can therefore drop an asset either by omitting it from the next write or
by listing it with an explicit `0`.

Mempool entries carry `nFeeAsset` and `nFeeValue` (the rfa value); the miner
(`src/node/miner.cpp`) ranks packages by rfa value, and `RecomputeFees()`
re-values the mempool whenever rates change.

## 2. Per-producer acceptance: a single whitelist

A producer keeps **one** `{asset → rate}` whitelist - the `ExchangeRateMap`
singleton. There are no static and dynamic layers and no precedence between
writers: the most recent write replaces the table (last-writer-wins), and there
is no per-asset "source" or provenance.

The table is written with `setfeeexchangerates` and read with
`getfeeexchangerates`. Writing persists the table to `exchangerates.json` and
calls `RecomputeFees()`. A price server ([§5](#5-the-price-server)) writes
the same single table; `getfeeacceptancepolicy` returns the current acceptance
set. The operator-facing setup - listing assets, running the price server, and
constructing transactions that pay fees in a chosen asset - is in
[`05-operating-sequentia.md`](05-operating-sequentia.md).

## 3. Paying fees in an arbitrary asset

A wallet holding **zero SEQ** can transact entirely in another asset, provided a
producer prices that asset. The fee is paid in the chosen asset and the resulting
transaction's fee output is denominated in that asset, not SEQ. The wallet flow
(`assetlabel` for the asset sent, `fee_asset_label` for the fee asset) and worked
commands are in [`05-operating-sequentia.md`](05-operating-sequentia.md) §4. On-chain
stake-registration transactions and ordinary asset transfers both relay under
default policy.

### Naming the fee asset

**The fee asset must be named explicitly unless the transaction already
determines it.** The wallet RPCs that build a transaction - `sendtoaddress`,
`sendmany`, `send`, `fundrawtransaction`, `walletcreatefundedpsbt`, `issueasset`,
`reissueasset` - apply exactly that rule and nothing else. They do not fall back
to the policy asset, and they do not infer an asset from what the transaction
happens to send.

There is no default because a default would be a privilege. Outside staking
eligibility SEQ has exactly the standing of every issued asset; a wallet that
settled on SEQ whenever the caller stayed silent would make it the network's fee
currency by default and reintroduce the coin the design does away with. Inferring
the asset being sent is no better: it is a policy decision taken out of the
caller's sight, from data that says nothing about which asset can pay a fee, and
it breaks outright on an asset the node cannot price (a reissuance token above
all).

A transaction determines its fee asset when the value is already implied by what
the caller handed in, in either of two ways:

- it carries an explicit **fee output**, which names the asset the fee is paid in;
- the fee is **subtracted from** an output, so it is taken out of that output's
  amount and can only be denominated in that output's asset - `output_amount -=
  fee`, and a GOLD output cannot be reduced by an amount denominated in USDX.

Where the transaction states the answer, passing `fee_asset_label` /
`options.fee_asset` alongside it is **refused, whether or not it agrees**: it
would be a parameter that looks like a selection and is not one, since changing
the transaction would silently change the "chosen" fee asset. Where it states the
answer twice, the two must agree - a raw transaction with a GOLD fee output whose
fee is subtracted from a GOLD output is fine; one that names GOLD and USDX is
impossible and is refused, naming both. Fee outputs of two different assets, or
subtract-from outputs spanning two assets, are refused for the same reason: a
transaction pays its fee in exactly one. All of this holds identically for the
policy asset and for every issued asset.

`bumpfee` reads the fee asset off the transaction it replaces when no
`fee_asset` is given. That too is determined rather than chosen.

The GUI preselects a fee asset in the Send form. That is a visible, overridable
affordance in front of the user, not a rule hidden in the back end.

## 4. Fee floors and replacement, in reference units

Every configured fee floor is denominated in the reference unit, so the mempool
and miner treat all assets uniformly. Because the seed prices SEQ at 1:1 with
rfa, a SEQ-atom floor equals an rfa floor out of the box; a producer that
re-prices SEQ, or drops it in favour of other assets, changes that equivalence
while the floors stay rfa-denominated.

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
- **Fee estimation** works for any fee asset through the same unit: the
  block-policy estimator's feerate is converted into the wallet's chosen fee
  asset at query time via the whitelist (`CFeeRate::GetFee(num_bytes, asset)`,
  `src/policy/feerate.cpp`; the wallet threads `coin_control.m_fee_asset`
  through `GetMinimumFeeRate`, `src/wallet/fees.cpp`). Fee rates therefore
  surface in the fee asset's own units per vByte, never in a foreign unit.

The operator how-tos for RBF and CPFP with asset fees are in
[`05-operating-sequentia.md`](05-operating-sequentia.md) §5.

## 5. The price server

The price server is a locally-run sidecar (`contrib/price-server/`) - a standalone
program the operator runs alongside the node. Keeping it out of the consensus
daemon isolates third-party HTTP, API keys, and JSON parsing from the node and
keeps that outbound-network surface out of `sequentiad`; the sidecar is
independently restartable and testable.

It periodically queries operator-designated external APIs (exchange endpoints,
DEX oracles) for per-asset market data, applies operator-defined **admission
thresholds** (e.g. market cap, 24h volume, volatility), computes each admitted
asset's rate from its price relative to the reference unit, and writes the
resulting `{asset → rate}` table into the node's single whitelist through
`setfeeexchangerates` (`src/rpc/exchangerates.cpp`):

| RPC | Purpose |
|---|---|
| `setfeeexchangerates {asset: rate, …} [persist=true]` | Replace the whole whitelist and `RecomputeFees()`. With `persist=true` (the default) it also writes `exchangerates.json` so the table survives a restart; with `persist=false` it updates only the in-memory whitelist. Pass `{}` to clear it, which leaves the node accepting **no** fee asset at all, SEQ included, and empties its mempool. |
| `getfeeexchangerates` | Return the current whitelist as `{asset: rate}`. |
| `getfeeacceptancepolicy` | Return the current acceptance set. |
| `getfeeassetinfo [asset]` | Per asset: whether this node accepts it (`accepted`, plus `listed`/`rate` so a refusal written down as rate 0 is distinguishable from an asset nobody configured), whether the Asset Registry publishes it (`registry_listed`), and whether the reference feed prices it (`market_price`). |

`getfeeassetinfo` is what a wallet asks before offering an asset as a way to
pay, and it keeps those three facts apart because only the first is decisive.
An asset missing from **this node's** whitelist is refused by the wallet's own
mempool, so the transaction never reaches a producer at all. An asset that is
accepted here but absent from the registry, or unpriced, is a different and
milder problem: other producers build their whitelists from exactly those two
sources, so the payment may confirm only in a block this node produces. A
wallet that collapses the three into one "is it usable" flag warns about assets
that work and stays silent about assets that cannot be sent.

`estimatesmartfee` takes an optional third argument, `fee_asset`. The estimate
does not depend on it — a fee rate is a rate in the reference unit, which is
what every asset's fee is valued into — so it only converts the answer at this
node's whitelist rate and reports acceptance. Acceptance is reported even when
there is no estimate to convert, since a node with no fee history yet would
otherwise look like it had a problem with the asset.

There is a single whitelist; "static" versus "dynamic" is only how it is
*operated*, not a protocol distinction. An operator setting rates by hand uses
the default `persist=true` so the table survives a restart. A price server driving
the whitelist automatically uses `persist=false`: it re-pushes every poll, so
persisting would only churn the file and, worse, leave its last rates in force
across a restart instead of failing back to the persisted static whitelist.

The reference unit is anchored to a chosen value (for example a USD-equivalent
stablecoin) so rates are meaningful; the choice is operator policy, not consensus.
The node holds the last-set rates indefinitely - there is **no** staleness or
max-age option; keeping rates fresh (and refusing assets when a feed dies, by
writing `0` or omitting them) is the sidecar's job. The one rule the node enforces
is the **non-negative-rate** floor: a negative quote is rejected outright, while a
zero is accepted and read as "refuse this asset". Vetting source data - quorum
across feeds, guarding implausible inter-poll jumps and dust-priced rates - is the
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
