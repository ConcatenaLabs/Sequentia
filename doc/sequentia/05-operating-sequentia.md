# Operating Sequentia

This is the operator and wallet manual: how to configure a node, set fee-asset
acceptance, run the price server, anchor against Bitcoin, produce blocks as a
Proof-of-Stake committee, manage the stake lifecycle, defend against long-range
attacks, and monitor a running network. Every command here is exact. The design
behind each capability lives in its own chapter — [`01-architecture.md`](01-architecture.md),
[`02-open-fee-market.md`](02-open-fee-market.md), [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md),
and [`04-proof-of-stake.md`](04-proof-of-stake.md) — and the governance decisions
that frame a launch live in [`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md).

## 1. The model

A Sequentia network is a set of operators running `elementsd` with an
**identical consensus configuration**. Sequentia runs as an Elements *custom
chain*: the genesis block and every consensus rule are derived from the
configuration, so all operators must share the exact same consensus-affecting
settings — chain name, `signblockscript`, the staker seed, committee size,
anchoring, fee mode, and address mode. A mismatch in any of these produces a
different genesis or forks the chain.

Node-local settings are separate and per-operator: RPC credentials, the RPC and
P2P ports, and the parent (Bitcoin) connection. These never affect consensus and
must **not** be shared.

The discipline is therefore: keep the consensus block in a shared,
version-pinned config file that every operator uses verbatim, and keep
node-local settings in a drop-in or the tail of the same file. The genesis block
hash printed at first start is identical across all founding nodes only when the
consensus block matches — that equality is the launch go/no-go check (see
[`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md)).

## 2. Configuration

The reference file is [`contrib/sequentia/sequentia.conf.example`](../../contrib/sequentia/sequentia.conf.example).
It is split into a consensus block (shared verbatim) and a node-local block (per
operator). The consensus block:

```ini
chain=sequentia              # a custom chain name shared by all operators

# (1) Open / "no-coin" fee market: any issued asset may pay fees.
con_any_asset_fees=1

# (2) Bitcoin anchoring: every block references a parent Bitcoin block at a
#     non-decreasing height; the chain reorganizes iff Bitcoin does.
con_bitcoin_anchor=1

# (3) Proof-of-Stake: private VRF sortition + BLS-aggregated committee
#     (MuSig2 is the -posbls=0 fallback).
con_pos=1
posvrf=1
posaggcommittee=1
poscommitteesize=100         # expected committee size; quorum = strict majority
                             # (the paper's 51-of-100). The staker set must hold
                             # at least quorum-many sortition-eligible members.
posslotinterval=30           # seconds per VRF sortition slot; 30s on
                             # mainnet/testnet, configurable on custom chains.
posunbonding=43200           # minimum unbonding lock in (SEQ) blocks; required
                             # wall-clock = this x posslotinterval (x30s ~= 15
                             # days). Exceeds the 16-bit height-CSV range, so
                             # staking outputs use a time-based CSV lock
                             # (getstakescript csv_seconds=).
con_blocksubsidy=0           # no inflation: SEQ is pre-mined at genesis, never
                             # minted by production; producers earn fees only.
posminstake=4000000000000    # minimum blocksigner stake in SEQ atoms: 0.01% of
                             # supply = 40,000 SEQ. 0 disables the floor.

# The genesis staker set (governance; identical on every node). Replace these.
#staker=02<pubkey-hex>:1000000
#staker=03<pubkey-hex>:1000000

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

# Dynamic fee rates fail closed if the price server stops publishing (§3).
dynfeeratemaxage=600
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

### Static acceptance

Set and read the static whitelist directly:

```
elements-cli setfeeexchangerates '{"<asset-hex>": 100000000, "<other-asset>": 50000000}'
elements-cli getfeeexchangerates
```

The rate is an **integer**: a fee output's reference value is
`value * rate / 1e8`. So `100000000` is 1:1. A **higher** rate values the asset
**more** per unit; a **lower** rate values it less — `50000000` values the asset
at half the reference unit, `200000000` at twice it. An asset that is **not
listed is not accepted**, with one exception: SEQ defaults to 1:1 for an
unconfigured producer (it holds no privileged status — a producer may re-price
it, refuse it, or make another asset the reference).

### Dynamic acceptance

The price server in [`contrib/price-server/`](../../contrib/price-server) maintains a
dynamic layer: it polls operator-designated market-data APIs, applies admission
thresholds (market cap, 24h volume, source agreement, price clamps), and pushes
qualifying assets' rates via `setdynamicfeerates`.

```
cp contrib/price-server/config.example.json config.json
# edit: node RPC creds, the asset map, and thresholds
python3 contrib/price-server/price_server.py --config config.json            # poll loop
python3 contrib/price-server/price_server.py --config config.json --once     # single poll
python3 contrib/price-server/price_server.py --config config.json --dry-run  # decisions only
```

A rate is `round(price_in_reference * 1e8)`; an asset trading at exactly one
reference unit has rate `100000000`. Static pins always win over the dynamic
layer. On clean shutdown the server clears its layer.

Set `-dynfeeratemaxage` (e.g. `600`) so dynamic rates **fail closed**: they
expire that many seconds after the server stops publishing. The built-in default
is `0` = no expiry, which would honour stale rates forever if the server
crashed. Inspect and clear the dynamic layer:

```
elements-cli getdynamicfeerates      # dynamic layer with source / age / staleness
elements-cli cleardynamicfeerates    # manual kill-switch
```

`getfeeacceptancepolicy` shows the **effective** acceptance — the merged static
and dynamic layers with provenance — which is what the node actually uses when
building blocks.

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

Import the **WIF** form (`wpkh(<WIF>)`), not the public descriptor — the wallet
needs the private key to sign. Use the checksum that `getdescriptorinfo` returns
for the WIF descriptor.

## 5. Unsticking transactions (RBF / CPFP) with asset fees

Both replacement strategies work with asset-denominated fees. The mempool
compares fees in **reference value**, not raw asset amounts: a replacement must
be worth more in reference terms, and the `maxtxfee`/absurd-fee ceiling is also
checked in reference value. You therefore cannot "replace" a transaction by
paying a larger number of a cheap asset for less real value.

### RBF (replace-by-fee)

Send the original as replaceable, then bump it — the replacement may pay a
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

**Reaching an HTTPS Bitcoin endpoint.** The mainchain RPC client speaks plain
HTTP only and always sends a Basic-auth header. To anchor against an HTTPS
endpoint, run a local TLS-terminating proxy, point `mainchainrpchost`/
`mainchainrpcport` at the proxy, and set a (possibly dummy)
`mainchainrpcpassword` so the Basic-auth header is well-formed.

**Fresh-chain status.** `getanchorstatus` reads `not_validated` on a fresh chain
at height 0 — the genesis tip carries no anchor. **Block 1 creates the first
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
certification needs a strict majority of the committee to countersign, so a lone
founder can only produce escaping-stall blocks (one per few Bitcoin blocks) until
a quorum of stakers exists. The helper
[`contrib/sequentia/bootstrap-committee.py`](../../contrib/sequentia/bootstrap-committee.py)
stands up a small committee for a single operator.

On testnet, size the committee to the staker count with `-poscommitteesize`
(e.g. `-poscommitteesize=3` for the founder plus two new stakers), start the
node on a **fresh** datadir at height 0 with anchoring configured, and run:

```
python3 contrib/sequentia/bootstrap-committee.py \
    --founder-wif "<genesis-founder-WIF>" --members 2
```

Inputs: `--founder-wif` (the genesis founder's WIF, required); `--members` (the
number of new stakers to create, default 2); optional `--amount` (SEQ locked per
new staker, default `1000000` — keep stakers equal so VRF sortition always
selects the whole committee), `--csv-seconds` (the unbonding lock per output,
default `1300000` ≈ 15 days, which must meet the chain minimum), `--cli`, and
`--fee`. It:

1. generates the new staker keys,
2. spends the founder's genesis coins into CSV-locked staking outputs (each
   output registers that key as an on-chain staker), signing with the founder
   WIF directly — no wallet rescan required,
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
committee certificate themselves — no coordinator, no per-slot RPC choreography.
This is the production path. A worked single-machine bring-up (founder →
escaping-stall bootstrap → autonomous quorum, anchored to Bitcoin) is in
[`contrib/sequentia/bootstrap-autonomous-testnet.py`](../../contrib/sequentia/bootstrap-autonomous-testnet.py)
and [`doc/sequentia/demos/100-node-bootstrap-runbook.md`](demos/100-node-bootstrap-runbook.md).

**Manual MuSig2 fallback (`-posbls=0`).** Setting `-posbls=0` reverts to the
older coordinator model, where each slot's certificate is assembled by hand
(or by external tooling) over the per-slot flow below:

1. **Eligibility.** Each member proves its slot eligibility over the seed from
   `getposschedule` (fed in internal byte order — reverse the `getposschedule`
   hex, which is what `getposblocktemplate` also returns):
   ```
   elements-cli vrfprove "<member-WIF>" "<seed>"
   ```
2. **Template.** The leader assembles the unsigned block and the hash to sign:
   ```
   elements-cli getposblocktemplate "<leader-WIF>" \
     '[{"pubkey":"<m1>","vrfproof":"<p1>"}, {"pubkey":"<m2>","vrfproof":"<p2>"}, ...]'
   ```
3. **Round 1** — each member, on its own node, with a fresh session id:
   ```
   elements-cli musignonce "<sid>" "<member-WIF>" '[<members>]' "<signhash>"
   ```
4. **Round 2** — each member, given all the public nonces:
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

Register stake on-chain by paying to a staking script from `getstakescript`,
whose weight is its explicit policy-asset amount and whose CSV lock must elapse
before it can be spent — **unbonding is that spend**. For a lock beyond the
16-bit height-CSV range (e.g. the ~2-week checkpoint window), request a
time-based script:

```
elements-cli getstakescript "<pubkey>" null <csv_seconds>
```

The registry is rebuilt from the UTXO set at startup and mirrored on every
reorg. Introspect with `getstakerinfo` (the active stake registry) and
`getposschedule` (the next slot's seed, leader schedule, committee, and quorum).

## 9. Long-range-attack defenses

Two complementary layers (see [`04-proof-of-stake.md`](04-proof-of-stake.md)):

- **Dynamic Bitcoin checkpoints.** Anyone commits a block hash into the parent
  chain — `getcheckpointpayload` produces the OP_RETURN to embed. Once buried
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
| [`contrib/sequentia/bootstrap-committee.py`](../../contrib/sequentia/bootstrap-committee.py) | Stand up a small PoS committee for a single operator on a fresh testnet (creates N stakers, registers them, produces a first full-committee block). | `python3 contrib/sequentia/bootstrap-committee.py --founder-wif <WIF> --members 2` |
| [`contrib/sequentia/swap-demo.py`](../../contrib/sequentia/swap-demo.py) | Self-contained cross-chain BTC<->asset atomic-swap demonstration: spins up two local chains, runs the HTLC swap and the timeout-refund path, then cleans up. | `python3 contrib/sequentia/swap-demo.py` |
| [`contrib/price-server/`](../../contrib/price-server) | The dynamic-fee price server: polls market-data APIs and publishes qualifying assets' rates via `setdynamicfeerates`. | `python3 contrib/price-server/price_server.py --config config.json` |

## 11. Monitoring quick reference

| RPC | Tells you |
|---|---|
| `getanchorstatus` | current Bitcoin anchor and parent-connection health (`not_validated` at height 0 is normal) |
| `getposschedule` | next slot's seed, leader schedule, committee, and quorum |
| `getstakerinfo` | the active stake registry |
| `getcheckpointinfo` | checkpoints, finality height, conflict alarm, configured pins |
| `getfeeacceptancepolicy` | effective static + dynamic fee-asset acceptance |

## 12. Launch checklist

Operational pointers, in launch order:

1. Agree the consensus block (§2) and distribute it version-pinned. Confirm
   every founding node prints the **same genesis hash** at first start.
2. Configure each node's parent (Bitcoin) connection (§6), including a
   TLS-terminating proxy if the endpoint is HTTPS. Verify `getanchorstatus`.
3. Bootstrap the committee (§7) and confirm full-committee blocks reach quorum.
4. Set fee-asset acceptance (§3): static pins and/or the price server, with
   `-dynfeeratemaxage` so dynamic rates fail closed. Verify with
   `getfeeacceptancepolicy`.
5. Publish initial checkpoints (§9) and put `poscheckpoint=` pins in the shared
   config for syncing nodes.

The division between what is a **governance** decision (genesis staker seed and
distribution, total SEQ supply, minimum stake, committee size, unbonding period,
slot interval, which parent chain to anchor to, published checkpoints) and what
is **engineering** here is set out in
[`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md). Once the
governance values are fixed and shared as the consensus config, every operator
derives the same genesis and the network is live.
