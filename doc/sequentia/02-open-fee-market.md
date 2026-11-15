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
asset: a producer may price SEQ at any rate, refuse it, or leave it out and
price other assets instead. On mainnet a node accepts no asset at all until its
operator says which, so SEQ is not even the default. The reference unit stays an
abstract factor throughout: no asset is ever *defined* to be the reference, and
none is valued without being listed.
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
asset, SEQ, is unlisted-means-refused like everything else.

A whitelist is **static** when it is written by hand, and **dynamic** when a
price server updates it. A price server ([§5](#5-the-price-server)) takes the
prices a price feed API reports, filters and treats them under its own
configuration, and writes the result; nothing goes from a price feed API to the
whitelist without one. What a never configured node starts with depends on the
chain:

- **The testnet (`test`) is connected to the network's price server.** It runs
  no price server of its own: it asks for the list the network's price server
  publishes (`http://sequentiatestnet.com/feewhitelist`) as it starts, before
  its wallet can be asked to send, so its whitelist is populated at once and
  dynamic from then on, the same list the public nodes use. No asset is listed
  before that list arrives.
- **The mainnet (`sequentia`) has no default price server**, so it starts with
  an empty, static whitelist. The node accepts no asset, and says so at startup.
  It validates the chain and its wallet receives as usual, but it relays no
  transaction and its wallet refuses to send, until the operator writes the
  whitelist by hand or connects to a price server. No asset is pre-loaded, so
  none is favoured, and the node does not appear to work before somebody has
  decided what it accepts and at what price.
- **Custom chains start from a seed**: the map is constructed holding SEQ at
  `1e8` (`ExchangeRateMap::ResetToBootstrapRates`), so a regtest node can send
  without any setup, and any write that replaces the table replaces the seed
  along with it. `-con_seed_fee_whitelist=0` makes a custom chain start empty
  as the public chains do (`CChainParams::SeedFeeWhitelist`), and
  `-con_default_fee_price_server=<address>` gives it a default price server as
  the testnet has.

The testnet once seeded SEQ, and a node saved its whitelist to
`exchangerates.json` at every start, so a testnet data directory can hold that
seed without anyone having chosen it. On a chain that no longer seeds, a file
holding the policy asset at `1e8` and nothing else is therefore moved aside to
`exchangerates.json.old-seed` rather than loaded. This is checked once per data
directory, at the first start, and the check is recorded in `settings.json`:
from then on the file holds only what is written to it, so an operator who lists
the policy asset alone at `1e8` keeps that list. Any other content
is a static whitelist its operator wrote: it is loaded as written, and on the
testnet it takes precedence over the default price server, as does a file named
with `-initialexchangeratesjsonfile`. The mainnet never seeded, so there every
file is loaded.

An empty whitelist is a state the operator can also return to on any chain, by
writing `{}`. The node then behaves as an unconfigured mainnet node does, and an
emptied list stays empty: writing it by hand makes the whitelist static, which
ends any connection to a price server.

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
singleton. There are no static and dynamic layers inside it and no per-asset
"source" or provenance: static or dynamic says who writes the table, not how it
is stored.

The table has two states. It is **static** when the operator writes it by hand,
and **dynamic** when the node is connected to a price server, its operator's own
or another operator's ([§5](#5-the-price-server)), which is then the table's
only writer. There is no third state. A node may also fetch
market prices for display (`-referencepricesurl`, used to show amounts in a
reference currency); they never enter the whitelist, so an asset is accepted
only because the operator, or a price server, listed it.

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
and miner treat all assets uniformly. Where the seed is in place (custom
chains) it prices SEQ at 1:1 with rfa, so a SEQ-atom floor equals an rfa floor
there; a producer that re-prices SEQ, or drops it in favour of other assets,
changes that equivalence while the floors stay rfa-denominated.

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
  unconfirmed parent contributes its rfa fee to the package's rfa rate, and the
  package figures stay sums of values when a rate changes. A coin of an asset no
  node accepts for fees is sped up by a child that adds a second input in an
  accepted asset to pay the fee. A child cannot keep a parent alive whose own
  fee asset the node stops accepting: the parent leaves the mempool with its
  descendants. `test/functional/feature_any_asset_fee_cpfp.py` shows each case.
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

A price server (`contrib/price-server/`) is a program that runs beside a node,
not inside it. It takes the prices a **price feed API** reports (an exchange
endpoint, an oracle, another operator's price server), filters and treats them
under its own configuration (admission thresholds such as market cap, 24h volume
and volatility, manual prices, exceptions), computes each admitted asset's rate
from its price relative to the reference unit, and publishes the resulting
`{asset → rate}` table. Keeping it out of the consensus daemon isolates
third-party HTTP, API keys and JSON parsing from the node; it is independently
restartable and testable.

A node's whitelist is dynamic exactly when the node is connected to a price
server, its operator's own or another operator's. A node is connected in one of
two ways, and `getfeepriceserver` says which (`connection`):

- **By address** (`-feepriceserver=<address>`, `src/feepriceserver.{h,cpp}`). The
  node asks for the list the price server publishes, the body of its
  `GET /api/whitelist` or the file it writes with `--publish-file`, at startup
  and every `-feepriceserverpoll` seconds (60), and makes it the whitelist. This
  is how the testnet works by default, how a node uses another operator's price
  server, and how it uses its operator's own: the desktop wallet's "Run my own
  price server" starts the bundled one publishing on this machine and connects
  the node to `http://127.0.0.1:8089/api/whitelist`, so that price server needs
  no RPC login to the node.
- **By RPC.** The price server writes to the node with `setfeeexchangerates`
  and `persist=false`, using the node's RPC credentials; the network's price
  server writes to the public nodes this way. The node tells such a write from a
  hand write by that flag alone: `persist=false` is how a price server writes,
  and `persist=true`, the default, is what a person and the desktop wallet use.
  The node shows the whitelist as dynamic, connected by RPC, with when it was
  last written; it knows no address for that price server. Someone passing
  `persist=false` by hand is shown the same way, since the node cannot tell
  them apart. A list written this way is not persisted, so after a restart the
  node shows its static file until the price server writes again.

A node connected by address takes that server's published list and nothing
else:

- **Only a price server's list.** The answer must be the published shape,
  `{"updated": <time>, "rates": {"<asset id>": <rate>, ...}}`. A price feed API's
  answer, the registry's index or anything else is refused as "not a price
  server", and `setfeepriceserver` will not connect to such an address at all.
  Going from a price feed API to the whitelist without a price server is not
  possible.
- **Whole or not at all.** A list is refused, and the list in force kept, when it
  is empty, has more than 1000 entries, holds an entry that is not a 64-hex asset
  id with an integer rate in `(0, 1e18]`, is older than the list in force, or is
  over 1 MiB. An empty published list is never a policy (a price server refuses
  to publish a round that admitted nothing), so reading it as "accept nothing"
  would let one bad answer stop the node relaying.
- **Unreachable.** The last list is kept for as long as the server cannot be
  reached; `getfeepriceserver` and the desktop wallet show when it was last
  written and the last error. It is never cleared, and nothing falls back to one
  asset. Each list taken is cached in `feepriceserver.json` in the data directory,
  so a restart has it at once.
- **One writer.** Any write over RPC ends a connection by address for good, and
  the node records `-nofeepriceserver` in its `settings.json`: a hand write
  (`setfeeexchangerates`, or an edit in the desktop wallet) makes the whitelist
  static, an empty one included, and a price server's write connects the node
  to that price server by RPC instead. `setfeepriceserver "<address>"` (or
  Connect in the desktop wallet's Fee acceptance dialog) connects it by address
  again; `setfeepriceserver ""` disconnects it and keeps the list in force as a
  static whitelist. An address given on the command line is connected at every
  start, as any command-line setting wins.
- **Plain HTTP.** The node links no TLS library, so the list is not authenticated
  in transit: someone on the network path can answer in the price server's
  place. The bounds above limit what such an answer can carry, not who sent it;
  a list signed by the price server and checked against a key the node is given
  is the fix ([Sequentia #397](https://github.com/ConcatenaLabs/Sequentia/issues/397)).

| RPC | Purpose |
|---|---|
| `setfeeexchangerates {asset: rate, …} [persist=true]` | Replace the whole whitelist and `RecomputeFees()`. With `persist=true` (the default) it is a hand write: the whitelist is static, and the table is also written to `exchangerates.json` so it survives a restart. With `persist=false` it is how a price server writes: the node is connected to that price server by RPC, its whitelist dynamic, and only the in-memory whitelist changes. Either ends a connection by address. Pass `{}` to clear it, which leaves the node accepting **no** fee asset at all, SEQ included, and empties its mempool. |
| `getfeeexchangerates` | Return the current whitelist as `{asset: rate}`. |
| `getfeeacceptancepolicy` | Return the current acceptance set. |
| `getfeepriceserver` | Whether the node is connected to a price server (its whitelist then `dynamic`, else `static`) and how (`address` or `rpc`), the address, whether it is the network's default, when the list in force was taken and published, the last request and its error, and why the node disconnected if it did. |
| `setfeepriceserver "<address>"` | Connect to a price server, after taking its list (nothing changes if the address does not answer with one). `""` disconnects and keeps the list in force as a static whitelist. Both are recorded in `settings.json`. |
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

There is a single whitelist; static or dynamic is who writes it, not a protocol
distinction. An operator writing rates by hand uses the default `persist=true`
so the table survives a restart. A price server writing to the node over RPC uses
`persist=false`: it re-pushes every poll, so persisting would only churn the file
and, worse, leave its last rates in force across a restart instead of the static
whitelist.

The reference unit is anchored to a chosen value (for example a USD-equivalent
stablecoin) so rates are meaningful; the choice is operator policy, not consensus.
The node holds the last-set rates indefinitely - there is **no** staleness or
max-age option; keeping rates fresh (and refusing assets when a feed dies, by
writing `0` or omitting them) is the price server's job. The one rule the node enforces
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
