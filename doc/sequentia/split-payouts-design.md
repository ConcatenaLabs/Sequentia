# Split payouts: proportional pool rewards through a claimable pot

A third payout mode, beside `direct` and `lottery`: a pool commits to paying its
delegators **in proportion to what each lent**. This is the arrangement most
delegators expect, and it reverses the 2026-07-09 decision to ship the lottery
alone: the lottery buys exact proportionality *in expectation* at the cost of
paying one delegator per block, which reads as "I lent my stake and got nothing
for six weeks" to everyone who is not that delegator.

This document describes the mode **as built** (`pos.{h,cpp}`, enforced from
`Consensus::Params::split_payout_height`); the functional proof is
`test/functional/feature_pos_split.py` and the arithmetic is pinned by
`pos_split_shares` in `src/test/pos_tests.cpp`.

## What the numbers force

A per-block split fails across the whole activity spectrum, for different
reasons at each end:

1. **on a busy chain**, one output per delegator per fee asset *per block* does
   not fit in blocks — the split must batch regardless of how much there is to
   split;
2. **at any activity level**, a small delegator's share of a single block is
   dust: payouts must accumulate until they are worth making, and sub-dust
   shares must roll forward rather than round away;
3. **at the quiet end** — which is where the current testnet sits, having
   invited no outside participants yet (measured near height 100,100: 12 of 400
   blocks paid any reward, median 745 atoms; there is no subsidy, so an empty
   block pays nothing) — a per-block split mostly divides zero. Not the
   permanent state of the chain, but the worst case the mode must survive
   without producing garbage.

Accumulate-then-claim is the design that is correct at both ends and everywhere
between.

## The mechanism

**The pot.** Under a `split` policy, `PosRequiredCoinbaseScript` directs every
fee-bearing coinbase output of the pool's blocks to the pot script:

```
<"SEQPOT"> OP_DROP <signer> OP_DROP OP_TRUE
```

one output per fee asset, exactly as the coinbase already accumulates them. The
pot is **anyone-can-spend at the script layer on purpose**: the consensus
overlay below is the entire spend condition, and it fully determines the
outputs, so the only thing "anyone" can do with a pot is distribute it correctly
and keep the bounded margin. A signature here would make claiming
operator-permissioned — the opposite of the point, which is that nobody's payout
depends on the operator staying interested.

**Commission** reuses the lottery's own mechanism: a `commission_bp`/10000
chance, drawn from the payout seed (from the hardening fork, fixed by the anchor
of the block three below; `PosPayoutSeedForChild`), that the block pays the
leader instead of the pot. Exact in expectation, it keeps the
coinbase a single required script, and it needs no claim-time policy lookup —
which closes a rug: commission taken at claim time under "the policy in force
now" would let an operator raise it against rewards already earned.

**A claim distributes the pot.** Any transaction spending a pot output must be a
valid claim (`CheckPosPotClaim`), enforced at `ConnectBlock` and, because the
script is anyone-can-spend, **also at mempool acceptance** — without that,
anyone could park an invalid pot spend in the mempool and every producer would
mine a doomed block. A claim:

- sweeps pot outputs of **one signer** (any subset; several assets at once);
- pays each eligible participant **exactly**
  `floor(distributable_i x weight / total_weight)` summed over the swept inputs,
  where `distributable_i` is the input's value minus its reserve (below);
- pays participants at **P2WPKH of their controller key**, which makes the whole
  transaction deterministic: anyone builds the same claim from the same UTXO
  set, so permissionless claiming is trustless rather than merely permitted;
- rolls shares below `POS_SPLIT_MIN_PAYOUT` (1,000 atoms — a consensus constant,
  not the node-configurable relay dust) into a fresh pot for the same signer,
  where they accumulate for the next claim instead of being rounded away;
- may withhold — network fee plus the claimer's margin, net of any coins the
  claimer brought — at most **1/99 of what it delivers to delegators**.

**The 1% reserve.** Each pot input reserves `1/POS_SPLIT_RESERVE_DENOM` (1%) of
itself; shares are computed over the other 99%. The reserve and the withhold cap
are two sides of the same 1%: the cap bounds what a claim may take, the reserve
is where it comes from, and whatever the fee does not use rolls back into the
pot. Without the reserve the shares would sum to the whole pot and no claim
could ever pay its own fee.

**Eligibility is per pot output, by creation height — no age constant.** A
participant counts toward a pot output only if its delegation record *and* its
stake outputs were created before that pot output was. Every quantity involved
is a creation height the UTXO set already carries, and exactness rests on UTXO
immutability: a record alive now with height `g` has stood, unchanged, since
`g`. Delegating one block before a claim therefore earns exactly nothing from
existing pots — the front-running attack is not mitigated but absent, with no
`MIN_AGE` constant to tune. (The registry tracks stake in height buckets and
delegation-record heights for this; both remain pure functions of the UTXO set.)

**Maturity.** Pot outputs are coinbase value and mature like any other coinbase
reward: a claim sweeps only the pots that are mature at the height the claim
confirms in, under the chain's coinbase maturity (`CoinbaseMaturityAt`: 1,000
blocks on the real chains, the inherited 100 on custom chains unless
`-con_coinbase_maturity` sets it). Younger pots are left for the next claim. A
previous claim's own re-pot output is ordinary transaction value and carries no
such delay.

## The one invariant that replaced three constants

There is **no claim interval, no keeper-cut constant, and no minimum-pot
constant**. The withhold cap does all three jobs: a grief-claimer must deliver
99x its own burn; the claimer's incentive is whatever the cap allows beyond the
fee (`claimpoolrewards` pays it to the claiming wallet); and a claim on a pot
too small to cover its fee under the cap is simply invalid, so claims happen
exactly when a pot is worth claiming — a threshold that scales with chain
traffic instead of being tuned to today's and wrong tomorrow.

Formulating the cap against **delivered** value (not swept) closes the skim: a
claim that rolls everything forward and pays nobody may withhold nothing.

## Sharp edges, stated rather than hidden

- **Leaving forfeits unclaimed accruals.** A delegator who withdraws its stake
  or re-points its record before a claim is no longer in the participant set and
  its accrued share falls to the others. Claim before you leave; the wallets say
  so.
- A block producer can snipe a broadcast claim and take the margin themselves —
  the pot script is anyone-can-spend, so the claim is not theirs to censor but
  the margin is theirs to take. Keeper rewards drifting to stakers is
  acceptable, and stated.
- Rolled crumbs re-dilute: a fresh pot output's participant set is judged at its
  own (new) creation height. The amounts involved are, by construction, crumbs.
- The signer's own stake participates like any delegator's (as in the lottery).
  One corner is approximate in its favour only: a signer that had delegated its
  own weight elsewhere at pot time and reclaimed it since is counted.

## Second generation: rounds, paid a bucket at a time

The first generation pays every delegator of the swept pots in one transaction.
A claim therefore grows with the pool, and past roughly 1,500 delegators no
block could hold one: the pot was locked, and anyone could lock a pool that way
with dust delegations (audit A11). The hardening fork capped the participants at
the 100 heaviest as a stopgap. From `pos_records_v2_height` the cap is gone.

**Rounds.** A pool's pots are grouped by **epoch** (`pos_split_epoch` blocks,
1,440 on the real chains, about a day) and asset. A claim turns the mature pots
of one (epoch, asset) into a **round**, an output

```
<"SEQRND"> OP_DROP <signer> OP_DROP <round data> OP_DROP OP_TRUE
```

whose data fixes, once and for all: the epoch, the total eligible weight `W`,
the round's value at creation `D`, a bucket count and a salt, followed by a
bitmap of paid buckets. Anyone-can-spend at the script layer, like the pot: the
claim rules are the whole spend condition.

**Eligibility is by epoch.** A round pays those who stood behind the pool when
its epoch began: delegation record and stake created before the epoch's first
block. Joining during an epoch earns nothing from it, from the next one in full.
The first generation judged each pot output by its own height; the epoch is
coarser by up to a day and keeps the same property, that arriving just before a
claim buys nothing.

**Buckets.** The participants are split into buckets of about 32 (a power of
two, at most 2,048) by a hash of the round's salt and each controller key. The
salt hashes the outpoints of the pots the round was made of, which did not
exist when the delegators' keys were fixed, so nobody can arrange to share a
bucket with a victim.

**A claim pays one bucket per round it touches**, any number of rounds and new
rounds at once. A round that keeps unpaid buckets continues in an output with
the same terms and exactly one bit added; one whose last bucket is paid closes,
and what it leaves (sub-minimum shares, the unused reserve, the shares of those
who left) goes back into a pot, which joins a later epoch. Each member of the
paid bucket gets exactly `floor(99% of D x weight / W)`, its weight as the
registry stands now: stake can only leave a round's participant set, never join
it, so the shares can never add up to more than the round holds.

**Fees.** Each round may give up at most 1/99 of what it delivers, and the claim
as a whole keeps the first generation's withhold cap. Over all its buckets a
round can therefore spend at most its 1% reserve.

**What it buys.** A delegator collects its own share with a transaction the size
of one bucket, about 32 outputs per round, whatever the pool's size;
`claimpoolrewards` does it for the wallet's own delegated key by default. Nobody
can block anyone else's payout: any bucket of any round can be paid at any time
by anyone. `listpools` lists each pool's open rounds.

**What it does not change.** First-generation pots (created below the height)
are claimed as before, with the 100-participant cap where it applies; a claim
may not mix the two. The coinbase still pays the pot.

## Activation

To a node without the mode, a split policy record is an **inert output** (its
`ParsePayoutScript` rejects the mode byte), so the moment a new node produces a
pot-paying coinbase, old nodes reject the block: a hard fork, therefore a flag
day — `split_payout_height = 102,150` on the live testnet (24.4.1; 24.4.0 said
106,500 before the operators were asked how much runway they wanted), genesis-pinned, and
active from genesis everywhere else (`-con_splitpayoutheight` on custom chains).

Below the flag a new node treats split records exactly as an old node does:
inert, unregistered, exempt from the record-creation rules — so the two cannot
diverge before the date. Recognition is keyed on the **record's creation
height**, not the current height, which keeps the registry a pure function of
the UTXO set: the same record is inert or recognised identically on every node,
at every rescan, forever. `announcepayout split` refuses to create a record
before the flag, naming the height.

Ships in 24.4.0; the version bump rides in the same pull request, per
CONTRIBUTING.md.
