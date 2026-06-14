# Launch & bootstrap — the genesis-seeded PoS chain

> **Status: implemented.** The bundled Sequentia chain is now a Proof-of-Stake
> chain *by default* (`CTestNetParams`: `g_con_pos/g_pos_vrf/g_pos_agg_committee
> = true`). It bootstraps from a **genesis-seeded staking output** with **no
> `-staker` config layer** — the staker set is entirely on-chain. The
> signed-block "anyone-signs" PoC path now lives only on the custom/regtest
> chains (`-con_pos=0`), i.e. the dev/test harness.

## 1. The launch sequence

The whitepaper's chain starts from an empty ledger and grows its staker set
on-chain. Concretely:

1. **Genesis (no staker required).** Genesis is exempt from PoS validation
   (`CheckPosStakeRules` returns early when `pindexPrev == nullptr`; `ConnectBlock`
   adds genesis outputs without validating them). So the chain creator simply
   *defines* the genesis — they need not be a staker, because there are none yet.
2. **Genesis distributes the 400,000,000 SEQ hard cap to the creator.** The
   genesis issuance creates the entire pre-mined supply (no inflation, §3.9) as
   outputs the founder controls.
3. **The creator is the sole genesis staker, and bootstraps slowly.** At least
   the minimum stake is placed, *in genesis*, into a **CSV-locked staking
   output** owned by the creator. This is what breaks the chicken-and-egg: block
   1 is validated against the stake registry *as of genesis*, so without a
   genesis staking output that registry would be empty and no leader could ever
   be elected. With it, the creator is rank-0 leader and the entire (size-1)
   committee. A full 51-of-100 quorum is impossible with one staker, so each
   early block is certified under the **escaping-stall rule** (sub-threshold,
   down to a single signer, permitted when the Bitcoin anchor advances +3) — one
   block per ~3 Bitcoin blocks (~30 min). A deliberately slow start, which does
   not matter in the setup phase.
4. **The set grows; the chain speeds up.** The creator spends the plain portion
   of the supply to the first users, who lock their own staking outputs. Each
   on-chain staking output enters the UTXO stake layer (below), so the eligible
   set grows. Once ≥ quorum sortition-eligible stakers participate, normal
   full-committee certification takes over and the chain runs at its 30s cadence.

## 2. Why no `-staker` config is needed

The `StakeRegistry` (`src/pos.h`) merges two layers and sorts on the **sum**:

- `m_config` — the `-staker pubkey:weight` layer (a static, all-nodes-identical
  set). Still supported, but **optional**.
- `m_utxo` — derived from on-chain **staking outputs** (`BuildStakeScript`:
  `<csv> CSV DROP <pubkey> CHECKSIG`, of the policy asset, with a relative
  timelock ≥ the unbonding requirement).

`RebuildUtxoStake` (`src/pos.cpp`) re-derives `m_utxo` from the **entire UTXO
set, including genesis (height-0) outputs**, at every node start; the incremental
`PosApplyBlockStake`/`PosRevertBlockStake` mirror connects/disconnects at height
> 0. Because the UTXO layer is always re-derived at init and genesis is only ever
*spent* incrementally (never re-added), fresh, IBD, and restarted nodes converge
on the identical registry. So a genesis staking output alone is enough to
bootstrap a chain with an empty config layer.

## 3. The bundled chain's placeholder genesis

`CTestNetParams` bakes a **placeholder** founder (a throwaway key — regenerate it
at the real launch ceremony):

| | |
|---|---|
| Founder pubkey | `028f88c9848c86c311934a5939ceb98408975055fc7ee6b40b479969665afe0e6b` |
| Founder key (testnet WIF) | `cURsyjY6KwZM9pBk7rfWwdDzYS1R4w85M2pPzh5RySfGpA8n9LB4` |
| Seed staking output | 1,000,000 SEQ, CSV time-lock ~15 days (`2532 × 512 s`) |
| Plain output (P2WPKH founder) | 399,000,000 SEQ |
| Total | **400,000,000 SEQ** |

The 400M cap required raising the inherited money range: `MAX_MONEY` is now
`400,000,000 × COIN` (4e16 atoms), the hard cap and sanity bound (well below
int64's ceiling; see `src/consensus/amount.h`). **This is a placeholder** — the
real mainnet genesis (founder key, exact distribution) is a launch-ceremony
artifact and replaces this byte-for-byte (a new genesis hash).

## 4. Custom chains: `-con_genesis_stake`

For testnets and the real launch (both custom chains) the same mechanism is
config-driven: `-con_genesis_stake=<pubkeyhex>:<amount_atoms>:<csv>` places a
genesis staking output, and `-initialfreecoins` provides the spendable remainder.
This is what the functional test `feature_pos_genesis_bootstrap.py` exercises:
a custom PoS chain with **no `-staker`**, a genesis-seeded founder who produces
the first blocks via escaping-stall, distributes coins, and is joined by new
on-chain stakers until a committee forms.

## 5. Running the signed-block dev/PoC chain

The trivially-runnable "anyone-signs" chain (no staker setup, no Bitcoin parent
required) is now the **custom/regtest** path: start with `-con_pos=0
-signblockscript=51` (the framework default for `elementsregtest`). This is the
dev/test harness; it is *not* Sequentia consensus. See doc 04.
