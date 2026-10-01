# The autonomous gossip-and-sign committee

> This document specifies Sequentia's autonomous (coordinator-free)
> Proof-of-Stake committee: the layer that drives block *production* on the wire.
> Block *validation* is specified in
> [`../04-proof-of-stake.md`](../04-proof-of-stake.md). The autonomous producer
> thread, BLS aggregate certification, and the gossip committee
> (`posproposal` / `poscmpctprop` / `getposprop` / `posshare` / `poscert` /
> `getposcert`) that assembles a BLS-certified block across separate hosts with
> no coordinator are how the bundled chains produce blocks. One item of the
> Theoretical Paper is not implemented: the Bitcoin-hash *leadership reshuffle*
> (Principle 7 rule II), which is marginal under production-time freshest
> anchoring (§12.4).

## 0. What this layer is

Block **validation** is fully decentralized. Every node independently verifies
the leader's VRF proof, the committee and its quorum, the aggregate signature,
the minimum block spacing, the anchor, and the immediate-finality gate
([`../04-proof-of-stake.md`](../04-proof-of-stake.md) §§3–6,
[`../07-security-and-audit.md`](../07-security-and-audit.md) §6).

Block **production** is decentralized by the committee specified here: a
producer thread and a small peer-to-peer protocol let independent stakers
discover their own eligibility and converge on one certified block per height
with no coordinator. It realizes, on the wire, the round protocol of the
theoretical paper (Alberto De Luigi, *Sequentia Theoretical Paper*, 2022;
ed. A. Kohl, 2024), Principle 6.

A coordinator path also exists, for chains certified with MuSig2
(`-posbls=0`, custom chains only): `getposschedule`, `vrfprove`,
`getposblocktemplate`, `musignonce`, `musigpartialsign`, `musigaggregate`,
`submitposblock`, and the single-host shortcut `generateposblock`. The bundled
chains pin BLS certification, and there the gossip committee is the production
path.

### Building blocks

| Component | Where | Role |
|---|---|---|
| Election seed | `PosSeedForChild` → `ComputePosSeed(parent anchor hash, height)` (`src/pos.cpp`) | Deterministic per-height randomness, taken from Bitcoin's proof of work. |
| Leader election | `PosVrfSlotExp` / `PosVrfScoreExp` (`src/pos.h`), `src/vrf.{h,cpp}` (ECVRF-SECP256K1) | Each staker privately computes its own slot; the proof is published with the block. |
| Committee | `PosPublicCommittee`, `PosPublicQuorum`, `PosSlotQuorum` (`src/pos.h`) | The public fixed-size committee and its quorum, derived from the seed and the stake registry. |
| Leader time gate | `PosSlotGateSeconds` (`src/pos.cpp`) | How long after the parent a leader in a given slot may propose. |
| Stake set | `StakeRegistry`, `StakeFromTxOut` (`src/pos.h`) | The eligible-signer set and its registered BLS keys, rebuilt from the UTXO set. |
| BLS | `src/bls.{h,cpp}` over the vendored `src/blst/` | Sign, verify, aggregate, fast-aggregate-verify, proof of possession. |
| Round engine | `PosProducer` (`src/pos_producer.{h,cpp}`) | The producer thread, the per-height round state, the gossip handlers. |

The consensus rules this layer produces blocks under - minimum spacing, the
leader time gate, the election, the certificate format and quorum, on-chain BLS
registration, and the escaping-stall evidence - are validation rules and are
specified in `04-proof-of-stake.md`. This document covers how blocks that
satisfy them get made.

## 1. The paper's protocol, mapped to the wire

Principle 6 defines a 12-step round. The mapping:

| Paper step (P6) | What the node does | Message |
|---|---|---|
| 1. Stake, publish the verification key | On-chain staking output, carrying the staker's BLS key and proof of possession | — (UTXO) |
| 2. Block r-1 gives the public seed | `PosSeedForChild` over the parent's Bitcoin anchor hash | — (local) |
| 3. Each participant runs the VRF | For each staking key held, compute its slot over the seed (`PosVrfSlotExp`) | — (local) |
| 4. Leaders announce themselves through a proposed block | Every eligible staker proposes once per height when its time gate opens; the block carries the VRF proof and the leader's signature | `poscmpctprop` |
| 5. Peers verify VRF outputs | A proposal is recorded and relayed only if its leader has stake and its VRF proof and signature verify | (relay gate) |
| 6. Converge on one proposal | After a collection window, every node backs the best-ranked candidate: freshest Bitcoin anchor first, then lowest election score (`BackedForRound`) | — (local) |
| 7. A fresher Bitcoin block may reshuffle the leader | Not implemented as a re-election. Freshness is served by the ordering in step 6 (§12.4) | — |
| 8. Every node votes for the chosen block | Committee members sign the backed proposal; a share *is* a vote | `posshare` |
| 9. No quorum after the timeout: re-vote round-robin | The round index advances on a shared clock and members back the next candidate, subject to the share-lock (§3) | `posshare` |
| 10. Check consensus rules before committing | `TestBlockValidity` runs on the backed proposal immediately before signing; an invalid block excludes its leader | — (local) |
| 11. Aggregate the signatures | Any node holding a quorum of shares aggregates them | — (local) |
| 12. Certified; its seed drives the next round | The assembler submits the block and floods its certificate | `poscert`, block relay |

Three liveness and safety rules sit alongside the happy path:

- **Escaping stall (P8).** A block may be certified below quorum, down to one
  signer, when the Bitcoin anchor has advanced at least
  `POS_ESCAPING_STALL_ANCHOR_GAP` (3) past the parent's anchor *and* the two
  anchors are separated by at least `-posescapestallmtpgap` seconds of Bitcoin
  median-time-past (default 600). The producer lowers its own minimum to one
  signer whenever both hold for the proposal it is backing; a node that holds a
  certificate for the height does not use the valve.
- **A quorum cannot make an invalid block valid.** Every node validates every
  block, so a block that breaks a rule is rejected whoever signed it. The
  paper's *enforce-consensus* procedure - certifying an alternative block with a
  majority of the members who did not sign the invalid one - is not implemented;
  after an invalid certified block, production resumes from the last valid block
  under the normal quorum or, failing that, the escaping-stall rule.
- **Convergence (P8).** Same-height candidates are ordered by countersignature
  count, then leader VRF score, then block hash (`CBlockIndexWorkComparator`),
  so a quorum-certified block beats a sub-quorum one. The gossip layer only
  feeds that comparator.

## 2. Component architecture

The engine runs on a node that produces: one started with `-posproducer` and at
least one `-posproducerkey`, or one whose wallet has started staking. A node
with no producer validates and relays blocks as usual but takes no part in
committee gossip: every committee message handler returns at once when no
producer is running.

```
            ┌──────────────────────────────────────────────┐
            │  PoS round engine (thread, src/pos_producer)  │
            │  - self-eligibility (VRF over the seed)       │
            │  - the per-height round state (§3)            │
            └───────────────┬───────────────┬──────────────┘
                            │ produces       │ consumes
                            ▼                ▼
   ┌────────────────────────────┐   ┌────────────────────────────┐
   │ Per-height round state:    │   │  Validation / chainstate    │
   │  candidates, shares,        │   │  seed, registry, committee,  │
   │  the share-lock, the        │   │  block accept, finality gate │
   │  certificate pin            │   └────────────────────────────┘
   └─────────────┬──────────────┘
                 │ emit / ingest
                 ▼
   ┌────────────────────────────────────────────────────────────┐
   │ net_processing: NetMsgType handlers + relay rules (§4)      │
   │   posproposal · poscmpctprop · getposprop · posshare        │
   │   poscert · getposcert                                      │
   └────────────────────────────────────────────────────────────┘
```

The thread wakes on a new tip, on an inbound committee message, and on its own
poll timer (one second when idle, 150 ms while a round is open, or exactly at
the moment its own slot opens).

## 3. The per-height round

One round state per height being worked, discarded when the tip advances.

```
        new tip at h-1
              │  seed = PosSeedForChild(tip)
              ▼
        ┌───────────┐  no key with stake
        │ ELIGIBLE? │──────────────► take no part in this height
        └─────┬─────┘
              ▼  time gate open: nTime ≥ max(parent + slot gate, parent + spacing)
        ┌───────────┐
        │  PROPOSE  │  assemble a block, sign it as leader, flood poscmpctprop
        └─────┬─────┘
              ▼
        ┌───────────┐  record one candidate per leader (cheap checks only);
        │  COLLECT  │  relay each valid new one; exclude a leader that sends two
        └─────┬─────┘
              ▼  WINDOW_MS after the best candidate's timestamp
        ┌───────────┐  back candidate r in (freshest anchor, election score) order;
        │   BACK    │  validate it fully; if invalid, exclude its leader and re-pick
        └─────┬─────┘
              ▼  committee members only
        ┌───────────┐  flood a BLS share over the block hash; collect others'
        │   SIGN    │
        └─────┬─────┘
     quorum of shares            no quorum within ROUND_MS
              ▼                            │
        ┌───────────┐                      ▼
        │ CERTIFIED │              round r+1: back the next candidate,
        └───────────┘              subject to the share-lock
   assemble, submit, flood poscert
```

**Backing.** Candidates are ordered by Bitcoin anchor height, fresher first,
and then by the election key: the exponential-race score from
`pos_exprace_height`, the raw leader VRF output below it. Round *r* backs the
(r+1)-th candidate in that order. Every eligible staker proposes, not only
committee members, so the order is complete and the same on every node that has
seen the same proposals.

**The round clock.** The round index is
`floor((now − T·1000 − WINDOW_MS) / ROUND_MS)`, where `T` is the block
timestamp of the best-ranked candidate currently known. Reading the clock off a
gossiped block rather than off each node's arrival time keeps honest nodes in
the same round. `WINDOW_MS` and `ROUND_MS` are in §6.

**Validate before signing.** Proposals are recorded after only the cheap
checks. Full validation (`TestBlockValidity`) runs once, on the proposal a node
is about to sign. A block that fails it excludes its leader for the height, and
the node backs the next candidate. Only validated blocks are ever signed.

**The share-lock.** A member that has signed block X at a height does not sign
a rival at that height until the round has ended, it has asked its peers for a
certificate on X (`getposcert`), and one further `ROUND_MS` has passed in
silence. It also does not sign or lead at the next height on a parent that is a
rival of X until the escaping-stall evidence holds for that parent. The
arithmetic that makes this safe, and what it cannot bind, are in
[`../04-proof-of-stake.md`](../04-proof-of-stake.md) §9.

**The certificate pin.** A node that receives a valid quorum certificate for a
height (`poscert`) treats the round as over: it signs nothing more at that
height, and fetches the block body if it does not have it. The hold is bounded
at 30 seconds, so a certificate whose body never arrives cannot stall a node.

**The anchor-rollback pause.** While the anchor watcher holds a
quorum-certified child of the current tip whose anchor it has not yet confirmed
off Bitcoin's best chain, the producer neither proposes nor drives rounds at
that height. The pause ends when the block is restored, when its anchor is
confirmed stale, or after `-posanchorrecoverywait` seconds (default 30), so an
unreachable Bitcoin daemon cannot deadlock production.

**Assembly.** The leader ships its own signature inside the proposal, so any
node that gathers a quorum of shares can assemble the certificate and submit
the block. Competing assemblies share the block hash and duplicates are
dropped. A leader that proposes and then withholds or crashes is covered by
whichever node completes the quorum first.

**Exhaustion.** If the round index runs past the last candidate, collection
restarts and the node re-arms its own proposal.

## 4. P2P messages

Six message types in `NetMsgType` (`src/protocol.{h,cpp}`), dispatched in
`net_processing.cpp`. With BLS aggregation (§7) signing is one non-interactive
share; there is no nonce or partial-signature round. A message that fails
validation scores the sending peer 10 misbehaviour points.

| Message | Payload | Handling |
|---|---|---|
| `poscmpctprop` | `PosCompactProposal`: block header, coinbase, and the ids of the other transactions (BIP152-style) | The normal form of a proposal. Rebuilt from the receiver's mempool and checked against the header's merkle root; on a miss the receiver sends `getposprop` to the sender. |
| `posproposal` | One serialized block. The leader's key is in the `OP_2 <leader>` challenge, its VRF proof in the coinbase, its signature in the proof solution | Sent in reply to `getposprop`. Accepted only if it extends the active tip, the leader has registered stake, and the VRF proof and signature verify. A valid new proposal is relayed, always in compact form. A second, different block from the same leader excludes that leader for the height and is relayed as evidence. |
| `getposprop` | A block hash | Answered from the live candidate set, or from disk for a connected block. |
| `posshare` | `PosShare`: block hash, member key, VRF proof, BLS public key, proof of possession, BLS signature share | Checked for sizes, proof of possession and share signature. Under the public committee the member must be in the committee for the height and its BLS key must be the registered one; under threshold sortition the member's VRF proof is verified. Shares for unknown blocks are dropped. Deduplicated on (block hash, member). |
| `poscert` | The header of a certified block; the certificate is its proof solution | Verified against the registered keys. A certificate below quorum is ignored and never pins. On success the node pins the height (§3), relays the certificate, and fetches the body if it lacks it. |
| `getposcert` | A block hash | Answered with `poscert` if the node holds that certificate, otherwise with silence. Sent once per lock by a share-locked member. |

A share carries no height and no round index: the round is local and
clock-derived, and a share is identified by the block it signs. The certified
block itself rides standard block relay; `poscert` exists so that proof of
certification, a few hundred bytes, travels independently of the block and
reaches a node that a partition kept from the body.

Proposals are accepted only when they extend the active tip, and shares only
for known candidates. The deduplication sets are bounded and cleared when they
fill, and the per-height candidate and share maps are capped (§9).

## 5. Self-eligibility

No coordinator tells a node it is selected. On a new tip at *h-1* a node:

1. computes the seed for *h* from the tip's Bitcoin anchor hash;
2. for each staking key it holds with at least the minimum stake, computes that
   key's VRF output over the seed and from it the key's slot. Nobody without
   the key can predict this, so the leader order is not publicly knowable in
   advance;
3. proposes with its lowest-slot key when that slot's time gate opens (§6);
4. signs as a committee member with every key it holds that is in the
   committee for *h*.

Leader election is private; committee membership on the bundled chains is not.
The committee is `PosPublicCommittee`: the first
`K = min(BLS-registered stakers, cap)` entries of the public schedule, a
function of the seed and the registry that any node can compute. A leader need
not be a committee member.

## 6. Timing

Two timings are consensus rules; two are local.

**Consensus: minimum block spacing.** `block.nTime ≥ parent.nTime +
pos_block_spacing`, else `bad-pos-spacing`. It is 60 seconds on the bundled
chains. The check compares two timestamps written in blocks, never a local
clock, so every node reaches the same verdict. The block weight cap is 400,000
on the bundled chains, which keeps a saturated ledger growing at exactly
Bitcoin's saturated rate: `400,000 / 60 s = 4,000,000 / 600 s`.

**Consensus: the leader time gate.** `block.nTime ≥ parent.nTime +
PosSlotGateSeconds(slot)`, else `bad-posvrf-early`. The slot is the integer
part of the leader's election score, and the gate is 10 seconds per slot on the
bundled chains. A leader whose score is below 1 has slot 0 and may propose as
soon as the spacing allows; with a 10-second unit under a 60-second spacing,
slots 0 to 6 all open at the spacing floor. The gate orders leaders when the best ones are
absent; the spacing sets the cadence
([`../04-proof-of-stake.md`](../04-proof-of-stake.md) §4b explains why the two
are separate numbers).

A producer proposes at `max(parent.nTime + gate, parent.nTime + spacing)`, and
the block assembler clamps the block's timestamp the same way.

**Local: the collection window and the round.** After the best candidate's
timestamp a node collects proposals for `WINDOW_MS = 500 + 25 × cap`
milliseconds before it backs one, and each backed leader then has
`ROUND_MS = 700 + 35 × cap` milliseconds to be certified before the round
advances. `cap` is the configured committee size, so on the bundled chains
(cap 250) these are 6.75 and 9.45 seconds. They are tuned engineering
constants, overridable with `-poswindowms` and `-posroundms`, and they are not
consensus rules: no block is valid or invalid because of them. A node tuned
differently from its peers contributes late; the share-lock keeps a timing
disagreement from becoming a double-sign (§3).

Certification runs at the speed of the fastest quorum, not the slowest member:
a block certifies as soon as a quorum of shares reaches any one node.

There is no retargeting of any of these, and no stagger between leaders beyond
the time gate. A stagger wider than the collection window would let an early
proposer be signed before others' proposals arrived and split the shares; the
window plus the common ordering resolve multiple proposers on their own.

## 7. Signature scheme: BLS aggregate

The committee certifies blocks with **BLS aggregate signatures (BLS12-381),
non-interactively.** The Sequentia mainnet parameters pin this and refuse the
`-pos*` consensus flags outright; the testnet defaults to it and refuses a
conflicting value. MuSig2 (BIP327) certification remains for custom chains run
with `-posbls=0`, where a known, reliable signer set suits it.

### Why not MuSig2 for the autonomous path

MuSig2 is an *n-of-n*, *two-round interactive* aggregate. To realize a quorum
of a larger committee it must aggregate *exactly* a chosen subset, which on the
wire means fixing that subset, gossiping a nonce from every member of it,
gossiping a partial signature from every member of it, and then aggregating. In
an open, lossy committee that is brittle: one member failing to send its nonce
or partial fails the whole aggregate, and the engine must pick a different
subset and restart both rounds. It is a fine scheme for a known, reliable set;
it fights the gossip model.

### Why BLS

BLS aggregation is non-interactive, which is what the paper called for (P6 step
11: *"any party can aggregate signatures after the broadcast without
communicating with the original signers"*). Each committee member signs the
proposal independently and floods one share; any node aggregates whichever
shares arrive, with no subset pre-commitment and no second round, and the
aggregate is a single constant-size group element regardless of signer count.
The signing half of the protocol is robust to offline members by construction:
you collect whoever responds.

### Member-independent block hash

Members must be able to sign the instant a proposal arrives, before anyone
knows which of them will sign. So the message they sign cannot depend on who
signs. The certificate lives in the block proof `solution`, which Elements
already excludes from `block.GetHash()`: the hash is determined by the leader's
proposal alone (its transactions, anchor and VRF proof), members countersign
that fixed block in **one round**, and any node aggregates the shares. The
challenge collapses to `OP_2 <leader>`.

### Registration and the certificate

A staker registers its BLS key on-chain, once, inside its staking output:

```
<csv> OP_CHECKSEQUENCEVERIFY OP_DROP <blspubkey(48)> OP_DROP <pop(96)> OP_DROP <pubkey> OP_CHECKSIG
```

The proof of possession closes the rogue-key attack and is verified when the
output is connected (`bad-stake-bls-pop`); a staker has one BLS key
(`bad-stake-bls-conflict`). Both values are derived deterministically from the
staking key, so a staker manages one key (`getblsregistration`).

Because the keys are in the registry, the certificate on the bundled chains
carries no per-member data:

```
<leader signature> <aggregate signature (96)> <bitfield>
```

Bit *i* of the bitfield, least significant first, is seat *i* of
`PosPublicCommittee`. At a 250-seat committee the whole certificate is about
200 bytes. The leader's signature is checked with the proof; the aggregate is
verified at connect time against the registered keys of the seats the bitfield
names (`PosVerifyBitfieldCertificate`), and the number of signers against the
quorum (`bad-posbls-agg-quorum`).

Under threshold VRF sortition (custom chains) the certificate takes the
full-member form instead, carrying each member's key, VRF proof, BLS key and
proof of possession, 258 bytes a member.

### On forward security

BLS keys are long-lived and not forward-secure, so the paper's Principle 11
*posterior corruption* concern - old signer keys sold and reused for a
long-range attack - is not addressed by the signature scheme. The long-range
defence is the Bitcoin-anchored checkpoint
([`../04-proof-of-stake.md`](../04-proof-of-stake.md) §8): a checkpoint
consolidates after 2016 Bitcoin confirmations and is then irreversible for a
node that holds the checkpointed block. The paper's forward-secure option,
Pixel, is not adopted; it would add per-period key evolution and mandatory
secure deletion for every staker. It remains a compatible upgrade path, since
it is BLS plus key evolution.

## 8. Equivocation and convergence

- **A leader that proposes two blocks** at one height is excluded for that
  height by every node that sees both: it backs neither, and relays the second
  as evidence so its peers do the same. The committee converges on the next
  candidate. The sender is not penalised, since relaying evidence is correct
  behaviour.
- **A member that signs two blocks** is not detected or scored. Honest members
  do sign different blocks in different rounds once the share-lock has released
  them, so two shares from one member are not in themselves a fault. What
  prevents honest members from producing two certificates at one height is the
  share-lock (§3). A member that deliberately signs twice is bound by nothing
  in the protocol, and there is no slashing (§12.4).
- **Convergence** is a fork-choice matter: the comparator and the
  immediate-finality gate decide between competing certified blocks, and the
  gossip layer only supplies them. For a node left on a branch that is no
  longer being certified, the finality-reconciliation monitor (`-posreconcile`,
  on by default) releases its finalized point, but only for a rival carrying a
  full-quorum certificate at least `-posreconcilemindepth` (3) heights above
  it, and only after `-posreconcilepatience` (600) seconds without a certified
  extension of its own ([`../04-proof-of-stake.md`](../04-proof-of-stake.md)
  §6).

## 9. Anti-DoS and validation

The committee gossip is an inbound surface of its own, so its rules are cheap
to reject and bounded:

- **Only producers take part.** A node with no running producer ignores
  committee messages and relays none.
- **Proposals are gated on the leader.** A proposal is recorded and relayed
  only if it extends the active tip, its leader has registered stake, and its
  VRF proof and leader signature verify. One candidate is kept per leader, and
  at most 100 candidates per height.
- **Shares are gated on the member.** Sizes, proof of possession (cached) and
  the share signature are checked before anything else, and for the backed
  proposal the member must be in the committee. The share map is capped at the
  committee size.
- **Validation is lazy.** Cheap checks gate relay; full block validation runs
  once per round, on the proposal about to be signed (§3).
- **Compact relay** bounds proposal bandwidth: a proposal floods as a header, a
  coinbase and transaction ids.
- **Bounded memory.** Deduplication sets are cleared when they reach 20,000
  proposals, 200,000 shares or 20,000 certificates; the certificate caches hold
  100 entries.
- **Penalties.** An invalid proposal, share or malformed certificate scores its
  sender 10 points against the usual discouragement threshold of 100.
  Duplicates and equivocation evidence are not scored.

## 10. Integration points

| Concern | Where |
|---|---|
| Round engine, round state, gossip handlers | `src/pos_producer.{h,cpp}` |
| Message type constants | `src/protocol.{h,cpp}` (`NetMsgType`) |
| Receive and relay | `src/net_processing.cpp` |
| Starting the producer | `src/init.cpp` (`-posproducer`, `-posproducerkey`); `src/node/pos_control.cpp` (wallet staking) |
| Block assembly for a proposal | `BuildUnsignedBlsBlock` → `BlockAssembler::CreateNewBlock`; accepted through `ProcessNewBlock` |
| Single-host production | `ProducePosBlock`, shared with `generateposblock` |
| Signatures | `src/bls.*` over `src/blst/`; `src/musig.*` for MuSig2 chains |
| Election and committee | `PosSeedForChild`, `PosVrfSlotExp`, `PosVrfScoreExp`, `PosSlotGateSeconds`, `PosPublicCommittee`, `PosPublicQuorum` (`src/pos.h`), `src/vrf.*` |
| Anchor watcher, reconciliation monitor | `src/anchor.{h,cpp}` |

## 11. Compatibility

- Producing is opt-in. A node that neither sets `-posproducer` nor stakes from
  its wallet validates the chain and is silent on the committee protocol.
- The certification scheme and the committee regime are chain-wide: every node
  on a chain must agree on them. The mainnet parameters pin them and refuse the
  flags that would change them; the testnet refuses a value that conflicts with
  its own.
- The functional tests start *N* nodes with no coordinator, each holding one or
  more staking keys, and assert that they produce and certify a chain end to
  end, including with members offline and through recovery:
  `feature_pos_bls_gossip.py`, `feature_pos_gossip_failover.py`,
  `feature_pos_bls_large_committee.py`, `feature_pos_public_committee.py`,
  `feature_pos_cert_gossip.py`. `feature_pos_distributed_committee.py` covers
  the coordinator path.

## 12. The layers, end to end

The committee is built from four layers, each independently testable.

1. **Engine, self-eligibility, single-node production.**
   `src/pos_producer.{h,cpp}`, `-posproducer` / `-posproducerkey`; test
   `feature_pos_autonomous_producer.py`. A node with one or more staking keys
   leads with its best key each round, waits for its time gate and the block
   spacing, and assembles, signs and submits a block. A single-node producer
   needs no committee message; its blocks propagate by normal block relay.
2. **BLS certification.** `src/bls.*`; tests `bls_tests`,
   `feature_pos_bls_committee.py`, `feature_pos_bls_registration.py`. Blocks are
   certified by a non-interactive BLS12-381 aggregate carried in the proof
   solution, so the signed block hash is member-independent (§7).
3. **The gossip rounds.** The six messages of §4, the round engine, and the
   relay rules; tests `feature_pos_bls_gossip.py`,
   `feature_pos_public_committee.py`. Each eligible staker floods its unsigned
   block; each node collects for a window, backs the best-ranked candidate,
   validates it and signs; any node that gathers a quorum assembles the
   certificate and submits. Because the signed hash is member-independent this
   is a single round.
4. **Hardening.** What keeps the rounds safe and live:
   - **Anti-DoS** (`feature_pos_gossip_dos.py`): §9.
   - **Liveness under failure** (`feature_pos_gossip_failover.py`): a leader
     that crashes or withholds costs one round; the round index advances on the
     shared clock and the committee backs the next candidate.
   - **Scale** (`feature_pos_bls_large_committee.py`): multi-key hosts and
     larger committees, including through a host failure.
   - **Leader-equivocator exclusion** (`feature_pos_gossip_byzantine.py`): a
     leader that sends two blocks is excluded for the height (`m_excluded`,
     which also holds leaders whose block failed validation). The fault
     injector `-posbyzantineequivocate` confirms that an equivocating leader is
     excluded at every height and the honest nodes never diverge.
   - **Lazy validation** (`feature_pos_gossip_invalid.py`): an invalid backed
     block excludes its leader and the committee re-picks.
   - **Certificate gossip and the share-lock**
     (`feature_pos_cert_gossip.py`, `feature_pos_certified_sibling_guard.py`):
     §3.
   - **Freshest-anchor preference** (`feature_pos_anchor_freshness.py`; P7 rule
     III): candidates are ordered by Bitcoin anchor height before the election
     score, so the committee converges on the freshest-anchored proposal and
     the tip tracks Bitcoin's tip. The Bitcoin-hash *leadership reshuffle* (P7
     rule II, a new Bitcoin block re-running the leader election) is not
     implemented; with production-time freshest anchoring it is a marginal
     refinement.
   - **Compact proposals** (`src/test/pos_compact_tests.cpp`): proposals flood
     as header, coinbase and transaction ids. The merkle root verifies the
     reconstruction, and any failure degrades to fetching the full block, never
     to a wrong one. The paper's "relay only the lowest-VRF proposal" (step 6)
     is not used: it would leave nodes with different candidate sets and break
     the common ordering.

   **Certificate weight.** The certificate lives in the legacy
   `CProof.solution`, which `GetBlockWeight` would count in both serialization
   passes, four times its size. On PoS chains `CProof::Serialize` skips the
   solution in the no-witness pass, so the certificate is weighted once, like a
   witness; the hash already excluded the solution, so what the committee signs
   is unchanged (`feature_pos_cert_weight.py`). With the bitfield form this
   matters little, since the certificate is about 200 bytes. It matters for the
   full-member form, where a 100-member certificate is about 26 KB. The block
   assembler reserves `max_block_signature_size` (32,000 on the bundled chains,
   sized for the full-member form) in every template whatever the certificate
   actually weighs.

   **Quorum: a strict majority, not two-thirds.** The certification quorum is a
   majority of the committee, as the Theoretical Paper specifies (Principle 6):
   `PosPublicQuorum(K) = K/2 + 1`, plus one more when `K` is odd, so 126 at the
   250-seat cap. The paper rejects a two-thirds threshold because "maximising
   persistence also stalls blocks if the 2/3rd threshold is unmet because some
   participants are [offline]" (§i.5).

   - *The would-be fork.* Two quorums of one committee overlap in at least two
     members, so two conflicting blocks can each reach a quorum only if at
     least two members sign both.
   - *Why honest members do not do that.* They back the same candidate, exclude
     a leader that proposes twice, and are held by the share-lock once they have
     signed. So a second certificate cannot form from honest members.
   - *The residual.* A member that deliberately signs twice is not bound. With
     only a few such members a double certification also takes an adversary
     able to make the honest members back different blocks during the
     collection window; when the honest members see the same proposals, it
     takes a dishonest majority of the committee. That is the accepted limit of
     a majority quorum. The alternative, two-thirds, is rejected for the
     liveness reason above: it would force anchor-gated escaping-stalls
     whenever participation dipped toward two-thirds. Beyond this, a finalized
     block changes only if Bitcoin reorganizes its anchor.

   The margin against a dishonest coalition comes from the size of the
   committee rather than from the quorum fraction: the committee is a
   stake-weighted sample, and the cap is chosen so that a coalition short of a
   majority of the stake is very unlikely to hold a majority of the seats.

   **Round-schedule alignment, and the share-lock.** The round index is a
   function of time, so the schedule is anchored to the best candidate's own
   block timestamp, a value every node reads identically off the same gossiped
   block, and honest nodes step through rounds together regardless of when each
   received the proposal.

   Safety does not rest on that alignment. A member that signs across a round
   boundary would be double-signing, and a certificate that completes just as
   the round rolls over would race the re-vote; either way two blocks could
   certify at one height out of nothing but honest nodes following the clock.
   The share-lock (§3) closes both. Every signer of a certifiable block is held,
   fewer than a quorum remain free, and a second certificate cannot form from
   honest members however far their clocks disagree. Clock skew costs liveness,
   not safety.

   Injecting a gradient of per-node round-clock skew (`-posdebugroundskewms`, a
   test-only option) and checking hash agreement at every height
   (`test/functional/pos_round_skew_experiment.py`) gives, over five 35-second
   trials each at a 12-member committee (`ROUND_MS` = 1120 at that size):

   | Max inter-node skew | Forks | Blocks certified per trial |
   |---|---|---|
   | 0 ms (synchronized)        | 0 / 5 | 30 to 42 |
   | 561 ms (0.5 × ROUND_MS)    | 0 / 5 | 27 to 37 |
   | 1683 ms (1.5 × ROUND_MS)   | 0 / 5 | 2 to 14 |
   | 4477 ms (4 × ROUND_MS)     | 0 / 5 | 0 to 2 |

   Past one round of skew, nodes land in different rounds, the locked members
   wait out their grace, and the chain slows; at several rounds of skew almost
   nothing certifies (a stall, not a fork). Real-world NTP keeps skew at the
   millisecond scale, far inside the margin at which even liveness is affected.

   **Not a goal: stake slashing.** In economic-finality PoS, slashing *is* the
   finality guarantee: reverting a block must burn a third of the stake.
   Sequentia's safety does not rest on stake at risk. It rests on (a)
   **independent validation** - every node verifies each block, so a committee
   can never make an invalid block valid, only censor or stall; (b) the
   **leader exclusion and share-lock** above; and (c) **Bitcoin-anchored
   checkpoints** for the long-range case. There is also a reason specific to
   anchoring: when Bitcoin reorganizes from one branch to another and back, a
   committee that follows it correctly signs different blocks at the same
   height, so punishing a double signature would punish honest behaviour. Two
   signatures from one member remain attributable evidence; the protocol does
   not act on it.

## 13. Settled design points

**Signature scheme (§7).** The committee certifies with BLS aggregate
signatures (BLS12-381), non-interactively; MuSig2 is for custom chains run with
`-posbls=0`. Pixel and protocol-level forward security are not adopted: the
long-range defence is the Bitcoin-anchored checkpoint.

**Committee (§5, §7).** On the bundled chains the committee is a public
fixed-size list, capped at 250, drawn from the stakers that registered a BLS
key; only leader election stays private. Threshold VRF sortition, with a
private committee of expected size up to 100, is the base model and remains on
custom chains.

**Timing (§6).** The cadence is a consensus rule: a minimum spacing of 60
seconds, with a block weight cap of 400,000 that holds ledger growth equal to
Bitcoin's. The leader time gate is 10 seconds per slot. The collection window
and the round length are local, and scale with the committee cap.

**Who relays (§2).** Only nodes running a producer take part in committee
gossip. Producing is opt-in.

**Quorum (§12.4).** A strict majority, not two-thirds, as the Theoretical Paper
specifies. Honest members cannot produce two certificates at one height,
because of leader exclusion and the share-lock; the margin against dishonest
members comes from the committee's size; Bitcoin is the long-range root.

**Not implemented.** The Bitcoin-hash leadership reshuffle (Principle 7 rule
II), and the paper's enforce-consensus procedure for certifying an alternative
to an invalid certified block (§1).
