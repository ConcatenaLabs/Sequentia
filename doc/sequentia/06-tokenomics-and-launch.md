# Tokenomics, genesis & launch

SEQ is the staking asset and the only token native to Sequentia. Its supply is
fixed, minted once at genesis, and never inflated. This chapter describes the
supply and what it confers, how the genesis block is constructed to seed the
first staker, how a chain bootstraps from that seed with no operator-side staker
configuration, the bundled and custom chains, and which choices belong to launch
governance versus per-node engineering.

## SEQ supply

- **Fixed supply: 400,000,000 SEQ**, pre-mined and fully distributed at genesis.
  The money range matches: `MAX_MONEY = 400,000,000 × COIN` (4e16 atoms at 8
  decimals — the hard cap and per-output sanity bound, well below int64's
  ceiling). `MAX_MONEY` is **per chain** (`src/consensus/amount.h`, set in each
  `CChainParams` constructor): the Sequentia chains (`sequentia`, `test`) use
  4e16; the inherited Bitcoin/Elements chains keep Bitcoin's 2.1e15.
- **No inflation, no block subsidy.** `con_blocksubsidy = 0` on the Sequentia
  chains (`genesis_subsidy` in `src/chainparams.cpp`), so block production mints
  no new SEQ. Block producers are paid **only in transaction fees** (see
  [`02-open-fee-market.md`](02-open-fee-market.md)).
- SEQ's only privileged role is **staking**: it is the asset stake weight is
  denominated in (staking outputs are policy-asset outputs; see
  [`04-proof-of-stake.md`](04-proof-of-stake.md)). For **fees** SEQ is just
  another asset: it is accepted 1:1 only as the default an unconfigured producer
  uses, and a producer may re-price it (any rate), refuse it (rate 0), or peg a
  different asset as the 1:1 reference (e.g. USDT). See
  [`02-open-fee-market.md`](02-open-fee-market.md).

## Minimum stake

The **minimum blocksigner stake is 0.01% of supply = 40,000 SEQ**
(`posminstake = 4000000000000` atoms). A staking output below this threshold
confers no eligibility: only outputs holding at least the minimum register a
participant in the stake set. See [`04-proof-of-stake.md`](04-proof-of-stake.md)
for how the threshold gates sortition eligibility.

## Genesis construction

The genesis block distributes the entire supply and seeds the first staker. It
is the one block that needs no staker and no prior history: **height 0 is exempt
from PoS validation** — `CheckPosStakeRules` returns early when
`pindexPrev == nullptr`, and `ConnectBlock` adds genesis outputs without
validating them (`connect_genesis_outputs = true` places them in the UTXO set).
So genesis is accepted without a leader, committee, or VRF proof; the chain
creator simply *defines* it, and need not be a staker, because there are none
yet.

Genesis carries two kinds of output:

- A **CSV-locked staking output** that seeds the founder's bootstrap stake. It
  uses the standard staking script from `BuildStakeScript`
  (`<csv> CSV DROP <pubkey> CHECKSIG`), of the policy asset, with a relative
  timelock at least the unbonding requirement. Because `RebuildUtxoStake`
  counts staking outputs at *all* heights including genesis, block 1 sees a
  non-empty stake registry and the founder is a registered staker — so block 1
  can be certified. The bundled chains use a **time-based CSV lock** of ~15 days
  (`2532 × 512 s`).
- A **plain (P2WPKH) output** holding the spendable remainder of the supply, the
  founder's to distribute to the first users.

On the bundled chains a fixed genesis bakes both: 1,000,000 SEQ in the CSV-locked
seed staking output + 399,000,000 SEQ in the plain founder output =
**400,000,000 SEQ** total. The only thing that varies between bundled chains is
the founder key and the address format.

A real launch regenerates this genesis with a **fresh, secret founder key** and
the desired distribution, producing a new genesis hash. The founder key shipped
on `-chain=sequentia` is a **placeholder**: it is published only so the mainnet
*config* is runnable before launch, and it controls the entire 400M supply, so it
must be replaced. A node refuses to start on `-chain=sequentia` with the
placeholder genesis unless `-allowplaceholdergenesis` is set (see
`src/init.cpp`). Only the genesis block changes at launch — the founder key and
optionally the distribution layout; every consensus rule and parameter (PoS, the
100-member committee, the 40,000-SEQ min stake, 30s slots, ~15-day unbonding, the
block weight, anchoring, `MAX_MONEY`) is already the real value and stays
identical.

## The genesis-seeded bootstrap

A Sequentia chain needs **no `-staker` configuration**. The staker set is
entirely on-chain, derived from staking outputs by the `StakeRegistry`
(`src/pos.h`), which merges two layers and sorts on their sum:

- `m_config` — the `-staker pubkey:weight` layer, a static all-nodes-identical
  set. It is still supported but **optional**.
- `m_utxo` — derived from on-chain staking outputs. `RebuildUtxoStake`
  (`src/pos.cpp`) re-derives this layer from the **entire UTXO set, including
  genesis (height-0) outputs**, at every node start, while the incremental
  `PosApplyBlockStake` / `PosRevertBlockStake` mirror connects and disconnects at
  height > 0. Because the UTXO layer is always re-derived at init and genesis is
  only ever *spent* incrementally (never re-added), fresh, IBD, and restarted
  nodes converge on the identical registry. A genesis staking output alone is
  therefore enough to bootstrap a chain with an empty config layer.

The launch sequence runs through three regimes:

1. **Empty → founder-only.** Genesis distributes the 400M supply and seeds the
   founder's CSV-locked staking output. As of block 1 the founder is the rank-0
   leader and the entire size-1 committee.
2. **Slow start under escaping-stall.** A full quorum is impossible with one
   staker, so each early block is certified under the **escaping-stall rule**
   (sub-threshold certification, down to a single signer, permitted when the
   Bitcoin anchor advances by +3) — roughly one block per ~3 Bitcoin blocks
   (~30 min). The slow cadence is harmless during setup. See
   [`04-proof-of-stake.md`](04-proof-of-stake.md) for escaping-stall.
3. **Full speed.** The founder spends the plain portion of the supply to the
   first users, who lock their own staking outputs. Each on-chain staking output
   enters `m_utxo`, so the eligible set grows. Once a committee quorum of
   sortition-eligible stakers participates, normal full-committee certification
   takes over and the chain runs at its nominal 30s cadence.

The bootstrap tooling that drives a fresh network through this growth — seeding
the founder, distributing to and registering the first committee members — lives
in `contrib/sequentia/bootstrap-committee.py`; see
[`05-operating-sequentia.md`](05-operating-sequentia.md).

## The bundled and custom chains

Both built-in Sequentia chains run Proof-of-Stake by default
(`g_con_pos`, `g_pos_vrf`, `g_pos_agg_committee` are true) and bootstrap from a
genesis-seeded staking output with no `-staker` layer. Dynamic federation is
never active on either.

- **`-chain=sequentia`** is the **real Sequentia network**: its own dedicated
  chain id, mainnet address format, and distinct network magic. Its genesis
  founder key is the placeholder that must be replaced at the launch ceremony
  (above). (`-chain=main` is deliberately left as Bitcoin-Elements — the
  inherited unit-test harness default and a parent-chain interop target — so
  Sequentia gets its own slot rather than repurposing `main`.)
- **`-chain=test`** is the **public testnet**: identical consensus rules, testnet
  address format, and a published founder key anyone can use to run and
  experiment. Its founder key is a **throwaway** whose private key is published on
  purpose (testnet only); the network is never "launched" or replaced.

**Custom chains** assemble the same mechanism from config:
`-con_genesis_stake=<pubkeyhex>:<amount_atoms>:<csv>` places a genesis staking
output, and `-initialfreecoins` provides the spendable remainder. This is the
path exercised by the functional test `feature_pos_genesis_bootstrap.py`: a
custom PoS chain with no `-staker`, a genesis-seeded founder who produces the
first blocks via escaping-stall, distributes coins, and is joined by new on-chain
stakers until a committee forms.

The signed-block **"anyone-signs" dev path** lives only on the custom/regtest
chains: start with `-con_pos=0 -signblockscript=51` (the defaults for custom
chains via `CCustomParams`; `elementsregtest` is one example custom-chain name). This is the dev/test harness — trivially runnable, no staker
setup, no Bitcoin parent required — and it is **not** Sequentia consensus. See
[`04-proof-of-stake.md`](04-proof-of-stake.md).

## Governance versus engineering

A launch-governance decision is one all nodes on a network must agree on for the
chain to exist and stay in consensus: it is fixed at genesis or pinned in the
shared consensus config. A per-operator setting is node-local and may differ
between honest operators without forking the chain (see
[`05-operating-sequentia.md`](05-operating-sequentia.md)).

| Decision | Belongs to | Where set |
|---|---|---|
| Total supply (400,000,000 SEQ) | Launch governance | Genesis issuance; `MAX_MONEY` in `src/consensus/amount.h` |
| Genesis staker seed & SEQ distribution | Launch governance | Genesis outputs (`-con_genesis_stake`, `-initialfreecoins`); no `-staker` needed |
| No inflation / no block subsidy | Launch governance | `con_blocksubsidy = 0`, fixed in code |
| Minimum stake (0.01% = 40,000 SEQ) | Launch governance | Hardcoded `g_pos_min_stake` in `CSequentiaParams`; `-posminstake` on custom chains |
| Committee size (100) | Launch governance | Consensus config |
| Unbonding period (~15 days) | Launch governance | Staking-output CSV requirement |
| Slot interval (~30s) | Launch governance | Hardcoded `g_pos_slot_interval = 30` in `CSequentiaParams`; `-posslotinterval` on custom chains |
| Published Bitcoin checkpoints | Launch governance | Consensus config; see [`04-proof-of-stake.md`](04-proof-of-stake.md) |
| Which parent chain to anchor to | Launch governance | Consensus config; see [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) |
| Bitcoin-RPC endpoint & credentials | Per operator | Node config |
| Accepted fee assets & exchange rates | Per operator | Node config; see [`02-open-fee-market.md`](02-open-fee-market.md) |
| Producer/staker keys & wallet | Per operator | Node config; see [`05-operating-sequentia.md`](05-operating-sequentia.md) |
| Peers, ports, data directory, logging | Per operator | Node config |

For the architectural substrate these choices sit on, see
[`01-architecture.md`](01-architecture.md); for the security model and the
pre-mainnet audit, see [`07-security-and-audit.md`](07-security-and-audit.md);
for the system overview and chapter map, see [`00-overview.md`](00-overview.md).
