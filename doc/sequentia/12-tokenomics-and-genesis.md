# SEQ tokenomics & genesis launch

How the SEQ token and the chain's bootstrap map onto the implementation. The
*amounts* and *recipients* are launch decisions (set when the founding genesis
block is created); the *mechanisms* below are implemented and in place.

## SEQ supply

- **Fixed supply: 400,000,000 SEQ**, pre-mined and fully distributed at genesis.
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
  genesis outputs (`-initialfreecoins`, or a genesis issuance;
  `connect_genesis_outputs = true` adds them to the UTXO set).
- **Height 0 is exempt from PoS validation**: `CheckPosStakeRules` and the
  stake-registry rebuild both skip height 0 (`src/validation.cpp`,
  `RebuildUtxoStake`), so genesis is accepted without a leader, committee, or
  VRF proof — there are no stakers or parent yet.
- From **block 1** onward, normal PoS applies. The founding staker set is the
  `-staker=<pubkey>:<weight>` configuration shared by the launch operators
  (their weights are the pre-mined SEQ they hold); on-chain staking outputs add
  to it thereafter (doc 06 §5). The staker set and weights are a governance
  decision, defined per the whitepaper.

So the launch sequence is: (1) agree the consensus config (doc 09 §3) including
the founding `-staker` set and the genesis SEQ distribution; (2) each operator
derives the identical genesis block — the matching genesis hash across nodes is
the go/no-go check (doc 09 §8); (3) block production begins under PoS.

## What is a launch decision vs. fixed in code

| Item | Where |
|---|---|
| Total supply (400M) | genesis issuance amount (launch) |
| SEQ distribution among founders | genesis outputs (launch / governance) |
| Founding staker set & weights | `-staker` config (launch / governance, identical on all nodes) |
| No inflation | fixed in code (`genesis_subsidy = 0`) |
| Min stake (0.01% = 40,000 SEQ) | `-posminstake` (governance; whitepaper-pinned) |
| Block weight (400,000) | fixed on the Sequentia chain (`nMaxBlockWeight`; doc 11 §4) |
