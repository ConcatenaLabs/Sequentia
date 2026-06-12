# Sequentia price server

A locally run sidecar that maintains the **dynamic layer** of a Sequentia
node's fee-asset whitelist, implementing the "dynamic whitelist" half of the
open fee market (see `doc/sequentia/02-open-fee-market.md`).

It periodically queries operator-designated market data APIs (CEX endpoints,
aggregators, DEX oracles), applies operator-defined **admission thresholds**
(market cap, 24h volume, source quorum/agreement, price sanity clamps), and
publishes qualifying assets' exchange rates to the node via the
`setdynamicfeerates` RPC. The node then accepts those assets as transaction
fee payment and ranks transactions by reference-fee-atom value when building
blocks.

Properties:

- **Static pins win.** A rate set on the node via `setfeeexchangerates` /
  `exchangerates.json` is never overridden by this sidecar.
- **Fail safe.** Assets leave the whitelist when sources fail, thresholds stop
  being met, the sidecar shuts down (it clears its layer on exit), or rates go
  stale on the node (`-dynfeeratemaxage`).
- **No dependencies.** Python 3 stdlib only.

## Usage

```sh
cp config.example.json config.json    # edit: node RPC creds, assets, thresholds
./price_server.py --config config.json            # run the poll loop
./price_server.py --config config.json --once     # single poll, print result
./price_server.py --config config.json --dry-run  # decisions only, no publish
```

Run the node with `-con_any_asset_fees=1` and, recommended, a staleness bound
such as `-dynfeeratemaxage=600` so that rates stop being honoured if the
sidecar dies.

## Rate semantics

The node values fees in **reference fee atoms** (rfa): the rate for an asset is
*how many atoms of the asset equal 1 whole reference unit (1e8 rfa)*, scaled by
1e8. The operator decides what the reference is (e.g. USD) by pricing all
sources in it: `rate = round(price_in_reference * 1e8)`. An asset trading at
exactly 1 reference unit has rate `100000000`.

## Inspecting state on the node

```sh
elements-cli getdynamicfeerates      # dynamic layer with source/age/staleness
elements-cli getfeeacceptancepolicy  # effective whitelist with provenance
elements-cli getfeeexchangerates     # effective (merged) rates
elements-cli cleardynamicfeerates    # manual kill-switch for the dynamic layer
```
