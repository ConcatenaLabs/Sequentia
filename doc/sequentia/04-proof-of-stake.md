# Proof-of-Stake consensus

Sequentia's consensus is Proof-of-Stake. Block production is a stake-weighted
election with private VRF sortition; a committee certifies each block with a
single MuSig2-aggregated signature; and the certified block is final the moment
it is accepted. SEQ is the staking asset and the one thing that confers
production eligibility. Voluntary Bitcoin checkpoints resist long-range attacks.

This chapter is the full consensus specification: the stake registry, leader
election and VRF sortition, committee certification and aggregation, liveness,
fork choice and the immediate-finality gate, anchor freshness, long-range
defenses, and the production layer.

## 1. Overview

The consensus is a BFT protocol in the shape of the Sequentia whitepaper: a
committee of up to 100 members, private VRF cryptographic sortition,
strict-majority (51-of-100) countersignature certification, and Bitcoin
checkpoints against long-range attacks. It is built on the *signed-block*
substrate Sequentia inherits from Elements — Elements replaces Bitcoin's
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
| `CheckChallenge` | header time | the challenge's *form* only — a recognized PoS leader/committee challenge |
| `CheckPosStakeRules` | `ConnectBlock` (registry = parent state) | the election: leader is a registered staker, the VRF proof and slot, the committee, and the quorum |
| `CheckProof` | block-connect | the block signature satisfies the challenge — the block really is signed by the leader (and the aggregate by the committee) |

Everything registry-dependent waits for connect time, where the registry equals
the block's parent state, so headers-first sync and parallel block download can
never mis-evaluate eligibility.

The chain is enabled with `-con_pos`. The defining flags layered on it are
`-posvrf` (private sortition), `-posaggcommittee` (MuSig2 committee
aggregation), `-poscommitteesize`, `-posslotinterval`, `-posunbonding`,
`-posminstake`, `-poscheckpoint`, and `-poscheckpointdepth`. The bundled
Sequentia chains enable VRF and committee aggregation by default.

## 2. The stake registry

The stake registry is the map `{staker pubkey → stake weight}` from which every
election is computed (`src/pos.{h,cpp}`, `StakeRegistry`). On the bundled chains
it is built entirely from on-chain stake.

### The on-chain UTXO layer

Stake is registered by holding SEQ in a **staking output** — the bare script

```
<csv> OP_CHECKSEQUENCEVERIFY OP_DROP <pubkey> OP_CHECKSIG
```

holding an explicit policy-asset (SEQ) amount. While such an output is unspent,
its amount adds to its key's weight. Unbonding is simply the CSV-gated spend:
the `OP_CHECKSEQUENCEVERIFY` lock — the whitepaper's stake locktime — is
enforced by the script itself, so unstaking is delayed by the configured period
and there is no separate unbonding ceremony.

The layer is a pure function of the UTXO set: it is rebuilt from the UTXO set at
node startup and mirrored exactly on every tip connect and disconnect, so it is
reorg-safe. Confidential outputs cannot carry weight, because their amounts are
hidden. The staked asset is always SEQ (`::policyAsset`), which `StakeFromTxOut`
requires; this staking-weight role is SEQ's *only* privileged status — for fees
SEQ is just another asset (see [`02-open-fee-market.md`](02-open-fee-market.md)).

On-chain staking outputs are **standard relay and mempool outputs**. When PoS is
enabled the staking script is recognized as standard, so a stake-registration
transaction relays across the network and is mined under default policy like any
ordinary payment — no special relay configuration is required.

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
committee membership, and from the eligible-total sortition denominator — a
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
`-staker` config — see [`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md).

## 3. Leader election & VRF sortition

### The election seed

The per-slot seed is **anchor-derived and deterministic**:

```
seed_h = ComputePosSeed( parent block's Bitcoin-anchor hash, height h )
```

The seed is built from the parent's committed Bitcoin-anchor hash and the
height — both header fields fixed at block-index creation, so the seed is
identical on every node. It is deliberately *not* the SEQ block hash (which a
producer could grind) and *not* a VRF score. The anchor hash is Bitcoin's
proof-of-work, which a SEQ producer cannot bias; its only freedom is which
recent, monotone, anchor-valid Bitcoin block to reference, and that influences
only the *next* block's committee — a committee that is itself privately
VRF-sortitioned, so the residual grinding is limited and VRF-mitigated. See
[`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) for the anchor commitment.

### Private VRF sortition

A verifiable random function makes the schedule unpredictable to everyone but
the winner. For a secret key `sk` (public `Y = sk·G`) and input `alpha`, the
holder of `sk` computes a 32-byte pseudorandom output `beta` and a proof `pi`
such that anyone with `(Y, alpha, pi)` can verify `beta` is the unique correct
output — but nobody without `sk` can predict it. A staker learns *privately*
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

`PosVrfSlot` is locally checkable from the single published proof — a validator
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

### Time-gating

A block records the leader's VRF proof in a coinbase `OP_RETURN` (tagged
`SEQVRF`), covered by the merkle root and hence by the leader's signature. At
connect time `CheckPosStakeRules` verifies the proof against the leader's
challenge key over the slot seed, recomputes `slot`, and requires
`block.nTime ≥ parent.nTime + slot · posslotinterval` (`bad-posvrf-early`). So
the rank-0 leader may produce earliest; if it is absent, a higher-slot staker
may step in after its slot opens. This is the whitepaper's local wall-clock
round timeout with the lowest-VRF participant as proposer.

## 4. Committee certification & aggregation

A block becomes final by carrying a committee certification. The committee is
the set of sortition-selected members (up to 100), and the **quorum is a strict
majority** — the whitepaper's 51-of-100. The quorum is fixed at a majority of
the *expected* committee size, independent of how many members actually sign, so
a block cannot exist without genuine majority participation. This is what gives
the chain immediate finality (§6).

### Aggregation into one signature

With `-posaggcommittee` (which raises the `-poscommitteesize` cap to 100), the
members' signatures are **MuSig2-aggregated** into a single 64-byte BIP340
Schnorr signature, so block size is constant in committee size. The primitive is
BIP327 over the vendored secp256k1 (`src/musig.{h,cpp}`): it aggregates a signer
set into one 32-byte x-only key (order-independent — the aggregate depends only
on the *set*), produces one 64-byte signature, and verifies it. A `q`-of-`m`
quorum is realized by aggregating exactly the `q` signing members.

The block challenge commits to the leader plus the aggregate key:

```
OP_1 <leader(33)> <aggkey(32)>      # BuildPosAggChallenge
```

The leading `OP_1` is a version marker no other challenge form begins with. The
challenge no longer lists members — it commits to the single MuSig2 aggregate of
the member set.

### Independent re-verification

The coinbase carries each member's VRF eligibility commitment (tagged `SEQCMT`,
`pubkey‖proof`), so every validator independently re-verifies eligibility and
re-derives the aggregate. Under aggregation those commitments do not merely
prove a claimed list — they *are* the member set. `CheckPosStakeRules` requires:
every named member distinct and within the 100 cap (`bad-posvrf-member-count`);
every member sortition-selected (`bad-posvrf-member-missing` /
`-invalid` / `-not-selected`); at least `PosQuorum(committee_size)` members
named (`bad-posvrf-agg-quorum`); and `MuSigAggregatePubkey(named set) == aggkey`
(`bad-posvrf-agg-key`). So the one signature is by precisely the proven-eligible
members.

### The block solution

The solution is two pushes: the leader's ~73-byte DER signature and the 64-byte
BIP340 aggregate, both over the block hash. `CheckProof` verifies them directly
(ECDSA for the leader, Schnorr for the aggregate) rather than through the script
interpreter, because `OP_CHECKMULTISIG` cannot express one signature over an
aggregate of up to 100 keys. The per-chain `max_block_signature_size` is sized
for this combined solution — **150** on the bundled chains.

(For small custom committees a script form also exists:
`<leader> OP_CHECKSIGVERIFY <q> <c_1..c_n> <n> OP_CHECKMULTISIG`, capped at 16
members because each member is a separate pubkey and signature push. The
aggregate form above is the paper-scale path.)

## 5. Liveness — escaping-stall

Normal operation requires a quorum, which fails a young or stalled chain. The
escaping-stall rule restores liveness: a block may be certified **below quorum,
down to a single signer**, but only when the Bitcoin anchor has advanced at
least `POS_ESCAPING_STALL_ANCHOR_GAP` (3) past the parent block's anchor.

The condition is the pure, deterministic function
`PosEscapingStallAllowed(parent_anchor_height, block_anchor_height)`
(`src/pos.h`), computed only from SEQ-committed anchor heights — no live parent
query enters the validity rule, so every node agrees. When it holds,
`CheckPosStakeRules` relaxes the named-member quorum to a single member; the
block stays a valid aggregate-committee block (every named member
sortition-eligible, `aggkey` equal to the aggregate of the named set) — only the
*count* relaxes. Otherwise the full strict-majority quorum is required.

The path is self-limiting and abuse-proof: a `+3` anchor gap requires Bitcoin to
have genuinely produced three blocks (~30 minutes), which a healthy
~30-second chain never permits, and each further sub-threshold block needs
another `+3` of parent-chain progress. It is what lets a young chain (or a
temporarily under-quorum committee) make progress, and it is what the
genesis-seeded launch uses for its slow start — see
[`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md). Tested in
`pos_escaping_stall_gap` (unit) and `feature_pos_escaping_stall.py`.

## 6. Fork choice & immediate finality

### The same-height comparator

Signed blocks all have equal nominal "work" (height), so same-height candidates
are ordered by a PoS-specific comparator in `CBlockIndexWorkComparator`
(`src/validation.cpp`), using two keys set on `CBlockIndex` at acceptance and
never mutated:

1. **more committee countersignatures wins** — `m_pos_countersigs`, the named
   committee size (so a full-threshold block always beats an escaping-stall
   sub-threshold one);
2. on an equal count, the **lower leader VRF score** wins — `m_pos_vrf_score`,
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
against every SEQ-internal competitor — *including one that later gathers more
signatures* — and is never reorged to chase a fresher anchor. The VRF/committee
result is the ultimate truth.

The rejection is the soft, non-banning `BLOCK_RECENT_CONSENSUS_CHANGE`, because
the one legitimate exception is a **Bitcoin reorg** of a finalized block's
anchor: the anchor watcher invalidates the affected block on its own path (not
the accept-time gate), which lowers the finalized point via `UpdateTip`, after
which the Bitcoin-consistent chain is accepted. Bitcoin stays the security root —
SEQ finality is immediate *modulo* a Bitcoin reorg. Tested in
`feature_pos_finality.py` (a higher-countersignature competitor does not reorg a
finalized block) and `feature_pos_fork_choice.py`.

## 7. Anchor freshness for real-time swaps

Real-time cross-chain atomic swaps need the Sequentia tip to reference the
freshest Bitcoin block, so a swap's Sequentia leg confirms with
`anchor ≥ the Bitcoin leg's height` promptly — no extra reorg-protection
timelock (see the definition in
[`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md)). This freshness is
delivered by *production*, never by fork choice, because in an immediate-finality
system a fork-choice rule that could prevail over the VRF result would let a
newly-arrived Bitcoin block reorder an already-certified block.

Freshness is delivered at two safe layers:

1. **Leaders build on the freshest anchor.** `GetAnchorForNewBlock` anchors
   every new block to the freshest Bitcoin block, so the canonical tip tracks
   Bitcoin's tip within one block — by *extending* the chain, never reorging it.

2. **A committee signing preference.** When members face competing proposals at
   the same height, they preferentially sign the one referencing the freshest
   Bitcoin block. The leader's freshest-anchored proposal carries a **fixed local
   commit-timing weight of about `0.3 × quorum`** (e.g. **+15** at a 100-member,
   51-quorum committee) so it reaches the effective-signature threshold first and
   is the proposal the committee converges on. This weight **never counts toward
   the 51/100 real-signature finality threshold** — finality is always at least
   51 genuine signatures over the VRF-determined committee, view-independent — so
   the preference is pure coordination and can never create two "final" blocks.

This signing preference lives in the production layer (§9); it is designed to be
built into an autonomous gossip-and-sign committee's voting.

## 8. Long-range-attack defenses

Two checkpoint mechanisms, combined with the CSV stake locktimes of §2 (which
must exceed the checkpoint cadence), close the posterior-corruption window.

**Dynamic Bitcoin checkpoints.** Anyone may commit a Sequentia block hash into
the Bitcoin parent chain (a `SEQCKPT` OP_RETURN; `getcheckpointpayload`). Once
that commitment is buried `-poscheckpointdepth` deep, a node that has the block
on its active chain treats it as finalized and rejects forks below it
(`bad-fork-prior-to-pos-checkpoint`) — even longer, validly-signed branches.
Checkpoints only lock in history a node has *already validated* and never replace
it, so conflicting or bogus commitments are harmless; a node that passed a
checkpointed height *without* the checkpointed block raises a `conflicts` alarm
(`getcheckpointinfo`) rather than silently following a checkpoint it never
validated. Tested in `feature_pos_checkpoints.py`.

**Configured static checkpoints.** `-poscheckpoint=height:hash` (repeatable)
pins a height-to-hash mapping in configuration, known before any block is
downloaded. A block presented at the pinned height must carry the pinned hash,
otherwise it — and any branch built on it — is rejected in
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

**Block production coordination is RPC/coordinator-driven.** The cryptographic
protocol and the RPCs to assemble a block exist in full —

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

— but organizing a committee to assemble a block each slot is done by external
tooling. There is no automatic block-producer thread and no autonomous
peer-to-peer gossip-and-sign committee. BIP327's secret nonce is deliberately
non-serializable; each member's node keeps the live secret nonce in an in-memory
session store between the two rounds and consumes it exactly once
(`feature_pos_distributed_committee.py` runs the full loop across three separate
nodes, each holding one key).

Two paths lead to a live network:

- **A coordinator-driven committee** works with the current code and suits a
  known founding committee: a coordinator orchestrates `getposblocktemplate`,
  the per-member `musignonce` / `musigpartialsign` round trips,
  `musigaggregate`, and `submitposblock`. This is semi-centralized at the
  *production* layer — the coordinator is a liveness and orchestration point —
  while validation stays fully trustless.

- **An autonomous gossip-and-sign committee** is the full decentralization and
  is named future work: each node detects its own eligibility and gossips
  proposals, nonces, and partials to assemble the quorum with no coordinator.
  The anchor-freshness signing preference of §7 is designed to live in this
  layer.

Operating a producer and the surrounding tooling are covered in
[`05-operating-sequentia.md`](05-operating-sequentia.md); the security model and
audit findings in [`07-security-and-audit.md`](07-security-and-audit.md).
