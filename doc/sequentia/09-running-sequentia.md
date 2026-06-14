# Running Sequentia — operator runbook

This ties the implemented pieces together into a deployable system. It covers
standing up a validating node, the open fee market with its price server,
Bitcoin anchoring against a parent node, and a Proof-of-Stake committee
(single-host and fully distributed). Every command here mirrors a configuration
exercised by the functional test suite (`test/functional/feature_pos_*`,
`feature_bitcoin_anchoring.py`, `feature_dynamic_fee_rates.py`), so it is known
to work against the binaries this repository builds.

> **What this runbook is not.** It does not pin a *mainnet* genesis, an initial
> stake distribution, or a federation key set. Those are launch-governance
> decisions for the operators founding a network, not engineering defaults —
> see [§8](#8-launch-checklist-engineering-vs-governance). Everything below is
> the engineering: how to configure and operate the node once those decisions
> are made.

## 1. The model

A Sequentia network is a set of operators running `elementsd` with an
**identical consensus configuration**. Because Sequentia runs as an
Elements *custom chain*, the genesis block and all consensus rules are derived
from the configuration — so every operator must share the exact same
consensus-affecting settings (chain name, `-signblockscript`, the staker set,
committee size, anchoring, fee mode, address mode). A mismatch in any of these
produces a different genesis or a fork. Keep them in a shared, version-pinned
config file; treat node-local settings (RPC credentials, ports, the parent-node
connection) separately.

The four ways Sequentia differs from Liquid (see
[`00-overview-and-base-decision.md`](00-overview-and-base-decision.md)) map to
four configuration areas: the open fee market (§4), Bitcoin anchoring (§5),
Proof-of-Stake (§6), and Bitcoin-identical addresses with opt-in confidential
transactions (built in; see [`08-addresses-and-ct.md`](08-addresses-and-ct.md)).

## 2. Prerequisites

- A built `elementsd` / `elements-cli` (see the repo README for the build).
- **A Bitcoin node** reachable over RPC, for anchoring (§5). Anchoring reuses
  Elements' parent-chain RPC transport (`-mainchainrpc*`); the node may be
  Bitcoin mainnet, testnet, or regtest as long as all Sequentia operators agree
  on which parent chain they anchor to.
- Python 3 for the fee price server (§4).

## 3. The shared chain config

Put the consensus-affecting settings in a file every operator uses verbatim.
A reference is in [`contrib/sequentia/sequentia.conf.example`](../../contrib/sequentia/sequentia.conf.example);
the consensus block looks like:

```ini
chain=sequentia              # a custom chain name shared by all operators

# (1) Open fee market: any issued asset may pay fees
con_any_asset_fees=1

# (2) Bitcoin anchoring: every block references a parent (Bitcoin) block
con_bitcoin_anchor=1

# (3) Proof-of-Stake: private VRF sortition + MuSig2-aggregated committee
con_pos=1
posvrf=1
posaggcommittee=1
poscommitteesize=100         # quorum = strict majority of this: the paper's 51-of-100
posslotinterval=30           # nominal block time: 30s blocks (20x Bitcoin's
                             # cadence). With con_maxblockweight=200000 below,
                             # total disk grows at Bitcoin's rate (200000/30s).
posunbonding=43200           # min unbonding lock in (SEQ) blocks; required
                             # wall-clock = this x posslotinterval. ~15 days at
                             # 30s, exceeding the 2016-BTC-block (~2 week)
                             # checkpoint window (whitepaper §3.11). Beyond the
                             # 16-bit height-CSV range, so stakers must use a
                             # time-based CSV lock (getstakescript csv_seconds=).
con_blocksubsidy=0           # no inflation: SEQ is pre-mined at genesis, no
                             # coinbase generation; producers earn fees only
                             # (whitepaper §3.9). 0 is the default for a custom
                             # chain; set explicitly to make the tenet visible.
posminstake=4000000000000    # min stake to be a blocksigner: 0.01% of supply =
                             # 40,000 SEQ (40000 * 1e8 atoms), whitepaper §3.3.
# Bootstrap (preferred): seed the founder's stake IN GENESIS so the chain
# starts with no -staker config and grows its staker set entirely on-chain
# (the bundled chain does this; doc 13). On a custom chain:
#   con_genesis_stake=<founderpubkey>:<atoms>:<csv>   # one CSV-locked stake
#   initialfreecoins=<atoms>                          # spendable remainder
#
# Alternatively (optional, legacy): a static config staker set — IDENTICAL on
# every node (see §6 / §8). If used, it must hold at least quorum-many
# sortition-eligible members. The two layers are additive (doc 06 §5):
#staker=02<pubkey-hex>:1000000
#staker=03<pubkey-hex>:1000000

# Block weight cap: 200,000 (a twentieth of Bitcoin's) for ~30s blocks; total
# disk grows at Bitcoin's rate: 200,000 / 30s == 4,000,000 / 600s (§3.10)
con_maxblockweight=200000

# (4) Bitcoin-identical addresses, opt-in confidential transactions
con_default_blinded_addresses=0

signblockscript=51           # PoS computes the per-block challenge; this is the placeholder
```

Node-local settings (NOT shared) go in the same file or a drop-in: `rpcuser`/
`rpcpassword`, `rpcport`, `port`, and the `-mainchainrpc*` block in §5.

## 4. The open fee market

With `con_any_asset_fees=1`, producers accept fees in any asset they choose,
valued by a relative exchange-rate table. Two layers (see
[`02-open-fee-market.md`](02-open-fee-market.md)):

- **Static** — a fixed `{asset → relative value}` whitelist. Rates are
  *integers*: how many atoms of the asset equal 1 policy unit (100000000
  atoms), so 100000000 means 1:1 and 50000000 values the asset at twice the
  policy asset:
  ```
  elements-cli setfeeexchangerates '{"<asset-hex>": 100000000, "<other-asset>": 50000000}'
  elements-cli getfeeexchangerates
  ```
- **Dynamic** — a locally-run **price server** that polls exchange/DEX APIs and
  auto-admits assets crossing operator thresholds (market cap, volume,
  volatility), pushing rates via `setdynamicfeerates`:
  ```
  cp contrib/price-server/config.example.json price-server.json
  # edit thresholds, asset map, and RPC credentials
  python3 contrib/price-server/price_server.py --config price-server.json
  ```
  **Set `-dynfeeratemaxage` (e.g. 600).** With it, dynamic rates expire that
  many seconds after the server stops publishing, so a dead price server fails
  closed; the default is 0 = *no expiry*, which would honour stale rates
  forever if the server crashes. Inspect with `getdynamicfeerates` /
  `getfeeacceptancepolicy`; clear with `cleardynamicfeerates`.

## 5. Bitcoin anchoring

Point the node at your parent Bitcoin node (node-local settings):

```ini
con_bitcoin_anchor=1
validateanchor=1
anchorminconf=1              # anchor to the parent tip; raise to require burial
mainchainrpchost=127.0.0.1
mainchainrpcport=8332
mainchainrpccookiefile=/home/user/.bitcoin/.cookie   # or mainchainrpcuser/password
```

Every block then references a parent block at a non-decreasing height, and the
node reorganizes **if and only if** the parent reorganizes away a referenced block (see
[`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md)). Monitor with
`getanchorstatus`. This is what gives immediate finality and friction-free
cross-chain atomic swaps against native BTC.

## 6. Running a Proof-of-Stake producer

A staker in the configured set produces a block when its VRF sortition slot
opens. The committee then certifies it.

### Single host (one operator holds a committee quorum of keys)

```
elements-cli generateposblock "<leader-WIF>" '["<member-WIF>", "<member-WIF>", ...]'
```

The node computes the VRF proofs, builds the block, and (under
`posaggcommittee`) MuSig2-aggregates the members' signatures locally. Good for
testing and for a single operator running several stakers; it requires that one
host hold every signing key.

### Distributed committee (members on separate hosts)

For a real decentralized committee, no node holds more than its own key. The
flow (see [`07-vrf.md`](07-vrf.md) §6), which a coordinator script automates:

1. **Eligibility.** Each member proves its slot eligibility over the seed from
   `getposschedule`:
   ```
   # note: feed vrfprove the seed in internal byte order (reverse the
   # getposschedule hex), which is what getposblocktemplate also returns
   elements-cli vrfprove "<member-WIF>" "<seed>"
   ```
2. **Template.** The leader assembles the unsigned block and the hash to sign:
   ```
   elements-cli getposblocktemplate "<leader-WIF>" \
     '[{"pubkey":"<m1>","vrfproof":"<p1>"}, {"pubkey":"<m2>","vrfproof":"<p2>"}, ...]'
   # → { block, signhash, aggregate_pubkey, members, height }
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

`feature_pos_distributed_committee.py` is a runnable reference for this whole
loop. A session is single-use and bound to its (signer, member set, message); abandoned
round-1 sessions expire (10 min) and are capped, so an incomplete round can
never grow node memory.

### Stake lifecycle

Register stake on-chain by paying to a staking script (`getstakescript`), whose
weight is its explicit policy-asset amount and whose CSV lock must elapse before
it can be spent (unbonding *is* that spend). The lock must meet the chain
minimum as a wall-clock duration; for a lock past the 16-bit height range
(e.g. the ~2-week checkpoint window) request a time-based script with
`getstakescript "<pubkey>" csv_seconds=<seconds>`. The registry is rebuilt from
the UTXO set at startup and mirrored on every reorg. Introspect with
`getstakerinfo` / `getposschedule`.

## 7. Long-range-attack defenses

Two complementary layers (see [`06-proof-of-stake.md`](06-proof-of-stake.md) §6, roadmap items 9–10):

- **Dynamic Bitcoin checkpoints.** Anyone commits a block hash into the parent
  chain (`getcheckpointpayload` → an OP_RETURN); once buried
  `-poscheckpointdepth` deep, nodes that already validated the block treat it as
  final and reject forks below it. `getcheckpointinfo` shows checkpoints, the
  finality point, and a `conflicts` alarm if this node ever passed a
  checkpointed height without the checkpointed block.
- **Configured static checkpoints.** For a node syncing from genesis, pin known
  heights up front so a long-range alternate history is refused before any block
  is downloaded:
  ```ini
  poscheckpoint=100000:<blockhash>
  poscheckpoint=200000:<blockhash>
  ```
  A block at a pinned height must carry the pinned hash; a peer feeding a bogus
  chain is rejected and disconnected. Publish these with each release.

## 8. Launch checklist: engineering vs governance

What this runbook configures (engineering) vs what a founding-operator group
must decide before launch (governance):

| Decision | Kind | Where |
|---|---|---|
| Which parent chain to anchor to | governance | §5 `mainchainrpc*` |
| Genesis staker set & weights | **governance** | §3 `staker=` (must be identical on all nodes) |
| Total SEQ supply & pre-mine distribution | **governance** | genesis issuance (no block subsidy, §3 `con_blocksubsidy=0`) |
| Minimum blocksigner stake | governance | §3 `posminstake` (whitepaper: 0.01% of supply) |
| Committee size / quorum | governance | §3 `poscommitteesize` |
| Unbonding period (CSV) | governance | §3 `posunbonding` |
| Slot interval | governance | §3 `posslotinterval` |
| Fee-acceptance thresholds | per-operator policy | §4 price server |
| Published checkpoints | governance (ongoing) | §7 `poscheckpoint` |
| RPC creds, ports, parent connection | per-operator (node-local) | §3/§5 |

Once the governance values are fixed and shared as the consensus config (§3),
every operator derives the same genesis and the network is live. The genesis
block hash printed at first start should match across all founding nodes — that
equality is the launch's go/no-go check.

## 9. Monitoring quick reference

| RPC | Tells you |
|---|---|
| `getanchorstatus` | current Bitcoin anchor + parent connection health |
| `getposschedule` | next slot's seed, leader schedule, committee, quorum |
| `getstakerinfo` | the active stake registry (config + UTXO layers) |
| `getcheckpointinfo` | checkpoints, finality height, conflict alarm, configured pins |
| `getfeeacceptancepolicy` | effective static + dynamic fee-asset acceptance |
