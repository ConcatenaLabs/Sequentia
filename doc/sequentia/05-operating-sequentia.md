# Operating Sequentia

This is the operator and wallet manual: how to join the public testnet, set
fee-asset acceptance, run the price server, anchor against Bitcoin, produce
blocks as a Proof-of-Stake committee member, manage the stake lifecycle, defend
against long-range attacks, and monitor a running network. Every command here
is exact. The design behind each capability lives in its own chapter -
[`01-architecture.md`](01-architecture.md),
[`02-open-fee-market.md`](02-open-fee-market.md), [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md),
and [`04-proof-of-stake.md`](04-proof-of-stake.md) - and the governance decisions
that frame a launch live in [`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md).

## 1. The model

There are three ways to run a Sequentia node:

- **Join the public testnet** (`-chain=test`, the binary's default chain). The
  genesis and almost every consensus rule are baked into `src/chainparams.cpp`
  (`CTestNetParams`), and on this chain the node auto-configures the shared
  gateway with zero config (`InitParameterInteraction`, `src/init.cpp`): a
  peer (`-addnode=159.195.15.140:18444`), a shared Bitcoin testnet4 anchor RPC
  endpoint, the public asset registry, and the reference-price feed. Explicit
  settings always override these defaults. A few consensus rules remain
  arg-configurable and are **network-wide**: on the current chain
  (re-genesis 2026-07-05) set `pospubliccommittee=1` and `poscommitteesize=250`
  (see §2); `posbls` keeps its default (on).
- **The mainnet parameters** (`-chain=sequentia`) exist but carry a placeholder
  genesis; the node refuses to start on them without
  `-allowplaceholdergenesis`. They become real only at a launch ceremony
  ([`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md)).
- **A custom chain** (any other `-chain=` name, e.g. `elementsregtest`): the
  genesis block and every consensus rule are derived from configuration, so
  all operators must share the exact same consensus-affecting settings - chain
  name, `signblockscript`, the staker seed, committee size, anchoring, fee
  mode, and address mode. A mismatch produces a different genesis or forks the
  chain. This is the path for local development networks and the functional
  tests.

Node-local settings are separate and per-operator on every chain: RPC
credentials, the RPC and P2P ports, and the parent (Bitcoin) connection. These
never affect consensus and must **not** be shared.

For a shared custom network the discipline is: keep the consensus block in a
shared, version-pinned config file that every operator uses verbatim, and keep
node-local settings in a drop-in or the tail of the same file. The genesis
block hash printed at first start is identical across all founding nodes only
when the consensus block matches - that equality is the launch go/no-go check.

## 2. Configuration

### Joining the public testnet

The minimal working config:

```ini
# elements.conf
chain=test

[test]
# Network-wide consensus rules of the current public chain (re-genesis
# 2026-07-05): the public fixed-size committee, cap 250 (quorum 126).
pospubliccommittee=1
poscommitteesize=250

# Your node's local RPC.
server=1
rpcuser=CHANGE_ME
rpcpassword=CHANGE_ME_TO_A_LONG_RANDOM_STRING
```

Everything else (peer, anchor RPC against Bitcoin testnet4, asset registry,
price feed) is defaulted for this chain by the binary; to use your own Bitcoin
testnet4 node for anchor validation, set the `mainchainrpc*` options (§6).
First start should print and sync from genesis
`ddd11d54c87a2bd94400fd31ce05d8e1110bb4b78e7103f738342086fc4ea92e`.

### A custom chain

The reference file is [`contrib/sequentia/sequentia.conf.example`](../../contrib/sequentia/sequentia.conf.example).
It is split into a consensus block (shared verbatim) and a node-local block (per
operator). The consensus block, with the values the bundled chains bake:

```ini
chain=seqcustom              # a custom chain name shared by all operators

# (1) Open / "no-coin" fee market: any issued asset may pay fees.
con_any_asset_fees=1

# (2) Bitcoin anchoring: every block references a parent Bitcoin block at a
#     non-decreasing height; the chain reorganizes iff Bitcoin does.
con_bitcoin_anchor=1

# (3) Proof-of-Stake: private VRF leader sortition + BLS-aggregated committee
#     (MuSig2 is the posbls=0 fallback).
con_pos=1
posvrf=1
posbls=1
pospubliccommittee=1         # public fixed-size committee (what the public
                             # testnet runs); omit for threshold VRF sortition.
poscommitteesize=250         # committee cap under the public committee (the
                             # actual committee is min(#stakers, cap), quorum a
                             # strict majority +1 at odd sizes); expected size
                             # under threshold sortition (max 100, fixed
                             # majority quorum).
posslotinterval=30           # seconds per slot; 30s on the bundled chains.
posunbonding=43200           # minimum unbonding lock in (Sequentia) blocks; required
                             # wall-clock = this x posslotinterval (x30s ~= 15
                             # days). Exceeds the 16-bit height-CSV range, so
                             # staking outputs use a time-based CSV lock
                             # (getstakescript csv_seconds=).
con_blocksubsidy=0           # no inflation: SEQ is pre-mined at genesis, never
                             # minted by production; producers earn fees only.
posminstake=4000000000000    # minimum blocksigner stake in SEQ atoms: 0.01% of
                             # supply = 40,000 SEQ. 0 disables the floor.

# Genesis stake seed (governance; identical on every node): either a genesis
# staking output (the bundled chains' model) ...
#con_genesis_stake=02<pubkey-hex>:<amount_atoms>:<csv>
#initialfreecoins=<atoms>
# ... or a config staker set (custom chains only; add :blspubkey:pop from
# getblsregistration when running the public committee).
#staker=02<pubkey-hex>:1000000

con_maxblockweight=200000    # block weight cap, a twentieth of Bitcoin's
                             # 4,000,000; with the slot interval, a saturated
                             # chain grows at Bitcoin's rate.
con_default_blinded_addresses=0   # Bitcoin-identical addresses; CT opt-in.
signblockscript=51           # PoS computes the per-block challenge; placeholder.

#poscheckpoint=100000:<blockhash>   # published long-range-attack checkpoints
poscheckpointdepth=2016      # parent-chain confirmations before a checkpoint
                             # finalizes (~2 weeks).
```

The node-local block (do **not** share):

```ini
server=1
rpcuser=CHANGE_ME
rpcpassword=CHANGE_ME_TO_A_LONG_RANDOM_STRING
#rpcport=7041
#port=7040

# Parent Bitcoin node, for anchoring (§6).
validateanchor=1
anchorminconf=1
mainchainrpchost=127.0.0.1
mainchainrpcport=8332
mainchainrpccookiefile=/home/YOU/.bitcoin/.cookie   # or mainchainrpcuser/password
```

Prerequisites: a built `elementsd` / `elements-cli`; a Bitcoin node reachable
over RPC for anchoring (mainnet, testnet, or regtest, as long as all operators
agree on which parent chain to anchor to); and Python 3 for the bundled tooling
(§10).

## 3. The open fee market

With `con_any_asset_fees=1`, a producer accepts fees in whatever assets it
chooses, valued against a common reference unit by an exchange-rate table. The
design is in [`02-open-fee-market.md`](02-open-fee-market.md); the operator
controls are below.

### The whitelist

The node keeps exactly **one** fee-asset whitelist (`ExchangeRateMap`,
persisted to `<datadir>/exchangerates.json`; see
[`02-open-fee-market.md`](02-open-fee-market.md) §2). Set and read it directly:

```
elements-cli setfeeexchangerates '{"<asset-hex>": 100000000, "<other-asset>": 50000000}'
elements-cli getfeeexchangerates
```

The rate is an **integer**: a fee output's reference value is
`value * rate / 1e8`. So `100000000` is 1:1. A **higher** rate values the asset
**more** per unit; a **lower** rate values it less - `50000000` values the asset
at half the reference unit, `200000000` at twice it. A rate of `0` refuses the
asset. An asset that is **not listed is not accepted**, with one exception: SEQ
defaults to 1:1 for an unconfigured producer (it holds no privileged status - a
producer may re-price it, refuse it, or make another asset the reference).

`getfeeacceptancepolicy` returns the current acceptance set - what the node
actually uses when valuing mempool transactions and building blocks.

### Keeping the whitelist dynamic: the price server

The price server in [`contrib/price-server/`](../../contrib/price-server) is a
sidecar that keeps that same single whitelist fresh: it polls
operator-designated market-data sources, applies admission thresholds (market
cap, 24h volume, source agreement, price clamps), and pushes the resulting
whole whitelist to the node. The node is unaware of the sidecar; whether the
whitelist is "static" or "dynamic" is determined entirely by whether a sidecar
is running (see the sidecar's own
[README](../../contrib/price-server/README.md)).

```
cp contrib/price-server/config.example.json config.json
# edit: node RPC creds, the asset map, and thresholds
python3 contrib/price-server/price_server.py --config config.json            # poll loop
python3 contrib/price-server/price_server.py --config config.json --once     # single poll, print, exit
python3 contrib/price-server/price_server.py --config config.json --dry-run  # decisions only
```

A rate is `round(price_in_reference * 1e8)`; an asset trading at exactly one
reference unit has rate `100000000`.

The sidecar publishes through `setdynamicfeerates`, a deprecated alias of
`setfeeexchangerates` that does **not** persist to `exchangerates.json` (a
running sidecar re-pushes after a node restart). Two consequences to know:

- The node holds the last-set rates **indefinitely** - there is no built-in
  staleness or expiry. Keeping rates fresh, and refusing assets when a feed
  dies (by writing `0` or omitting them), is the sidecar's job. On clean
  shutdown the sidecar clears the whitelist (`cleardynamicfeerates`), leaving
  only the policy asset's 1:1 default.
- Writes are last-writer-wins on the whole table: a manual
  `setfeeexchangerates` is simply overwritten at the sidecar's next poll.

Manual kill-switch: `elements-cli cleardynamicfeerates` (empties the
whitelist).

## 4. Paying fees in an arbitrary asset

A headline capability: a wallet holding **zero SEQ** can transact by paying its
fee in an issued asset, provided some producer prices that asset. The wallet's
fee output is then denominated in that asset, not SEQ.

Worked flow. Issue or obtain asset `X`, and have the producer price it:

```
elements-cli setfeeexchangerates '{"<X>": 100000000}'
```

Send, paying the fee in `X`:

```
elements-cli sendtoaddress -named address=<dest> amount=<n> assetlabel=<X> fee_asset_label=<X>
```

The resulting transaction carries a fee output denominated in `X`. A producer
that prices `X` will accept and include it; producers that do not price `X`
ignore it.

### The importable-key wallet pattern

To drive a single externally-held key (for example, the founder key from a
bootstrap) as a wallet, create the wallet **non-blank** so it can generate its
own change and issuance addresses, then import the key as a descriptor:

```
elements-cli createwallet "<name>" false false   # NON-blank
elements-cli getdescriptorinfo "wpkh(<WIF>)"      # returns the checksum
elements-cli importdescriptors '[{"desc":"wpkh(<WIF>)#<checksum>","timestamp":0}]'
```

Import the **WIF** form (`wpkh(<WIF>)`), not the public descriptor - the wallet
needs the private key to sign. Use the checksum that `getdescriptorinfo` returns
for the WIF descriptor.

## 5. Unsticking transactions (RBF / CPFP) with asset fees

Both replacement strategies work with asset-denominated fees. The mempool
compares fees in **reference value**, not raw asset amounts: a replacement must
be worth more in reference terms, and the `maxtxfee`/absurd-fee ceiling is also
checked in reference value. You therefore cannot "replace" a transaction by
paying a larger number of a cheap asset for less real value.

### RBF (replace-by-fee)

Send the original as replaceable, then bump it - the replacement may pay a
higher fee and even switch the fee asset:

```
elements-cli sendtoaddress -named address=<dest> amount=<n> assetlabel=<X> fee_asset_label=<X> replaceable=true
elements-cli bumpfee <txid> '{"fee_rate":N,"fee_asset":"<asset>"}'
```

### CPFP (child-pays-for-parent)

A recipient can accelerate an unconfirmed **incoming** transaction by spending it
with a high-fee child. `include_unsafe` lets the wallet spend the not-yet-
confirmed parent:

```
elements-cli send '{"<dest>":<amount>}' '{"include_unsafe": true, "fee_asset": "<asset>"}'
```

## 6. Bitcoin anchoring

Point the node at the parent Bitcoin node (these are node-local settings; see
[`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) for the design):

```ini
con_bitcoin_anchor=1
validateanchor=1
anchorminconf=1              # anchor to the parent tip; raise to require burial
anchorpollinterval=<secs>    # how often to poll the parent for its tip
mainchainrpchost=127.0.0.1
mainchainrpcport=8332
mainchainrpccookiefile=/home/YOU/.bitcoin/.cookie   # or mainchainrpcuser/password
```

Every block then references a parent block at a non-decreasing height, and the
node reorganizes **if and only if** the parent reorganizes away a referenced
block. Monitor with `getanchorstatus`, which reports the current anchor and
parent-connection health.

On the public testnet (`-chain=test`) these settings default to a shared
Bitcoin **testnet4** endpoint (`src/init.cpp`, `InitParameterInteraction`), so
a fresh node validates anchors with no configuration; set the `mainchainrpc*`
options explicitly to use your own testnet4 node instead.

**Reaching an HTTPS Bitcoin endpoint.** The mainchain RPC client speaks plain
HTTP only and always sends a Basic-auth header. To anchor against an HTTPS
endpoint, run a local TLS-terminating proxy, point `mainchainrpchost`/
`mainchainrpcport` at the proxy, and set a (possibly dummy)
`mainchainrpcpassword` so the Basic-auth header is well-formed.

**Fresh-chain status.** `getanchorstatus` reads `not_validated` on a fresh chain
at height 0 - the genesis tip carries no anchor. **Block 1 creates the first
anchor**, so do not wait for an `ok` status at height 0; it will never appear
there.

## 7. Running a Proof-of-Stake producer

A staker in the configured set produces a block when its VRF sortition slot
opens; the committee then certifies it with a single BLS-aggregated signature
(MuSig2 is the `-posbls=0` fallback). See
[`04-proof-of-stake.md`](04-proof-of-stake.md) for the consensus.

### Single host (one operator holds a committee quorum of keys)

```
elements-cli generateposblock "<leader-WIF>" '["<member-WIF>", "<member-WIF>", ...]'
```

The node computes the VRF proofs, builds the block, and (under
`posaggcommittee`) aggregates the members' signatures locally. Good for testing
and for one operator running several stakers; it requires that the single host
hold every signing key.

### Single-operator bootstrap on a fresh chain

A bundled chain seeds exactly **one** genesis staker (the founder). Normal
certification needs a quorum of the committee to countersign, so a lone
founder can only produce escaping-stall blocks (one per few Bitcoin blocks)
until enough stakers exist - except under the public fixed-size committee,
where the founder alone is a size-1 committee (quorum 1) until others register
(this is how the 2026-07-05 public-testnet re-genesis self-started; the
end-to-end bring-up tool is
[`contrib/sequentia/bootstrap-autonomous-testnet.py`](../../contrib/sequentia/bootstrap-autonomous-testnet.py)
with `--public-committee`). The older helper
[`contrib/sequentia/bootstrap-committee.py`](../../contrib/sequentia/bootstrap-committee.py)
stands up a small threshold-sortition committee for a single operator.

For that helper: size the committee to the staker count with
`-poscommitteesize` (e.g. `-poscommitteesize=3` for the founder plus two new
stakers), start the node on a **fresh** datadir at height 0 with anchoring
configured, and run:

```
python3 contrib/sequentia/bootstrap-committee.py \
    --founder-wif "<genesis-founder-WIF>" --members 2
```

Inputs: `--founder-wif` (the genesis founder's WIF, required); `--members` (the
number of new stakers to create, default 2); optional `--amount` (SEQ locked per
new staker, default `1000000` - keep stakers equal so VRF sortition always
selects the whole committee), `--csv-seconds` (the unbonding lock per output,
default `1300000` ≈ 15 days, which must meet the chain minimum), `--cli`, and
`--fee`. It:

1. generates the new staker keys,
2. spends the founder's genesis coins into CSV-locked staking outputs (each
   output registers that key as an on-chain staker), signing with the founder
   WIF directly - no wallet rescan required,
3. mines one escaping-stall block to confirm the registrations, and
4. mines a first full-committee block to prove the quorum is reachable,
   checking it carries at least quorum countersignatures.

It then prints the new staker WIFs and the `generateposblock` command to keep
producing blocks at full speed.

### Distributed committee (members on separate hosts)

For a real decentralized committee, no node holds more than its own key.

**The default: the autonomous committee.** On `chain=test` and `chain=sequentia`
the autonomous BLS gossip-and-sign layer is **on by default** (`g_pos_bls`, set
with `-posbls`). Each staker simply runs `-posproducer -posproducerkey=<its WIF>`;
the nodes elect the round's leader, gossip BLS signature shares, and aggregate the
committee certificate themselves - no coordinator, no per-slot RPC choreography.
This is the production path, and it is how the public testnet runs (with
`pospubliccommittee=1 poscommitteesize=250`; membership then additionally
requires the on-chain BLS registration of §8). A worked single-machine
bring-up (founder → escaping-stall bootstrap → autonomous quorum, anchored to
Bitcoin) is in
[`contrib/sequentia/bootstrap-autonomous-testnet.py`](../../contrib/sequentia/bootstrap-autonomous-testnet.py)
and [`doc/sequentia/demos/100-node-bootstrap-runbook.md`](demos/100-node-bootstrap-runbook.md).

**Manual MuSig2 fallback (`-posbls=0`).** Setting `-posbls=0` reverts to the
older coordinator model, where each slot's certificate is assembled by hand
(or by external tooling) over the per-slot flow below:

1. **Eligibility.** Each member proves its slot eligibility over the seed from
   `getposschedule` (fed in internal byte order - reverse the `getposschedule`
   hex, which is what `getposblocktemplate` also returns):
   ```
   elements-cli vrfprove "<member-WIF>" "<seed>"
   ```
2. **Template.** The leader assembles the unsigned block and the hash to sign:
   ```
   elements-cli getposblocktemplate "<leader-WIF>" \
     '[{"pubkey":"<m1>","vrfproof":"<p1>"}, {"pubkey":"<m2>","vrfproof":"<p2>"}, ...]'
   ```
3. **Round 1** - each member, on its own node, with a fresh session id:
   ```
   elements-cli musignonce "<sid>" "<member-WIF>" '[<members>]' "<signhash>"
   ```
4. **Round 2** - each member, given all the public nonces:
   ```
   elements-cli musigpartialsign "<sid>" "<member-WIF>" '[<members>]' '[<pubnonces>]' "<signhash>"
   ```
5. **Aggregate** (coordinator, public data only) and submit:
   ```
   elements-cli musigaggregate '[<members>]' '[<pubnonces>]' '[<partials>]' "<signhash>"
   elements-cli submitposblock "<block>" "<leader-WIF>" "<aggregatesig>"
   ```

A signing session is single-use and bound to its `(signer, member set, message)`
triple. This manual flow is only needed under the `-posbls=0` fallback; with the
default autonomous committee the nodes perform the equivalent share exchange and
aggregation themselves over gossip.

## 8. Stake lifecycle

Staking is **opt-in and explicit - holding SEQ does nothing on its own.** SEQ
confers stake weight only while it sits in a dedicated **staking output**, the
bare script

```
<csv> OP_CHECKSEQUENCEVERIFY OP_DROP <pubkey> OP_CHECKSIG
```

holding an *explicit* (unblinded) policy-asset amount. A confidential output
carries no weight (its amount is hidden), and an amount below `-posminstake`
(40,000 SEQ on the bundled chains) is ignored for eligibility.

**Becoming a staker.**

1. On a public-committee chain (the public testnet), first derive your
   committee BLS registration from your staker key:
   ```
   elements-cli getblsregistration "<staker-WIF>"
   ```
   It returns `blspubkey` (48 bytes) and `pop` (the proof-of-possession,
   96 bytes), both deterministic from the key. A staking output without this
   registration still carries weight (and leader eligibility) but its key
   cannot sit on the public committee.
2. Get the staking script for your staker key:
   ```
   elements-cli getstakescript "<pubkey>" null <csv_seconds> <blspubkey> <pop>
   ```
   (omit the last two arguments on a chain without the public committee). It
   returns the `scriptPubKey`, the lock it encodes (`lock_seconds`), and the
   chain's required minimum (`min_unbonding_seconds`); a lock below that minimum
   is refused, because the output would not count as stake. `null` skips the
   height-based `csv_blocks` form - the ~2-week lock exceeds the 16-bit
   height-CSV range, so the bundled chains use a **time-based** CSV
   (`csv_seconds`).
3. Pay at least the minimum stake to that script in an ordinary transaction (it
   relays and mines under standard policy - no special configuration). The
   staking script is a bare script with no address form, so a plain
   `sendtoaddress` cannot target it: build the funding transaction raw
   (`createrawtransaction`-style flow against the script hex, as
   `contrib/sequentia/bootstrap-autonomous-testnet.py` does) or use an
   operator-provided helper. Once it
   confirms and stays unspent, its weight registers automatically: the registry
   is rebuilt from the UTXO set at startup and mirrored on every connect,
   disconnect, and reorg.
4. Run a producer holding that key (`-posproducer -posproducerkey=<WIF>`, with
   the default BLS gossip committee) so the node signs and produces when it is
   selected.

Introspect with `getstakerinfo` (the active registry) and `getposschedule` (the
next slot's seed, leader schedule, committee, and quorum).

**The lock is the unbonding delay, not a staking term - staking is indefinite.**
The CSV is a BIP68 *relative* locktime measured from the output's confirmation;
on the bundled chains it is `-posunbonding × -posslotinterval` = 43200 × 30 s ≈
**15 days** (it must exceed the Bitcoin-checkpoint window - §9). It neither
expires your stake nor needs renewing:

- While the lock is still maturing, the output **counts as stake** and is
  **unspendable** - you are committed.
- After the lock matures, the output **still counts as stake** - it is merely now
  *spendable* if you choose to withdraw. Maturity gates withdrawal, never
  participation.
- So an output stakes **continuously for as long as it is unspent - indefinitely.**
  There is no re-locking, renewal, or keep-alive: to keep staking, do nothing.

**Unbonding** is simply spending the staking output once its lock has matured
(the staker's signature satisfies the script) - there is no separate ceremony.
That spend ends the stake; the freed SEQ becomes ordinary coin to hold or to pay
into a fresh staking output and stake again.

## 9. Long-range-attack defenses

Two complementary layers (see [`04-proof-of-stake.md`](04-proof-of-stake.md)):

- **Dynamic Bitcoin checkpoints.** Anyone commits a block hash into the parent
  chain - `getcheckpointpayload` produces the OP_RETURN to embed. Once buried
  `-poscheckpointdepth` deep on the parent, nodes that already validated the
  block treat it as final and reject forks below it. `getcheckpointinfo` shows
  the checkpoints, the finality height, a `conflicts` alarm if this node ever
  passed a checkpointed height without the checkpointed block, and the
  configured pins.
- **Configured static checkpoints.** For a node syncing from genesis, pin known
  heights so a long-range alternate history is refused before any block is
  downloaded:
  ```ini
  poscheckpoint=100000:<blockhash>
  poscheckpoint=200000:<blockhash>
  ```
  A block at a pinned height must carry the pinned hash; a peer feeding a bogus
  chain is rejected and disconnected. Publish these with each release.

## 10. Bundled tooling

| Tool | Purpose | Run |
|---|---|---|
| [`contrib/sequentia/bootstrap-autonomous-testnet.py`](../../contrib/sequentia/bootstrap-autonomous-testnet.py) | End-to-end bring-up of an autonomous committee network (founder → stake registration → autonomous quorum), on `chain=test` or a custom chain; `--public-committee --committee-cap 250` launches the public fixed-size committee (how the 2026-07-05 re-genesis was executed). | `python3 contrib/sequentia/bootstrap-autonomous-testnet.py --chain test --public-committee --nodes 5` |
| [`contrib/sequentia/bootstrap-committee.py`](../../contrib/sequentia/bootstrap-committee.py) | Stand up a small threshold-sortition PoS committee for a single operator on a fresh chain (creates N stakers, registers them, produces a first full-committee block). | `python3 contrib/sequentia/bootstrap-committee.py --founder-wif <WIF> --members 2` |
| [`contrib/sequentia/run-local-testnet.py`](../../contrib/sequentia/run-local-testnet.py) | Run a small local network for development. | `python3 contrib/sequentia/run-local-testnet.py` |
| [`contrib/sequentia/swap-demo.py`](../../contrib/sequentia/swap-demo.py) | Self-contained cross-chain BTC<->asset atomic-swap demonstration: spins up two local chains, runs the HTLC swap and the timeout-refund path, then cleans up. | `python3 contrib/sequentia/swap-demo.py` |
| [`contrib/price-server/`](../../contrib/price-server) | The dynamic-fee price server: polls market-data APIs and publishes qualifying assets' rates via `setdynamicfeerates`. | `python3 contrib/price-server/price_server.py --config config.json` |

## 11. Monitoring quick reference

| RPC | Tells you |
|---|---|
| `getanchorstatus` | current Bitcoin anchor and parent-connection health (`not_validated` at height 0 is normal) |
| `getposschedule` | next slot's seed, leader schedule, committee, and quorum |
| `getstakerinfo` | the active stake registry |
| `getcheckpointinfo` | checkpoints, finality height, conflict alarm, configured pins |
| `getfeeacceptancepolicy` | the current fee-asset acceptance whitelist |

## 12. Launch checklist

Operational pointers, in launch order:

1. Agree the consensus block (§2) and distribute it version-pinned. Confirm
   every founding node prints the **same genesis hash** at first start.
2. Configure each node's parent (Bitcoin) connection (§6), including a
   TLS-terminating proxy if the endpoint is HTTPS. Verify `getanchorstatus`.
3. Bootstrap the committee (§7) and confirm full-committee blocks reach quorum.
4. Set fee-asset acceptance (§3): a hand-set whitelist and/or the price
   server. Verify with `getfeeacceptancepolicy`.
5. Publish initial checkpoints (§9) and put `poscheckpoint=` pins in the shared
   config for syncing nodes.

The division between what is a **governance** decision (genesis staker seed and
distribution, total SEQ supply, minimum stake, committee size, unbonding period,
slot interval, which parent chain to anchor to, published checkpoints) and what
is **engineering** here is set out in
[`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md). Once the
governance values are fixed and shared as the consensus config, every operator
derives the same genesis and the network is live.
