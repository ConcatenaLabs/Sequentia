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
consensus rules, so every node on a given network must agree on their values
(the public testnet runs `-pospubliccommittee=1 -poscommitteesize=250`).

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
weight; staking is opt-in. Unbonding is simply the CSV-gated spend:
the `OP_CHECKSEQUENCEVERIFY` lock - the whitepaper's stake locktime - is
enforced by the script itself, so unstaking is delayed by the configured period
and there is no separate unbonding ceremony.

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
recent, monotone, anchor-valid Bitcoin block to reference, and that influences
only the *next* block's committee - a committee that is itself privately
VRF-sortitioned, so the residual grinding is limited and VRF-mitigated. See
[`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) for the anchor commitment.

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
`pos_block_spacing` after its parent (`max(slot · interval, spacing)`,
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
`pos_vrf_exprace`'s own comment at `src/test/pos_tests.cpp:405`, which records
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

**Cadence is unchanged.** The leader is still the lowest-scoring staker, still
gated by `nTime ≥ parent.nTime + slot · posslotinterval`, and the slot interval
is still 30 seconds. The two rules even agree closely on how often some staker
draws slot 0, the case consensus lets produce with no wait at all beyond its
parent: `1 - (1 - 1/n)^n` under the legacy rule against `1 - e^{-1}` under the
exponential race. The two agree to within a percentage point for any committee
of a realistic size and converge as `n` grows. The producer's cadence floor holds
either winner to one slot interval after its parent in any case. What the fork
changes is *which* staker wins, not how fast blocks arrive.

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
`block.nTime ≥ parent.nTime + slot · posslotinterval` (`bad-posvrf-early`). So
the rank-0 leader may produce earliest; if it is absent, a higher-slot staker
may step in after its slot opens. This is the whitepaper's local wall-clock
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

### BLS certification (the default, `-posbls`)

The block challenge commits to the leader only:

```
OP_2 <leader(33)>                   # the BLS aggregate-committee form
```

The `OP_2` is a version marker. The certificate lives in the block's proof
*solution*, which is excluded from the signed block hash - so the hash the
committee signs does not depend on who signs, which is what lets members
produce non-interactive signature shares independently and lets the leader
aggregate any quorum of them (the gossip committee, §9). Two encodings:

- **Full-member form** (sortitioned committees): `<leader_sig> <agg_sig(96)>
  <member_1(257)> ... <member_m(257)>`, each member being
  `secp_pubkey(33) ‖ vrf_proof(80) ‖ bls_pubkey(48) ‖ bls_pop(96)` - the
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

**Why it is not the slot gate.** The gate delays a leader by
`slot · interval`, and `slot` scales with the reciprocal of the staker's weight,
so it can never be a uniform speed limit: the winning draw floors to 0 in
`1 - e^-1 ≈ 63%` of rounds, which is exactly when a brake would be needed. Left
to the gate alone, a set holding all the stake can drive the chain to a 17.5 s
cadence, and to 0.6 s if the gate is rescaled to one second per score unit.
Producers are paid in the fees of the blocks they lead and in nothing else -
there is no block subsidy (§3.9) - so the incentive to shorten the cadence is
permanent and grows with congestion, and more blocks in the same time is more
disk, more bandwidth and more validation for every node for ever.

The two parameters therefore stay separate: `pos_block_spacing` answers *how
fast may the chain run*, `-posslotinterval` answers *in what order may leaders
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
~30-second chain never permits, and each further sub-threshold block needs
another `+3` of parent-chain progress. It is what lets a young chain (or a
temporarily under-quorum committee) make progress, and it is what the
genesis-seeded launch uses for its slow start - see
[`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md). Tested in
`pos_escaping_stall_gap` (unit) and `feature_pos_escaping_stall.py`.

## 6. Fork choice & immediate finality

### The same-height comparator

Signed blocks all have equal nominal "work" (height), so same-height candidates
are ordered by a PoS-specific comparator in `CBlockIndexWorkComparator`
(`src/validation.cpp`), using two keys set on `CBlockIndex` at acceptance and
never mutated:

1. **more committee countersignatures wins** - `m_pos_countersigs`, the named
   committee size (so a full-threshold block always beats an escaping-stall
   sub-threshold one);
2. on an equal count, the **lower leader VRF score** wins - `m_pos_vrf_score`,
   the top 64 bits of the leader's `beta` over the slot seed (registry-
   independent, hence deterministic across nodes).

Both keys are computed from the block body in `SetPosForkChoiceKeys` before the
block enters the candidate set, and persisted in `CDiskBlockIndex` so a
restarted node orders identically. **Anchor freshness is deliberately not a
fork-choice key** (§7 explains why, and how freshness is delivered instead).

### The immediate-finality gate

A hard finality gate makes a quorum-certified block final. `UpdateTip` tracks
the highest active-chain quorum-certified block; `ContextualCheckBlockHeader`
rejects any block that would fork at or below it. So a certified block is locked
against every Sequentia-internal competitor - *including one that later gathers more
signatures* - and is never reorged to chase a fresher anchor. The VRF/committee
result is the ultimate truth.

The rejection is the soft, non-banning `BLOCK_RECENT_CONSENSUS_CHANGE`, because
the one legitimate exception is a **Bitcoin reorg** of a finalized block's
anchor: the anchor watcher invalidates the affected block on its own path (not
the accept-time gate), which lowers the finalized point via `UpdateTip`, after
which the Bitcoin-consistent chain is accepted. Bitcoin stays the security root -
Sequentia finality is immediate *modulo* a Bitcoin reorg. Tested in
`feature_pos_finality.py` (a higher-countersignature competitor does not reorg a
finalized block) and `feature_pos_fork_choice.py`.

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
   gate forbids that; §6), and it never lowers the 51-genuine-signature finality
   threshold - so it is pure coordination and can never create two "final" blocks.

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
  RPC (`src/pos_producer.*`).

- **An autonomous gossip-and-sign committee** (`-posbls` + `-posproducer`): the
  full decentralization, fully implemented and the default on the bundled chains.
  Each node detects its own eligibility; the elected leader floods its unsigned
  block, every node signs the best-ranked proposal it has collected
  (`BackedForRound`: freshest anchor, then lowest election score) and floods a
  non-interactive
  BLS share, and the leader aggregates a quorum into the certificate - assembling
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

Operating a producer and the surrounding tooling are covered in
[`05-operating-sequentia.md`](05-operating-sequentia.md); the security model and
audit findings in [`07-security-and-audit.md`](07-security-and-audit.md).
