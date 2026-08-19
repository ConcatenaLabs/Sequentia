# Proportional payouts: a claimable pool pot

A third payout mode, beside `direct` and `lottery`: a pool commits to paying its
delegators **in proportion to what each lent**, rather than by a weighted draw.

This reverses the 2026-07-09 decision to ship the lottery alone. The reason to
revisit it is that the lottery buys exact proportionality *in expectation* at the
cost of paying one delegator per block, which reads as "I lent my stake and got
nothing for six weeks" to everyone who is not that delegator.

## What the numbers force

Measured over 400 consecutive blocks on the live testnet (heights ~100,100 to
~100,500):

| | |
|---|---|
| blocks paying any reward at all | **12 of 400** |
| median reward on a paying block | 745 atoms (0.00000745 SEQ) |
| a 10% delegator's share of that | 74 atoms |
| a 1% delegator's share | 7 atoms |
| a 0.1% delegator's share | 0 atoms |

There is no subsidy, so a block with no transactions pays nothing, and most
blocks have no transactions. Three consequences, and they decide the design:

1. **A per-block split divides zero 97% of the time.** Whatever is built must
   accumulate across blocks before paying anything.
2. **Most per-block shares are below the dust floor.** Paying them out as
   outputs is not merely wasteful, it is impossible: the network will not relay
   them and they would bloat the UTXO set with unspendable change.
3. **One output per delegator per fee asset does not fit.** Fees are payable in
   any accepted asset, so a per-block split is `delegators x assets` outputs
   every block. At any real pool size that is most of the block.

So the mode has to accumulate, and pay out in chunks that clear dust. That is
what "claim-based accrual" means here.

## The mechanism

**Accrual is a UTXO, not a balance.** Under `split`, the coinbase of every block
the pool produces must pay its reward into the pool's **pot**: a bare output

```
<"SEQPOT"> OP_DROP <signer> OP_CHECKSIG
```

one per fee asset. The pot is not spendable by the signer despite the
`OP_CHECKSIG`: consensus additionally requires that any transaction spending a
pot output be a **valid claim** (below). The signature keeps a stranger from
grinding claims at the pool's expense; the consensus rule keeps the operator
from taking the pot.

Accrual therefore needs **no new consensus state**. The amount owed to the pool's
delegators is exactly the value sitting in its pot outputs, which is a pure
function of the UTXO set, like every other layer of the stake registry, and is
reorg-safe for the same reason.

**A claim distributes the whole pot at once.** A claim transaction spends one
pot output and must pay every eligible delegator their exact proportional share
of it. Anyone may broadcast it, not only the operator or a delegator: the
distribution is fully determined by the chain, so there is nothing to trust the
broadcaster with, and permissionless claiming means nobody's payout depends on
the operator staying interested.

**Eligibility is weight that has been lent long enough.** A delegator counts in a
claim if its delegation record has been unspent for at least `SPLIT_MIN_AGE`
blocks at the claiming height. Without that, anyone could delegate a large stake
one block before a claim, take a proportional share of fees earned over weeks
they had no part in, and leave. The age is checkable from the UTXO set (a coin
carries the height it was created at), so this too needs no stored state.

**Shares below dust roll over.** A delegator whose share of this pot would not
clear the dust floor is paid nothing and keeps its claim on the next one: the
remainder stays in a fresh pot output. This is what makes the mode work for small
delegators at all -- they accumulate across claims until they are worth paying,
instead of being rounded to zero every block.

**Commission** is the operator's share in basis points, exactly as in `lottery`,
paid to a script the operator commits to in the same policy record.

## What consensus must enforce

On a block whose leader has a `split` policy in force:

- every coinbase output carrying reward pays a pot output for its asset;
- the pot script names the leader.

On a transaction spending a pot output:

- it spends pot outputs only, and pays out in one pass: no partial claims, so
  there is no "who has already claimed" to remember;
- the payee set is exactly the eligible delegators at this height, each paid
  `floor(pot x weight / total_weight)` of the asset, plus the operator's
  commission to its committed script;
- anything undistributable (sub-dust shares, rounding remainder) goes back into
  a fresh pot output for the same signer and asset;
- at most one claim per pot per `SPLIT_CLAIM_INTERVAL` blocks, so a griefer
  cannot burn the pot down in fees by claiming it every block.

Every one of those is a function of the spending transaction, the UTXO set and
the current height. Nothing is carried between blocks.

## Why not the alternatives

**Per-block split.** Ruled out by the numbers above.

**Balance accounting (the Cardano shape).** Consensus tracks `owed[delegator]`,
incremented every block, decremented on withdrawal. It is the obvious design and
it is what "claim-based accrual" usually means, but it introduces consensus state
that is *not* derivable from the UTXO set: it must be persisted, updated on
connect, reversed exactly on disconnect, and rebuilt on reindex. Every other
layer of this chain's stake machinery is a pure function of the UTXO set, and
that property is the reason reorg handling has needed no special care. The pot
buys the same behaviour without giving it up.

**Paying delegators directly from the coinbase over time.** A coinbase cannot pay
a delegator who is owed less than dust, so the pool would have to remember the
remainder, which is balance accounting again.

## Open decisions

1. **`SPLIT_CLAIM_INTERVAL`.** How often a pot may be claimed. Too short and the
   claim's own fee eats the payout; too long and delegators wait. A first
   estimate: the interval where a claim's fee is under 1% of a median pot.
2. **`SPLIT_MIN_AGE`.** How long weight must have been lent to count. Long enough
   that joining to farm an imminent claim does not pay, short enough that a new
   delegator is not waiting weeks.
3. **Who pays the claim's fee, and is there a keeper's cut?** Permissionless
   claiming only happens if somebody is motivated to do it. A small cut of the
   pot, or nothing and we accept that the operator claims.
4. **Multi-asset pots.** One pot per asset is simple but a pool accepting five
   fee assets accumulates five pots, each claimed separately. Acceptable, or
   should the mode restrict payouts to the policy asset?

## Activation

A new payout mode is a **relaxation**: a `split` record is meaningless to an
older node, which would accept a coinbase the new rule rejects. That makes it a
hard fork for the running chain and therefore a flag day, gated on height and
genesis hash like `simplicity_budget4_height`, per CONTRIBUTING.md. It bumps the
version in the same pull request.
