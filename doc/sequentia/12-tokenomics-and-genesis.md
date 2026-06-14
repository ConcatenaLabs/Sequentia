# SEQ tokenomics & genesis launch

How the SEQ token and the chain's bootstrap map onto the implementation. The
*amounts* and *recipients* are launch decisions (set when the founding genesis
block is created); the *mechanisms* below are implemented and in place.

## SEQ supply

- **Fixed supply: 400,000,000 SEQ**, pre-mined and fully distributed at genesis.
  The money range matches: `MAX_MONEY = 400,000,000 × COIN` (4e16 atoms at 8
  decimals — the hard cap and per-output sanity bound). `MAX_MONEY` is **per
  chain** (`src/consensus/amount.h`, set in each `CChainParams` constructor): the
  Sequentia chains (`sequentia`, `test`) use 4e16; the inherited Bitcoin/Elements
  chains (incl. `main`, the test harness) keep Bitcoin's 2.1e15.
- **No inflation / no block subsidy.** `genesis_subsidy = 0` on the Sequentia
  chain (`src/chainparams.cpp`), so block production mints no new SEQ; block
  producers are paid only in transaction fees (whitepaper §3.9, doc 06 §5).
- SEQ is the chain's policy/reference asset: it is always payable for fees 1:1
  (`ExchangeRateMap::Convert*`, doc 02), and it is the asset stake weight is
  denominated in (staking outputs are policy-asset outputs, doc 06 §5).
- The **minimum blocksigner stake is 0.01% of supply = 40,000 SEQ**
  (`-posminstake = 4000000000000` atoms; whitepaper §3.3, doc 06 §5).

## Genesis bootstrap (the "special" first block)

The genesis block is the one block that needs no staker and no prior history —
exactly the special case the whitepaper describes. This is already how the node
works:

- The launching operator creates the genesis block with the SEQ pre-mine as
  genesis outputs, **including a CSV-locked staking output** that seeds the
  founder's bootstrap stake (`connect_genesis_outputs = true` adds them to the
  UTXO set). On the bundled chain this is baked in; on custom chains it is
  `-con_genesis_stake=<pubkey>:<atoms>:<csv>` plus `-initialfreecoins` for the
  spendable remainder.
- **Height 0 is exempt from PoS validation**: `CheckPosStakeRules` returns early
  for genesis and `ConnectBlock` adds genesis outputs without validating them, so
  genesis is accepted without a leader, committee, or VRF proof.
- **The genesis staking output makes the founder a registered staker**:
  `RebuildUtxoStake` counts staking outputs at *all* heights including genesis,
  so block 1 sees a non-empty stake registry — **no `-staker` config needed**.
- From **block 1** onward, normal PoS applies. The sole founder bootstraps slowly
  via the escaping-stall rule (one block per ~3 Bitcoin blocks) until recipients
  of the pre-mined SEQ create their own on-chain staking outputs and a committee
  forms. Full launch model in [doc 13](13-launch-and-bootstrap.md).

So the launch sequence is: (1) agree the consensus config (doc 09 §3) and the
genesis SEQ distribution incl. the founder seed stake; (2) each operator derives
the identical genesis block — the matching genesis hash across nodes is the
go/no-go check (doc 09 §8); (3) block production begins under PoS.

## What is a launch decision vs. fixed in code

| Item | Where |
|---|---|
| Total supply (400M) | genesis issuance amount (launch) |
| SEQ distribution among founders | genesis outputs (launch / governance) |
| Founding staker (bootstrap) | genesis-seeded staking output (`-con_genesis_stake`; no `-staker` needed) |
| Money range (`MAX_MONEY` = 400M) | fixed in code (`src/consensus/amount.h`) |
| No inflation | fixed in code (`genesis_subsidy = 0`) |
| Min stake (0.01% = 40,000 SEQ) | `-posminstake` (governance; whitepaper-pinned) |
| Block weight (200,000) | fixed on the Sequentia chain (`nMaxBlockWeight`; doc 11 §4) |
| Block cadence (~30s) | `-posslotinterval=30` (nominal block time; doc 11 §4) |
