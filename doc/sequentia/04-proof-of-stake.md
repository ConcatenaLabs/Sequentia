# Proof-of-Stake consensus

Sequentia's consensus is Proof-of-Stake. Block production is a stake-weighted
election with private VRF sortition; a committee certifies each block with a
single aggregated signature (BLS12-381 by default; MuSig2 the `-posbls=0`
fallback); and the certified block is final the moment
it is accepted, subject only to a Bitcoin reorg of its anchor. SEQ is the staking asset and the one thing that confers
production eligibility. Voluntary Bitcoin checkpoints resist long-range attacks.

This chapter is the full consensus specification: the stake registry, leader
election and VRF sortition, committee certification and aggregation, liveness,
fork choice and the immediate-finality gate, anchor freshness, long-range
defenses, and the production layer.

## 1. Overview

The consensus is a BFT protocol in the shape of the Sequentia whitepaper:
committee certification with strict-majority countersignatures, private VRF
cryptographic sortition for leader election, and Bitcoin checkpoints against
long-range attacks. Committee *membership* comes in two regimes (§4): private
threshold VRF sortition (up to 100 members, the paper's 51-of-100), or the
**public fixed-size committee** (`-pospubliccommittee`) whose membership is a
deterministic public schedule prefix with the quorum derived from the actual
committee size - the regime the public testnet has run since the 2026-07-05
re-genesis (cap 250, quorum 126). It is built on the *signed-block*
substrate Sequentia inherits from Elements - Elements replaces Bitcoin's
proof-of-work with a per-block signature carried in the header, and Sequentia
drives *who* must sign each block from the stake-weighted election rather than a
fixed federation. See [`01-architecture.md`](01-architecture.md) for the
signed-block machinery (`consensus.signblockscript`, the `CProof` /
`m_signblock_witness` solution plumbing, and the `CheckChallenge` /
`CheckProof` validation entry points) that this layer attaches to.

PoS changes exactly one thing about signed blocks: the block *challenge* is
**computed per block** from a stake-weighted, anchor-seeded election instead of
being inherited as a fixed federation script. The signature itself rides the
existing solution plumbing untouched. Validation therefore splits across two
stages, because the stake registry mirrors the *active tip's* UTXO set while
headers and blocks may be accepted far ahead of it or on another branch:

| Stage | When | What it checks |
|---|---|---|
| `CheckChallenge` | header time | the challenge's *form* only - a recognized PoS leader/committee challenge |
| `ContextualCheckBlockHeader` | header time | the **minimum block spacing** - registry-independent, so it binds a header before its block is fetched |
| `CheckPosStakeRules` | `ConnectBlock` (registry = parent state) | the election: leader is a registered staker, the VRF proof and slot, the committee, and the quorum |
| `CheckProof` | block-connect | the block signature satisfies the challenge - the block really is signed by the leader (and the aggregate by the committee) |

Everything registry-dependent waits for connect time, where the registry equals
the block's parent state, so headers-first sync and parallel block download can
never mis-evaluate eligibility.

The chain is enabled with `-con_pos`. The defining flags layered on it are
`-posvrf` (private sortition), `-posaggcommittee` (single-signature committee
aggregation; `-posbls` selects the scheme), `-pospubliccommittee` (the public
fixed-size committee; requires `-posbls`), `-poscommitteesize`,
`-posslotinterval`, `-posunbonding`,
`-posminstake`, `-poscheckpoint`, and `-poscheckpointdepth`. The bundled
Sequentia chains enable VRF and BLS committee aggregation by default;
`-posbls`, `-pospubliccommittee`, and `-poscommitteesize` are network-wide
consensus rules, so every node on a given network must agree on their values.
The mainnet parameters pin them, together with the other `-pos*` consensus
flags, and a node that passes any of them refuses to start; the testnet
defaults to the public committee at a cap of 250 and refuses a value that
conflicts with its own.

## 2. The stake registry

The stake registry is the map `{staker pubkey → stake weight}` from which every
election is computed (`src/pos.{h,cpp}`, `StakeRegistry`). On the bundled chains
it is built entirely from on-chain stake.

### The on-chain UTXO layer

Stake is registered by holding SEQ in a **staking output** - the bare script

```
<csv> OP_CHECKSEQUENCEVERIFY OP_DROP <pubkey> OP_CHECKSIG
```

holding an explicit policy-asset (SEQ) amount. The script has an optional
extended form carrying the staker's **committee BLS registration**, needed to
join the public fixed-size committee (§4):

```
<csv> OP_CHECKSEQUENCEVERIFY OP_DROP <blspubkey(48)> OP_DROP <pop(96)> OP_DROP <pubkey> OP_CHECKSIG
```

where `blspubkey` is the 48-byte BLS public key the staker will sign committee
certificates with and `pop` its 96-byte proof-of-possession (closing the
rogue-key attack). Both are derived deterministically from the staking key
(`getblsregistration`), so a staker manages one key;
`getstakescript <pubkey> null <csv_seconds> <blspubkey> <pop>` builds the
extended script (`ParseStakeScriptFull`, `src/pos.cpp`). While such an output is unspent,
its amount adds to its key's weight, indefinitely: the lock gates *withdrawal*,
not participation, so a staking output keeps its weight for as long as it is
unspent - once the lock matures it still stakes until actually spent, and it
never needs renewing. Holding ordinary (non-staking-output) SEQ confers no
weight; staking is opt-in. The `OP_CHECKSEQUENCEVERIFY` lock - the
whitepaper's stake locktime - is enforced by the script itself.

**Two-step unbonding** (`Consensus::Params::pos_unbond_height`; the mainnet and
custom chains from their first block, the testnet from height 159,000;
`-posunbondheight` moves it on a custom chain, and 0 turns it off). Because the lock runs from the
output's creation and the weight lasts until the spend, a stake older than the
lock could otherwise sign at one height and leave at the next, its keys worth
nothing to their owner long before a Bitcoin checkpoint secured what they
signed. Where the rule is active, a staking output may only be spent into
another staking output or into an **unbonding output** of the same key,
`<"SEQUNBOND"> OP_DROP <pubkey> OP_CHECKSIG`, which carries no weight: the stake
stops counting as soon as it is created. An unbonding output may be spent only
once the parent chain has advanced `pos_unbond_anchor_depth` blocks (2,016, the
checkpoint depth; `-posunbonddepth` on custom chains) past the anchor of the
block that created it - counted in Bitcoin blocks, like the checkpoint, so the
two cannot drift apart if the Sequentia cadence changes; on a chain without
anchoring, in Sequentia blocks. The only value that may leave a stake on the way
out is the network fee paid in SEQ, up to 1% of the staking outputs the
transaction spends (`CheckPosUnbondingTx`, reject reasons `bad-unbond-required`
and `bad-unbond-premature`). A fee paid in another asset, from other inputs, is
not taken from the stake and so needs no allowance: the whole stake goes to the
unbonding output.

The layer is a pure function of the UTXO set: it is rebuilt from the UTXO set at
node startup and mirrored exactly on every tip connect and disconnect, so it is
reorg-safe. Confidential outputs cannot carry weight, because their amounts are
hidden. The staked asset is always SEQ (`::policyAsset`), which `StakeFromTxOut`
requires; this staking-weight role is SEQ's *only* privileged status - for fees
SEQ is just another asset (see [`02-open-fee-market.md`](02-open-fee-market.md)).

On-chain staking outputs are **standard relay and mempool outputs**. When PoS is
enabled the staking script is recognized as standard, so a stake-registration
transaction relays across the network and is mined under default policy like any
ordinary payment - no special relay configuration is required.

The CSV lock is compared as a wall-clock duration (`PosStakeLockSeconds`
against `-posunbonding × posslotinterval`), so it may be height-based *or*
time-based (BIP68 512-second units). Time-based encoding is what lets the lock
exceed the 16-bit height-CSV range, which is required to lock stake longer than
the ~2-week Bitcoin checkpoint window at fast slot intervals.
`getstakescript ... csv_seconds=<n>` builds a time-based staking script. The
stake lifecycle is described operationally in
[`05-operating-sequentia.md`](05-operating-sequentia.md).

### The minimum-stake floor

`-posminstake` (atoms) is the floor a key must meet to be an eligible
blocksigner. Sub-floor stake is dropped from the leader schedule, from VRF
committee membership, and from the eligible-total sortition denominator - a
single chokepoint (`PosIsEligibleStake`). The whitepaper sets the floor at
0.01% of supply (40,000 SEQ); the bundled chains set it accordingly. The floor
defaults to 0 so it never silently breaks small-weight test chains. It is
enforced at connect time (`bad-posvrf-leader-below-min`) and at the producer
RPCs.

### The delegation layer: weight is keyed by SIGNER, not owner

The registry the elections actually read is keyed by **signer**. A staker (the
**controller**) may lend its weight to another key by holding an unspent
**delegation record**, a third bare script:

```
<"SEQDEL"> OP_DROP <signer> OP_DROP <controller> OP_CHECKSIG
```

`GetWeight` and `Weights` re-key the merged weight onto signers, so leader
election, VRF sortition and the eligibility floor all see what a *signer*
commands rather than what an owner holds. Like the weight and BLS layers, the
delegation map is a pure function of the UTXO set (`m_deleg_utxo`), so it is
reorg-safe by construction and rebuilt from scratch at startup.

Three properties follow from the record being a **separate output** rather than
a field inside the staking script, and they are the whole design:

- **The signer can never spend the stake.** Only the controller's key appears in
  the staking output's `OP_CHECKSIG`, and the record does not change that.
  Delegation is non-custodial in the strict sense: no coin moves to delegate, no
  coin moves to stop, and the key that *can* move the coins never has to be
  online. This is also what removes the hot-key hazard from ordinary staking,
  since a staker may delegate to its own hot key and keep the cold one offline.
- **It survives a vesting lock.** A staking output carrying a multi-year
  `liquid_locktime` cannot be spent at all, so a signer named inside it could
  never be replaced: one compromised operator key would be irrevocable for the
  whole lock. Re-pointing spends only the record.
- **Leaving is unilateral.** Spending the record needs the controller's
  signature and nothing else. There is no unbonding delay on it, no cooperation
  and no notice.

Two rules keep the map well-defined:

- **Resolution is one hop, never chained.** If A delegates to B and B delegates
  to C, A's weight counts for B and B's own weight counts for C. Chasing chains
  would admit cycles.
- **Only the controller creates a record** (from `pos_hardening_height`). A
  record took effect merely by existing, so anyone could create one naming any
  controller for a dust output and take its whole weight (audit A1). A record
  is now valid only in a transaction that spends a coin only the controller's
  key can spend: a P2PK/P2PKH/P2WPKH output of the key, its staking or
  unbonding output, or its previous record (which every rotation spends).
  `delegatestake` arranges it by paying a small amount to the key's P2WPKH and
  spending it in the record's transaction (`authorization_txid`). A record
  naming the controller as its own signer is refused, and so is a record spent
  and re-created byte for byte in one block (`bad-record-recreated`). The same
  authorisation applies to payout records and their signer
  (`bad-payout-unauthorized`): without it anyone could have announced a DIRECT
  policy paying themselves from any producer's blocks.
- **At most one unspent record per controller**, enforced in `ConnectBlock`
  (`bad-delegation-conflict`, `bad-delegation-exists`). Two live records would
  make the resolved signer depend on iteration order, and so on the node. This
  is why re-pointing must spend the old record and create the new one in the
  *same* transaction: as two loose transactions they could be mined in the order
  that produces the second record first, invalidating that block.

The apply/revert path has one ordering hazard worth naming. `PosApplyBlockStake`
adds created outputs *before* subtracting spent ones, so a rotation inside one
transaction would have its new record erased by the removal of the old.
`SubUtxoDelegation` therefore erases only if the record being removed is still
the one in force, and `PosRevertBlockStake` is the exact inverse: restore spent
records first, then remove created ones.

### Payout policies, and why the notice period exists

Delegation moves the *right to produce*, and blocks pay their fees to whoever
produced them. So a pool is trusted for exactly one thing: the reward. An
operator may commit to that on-chain with a payout record, spendable by the
signer:

```
<"SEQPAY"> OP_DROP <activation> OP_DROP <mode> OP_DROP <param> OP_DROP <signer> OP_CHECKSIG
```

`PosRequiredCoinbaseScript(leader, height, seed)` is the single seam: the
producer builds its coinbase from it and `ConnectBlock` enforces the same
function. Three modes ship:

- **DIRECT** - the coinbase must pay a committed scriptPubKey. This prevents a
  *silent* redirect; it does not enforce fairness, since an operator may commit
  to its own address. Trust-minimised, not trustless.
- **LOTTERY** - the coinbase must pay ONE participant drawn weighted by stake.
  Below `pos_hardening_height` the draw used the block's election seed, fixed
  by the parent's anchor, so the parent's producer could compute the draw for
  each Bitcoin block it might anchor to and pick the one paying itself (audit
  T2-PAY-1). From that height the draw's seed is fixed by the anchor of the
  block three below and the height (`PosPayoutSeedForChild`): whoever chose
  that anchor could not know who would lead the block it decides. Each delegator earns its exact proportional
  share over time with zero accounting. It does **not** smooth variance: 1% of a
  pool is 100% of one block in a hundred. Of the two reasons pools exist, this
  addresses participation below the floor but not variance reduction. Operator
  commission is in basis points; at 0 bp the operator still earns as one
  participant among the rest.
- **SPLIT** - the coinbase must pay an on-chain pot, and anyone may broadcast
  the claim (`claimpoolrewards`) that distributes it: each delegator is paid
  its exact proportional share of every pot output it was eligible for, at its
  own controller key. A pot created from `pos_hardening_height` is shared among
  the `POS_SPLIT_MAX_PARTICIPANTS` (100) largest participants only, in
  proportion among themselves: the claim pays every participant in one
  transaction, and without a bound a pool of ~1,500 delegators had a pot no
  block could hold (audit A11). From `pos_records_v2_height` that cap is
  replaced by rounds: an epoch's pots are paid a bucket of about 32 delegators
  at a time, any bucket by anyone, so a pool may have any number of
  delegators and each can collect its own share. The claim is fully determined by the UTXO set, so no
  payout depends on the operator staying interested, and variance is smoothed
  without any per-delegator accounting. Commission is a bp/10000 chance that a
  block pays the operator instead of the pot. Leaving the pool forfeits
  unclaimed accruals, so claim before you leave. Full mechanism:
  [`split-payouts-design.md`](split-payouts-design.md). Fresh chains carry it
  from genesis; on the testnet it is in force from height 102,150
  (`split_payout_height`).

A producer that has committed nothing keeps everything, which is the default
rather than an abuse.

Consensus requires `activation >= announce_height + g_pos_payout_notice` (1440
blocks, ~1 day at the 60-s spacing, pinned on the bundled chains;
`-pospayoutnotice`, default 2880, on custom chains only). Announcing does not
cancel an earlier policy: the policy in force at height h is the announced policy
with the greatest activation `<= h`, so a pending change and the current rule
coexist until the switch.

Ending a policy takes the same notice. From `pos_hardening_height` the record in
force cannot be spent (`bad-payout-in-force`): without it the coinbase would pay
the signer from the next block, with no notice at all. To stop sharing, an
operator announces a direct payout to itself and waits out the notice; once that
binds, the superseded record can be reclaimed. A record still inside its notice
binds nobody and may be withdrawn at any time.

**The notice period is the only reason a delegator can act at all, and it is
worth exactly what the delegator notices.** Since exit is instant and unilateral,
the entire protection is "see the change, then leave" - which makes surfacing it
a consensus-adjacent requirement rather than a nicety. `listdelegations` reports
every announced-but-not-yet-binding policy against the caller's own stake with
the blocks remaining; `listpools` does the same for the whole network, and the
public board renders it above everything else.

**Not built:** per-delegator epoch reward accounting (the Cardano approach).
SPLIT gives smoothed *and* trustless payouts without it, from a pot the UTXO
set fully determines, so that subsystem is not needed for what it would buy.

### The config layer (custom chains only)

A chain may also be configured with a `-staker=<pubkeyhex>:<weight>` layer
(repeatable), a stake set fixed in configuration rather than on-chain. This
exists only on custom chains. The bundled Sequentia chains are **on-chain-only**:
they carry no `-staker` entries and derive all weight from the UTXO layer. The
genesis-seeded launch bootstraps from a genesis staking output, not from
`-staker` config - see [`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md).

## 3. Leader election & VRF sortition

### The election seed

The per-slot seed is **anchor-derived and deterministic**:

```
seed_h = ComputePosSeed( parent block's Bitcoin-anchor hash, height h )
```

The seed is built from the parent's committed Bitcoin-anchor hash and the
height - both header fields fixed at block-index creation, so the seed is
identical on every node. It is deliberately *not* the Sequentia block hash (which a
producer could grind) and *not* a VRF score. The anchor hash is Bitcoin's
proof-of-work, which a Sequentia producer cannot bias; its only freedom is which
recent, monotone, anchor-valid Bitcoin block to reference, and that choice sets
the seed of the *next* block. See
[`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) for the anchor commitment.

What that freedom is worth depends on the committee regime (§4). Under threshold
VRF sortition the producer cannot see the committee a seed would give, because
membership is each staker's private VRF output, so the choice is blind. Under
the public fixed-size committee the committee is a public function of the seed
and the registry: a producer that leads a block can compute the next committee
for each of the `k` anchors open to it and take the one that seats most of its
own coalition. The gain is bounded. It is the best of a handful of choices, it
is available only on blocks the coalition leads, and nothing carries over from
one block to the next, since each committee is seeded by an independent Bitcoin
block. At a cap of 250 against a coalition holding one third of the stake, the
chance of the coalition holding a quorum of a given committee is about 1.8e-8
with no choice of anchor and about 3.6e-8 with four (`committee-seed-grind.py`,
beside this file, computes the table).

Two alternatives do not remove it. Seeding from an anchor further back only
hands the same choice to an earlier producer; the capture probability has no
term for the depth. A seed chained from the participants' own VRF outputs would
replace an external beacon that no Sequentia actor can influence with an
internal one that a participant can bias by withholding. If the residual ever
needs to shrink, the lever is the committee cap.

### Private VRF sortition

A verifiable random function makes the schedule unpredictable to everyone but
the winner. For a secret key `sk` (public `Y = sk·G`) and input `alpha`, the
holder of `sk` computes a 32-byte pseudorandom output `beta` and a proof `pi`
such that anyone with `(Y, alpha, pi)` can verify `beta` is the unique correct
output - but nobody without `sk` can predict it. A staker learns *privately*
whether it won a slot and publishes the proof only when it produces a block.

The primitive is **ECVRF-SECP256K1-SHA256-TAI** (`src/vrf.{h,cpp}`), structured
per RFC 9381 over secp256k1: encode-to-curve by try-and-increment, a public-key-
bound challenge truncated to 16 bytes, the RFC proof encoding (`Gamma‖c‖s`,
81 bytes) and proof-to-hash, with the experimental suite octet `0xFF`. Because
secp256k1 is not an RFC-registered ciphersuite there are no official test
vectors; the construction is pinned by golden known-answer vectors in
`vrf_tests.cpp`. The node exposes `vrfprove` and `vrfverify`.

### From VRF output to slot and committee

For the slot seed, a staker of weight `w` (total eligible weight `W`) computes
`beta = VRF(sk, seed)` and a stake-weighted slot:

```
q    = beta / w                      # 256-bit
slot = ⌊ top64(q) · W / 2^64 ⌋       # in [0, W); lower beta / higher w ⇒ lower slot
```

`PosVrfSlot` is locally checkable from the single published proof - a validator
does not need every staker's `beta`. Eligibility then follows directly:

- `slot < committee_size` ⇒ the staker is a **committee member**. Since
  `P(slot < T) = T·w/W`, the expected committee size is exactly
  `committee_size`, weight-proportionally.
- The **lowest** slot is the **rank-0 leader**; higher slots are fallback
  leaders.

Because computing `beta` requires the staker's private key, the schedule is not
publicly predictable, which mitigates targeted DoS of upcoming leaders and
identity grinding. (Without `-posvrf` the base layer falls back to a *public*
deterministic ranking `H(seed‖pubkey)/weight`; the bundled chains run private
VRF.)

### Exponential-race sortition (the `pos_exprace_height` hard fork)

Legacy leader election was neither exactly stake-proportional nor split-proof.
The Sybil incentive did not come from the weighted slot arithmetic being
slightly off: ranking by the weighted quantity `U · W / w` itself is not
proportional either, but it *punishes* splitting rather than rewarding it. The
incentive came from the election being decided by two *different* keys.

**Which stakers reach the field was stake-weighted; which of them won was not.**
With `U = beta / 2^256`, the legacy slot is `⌊ U · W / w ⌋`, so a staker reaches
slot 0 only when `U < w/W`, and slot 1 only when `U < 2w/W`: the smaller the
stake, the lower the `beta` it must draw before it may offer a block early at
all. The producer's cadence floor then holds every block to at least
`pos_block_spacing` after its parent (`max(slot gate, spacing)`,
`src/pos_producer.cpp`; consensus enforces the floor and the slot gate
independently - see *Minimum block spacing* below), which
collapses slots 0 and 1 into a single offering time, so the proposals a
committee collects in one gossip window routinely include several candidates.
Among them `BackedForRound` ranked by the **raw, unweighted** `beta`. Entry was
priced in stake; the win was not.

Conditional on reaching that field at all, a small identity's `beta` is
therefore drawn from a lower band than a large one's, so it usually takes the
unweighted tiebreak, and the smaller the identity the more decisively.
Splitting sharpens exactly that. Take a 70% staker, whose slot is at most 1 on
every draw, so it is always in the field. A 30% staker held as one identity
reaches the field on 60% of rounds and beats the 70% staker on 70% of those:
about **42% of blocks, 1.4x its stake share**. Split into 15 identities of 2%
each, the group reaches the field *less* often, and by a wide margin: 15 draws
against a threshold 15 times smaller give entry `1 - (1 - 0.04)^15`, about 46%
of rounds against 60%, a 14-point loss. It buys that loss back and more on the
win rate. Each candidate it does field carries a `beta` from a band 15 times
narrower, so it beats the 70% staker on 98% of those rounds rather than 70%:
about **45% of blocks, 1.5x**. In a field of one 80% staker and four 5%
stakers, each 5% staker took about **8% of blocks, 1.6x**.

Netted out, the *split* delta is about three percentage points of block share:
14 points of field entry traded for a near-certain win whenever the group does
appear. The deviation from proportionality underneath it is larger still, four
times that delta, and both are defects. A stake-weighted election must not
reward fragmenting stake at all, because the whole point of weighting by stake
is that identities are free and weight is not; and 1.4x is not "approximately
proportional". (The in-tree evidence for the size of that edge is
the comment in `pos_vrf_exprace`, `src/test/pos_tests.cpp`, which records
the jump for a raw `beta`
ordering key. The precise share figures above came from re-running that model
with *both* halves taken from the legacy rule, which is not a configuration the
tree carries, so read them as the scale of the effect rather than as measured
constants.)

The **exponential race** removes the edge exactly. With `U = beta / 2^256`
uniform in `(0,1)`, a candidate is scored

```
score = -ln(U) · W / w      # PosVrfScoreExp, Q32 fixed-point; lowest wins
slot  = ⌊ score ⌋           # PosVrfSlotExp, capped at POS_VRF_MAX_SLOT
```

`-ln(U)` is Exponential(1), so `-ln(U)/w` is exponentially distributed with rate
`w`; the `W` factor only rescales every staker's score by the same constant,
into the slot units the time-gate already uses, and cannot change who is lowest.
Two standard facts about exponential races then do all the work:

- the minimum of independent exponentials is itself exponential with the
  **summed** rate, and the probability that staker `i` attains that minimum is
  exactly `w_i / W`; and
- `min(Exp(w/2), Exp(w/2))` is distributed *identically* to `Exp(w)`.

The first makes the election exactly stake-proportional: a staker holding 8% of
eligible weight produces the winning candidate 8% of the time, not approximately.
The second makes it **split-proof**: a stake broken into any number of
identities yields a best-of-N draw with exactly the same distribution as the
undivided stake's single draw, so splitting is neither rewarded nor punished.
Both statements are exact in real arithmetic. That exactness is a property of
the distribution, not of the code: the Q32 fixed-point evaluation the node
actually runs approximates `-ln(U)`, and an approximation can only be checked
statistically, never shown exact by simulation. The in-tree check is
`pos_vrf_exprace` (`src/test/pos_tests.cpp`), 30,000 election rounds driven
through the real fixed-point functions: block shares track stake shares, and a
30% stake wins the same share whole as it does split into 15 identities. What it
actually asserts is looser than that summary, and worth reading before leaning
on it: an 80% staker between 74% and 86%, a 5% staker between 3.5% and 6.5%, the
30% stake between 27% and 33% both whole and split, and those last two within 3
points of each other. So the run establishes that the fixed-point evaluation
does not move the outcome by more than those margins, not that it tracks the
ideal any more closely. `pos_vrf_exprace` is the evidence that runs on every
build, and the only evidence this chapter rests on.

The fork also closes the two-key gap that produced the legacy edge. Among
candidates carrying the same Bitcoin anchor height - the committee's primary
ordering key, untouched by the fork and covered in §7 - the candidate ordering
key is now `PosVrfScoreExp`, and the time-gate slot is `⌊ PosVrfScoreExp ⌋`, so
entering the field and winning it are decided by the *same* weighted quantity:
the globally lowest-scoring candidate necessarily holds the lowest slot, so it
offers earliest and wins the field it is in. The cadence floor may still merge
two offering times, and it no longer matters - among equally-anchored
candidates, merging cannot admit one that outranks the lowest score.

In plain terms: give every staker a timer that runs at a speed proportional to
its stake and fires at a random moment, and let the first to fire produce the
block. Splitting a stake buys two timers that each run at half speed, and
because an exponential race is memoryless, the earlier of two half-speed timers
is distributed exactly like one full-speed timer. Two half-speed timers are
worth exactly one full-speed timer, no more and no less, and since the timer
that fires first is also the one the committee backs, a splitter has nothing
left to harvest.

The score is computed in **Q32 fixed point with no floating point**, so every
node derives bit-identical values: `-ln(U)` is evaluated as
`(256 - log2 beta) · ln2`, with the fractional part of `log2 beta` obtained by
the classic bit-by-bit squaring of the mantissa (`src/pos.cpp`). Candidate
ordering uses the fine Q32 score, not the truncated integer slot, so two
candidates that share a slot still resolve deterministically. A zero `beta` or
zero weight yields the sentinel `PosExpScoreInf`, `(POS_VRF_MAX_SLOT + 1) << 32`
or 1,048,577 in slot units. That is above any score a staker of consequential
size can draw, but it is not above every representable one: `-ln(U)` is at most
`256 · ln 2 ≈ 177.4`, so a real score can exceed the sentinel only for a staker
holding less than roughly 1/5,900 of eligible weight, and then only on a `beta`
small enough that the draw never occurs in practice.

**Cadence is unchanged.** The leader is still the lowest-scoring staker and is
still held back by a time gate proportional to its slot (*Time-gating* below).
The two rules even agree closely on how often some staker
draws slot 0, the case consensus lets produce with no wait at all beyond its
parent: `1 - (1 - 1/n)^n` under the legacy rule against `1 - e^{-1}` under the
exponential race. The two agree to within a percentage point for any committee
of a realistic size and converge as `n` grows. The minimum block spacing (§4b)
holds either winner to at least `pos_block_spacing` after its parent in any
case. What the fork changes is *which* staker wins, not how fast blocks arrive.

**It is a height-gated, coordinated hard fork.** Because the two rules elect
different blocks from the same draws, they cannot be mixed on one network: a node
still on the legacy rule backs a different candidate, and rejects as premature
(`bad-posvrf-early`) any certified block whose exponential-race slot opened
earlier than the legacy slot would have. The switch is therefore gated on a single
predicate, `PosExpRaceActive(params, height)`, true when
`consensus.pos_exprace_height` is greater than zero and the block's own height
is at or above it. Every election site consults that one predicate (the connect-time
time-gate in `CheckPosStakeRules`, the producer's own slot computation, the
block-template path, and the committee's candidate ordering), so a node flips all
of them at exactly the same height and no node can be half-forked.

A height of `0` means **disabled**, not active from genesis: `PosExpRaceActive`
requires `pos_exprace_height > 0` before it compares heights (`src/pos.cpp`), so
a chain configured with `0` runs the legacy rule at every height. Mind the
asymmetry with the neighbouring `pos_coinbase_leader_height`, where `0` means
*from genesis*. Custom chains read the height from `-posexpraceheight` and
default to `0`, which is what keeps the fork off by default there; the bundled
Sequentia chains pin it in `src/chainparams.cpp`. Custom is the case that
matters for testing, because `elementsregtest` - the chain the functional test
framework runs by default - is one, which is why `feature_pos_exprace.py` can
drive the activation boundary at all. Plain `-chain=regtest` is **not**:
`CreateChainParams` maps it to `CRegTestParams`, which never reads the flag, so
`-chain=regtest -posexpraceheight=10` is silently ignored. There is nothing for
it to fork in any case, because that chain runs no PoS election at all:
`CRegTestParams` sets `g_con_pos = false` (`src/chainparams.cpp`).

The **public testnet** activated at height **44300**, agreed between the
operators on 2026-07-22 and shipped in release 23.3.7; testnet nodes had to be
running that binary before the chain reached it. It is the one chain where this
is a fork of *live* consensus: the testnet carries a legacy-election history
behind the activation height, and block 44299 is still elected by the legacy
rule. **Mainnet is set to `1`, so the exponential race governs every elected
block from the first one on** (height 0 is genesis, which no election governs).
It is `1` and not `0` precisely because `0` disables the rule here; mainnet is
not live, so this is a launch parameter rather than a fork of live consensus,
and the chain never has a legacy-election era to leave behind - it is carried as
a launch-governance decision in
[`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md). All three answers
the code pins (mainnet 1, testnet 44300, regtest 0) are guarded by the
`pos_exprace_activation_heights` regression test.

Committee *membership* under the threshold-VRF regime still thresholds on the
legacy slot
(`PosVrfIsCommitteeMember`): this is a leader-election change, and the public
fixed-size committee the testnet runs derives membership from the public schedule
in any case (§4). Covered by `pos_vrf_exprace` (unit: a 30,000-round election
simulation asserting both proportionality and split-neutrality against the real
fixed-point functions) and `feature_pos_exprace.py` (functional: the activation
boundary itself, with a peer validating every block across it).

**Candidate ranking still prefers the freshest anchor.** The fork replaced only
the *secondary* key of the committee's candidate ordering. `BackedForRound`
still ranks proposals by Bitcoin anchor height first, freshest winning, and the
exponential-race score decides only among candidates carrying the *identical*
anchor height, where the legacy rule used the raw, unweighted `beta` (§7) - the
second of the two keys that produced the Sybil edge above. No anchoring code
is touched. The same-height fork-choice tiebreak of §6 is also unchanged and
remains the top 64 bits of the leader's raw `beta`: that key must be computable
from the block body without consulting the stake registry, so it deliberately
does not use the weighted score.

### Time-gating

A block records the leader's VRF proof in a coinbase `OP_RETURN` (tagged
`SEQVRF`), covered by the merkle root and hence by the leader's signature. At
connect time `CheckPosStakeRules` verifies the proof against the leader's
challenge key over the slot seed, recomputes `slot` under the sortition rule in
force at the block's own height, and requires
`block.nTime ≥ parent.nTime + PosSlotGateSeconds(slot)` (`bad-posvrf-early`).
The gate is the slot multiplied by a unit: `pos_slot_gate_seconds`, 10 seconds
on both real chains, once the exponential race and the slot gate are active;
`-posslotinterval` (30 seconds) under the legacy rule and on a chain that sets
no gate unit. The unit is smaller than the slot interval because the
exponential-race score is a rate with an unbounded tail, not a bounded rank:
scaled by a whole interval, a long draw silences the chain for that many
intervals, where a smaller unit lets the block spacing absorb most of the tail.
`-posslotinterval` itself stays at 30, since it also scales the unbonding
requirement. So a slot-0 leader may produce earliest; if it is absent, a
higher-slot staker may step in after its slot opens. This is the whitepaper's local wall-clock
round timeout with the lowest-ranked participant as proposer - lowest
exponential-race score from `pos_exprace_height` onward, lowest legacy slot
below it.

## 4. Committee certification & aggregation

A block becomes final by carrying a committee certification: one aggregated
signature by a quorum of committee members, so block size stays (nearly)
constant in committee size. Two questions define the regime: *who is on the
committee*, and *how the certificate is encoded*.

### Committee membership: two regimes

**Threshold VRF sortition** (the base `-posvrf` + `-posaggcommittee`/`-posbls`
model, committee cap 100): the committee is the set of members whose private
VRF slot falls under the committee-size threshold (§3), and the **quorum is a
strict majority of the *expected* committee size** (`PosQuorum`, the paper's
51-of-100), independent of how many members the draw actually selected. The
eligible set is a random variable, which above a large staker pool admits a
failure mode: two disjoint quorums could certify rival same-height blocks.

**The public fixed-size committee** (`-pospubliccommittee`, requires
`-posbls`; **the regime the public testnet runs**) closes that by
construction. Committee membership is the first
`K = min(#BLS-registered stakers, -poscommitteesize)` entries of the
deterministic public schedule (`PosPublicCommittee`, the `PosSchedule` ranking
restricted to stakers with a registered BLS key), and the certification quorum
derives from that **actual** size: a strict majority, plus one at odd `K`
(`PosPublicQuorum`), so any two quorums overlap in at least two members at
every size and the eligible set can never exceed the cap. Membership is
public; **leader election stays private-VRF** (§3). The committee cap can be
raised to `MAX_POS_PUBLIC_COMMITTEE_SIZE` (1000); the testnet runs cap 250
(quorum 126, the classical 1/3-Byzantine bound with margin). Members register
the BLS key they sign with on-chain, inside their staking output (§2), or via
the `-staker=<pubkey>:<weight>:<blspubkey>:<pop>` config form on custom
chains; `getblsregistration` derives the registration from the staking key.

**Seats by stake (from `pos_hardening_height`; 163,000 on the testnet, block 1
on mainnet).** One place per staker, ordered by `H(seed, key)/stake`, capped a
large stake at a single seat and handed the remaining seats almost uniformly to
whoever held many identities, so a coalition split into minimum-stake
identities took far more seats than its stake (audit A13): the capture figures
below hold for equal stakes only. From the hardening height the committee is
`K = -poscommitteesize` **seats** apportioned by stake
(`PosPublicCommitteeSeats`): each eligible, BLS-registered staker has a quota
`K·w/W`, receives its whole part, and the seats left over go to the fractional
parts in one systematic draw from the seed, so expected seats equal the quota
exactly and nobody gets more than one seat beyond its whole part. Splitting
stake across identities gains nothing. A member signs once and counts its
seats; the quorum is `PosPublicQuorum(K)` seats, in effect a strict majority of
the stake behind the committee. The bitfield still names members, and the
certificate carries a fourth push, the seat total of its signers, so fork
choice and finality can weigh a header before its block connects; `ConnectBlock`
checks it (`bad-posbls-seats-mismatch`). With equal stakes and as many seats as
stakers, every staker holds one seat, exactly as before.

### BLS certification (the default, `-posbls`)

The block challenge commits to the leader only:

```
OP_2 <leader(33)>                   # the BLS aggregate-committee form
```

The `OP_2` is a version marker. The certificate lives in the block's proof
*solution*, which is excluded from the signed block hash - so the hash the
committee signs does not depend on who signs, which is what lets members
produce non-interactive signature shares independently and lets any node
aggregate a quorum of them (the gossip committee, §9). Two encodings:

- **Full-member form** (sortitioned committees): `<leader_sig> <agg_sig(96)>
  <member_1(258)> ... <member_m(258)>`, each member being
  `secp_pubkey(33) ‖ vrf_proof(81) ‖ bls_pubkey(48) ‖ bls_pop(96)` - the
  members prove their own sortition eligibility and BLS keys inside the
  certificate (`PosBlsCertificate`, `src/pos.h`).
- **Bitfield form** (public fixed-size committee): `<leader_sig> <agg_sig(96)>
  <bitfield(⌈K/8⌉)>`, bit `i` set meaning "committee member `i` signed", in
  the public schedule's order (`PosBlsBitfieldCert`). Because the member set
  and their registered BLS keys are already known from the registry, the
  ~257-byte-per-member certificate collapses to the leader signature, one
  96-byte aggregate, and one bit per seat - the certificate's size is
  effectively independent of committee size (an empty certified block on the
  live testnet is under 700 bytes).

Validation resolves the signers (from the embedded members, or from the
bitfield against the registry-derived public committee), rejects phantom or
duplicate signers, checks the quorum, and verifies the one aggregate signature
against all signer keys in a single batched pairing check
(`PosVerifyBitfieldCertificate` / the full-member equivalent, `src/pos.cpp`; the
BLS12-381 implementation is the vendored `blst`, `src/blst/`).

### MuSig2 certification (the legacy `-posbls=0` fallback)

The primitive is BIP327 over the vendored secp256k1 (`src/musig.{h,cpp}`): it
aggregates a signer set into one 32-byte x-only key (order-independent - the
aggregate depends only on the *set*) and produces one 64-byte signature. A
`q`-of-`m` quorum is realized by aggregating exactly the `q` signing members.
The block challenge commits to the leader plus the aggregate key:

```
OP_1 <leader(33)> <aggkey(32)>      # BuildPosAggChallenge
```

The coinbase carries each member's VRF eligibility commitment (tagged
`SEQCMT`, `pubkey‖proof`), so every validator independently re-verifies
eligibility and re-derives the aggregate: every named member distinct and
within the cap (`bad-posvrf-member-count`), every member sortition-selected
(`bad-posvrf-member-missing` / `-invalid` / `-not-selected`), at least
`PosQuorum(committee_size)` members named (`bad-posvrf-agg-quorum`), and
`MuSigAggregatePubkey(named set) == aggkey` (`bad-posvrf-agg-key`). The
solution is two pushes - the leader's ~73-byte DER signature and the 64-byte
BIP340 aggregate. MuSig2 signing is interactive (two rounds), which is why
this path needs the coordinator flow of §9.

### Sizes and CheckProof

`CheckProof` verifies the leader signature (ECDSA) and the aggregate (BLS or
Schnorr) directly rather than through the script interpreter, because
`OP_CHECKMULTISIG` cannot express one signature over an aggregate of up to 100
(or 250) keys. The per-chain `max_block_signature_size` is sized for the
certification in force: **200** bytes for the MuSig2 path and **32,000** bytes
(`300 × MAX_POS_AGG_COMMITTEE_SIZE(=100) + 2000`, sized for the full-member
form) when `-posbls` is enabled.

(For small custom committees a script form also exists:
`<leader> OP_CHECKSIGVERIFY <q> <c_1..c_n> <n> OP_CHECKMULTISIG`, capped at 16
members because each member is a separate pubkey and signature push. The
aggregate forms above are the paper-scale path.)

## 4b. Minimum block spacing

The chain's cadence is a **consensus rule**, not a convention of the producer
software:

```
block.nTime >= parent.nTime + pos_block_spacing        # bad-pos-spacing
```

`Consensus::Params::pos_block_spacing` is 60 seconds on both real chains, bound
by `pos_block_spacing_height`, and checked in `ContextualCheckBlockHeader`
beside `time-too-old`.

**Why it is not the slot gate.** The gate delays a leader in proportion to its
`slot`, and `slot` scales with the reciprocal of the staker's weight,
so it can never be a uniform speed limit: the winning draw floors to 0 in
`1 - e^-1 ≈ 63%` of rounds, which is exactly when a brake would be needed. Left
to the gate alone, a set holding all the stake can drive the chain to a 17.5 s
cadence, and to 0.6 s if the gate is rescaled to one second per score unit.
Producers are paid in the fees of the blocks they lead and in nothing else -
there is no block subsidy (§3.9) - so the incentive to shorten the cadence is
permanent and grows with congestion, and more blocks in the same time is more
disk, more bandwidth and more validation for every node for ever.

The two parameters therefore stay separate: `pos_block_spacing` answers *how
fast may the chain run*, the slot gate answers *in what order may leaders
propose*. Folding them into one number is what makes every value of that number
a bad trade.

**Why a rule about timestamps is enough.** A validator cannot observe when a
block really appeared, only the time written in it, so written times are the
only thing a consensus rule can bind - and binding them suffices. Producing `N`
blocks costs `N · spacing` seconds of timestamp, and `MAX_FUTURE_BLOCK_TIME`
caps how far ahead of real time timestamps may run, so the long-run rate is
exactly one block per `spacing`, after a one-off burst of at most
`MAX_FUTURE_BLOCK_TIME / spacing` (120 blocks at the current values) that buys
nothing, because the chain then produces nothing until real time catches up.

Note what the check compares: two timestamps **written in blocks**. The
validating node's own clock is deliberately absent, so the verdict is identical
on every node regardless of when either block arrived or how far any local clock
has drifted. The only clock-dependent timestamp rule is the inherited
`time-too-new`, which stays soft - a node retries once its clock catches up -
precisely because it is clock-dependent.

**The producer-side clamp.** `node::PosEarliestBlockTime` holds every stamp to
`parent.nTime + spacing`, whatever the local clock says, and it follows the
spacing **value** rather than its activation height, so a release stops emitting
too-close blocks *before* the rule starts rejecting them. That ordering is not
optional on a running chain. A scan of the first 86,357 testnet blocks found
2,186 closer than 30 s to their parent - 2,183 at exactly 29 s, still occurring
at the tip. They are not cheating: the producer waits correctly and then the
assembler stamps `GetAdjustedTime()`, so a node whose clock trails the previous
producer's by a second writes `parent+29` having genuinely waited `parent+30`.
Activating the rule without the clamp would invalidate blocks honest producers
emit right now, over and over.

**Block weight follows the cadence.** The cap encodes weight *per second*, held
equal to a saturated Bitcoin: `400,000 / 60 s == 4,000,000 / 600 s` (§3.10).
Changing one without the other silently changes how fast the chain grows.

## 5. Liveness - escaping-stall

Normal operation requires a quorum, which fails a young or stalled chain. The
escaping-stall rule restores liveness: a block may be certified **below quorum,
down to a single signer**, but only when the Bitcoin anchor has advanced at
least `POS_ESCAPING_STALL_ANCHOR_GAP` (3) past the parent block's anchor.

The condition is the pure, deterministic function
`PosEscapingStallAllowed(parent_anchor_height, block_anchor_height)`
(`src/pos.h`), computed only from Sequentia-committed anchor heights - no live parent
query enters the validity rule, so every node agrees. When it holds,
`CheckPosStakeRules` relaxes the named-member quorum to a single member; the
block stays a valid aggregate-committee block (every named member
sortition-eligible, `aggkey` equal to the aggregate of the named set) - only the
*count* relaxes. Otherwise the full strict-majority quorum is required.

The path is self-limiting and abuse-proof: a `+3` anchor gap requires Bitcoin to
have genuinely produced three blocks (~30 minutes), which a healthy
60-second chain never permits, and each further sub-threshold block needs
another `+3` of parent-chain progress. It is what lets a young chain (or a
temporarily under-quorum committee) make progress, and it is what the
genesis-seeded launch uses for its slow start - see
[`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md). Tested in
`pos_escaping_stall_gap` (unit) and `feature_pos_escaping_stall.py`.

## 6. Fork choice & immediate finality

### The same-height comparator

Signed blocks all have equal nominal "work" (height), so same-height candidates
are ordered by a PoS-specific comparator in `CBlockIndexWorkComparator`
(`src/validation.cpp`), using keys set on `CBlockIndex` at acceptance, and then
the block hash:

1. a **certified block wins** over an uncertified one - `m_pos_certified`,
   whether the block's certificate names at least the certification quorum
   (`PosSlotQuorum`) of the stake state its **parent** leaves, the quorum
   `ConnectBlock` verifies the certificate against, so a full-threshold block
   always beats an escaping-stall or leader-only one;
2. between two certified or two uncertified blocks, the **lower leader VRF
   score** wins - `m_pos_vrf_score`, the top 64 bits of the leader's `beta`
   over the slot seed (registry-independent, hence deterministic across
   nodes);
3. on an equal score, the **lower block hash** wins, in `uint256` order: the
   hash's bytes compared as stored, which is the displayed hex read two
   digits at a time from the right. Unlike first-seen order, which two honest
   nodes can observe differently, the hash is the same everywhere, so every
   node breaks the tie the same way.

The number of members a certificate names (`m_pos_countersigs`, reported as
`poscountersigs`) is **not** a key. In the BLS certificate forms the
certificate is outside the block hash and any node holding a quorum of shares
can assemble one, so one block reaches different nodes with certificates of
different sizes, and each node keeps the count of the first it received. Ranked
by that count, two nodes holding the same two certified siblings could order
them oppositely and finalize different blocks.

Whether a block that cannot escape a stall is certified is the same on every
node, because it is judged against its parent's stake state and never against
the observing node's tip.
On the public committee the quorum follows the number of eligible stakers, so a
block that registers or drops a committee key changes the quorum for the blocks
after it, and a node whose tip is that block would hold a higher quorum than a
node on its sibling; measured against each node's own tip, the sibling would be
certified on one node and not on the other, and the two would finalize
different blocks. Siblings share a parent, and with it the quorum. Every valid
certificate of a block that cannot escape a stall carries its parent's quorum,
so for such a block the headers give the answer. Only a block that may escape
a stall, valid with fewer members, has its count judged, against the quorum its
parent leaves: a node knows that quorum once it has connected or disconnected
the parent, and always when the block itself connects. A node that does not yet
know it treats the block as certified for fork choice, learns the answer by
connecting it, and does not count it towards finality until then. A judged
answer is kept in the block's index entry (`BLOCK_POS_CERT_DECIDED`), so a
restart, which knows the stake state of the tip alone, does not lose it.
For a block that may escape a stall the count is that of the certificate the
node holds, which lies outside the block hash, so two nodes holding different
certificates for it can judge it differently. `getblockheader` reports the
answer as `poscertified`, which reads `false` while the node does not yet know
it; `poscertifiedknown` says whether it knows, so a caller can tell a block that
is not certified from one whose answer is still to come and wait on the latter.
The parent's quorum is `posquorum`, present once the node knows it. Tested in
`feature_pos_certified_parent_quorum.py` (siblings of which one changes the
committee) and `feature_pos_certified_stall_quorum.py` (the same on the
anchored committee, for blocks escaping a stall, across a restart).

The VRF score and the count are computed from the block in
`SetPosForkChoiceKeys` before the block enters the candidate set, and persisted
in `CDiskBlockIndex`; the certified answer is derived again for every block at
startup (`PosRefreshCertifiedKeys`). A changed answer is written only with the
block out of every candidate set, so the set ordering stays consistent.
**Anchor freshness is deliberately not a fork-choice key** (§7 explains why,
and how freshness is delivered instead).

### The immediate-finality gate

A hard finality gate makes a quorum-certified block final once it has stood on
the active chain for the **finality observation window** (`-posfinalitydelayms`,
default 3 s) with no unresolved competing certificate at its height (below).
`UpdateTip`, and a scheduler pass every 250 ms, track the highest active-chain
quorum-certified block that qualifies; `ContextualCheckBlockHeader` and the
activation-time gate reject any block that would fork at or below it. So a certified block is locked
against every Sequentia-internal competitor - *including one whose certificate names more
members* - and is never reorged to chase a fresher anchor. The VRF/committee
result is the ultimate truth.

Once final, a block stays final, and so does every ancestor of it: no pass opens
its window again, however long the chain grows behind it on escaping-stall
blocks. A window opens when a quorum block joins the active chain while the node
is past its initial block download; a block that was connected during that
download counts as observed. A pass looks only at the blocks connected since the
previous one and those still in their window, never at a whole stall stretch or,
on a chain with nothing to finalize, the whole chain (`-debug=bench` reports any
pass that examines more than a hundred blocks).

The finalized point is written with the block index and restored exactly at
startup (`PosRestoreImmediateFinality`), before the node connects to any peer,
so a restart neither moves it nor finalizes the tip merely because it is the
tip. A quorum block above the restored point that was still in its window when
the node stopped may have a better-ranked sibling the node has not seen yet, so
its window runs again from startup: the node then ends on the same block as one
that never stopped. A node with nothing written, because it never finalized a
block or because it was upgraded from a release that did not write the point,
holds nothing final for that first window and then finalizes its highest quorum
block. Tested in `feature_pos_finality_restart.py`; `feature_pos_finality_kept.py`
restarts a node holding a finalized block that Bitcoin later takes away.

The rejection is the soft, non-banning `BLOCK_RECENT_CONSENSUS_CHANGE`, because
the one legitimate exception is a **Bitcoin reorg** of a finalized block's
anchor: the anchor watcher invalidates the affected block on its own path (not
the accept-time gate), which lowers the finalized point via `UpdateTip`, after
which the Bitcoin-consistent chain is accepted. Bitcoin stays the security root -
Sequentia finality is immediate *modulo* a Bitcoin reorg. Tested in
`feature_pos_finality.py` (a competitor whose certificate names more members
does not reorg a finalized block) and `feature_pos_fork_choice.py`.

### Equivocating members and the observation window

Two quorums of a majority-quorum committee overlap in at least two members
(§4), so a double certification needs two members that sign both blocks - and
nothing more, if the honest members happen to be split between the two. An
attacker can provoke that split by timing alone: it watches the signature
shares in gossip, releases a competing proposal as the collection window closes
so that some members back it and others have already signed, and signs both
blocks only when the division came out even. Were a block final the moment it
connected, each half of the network would keep the certificate it saw first,
the gate would refuse the other, and the split would be permanent.

Two valid certificates at one height are themselves proof of equivocation, so
the observation window settles them deterministically instead: while a quorum
block is inside its window, a competing quorum-certified sibling is judged by
the ordinary comparator (both are certified, so leader VRF score, then block
hash, whatever number of members either certificate names), and every node that
holds both converges on the same one. A certificate
is ~300 bytes and every node relays it at once, so the window only has to cover
its travel. The competing *block* can be large, full of transactions nobody has
seen, and slow to validate, and its content is the attacker's choice - so a node
that has verified a competing full-quorum certificate whose block it has not yet
received and judged keeps that height, and every block above it, undecided until
it has, for at most `-posfinalityholdms` (default 30 s). Every node verifies
`poscert` messages for this, with or without a producer, relays them and fetches
the block from the sender. `getposfinality` reports the finalized point and any
certificate holding the next one.

Both settings are node-local fork-choice timing, like the gate itself; no block
is valid or invalid because of them. A node so slow that the competing
certificate reaches it only after the window is in the position of a pinned
minority below, and rejoins through reconciliation. Tested in
`feature_pos_split_equivocation.py` (finality on connection splits the network
permanently; the window converges it) and `feature_pos_finality_hold.py` (a
competing certificate holds the height until its block is judged, or until the
hold expires).

The watcher is not the only release valve anymore. Two rival branches can both
end up quorum-certified with canonical anchors (a committee legitimately
re-certifies a replacement branch after a transient parent-chain flap, then the
parent converges so both branches' anchors are canonical - the 2026-07-17
finality partition,
[`incident-2026-07-17-finality-partition.md`](incident-2026-07-17-finality-partition.md)),
a tie anchoring cannot break and that would pin the minority node forever.
**Finality reconciliation** (`-posreconcile`, default on) is the second,
node-local valve: rival branches are stored (the gate's rejection moves from
accept time to activation time, `FindMostWorkChain`), and a monitor in the
watcher thread releases the finalized point only for a rival that carries a
full-quorum certificate strictly above the local finalized height (never a
same-height comparison), anchored at/below the currently uncontested parent
height, after the local branch has received no certified extension for
`-posreconcilepatience`. The local blocks are not invalidated - they become
valid-but-inactive history. Forging the release evidence requires a committee
quorum (a stake majority), so no new attacker class; the majority side never
releases, so convergence is one-way. Relatedly, the escaping-stall relaxation
now also requires one parent-chain block interval of *median-time-past*
between the sub-quorum block's anchor and its parent's
(`-posescapestallmtpgap`): an anchor *height* gap alone is met within seconds
during a difficulty-1 parent block-storm with the chain fully alive, which is
how the incident's rival branch was seeded.

Because the watcher is the gate's only *Bitcoin-side* release valve, the gate is
enforced only by nodes that run it - `-validateanchor`, the default on a
Bitcoin-anchored chain. A node configured `validateanchor=0` does not watch Bitcoin, so it could
never lower its finalized point when Bitcoin reorgs a finalized block's anchor;
enforcing the gate there would make it reject the canonical recovery chain
forever and stall. Such a follower instead uses plain most-work fork choice and
follows the Bitcoin reorg transitively through the peers it relies on for anchor
validation - *finality modulo Bitcoin requires watching Bitcoin*. (A non-anchored
PoS chain has no Bitcoin to be modulo of, so the gate is absolute there.) The
gate and the watcher must therefore be coupled: enforced iff
`!con_bitcoin_anchor || validateanchor`.

## 7. Anchor freshness for real-time swaps

Real-time cross-chain atomic swaps need the Sequentia tip to reference the
freshest Bitcoin block, so a swap's Sequentia leg confirms with
`anchor ≥ the Bitcoin leg's height` promptly - no extra reorg-protection
timelock (see the definition in
[`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md)). This freshness is
delivered by *production*, never by fork choice, because in an immediate-finality
system a fork-choice rule that could prevail over the VRF result would let a
newly-arrived Bitcoin block reorder an already-certified block.

Freshness is delivered at two safe layers:

1. **Leaders build on the freshest anchor.** `GetAnchorForNewBlock` anchors
   every new block to the freshest Bitcoin block, so the canonical tip tracks
   Bitcoin's tip within one block - by *extending* the chain, never reorging it.
   The one exception, a producer-side back-off while the parent chain has live
   competing branches at its tip, is in
   [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) §5.

2. **A committee signing preference.** When members face competing proposals at
   the same height, the autonomous gossip committee backs the one referencing the
   **freshest Bitcoin anchor**, falling back to the lowest leader election score
   among equally-fresh proposals (`BackedForRound` orders candidates by anchor
   height, then by the exponential-race score from `pos_exprace_height` onward
   and the raw leader VRF below it; `src/pos_producer.cpp`). This realises the
   paper's Principle 7 rule III (a weighting that favours the newest Bitcoin
   block) as a strict ordering: a staler proposal can never out-rank a fresher
   one, so producers are incentivised to anchor fresh - and in the common case
   where all proposals already carry the freshest anchor it reduces to the pure
   lowest-score election of §3. It is
   a **pre-certification** preference only: it selects which proposal the committee
   converges on, never reorders an already-certified block (the immediate-finality
   gate forbids that; §6), and it never lowers the committee quorum (126-of-250
   on the testnet) - so it is pure coordination and can never create two "final"
   blocks.

## 8. Long-range-attack defenses

Two checkpoint mechanisms, combined with the CSV stake locktimes of §2 (which
must exceed the checkpoint cadence), close the posterior-corruption window.

**Dynamic Bitcoin checkpoints.** Anyone may commit a Sequentia block hash into
the Bitcoin parent chain (a `SEQCKPT` OP_RETURN; `getcheckpointpayload`). Once
that commitment is buried `-poscheckpointdepth` deep, a node that has the block
on its active chain treats it as finalized and rejects forks below it
(`bad-fork-prior-to-pos-checkpoint`) - even longer, validly-signed branches.
Checkpoints only lock in history a node has *already validated* and never replace
it, so conflicting or bogus commitments are harmless; a node that passed a
checkpointed height *without* the checkpointed block raises a `conflicts` alarm
(`getcheckpointinfo`) rather than silently following a checkpoint it never
validated. Tested in `feature_pos_checkpoints.py`.

**Configured static checkpoints.** `-poscheckpoint=height:hash` (repeatable)
pins a height-to-hash mapping in configuration, known before any block is
downloaded. A block presented at the pinned height must carry the pinned hash,
otherwise it - and any branch built on it - is rejected in
`ContextualCheckBlockHeader` (`bad-pos-checkpoint`), so a node fed a bogus
long-range chain from genesis refuses it and disconnects the lying peer. This is
reject-only; it never makes a node seek a particular branch (surfaced in
`getcheckpointinfo`'s `configured` array; `feature_pos_config_checkpoints.py`).

## 9. The production layer

Two things must be kept distinct: block *validation* and block *production
coordination*.

**Block validation is fully decentralized and complete.** Every node
independently verifies the VRF proofs, committee eligibility (the `SEQCMT`
commitments), the aggregate signature, the leader signature, the Bitcoin anchor,
the checkpoints, and the immediate-finality gate. Nothing in validation needs a
coordinator; a certified block is accepted network-wide like any other.

**Block production has both a coordinator/RPC path and autonomous paths.** The
MuSig2 cryptographic protocol and the RPCs to assemble a block by hand exist in
full:

| RPC | Role |
|---|---|
| `getposschedule` | the slot's committee and quorum |
| `vrfprove` | a member proves its slot eligibility |
| `getposblocktemplate` | the leader assembles the unsigned block (its `SEQVRF` proof, each member's `SEQCMT` commitment, the aggregate challenge) and returns the `signhash` |
| `musignonce` | MuSig2 round 1 on a member's node (public nonce) |
| `musigpartialsign` | MuSig2 round 2 on a member's node (partial signature) |
| `musigaggregate` | combine partials into the 64-byte aggregate |
| `submitposblock` | the leader attaches its signature plus the aggregate and submits |
| `generateposblock` | single-host shortcut: one node holding all keys produces a block |

With this path, organizing the committee each slot is done by external tooling
(BIP327's secret nonce is deliberately non-serializable; each member's node keeps
the live secret nonce in an in-memory session store between the two rounds and
consumes it once; `feature_pos_distributed_committee.py` runs the full loop
across three separate nodes). Alongside it, a built-in producer thread and a BLS
gossip committee remove the coordinator entirely (next).

Three paths lead to a live network:

- **A coordinator-driven committee** works with the MuSig2 RPCs and suits a
  known founding committee: a coordinator orchestrates `getposblocktemplate`,
  the per-member `musignonce` / `musigpartialsign` round trips,
  `musigaggregate`, and `submitposblock`. This is semi-centralized at the
  *production* layer - the coordinator is a liveness and orchestration point -
  while validation stays fully trustless.

- **An autonomous single-node producer** (`-posproducer`): a node holding one or
  more staking keys elects itself each slot and produces blocks on its own -
  the leader-only and single-host-committee cases - with no coordinator and no
  RPC (`src/pos_producer.*`). It certifies alone only while no other host
  commands an eligible stake. A node that holds a quorum of seats by itself but
  shares the network with other stakers takes part in the gossip round below
  like any member, countersigning whichever proposal the election backs:
  otherwise its seats, being part of every quorum, would be missing from every
  other staker's block, and it would produce every block itself.

- **An autonomous gossip-and-sign committee** (`-posbls` + `-posproducer`): the
  full decentralization, fully implemented and the default on the bundled chains.
  Each node detects its own eligibility; the elected leader floods its unsigned
  block, every node signs the best-ranked proposal it has collected
  (`BackedForRound`: freshest anchor, then lowest election score) and floods a
  non-interactive
  BLS share, and any node that gathers a quorum aggregates it into the certificate - assembling
  a committee-certified block across separate hosts with no coordinator. This
  rests on BLS aggregation and the member-independent block hash (the certificate
  lives in the proof solution). Under `-pospubliccommittee` the same gossip
  rounds run with the public schedule-prefix committee and the leader packs the
  quorum into the compact bitfield certificate (§4) - this is how the public
  testnet has produced every block since the 2026-07-05 re-genesis. The producer
  thread, BLS aggregate certification,
  the gossip rounds (`posproposal` / `poscmpctprop` / `getposprop` /
  `posshare`), and the hardening features - anti-DoS validate-before-relay,
  equivocation evidence, crashed-member failover, large-committee tuning, and the
  anchor-freshness signing preference of §7 - are all implemented and tested. The
  design is in
  [`proposals/autonomous-committee.md`](proposals/autonomous-committee.md).

### The share-lock

In the gossip committee a member signs one block per height for as long as that
block can still be certified. Having share-signed block X at height H, a node
does not sign a same-height rival until all three of these hold: the round in
which it signed X has ended; it has asked its peers for a certificate on X
(`getposcert`); and one further round has passed with no certificate arriving.
A certificate is a self-verifying object of a few hundred bytes that travels on
its own (`poscert`), so if X was certified anywhere, proof of it is one
round-trip away, and the node adopts X instead of backing a rival.

The quorum arithmetic is what gives the rule its force. For X to be certifiable
at all, at least a quorum must have signed it. Those members are all locked,
fewer than a quorum remain free, and so no rival can gather a quorum while the
lock holds. If too few members signed X for it ever to certify, fewer than a
quorum are locked and the next leader certifies without waiting. The lock
therefore delays the chain by one round only when a block was on the verge of
certifying, and costs nothing otherwise.

A partition can outlast any number of rounds: a member cut off from the side
where X certified can ask its peers indefinitely and hear nothing. So the lock
has a second arm, keyed on the parent chain rather than the clock. A node does
not sign or lead at H+1 on a parent that is a same-height rival of the block it
signed until the escaping-stall evidence holds for that parent - the Bitcoin
anchor gap of §5 together with the median-time-past gap of §6, the same
evidence consensus accepts for a sub-quorum block - and it asks its peers for
X's certificate while it waits. The cut-off minority waits out the partition; the
majority side never waits.

The share-lock is local signing policy, not a validity rule: no block is valid
or invalid because of it. It turns the round timings into a liveness matter
only. A node whose clock or round length differs from its peers' contributes
late; it does not sign twice. What the lock cannot bind is a member that
deliberately signs two blocks at one height, and there is no slashing to deter
one. Under the public fixed-size committee any two quorums overlap in at least
two members (§4), so certifying two blocks at one height takes at least two such
members. The lock state lives in `PosProducer` (`m_lock_hash`,
`src/pos_producer.{h,cpp}`); the certificate query is exercised in
`feature_pos_cert_gossip.py`.

### Held transactions

A certified block is final against everything inside Sequentia and falls only
when Bitcoin takes its anchor away (§6). Its transactions then return to the
mempool and are mined again, unless whoever signed one of them spends the same
coin differently first. Without a further rule, one producer is enough for
that. After a reorganization one Bitcoin block deep, the respend has to win a
round of a committee that signs any valid block. Two or more blocks deep, the
first new block may escape a stall (§5), and a single signer certifies it. The
committee then builds on it. Held transactions make the respend need the rest
of the committee, within a bound that keeps the chain moving.

**The record.** When blocks leave the active chain, the node keeps every
transaction of every departed block at or below the highest block of its old
branch that carried its parent's quorum and left too: a block is as final as
the highest certified block above it. These are the *held* transactions. The node keeps each whole,
so that it can put it back in the mempool, together with the outpoints it
spends. Two kinds are left out because they can never be mined on the new
chain: a transaction that spends the coinbase of a disconnected block, and one
that spends the output of such a transaction.

The record is made where blocks leave, whatever takes them off: the anchor
watcher's invalidation, fork choice moving to a branch Bitcoin returned to, a
block or a request from a peer that wins fork choice first, the finality
reconciliation, an operator's `invalidateblock`. The disconnect keeps the block
it has already read, and the activation that disconnected it (each step of
`ActivateBestChain`, each iteration of `InvalidateBlock` and of the set-aside)
turns what left into the record before it releases the validation lock
(`cs_main`) and before it updates the mempool, on every exit, a failed one
included. So no other thread, no signer and no mempool ever sees a departure
the record does not hold yet. An activation that finds a departure left
unrecorded by an earlier one records it at once and logs "carried into another
activation". Only the held transactions are written to `heldtxs.dat`, outside
every lock, after the activation, except that a flush of the chainstate writes
the record first whenever the chainstate it persists has the disconnect in it.
A node killed at any point therefore restarts either with the departed blocks
still connected, to leave again and be recorded then, or with the record
holding them. A member that restarts still refuses their respends. Nothing
about any block is written there or anywhere else.

**Mined from the record, whatever its fee.** A held transaction stays held
whatever fee it pays and whatever any mempool makes of it. A producer that holds
mines held transactions from its record, ahead of the mempool, oldest first, so
that parents precede children. They follow only the supervision records
(`addHeldTransactions` in `src/node/miner.cpp`). Each one is checked against
the tip by the rules a block applies, not by mempool policy: lock time, sequence
locks, inputs and amounts, and scripts under the block's own flags
(`CheckTxForNextBlock`). No fee is required. Where the mempool holds the same
transaction id, its copy is taken, since it may carry a newer witness. The block's `TestBlockValidity`
stands behind that check: a template that fails it is built again without the
held transactions, and only then without any. The held transactions take at
most `POS_HELD_BLOCK_SHARE_PCT` (50 %) of the block's weight, and of its
signature operations. The first held transaction that is minable and fits the
block always goes in, however large; an older one that is not minable yet is
passed over, and the share limits only what follows the first. The rest wait
for the next block. The transactions also go back into the mempool where its policy
takes them, so that nodes without the rule relay and mine them too, but a
refusal there releases nothing.

So no fee buys a release. A seller who certifies its own transaction at a fee no
honest mempool takes (one atom, say, or in an asset the members do not quote)
gets it mined by the next block an aware member leads. That transaction no
longer freezes its coin while the seller keeps a free option to mine it or
respend it later. An evicted mempool entry, a move of the fee asset's rate, and
members whose fee whitelists differ change nothing either: none of them is
consulted.

The share bounds what an attacker can make honest producers carry. Only
transactions of certified blocks that a Bitcoin reorganization disconnected are
held, and each is released once it is mined under a certified block. An
attacker that filled the blocks it led with fee-less transactions therefore
makes honest producers re-mine at most what those disconnected blocks carried,
and only after Bitcoin reorganizes under them. That work is spread over blocks
that stay open to the fee market. A disconnect of `k` full blocks is re-mined
within about `2k` blocks. In each of them the held transactions take at most
half the weight, or the first held transaction alone when it weighs more than
half; fee-paying transactions keep the rest. So the fee market keeps at least
half of a block unless one held transaction larger than that leads it, and
then it keeps what that one leaves.

**Arm one: signing.** A committee member does not sign a block that spends an
outpoint a held transaction spends, other than with that held transaction. Nor
does it sign a block built on an ancestor, above the disconnect point, that
does so. In the gossip round the check sits beside the lazy
`TestBlockValidity`.
- In a round whose backed proposal fails it, the member signs nothing, and it
  never signs a proposal other than the round's backed one.
- The leader is not excluded. Every member, with the rule or without it,
  keeps computing the same round order, so the next round that backs an honest
  proposal gathers every member's signature on that one block.
- A member that picked some other proposal instead, from its own view of the
  candidates, could split the aware members across blocks. Each would then be
  share-locked to its own pick, and they would keep splitting until the
  collection restarted: 7 rounds on a committee of 7, 250 at the cap of 250.

The same check guards a member's own proposal, `generateposblock`,
`getposblocktemplate`, `submitposblock` and `musigpartialsign`. While the node
holds anything, `musigpartialsign` needs the block whose hash it signs. Each
checks the block against the record with the validation lock held, and every
departure is in the record before the hold of that lock that made it ends, so
none of them signs over a departure, before the record is written to disk
included.

One signature escapes these checks. An autonomous leader checks its proposal
when it builds it, and signs it a moment later without holding a lock across
the two steps. A disconnect that falls between them can leave that leader's
signature on a proposal the record would now refuse. It is one member's signature, never a quorum: a member gives a share
only after it has checked the block again against the record, with the
validation lock held and the block still built on its tip.

**Arm two: sub-quorum blocks.** When the lowest block of the active chain that
carries such a spend does not carry its parent's quorum, a member keeps that
block out of its fork choice, and with it every descendant. It disconnects the
block and signs a sibling on its parent, and its mempool readmits the held
transaction, which the block had evicted. Nothing is marked failed: peers that
relay the block or build on it are not punished, their headers and blocks are
stored as usual, and a restart forgets the set-aside. A restart can also
leave the member off the block until a peer sends it again, because the node's
start-up can discard the stored body of a stall-escaping block. The
block returns to fork choice when any of these happens:
- a quorum-certified block stands at its height or above on the active chain;
- a quorum-certified block builds on it (the rest of the committee certified
  the respend, and the member follows). The member learns this when that block
  is judged certified, and from one look over the block index when it sets the
  block aside. It never scans the index again while the block stays set aside;
- nothing it conflicts with is held any more;
- the liveness bound or the backstop below is reached.

A member sets a given block aside once per hold. A block that a certified
block already builds on is not set aside at all. The sibling is often itself an
escaping-stall block with one signer, because the producer assembles a
stall-escaping block at its first share. Then the sibling and the respend rank
by leader score, and the respend is displaced by the certified block the
members build on the sibling: it is higher. Nodes without the rule follow by
ordinary fork choice.

**The mempool** refuses a transaction that spends a held outpoint
(`txn-held-conflict`), so honest leaders do not carry the respend even when it
pays more and signals replacement.

**The liveness bound and the backstop.** A member *holds* from its first
decline to sign or to propose for a held transaction, or its first set-aside,
until a new quorum-certified block appears on its active chain. A certified
block is new when the member first received it with a certificate after the
hold began, and after the last disconnect since. What counts is when the block was
certified, never its height or its branch:
- when a further Bitcoin reorganization lowers the chain, the certified
  blocks of the new branch end the hold although they stand lower;
- when Bitcoin returns to a branch it showed before, the certified blocks of
  that branch end nothing. They were certified before, and connecting a block
  again does not certify it again. So Bitcoin flipping between branches that
  already carry certified blocks restarts neither clock below.

The member numbers the blocks in the order it first receives them with a
certificate, while it holds anything, and marks the hold with the number
reached when the hold begins (and again at each disconnect); a block without a number was certified before everything the
member holds, and is older than every mark. The numbers and the clocks are
kept in memory only. A restart keeps the record and forgets them: the member
still refuses the respends of what it holds, its hold begins again at its
first decline after the restart, and the blocks it had connected before the
restart are old. A block the member connects for the first time is judged
then, however long ago it was signed.

Two clocks end a hold that lasts too long. Either one releases every held
transaction and every set-aside block, after which the member signs as if it
held nothing.
- **The bound** runs from the member's first honest proposal or share during
  the hold. It ends the hold when the chain goes `POS_HELD_BOUND_ROUNDS` (12)
  gossip rounds plus one collection window without a certified block. Rounds
  take `700 + 35·cap` ms and the window `500 + 25·cap` ms, so the bound is
  12.0 s for a committee cap of 7 and 120.15 s at the cap of 250.
- **The backstop** runs from the hold's first decline or set-aside, whatever
  the member has done or seen since: an isolated member that never proposes,
  say. It ends the hold at `POS_HELD_BACKSTOP_BOUNDS` (2) bounds: 24.0 s for a
  cap of 7 and 240.3 s at the cap of 250.

A departure restarts the bound's clock, whether or not anything certified
left: what the member holds now has had no round on this chain yet, so the
bound runs again from its next honest proposal or share after a decline. A
block the record refuses is the exception: one that respends a held
transaction, one built on such a block, and one the rule set aside leave
without restarting anything, since their leaving is what the rule asks for
(an operator's `invalidateblock` of the respend does on a member that signs
only through the RPCs what a set-aside does on a producer). The backstop's
clock runs on through every departure.

The bound is fixed at startup from the gossip timings (`-posroundms` and
`-poswindowms`, or the formulas), on each member, whether or not it runs the
producer. Members whose operators set those timings differently have different
bounds. A member that signs only through the RPCs is bound as well: a refusal
by `generateposblock`, `getposblocktemplate`, `submitposblock` or
`musigpartialsign` starts its hold, and a block or share they sign starts its
bound.

Both clocks are applied by the producer at every pass and by the node's
maintenance thread once a second (`POS_HELD_MAINTAIN_MS`). That thread is not
the anchor watcher's and asks Bitcoin Core nothing, so a Bitcoin Core that is
stopped, or that takes calls and never answers them, delays no release. It
runs on every node that has the rule, with or without the producer and with
or without the anchor watcher. A node started with `-validateanchor=0` has no
watcher, so it follows Bitcoin only through fork choice; whatever fork choice
takes off its active chain is recorded like any other departure, and it
refuses the respends of what its record holds, and releases them, by the same
clocks and the same window. A pass takes the node's validation lock
(`cs_main`), which every signature and every block needs too: while validation
keeps that lock, the release waits, and so does everything the release would
allow.

Whatever the mix of members with and without the rule, the rule adds to a
pause at most the bound plus the time one ordinary height takes, and no hold
lasts longer than the backstop plus one period of the maintenance thread.

**When a held transaction is released:**
- it is mined in a block at or below the highest quorum-certified block above
  the disconnect point. A Bitcoin reorganization that brings the original
  branch back mines it in its original block;
- a conflicting spend is mined in such a block. Then a quorum certified the
  conflict, and the held transaction cannot come back on this chain;
- it is invalid for good on the active chain, by what its transaction id
  commits to: the transaction itself, or its amounts against the very coins it
  spends, which can never change. A transaction that spends it is released with
  it. A script failure does not release it. The id does not commit to the
  witness, and the same id can come back with another one: a payment claim
  whose proof is re-attached after the lock was mined again, say. A missing
  input (a parent held or not valid yet, a conflict in a block not certified)
  keeps the hold too. So does a lock that time lifts: a lock time, a sequence
  lock, an immature coinbase, or a check that waits for Bitcoin;
- the liveness bound or the backstop is reached;
- the active tip anchors `POS_HELD_WINDOW` (144) Bitcoin blocks above the
  highest anchor the disconnected blocks had.

A held transaction that is not valid yet on the new chain (a lock time above
the lowered tip, say) stays held, and it is mined once it becomes valid. Its
conflicts stay refused meanwhile, as long as certified blocks keep coming:
each new one ends the member's hold, and with it both clocks, so neither the
bound nor the backstop runs out while the chain certifies. They stay refused
only until the window, and no longer once the chain certifies nothing for the
bound after a member's honest proposal, or for the backstop after its hold
began.

**What it guarantees.** A member is *aware* when it ran the rule through the
disconnect: it disconnected the block and holds the record. A member is
*unaware* when it was synced afresh after the disconnect or runs a binary
without the rule. A member that left the old branch by fork choice before its
own watcher ran is aware: the disconnect recorded what left. A member that was offline through the reorganization but held
the block disconnects it when it starts, and is aware. Let the aware members
hold `A` seats, the unaware and the dishonest `U` and `D`, and the quorum be
`q`.
- **Aware members hold a quorum (`A >= q`).** `U + D` cannot reach a quorum,
  and every aware leader mines the original from its record. No respend is
  certified unless the chain certifies nothing for the bound after an aware
  member's first honest proposal, or for the backstop after its hold began.
  Once that pause is over, a respend is certified as it would be without the
  rule. Two things produce such a pause:
  - *An attacker that leads every round of the bound.* Each round that backs
    an honest proposal certifies it, so the attacker has to hold the candidates
    of 12 consecutive rounds, which repeat once a collection has fewer than 12. With a share `s` of the stake spread over
    many keys, that is about `s^12`: 1.9·10⁻⁴ at 49 %, 1.7·10⁻⁵ at 40 %,
    5.3·10⁻⁷ at 30 % and 4.1·10⁻⁹ at 20 %. The figure is the same on the
    public committee (250 seats, quorum 126) and on the private-VRF committee
    (quorum 51 of an expected 100), because rounds are led by proposals, not
    by seats. With `c <= 12` candidates at the height the attacker needs all
    `c` of them. On the testnet's 20 operated nodes that means 12 of the 20.
    The figure is reasoned, not measured, and it is the chance at one height.
    A held transaction that is not minable yet (a lock time above the tip)
    gives the attacker one such try at every height until it is. It also
    takes the rounds as independent draws, while a round backs candidates in
    order of anchor freshness first, and a proposal is admitted at any time
    during a collection: an attacker that times its proposals to a fresh
    Bitcoin block can rank ahead more often than its stake alone gives it.
  - *A natural pause longer than the bound* after a disconnect: the committee
    cut off from itself for more than 12.0 s (harness) or 120.15 s (the cap of
    250), or for more than the backstop when no aware member could propose at
    all. After such a pause the attacker still needs a block that a quorum
    signs, or one that escapes a stall: three Bitcoin blocks, and 600 s of
    Bitcoin median time past at the testnet's setting, beyond its parent's
    anchor.
- **Neither side holds a quorum (`A < q` and `U + D < q`).** Without a stall
  escape, the original can be certified in a round that backs an honest
  proposal, as long as the members that sign it hold a quorum; unaware members
  sign it like any proposal. Unaware mempools take the respend by replacement,
  though, so their proposals carry it too. Unaware members that signed a
  respend round are share-locked to it, which can split the honest rounds that
  follow. Then nothing is certified until the bound, and the respend goes
  through. Which of the two is certified is a draw on the round order and the
  share-locks: with 3 aware of 6 (quorum 4), separate runs of the test of
  three attempts each have given the original once, twice and three times.
  With a single-signer respend block, the unaware
  members stay on it whenever it outranks the sibling on leader score. Then
  nothing is certified until the bound, the aware members release, and the
  respend is certified. In this mix, protection against a respend that escapes
  a stall is a matter of rank. The chain pauses at most the bound.
- **The unaware and dishonest hold a quorum (`U + D >= q`).** The respend is
  certified when a round backs it, as without the rule.

On the public committee (250 seats, quorum 126) the first case needs aware
members holding 126 seats, a strict majority of the stake behind the
committee. On the private-VRF committee (quorum 51 of an expected 100), a slot
draws 51 or more dishonest or unaware members, out of 40 % of the stake, about
1.7 % of the time (binomial) to 5.3 % (Poisson, many small stakers). At 30 %
that falls to under 0.03 %. A committee upgraded at one cutover has no unaware
members. One whose members upgrade one by one sits in the middle case until
most of the stake has upgraded.

**What it costs.**
- A legitimate conflicting spend (a refund, or a wallet replacing its own
  transaction after the reorganization) waits until the held transaction is
  mined, a quorum certifies a conflict, the hold ends at the bound or the
  backstop, or the window ends. Normally that is the next block an aware member
  leads, whatever the held transaction's fee.
- Up to half of each block an aware member leads goes to held transactions
  until the record is mined again (above).
- A seller's refund can wait about a day. The case: a claim was certified, a
  Bitcoin reorganization disconnected it, and the buyer's payment was
  double-spent in Bitcoin's new best chain. The claim is then premature, not
  invalid: its proof record could still be replaced, so it stays held. The
  refund conflicts with it and waits until the payment-claim waiting set gives
  the claim up, after 144 Bitcoin blocks (`BTCPAY_PENDING_WINDOW`, about a day),
  or until the held window ends at the same depth. Without the rule the refund
  waits only for its own lock: 60 Sequentia blocks after the first block
  anchored at the swap's deadline `D`, about an hour. The seller bears the
  wait, holding neither the asset nor the payment meanwhile. Only a buyer who
  double-spends its own payment after a reorganization causes it.
- Each round that backs a proposal carrying the respend costs that round for
  the aware members. A member keeps a block set aside for at most the backstop.
- Filling the record costs about 1.4 µs per transaction, in the hold of the
  validation lock that disconnects the block, which has already read it: 1,505
  transactions of 43 disconnected blocks took 2.15 ms in all, and 0.12 ms at
  most in one hold. Each held transaction's amounts and scripts are
  checked once against the active chain. Each template checks the held
  transactions it takes, as the mempool checks what it admits.

**What it does not cover.** The middle and last cases above. Transactions of
disconnected blocks above the highest certified one, which were never final. The
contested-anchor jump of a payment claim's refund floor, which the floor must
close itself.

**A return to a Bitcoin branch seen before.** When Bitcoin returns to a
branch it showed earlier, the Sequentia blocks once anchored on that branch
become valid again, and the branch the node stands on loses its anchors. The
watcher takes the current branch off the chain first: in each pass, its walk
for orphaned anchors runs before it reconsiders anything, so the blocks
anchored on the branch Bitcoin has just left are disconnected by the walk's
own invalidation, and their transactions are held as described above. Only
then does it reconsider the returning branch, and fork choice moves the node
onto it in the same pass. Whatever still leaves the active chain then is
recorded too: a branch that loses fork choice to the returning one, or one
whose anchor Bitcoin dropped while the pass ran. Either way the departure is
Bitcoin's doing, and its certified transactions are held. It makes no
difference whether the returning branch is higher than the current one, as
high, or lower.

Whichever thread takes the node off its branch first (the watcher's own
activation, a block from a peer, a peer's request for blocks, an RPC), the
disconnect records what left, before that thread releases the validation lock.
The record holds the departed blocks that are off the active chain then, each
branch from its fork with the active chain up; a block set aside by the rule's
second arm is not a departure. The watcher activates after each
reconsideration, before it asks Bitcoin about the next entry of its recovery
set, so the node follows the return without waiting on the Bitcoin daemon.

The reconsideration revives only blocks whose anchors Bitcoin has again. A
block above the reconsidered one whose own anchor is still off Bitcoin's best
chain stays invalid, together with everything above it, in the same step
(doc 03, "The reorg-following watcher"). So a held transaction never reads as
mined in a block that Bitcoin does not allow, not even for a moment.

**Departures that are not Bitcoin's doing.** Every disconnect is recorded,
whatever its cause, so the blocks that the finality reconciliation takes off
(when it releases a finalized branch the committee abandoned) and those an
operator's `invalidateblock` takes off are held too, when a block of theirs, or
one above it that left with them, carried its parent's quorum. Their
transactions are mined again from the record by the next block an aware member
leads, and released by the rules above: once mined, or once a conflict is
mined, under a certified block; at the liveness bound, counted from the
member's first honest proposal or share after a decline, or at the backstop,
counted from its first decline (12.0 s and 24.0 s for a committee cap of 7,
120.15 s and 240.3 s at the cap of 250); and at the window of
`POS_HELD_WINDOW` Bitcoin blocks at the latest. Until then the member signs no
respend of them. No block is kept out of fork choice for them beyond what a
set-aside already allows, so neither an operator nor the reconciliation is
resisted.

**Why a set-aside does not end when the sibling loses its anchor.** It can
happen that Bitcoin reorganizes again while a member keeps the respend block X
set aside and stands on the sibling it signed. The sibling's anchor goes, X's
stays, and the member is back on their common parent. Following X at that
moment would hand the attacker the respend on any shallow Bitcoin
reorganization. Consensus lets a producer anchor to any canonical Bitcoin
block at or above its parent's anchor (`-anchorminconf` is a local setting).
So the attacker anchors X a few blocks below Bitcoin's tip, while honest
siblings anchor at the tip, and any Bitcoin reorganization shallower than that
removes every honest sibling's anchor and keeps X's. Such reorganizations happen
without an attacker, and a Bitcoin miner can make one. The member therefore
stays on the parent, which Bitcoin still anchors, and makes and signs a fresh
sibling with a fresh anchor in its next slot. It follows X only when a certified
block builds on it, or at the bound or the backstop.

**Payment claims.** On a chain with the bitcoin payment opcodes, a claim
refused with `btcpay-not-in-anchor-chain` is a premature spend. It stays held,
because its payment may come back with the anchor. A claim refused for a
`bad-` reason is invalid and is released. The node's waiting set of claims is
what decides that a payment is gone for good. When the set gives a claim up
because no proof of its payment was found within its window of Bitcoin blocks,
it releases the held claim (`HeldTxsRelease`). A claim dropped only to make room
in the set is not released: that says nothing about its payment.

**Bitcoin stays supreme.** The rule decides only what a member signs and, for
a bounded time, which of two valid branches it builds on. The watcher
disconnects exactly as before. A set-aside block is never one whose anchor
Bitcoin replaced: the watcher has already invalidated those. A set-aside marks
nothing failed, so it can never clear or block an anchor invalidation, and
nothing held can resist a further Bitcoin reorganization. No node, with the
rule or without it, stays on a block whose anchor Bitcoin replaced for longer
than the watcher's tick. A set-aside can keep a member on an ancestor that
Bitcoin still anchors, instead of a sub-quorum block that carries the respend,
for at most the backstop and one period of the maintenance thread: 24.0 s for
a committee cap of 7 and 240.3 s at the cap of 250, plus that second. Bitcoin
flipping between branches that already carry certified blocks does not
lengthen it, because only a block certified after the set-aside began ends
the hold and starts another (measured with Bitcoin flipping four times per
backstop: 25.0 s and 241.3 s). A connected member leaves sooner, at the
committee's next certified block.

Like the share-lock, this is local signing policy: no block is valid or invalid
because of it, and a node that never saw the disconnected blocks validates
exactly as any other. `getheldtxs` lists what a node holds, what it has set
aside, and how long the bound and the backstop have run. The code is in
`src/pos_heldtxs.{h,cpp}`, the block assembler's `addHeldTransactions`, and,
for the set-aside, `FindMostWorkChain` and `CChainState::SetAsideBlock`. The
attacks are in `feature_pos_held_transactions.py`; the MuSig2 signing paths
in `feature_pos_held_transactions_musig.py`; a member that signs only through
the RPCs, held past the backstop, in `feature_pos_held_transactions_rpc_liveness.py`;
every member holding and refusing a respend before its record is written to
disk, in `feature_pos_held_transactions_departure_probe.py`; Bitcoin flipping
between two branches it has already shown, also across a restart of the
member, returning to one of them while a peer keeps the member's network
thread activating, a peer's block moving the member before its watcher runs,
and a member killed after a disconnect is persisted, in
`feature_pos_held_transactions_flips.py`; and a Bitcoin Core that stops
answering, a departure just before the backstop is due, and a member stopped
while it holds, in
`feature_pos_held_transactions_parent_hang.py`. In each, a real `bitcoind` is
the parent chain, the attacker runs with `-posdebugignoreheld`, a debug-only
switch that turns the rule off, and every refusal is asserted by the member's
own reason.

Operating a producer and the surrounding tooling are covered in
[`05-operating-sequentia.md`](05-operating-sequentia.md); the security model and
audit findings in [`07-security-and-audit.md`](07-security-and-audit.md).
