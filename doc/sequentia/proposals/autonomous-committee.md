# Design proposal — the autonomous gossip-and-sign committee

> **Status: RFC; Phase 1 implemented.** This document specifies the design for
> Sequentia's autonomous (coordinator-free) Proof-of-Stake committee. It is a
> planning artifact, distinct from the as-built specification in
> [`../04-proof-of-stake.md`](../04-proof-of-stake.md). Where it proposes a
> change to a shipped mechanism (notably the signature scheme, §7) that is
> called out explicitly. **Phase 1 (the autonomous producer thread) is built and
> tested** (`src/pos_producer.*`, `-posproducer`); Phases 2–4 (committee gossip,
> BLS aggregation, hardening) remain proposal. See §12 for status.

## 0. Where we are and what this closes

Block **validation** is already fully decentralized: every node independently
verifies VRF proofs, committee eligibility, the aggregate signature, the anchor,
and the immediate-finality gate ([`../04-proof-of-stake.md`](../04-proof-of-stake.md)
§§3–6, [`../07-security-and-audit.md`](../07-security-and-audit.md) §6).

Block **production** is not. The cryptographic protocol and a full set of RPCs
exist — `getposschedule`, `vrfprove`, `getposblocktemplate`, `musignonce`,
`musigpartialsign`, `musigaggregate`, `submitposblock`, and the single-host
shortcut `generateposblock` — but *orchestrating* a 100-member committee to
assemble one certified block each slot is done by external tooling. There is no
producer thread and no peer-to-peer protocol by which independent members
discover their own eligibility and converge on a block without a coordinator.

This proposal specifies that missing layer: a round engine plus a small family
of gossiped P2P messages that realize, on the wire, the 12-step round protocol
of the theoretical paper (Alberto De Luigi, *Sequentia Theoretical Paper*, 2022;
ed. A. Kohl, 2024), Principle 6.

### Reusable building blocks already in the tree

| Component | File | Role in the autonomous layer |
|---|---|---|
| Election seed | `ComputePosSeed(parent_anchor_hash, height)` (`src/pos.cpp`) | Deterministic per-round randomness; unchanged. |
| Sortition | `PosSchedule(registry, seed)` → ordered `vector<CPubKey>` (`src/pos.h:204`) | Committee + leader ranking; each node runs it locally. |
| VRF | `src/vrf.{h,cpp}` (ECVRF-SECP256K1), `vrfprove`/`vrfverify` | A member proves its slot eligibility; peers verify. |
| Aggregation | `src/musig.{h,cpp}` — `MuSigSessionNonce` / `MuSigSessionPartialSign` / `MuSigAggregatePartials` | The interactive two-round signing the gossip would drive (but see §7). |
| Template / submit | `getposblocktemplate` / `submitposblock` internals (`src/rpc/mining.cpp`) | The leader's block assembly and the final accept path, called internally instead of over RPC. |
| Stake set | `StakeRegistry`, `StakeFromTxOut` (`src/pos.h`) | The eligible-signer set, rebuilt from the UTXO set. |

What is missing is purely the *coordination*: a thread that drives the round, a
per-height session manager, and the wire messages. No consensus *rule* changes
(except the signature-scheme question in §7, which is a deliberate decision, not
an accident).

## 1. The paper's protocol, mapped to the wire

Principle 6 defines a 12-step round. The autonomous layer is a faithful
realization of it. The mapping:

| Paper step (P6) | Autonomous-layer action | Message |
|---|---|---|
| 1. Stake, publish VF | On-chain stake registration (exists) | — (UTXO) |
| 2. Block r-1 → public seed | `ComputePosSeed` over the parent's anchor hash | — (local) |
| 3. Each participant runs VRF | Local `PosSchedule` + VRF over the seed with the node's staking key | — (local) |
| 4. Committee/leader determined; members broadcast result via proposed block | Leader(s) build and flood a **block proposal** carrying the VRF proof | `posproposal` |
| 5. Peers verify VRF outputs | On receipt, verify the proof against the registry before relaying | (relay gate) |
| 6. Single out lowest-VRF; relay only that, at the timeout | Relay rule: forward only the lowest-VRF *valid* proposal seen; reset the timer on each new certified block | `posproposal` relay rule |
| 7. New Bitcoin block ⇒ a fresher-anchored proposal may reshuffle the leader | Anchor-reshuffle: a new VRF round keyed on the fresher anchor; committee unchanged; anchor-weight favours it (§6, §8) | `posproposal` |
| 8. Every node votes for the lowest-VRF block | Members emit a **vote** for the winning proposal | `posvote` |
| 9. After timeout, if < 51 votes, re-vote round-robin | Round-robin re-vote on the proposal store after the upper-bound timeout | `posvote` (new round-id) |
| 10. Verify consensus-rule compliance *after* the first vote | Full block validation gated to the post-first-vote step; on failure, resume round-robin | (local) |
| 11. Aggregate the 51 signatures | Members flood BLS shares; any node aggregates ≥51 (§7) | `posshare` |
| 12. 51/100 ⇒ certified; its seed drives r+1 | Assemble the certificate, `submitposblock` internally, reset to step 3 | `poscert` (= the certified block) |

Three liveness/safety rules sit alongside the happy path:

- **Escaping stall (P8).** If the last certified block anchors to a Bitcoin
  block now 4 deep (height *h*), a block referencing *h+3* may certify
  sub-quorum (down to one signer). `POS_ESCAPING_STALL_ANCHOR_GAP = 3`. The
  round engine falls into this mode automatically when the upper-bound timeout
  expires repeatedly and the anchor gap is satisfied.
- **Enforce consensus (P4 / Liveness Theorem 1).** If an invalid block `b0` is
  certified by a malicious quorum, the honest non-signers may certify a valid
  `b1` at the same height with a majority of the members who did *not* sign `b0`
  (e.g. 60 sign invalid ⇒ 21 of the remaining 40 suffice). The session manager
  must therefore support forming a certificate for an *alternative* block at an
  already-occupied height, not only building forward.
- **Convergence (P8 convergence theorem).** Competing escaping-stall blocks are
  ordered by countersignature count, then VRF score; a threshold-certified block
  beats an escaping-stall one. This is already the `CBlockIndexWorkComparator` +
  finality-gate behaviour; the gossip layer only needs to *feed* it.

## 2. Component architecture

Four new pieces, all node-local, behind a `-posproducer` (run the engine) and
`-posgossip` (relay committee messages) split so a pure validator can relay
committee traffic without producing. **`-posgossip` defaults on**: every full
node relays committee traffic by default, which densifies propagation and
strengthens liveness/convergence, while the eligibility gate (§9) keeps the added
surface bounded (a node only relays messages from provably-eligible committee
members). `-posproducer` is off unless the operator stakes and opts in. An
operator may set `-posgossip=0` to stay quiet.

```
            ┌──────────────────────────────────────────────┐
            │  PoS round engine  (new thread, src/pos_net)  │
            │  - local round clock (P10)                    │
            │  - self-eligibility (PosSchedule + VRF)       │
            │  - drives the per-height state machine (§3)   │
            └───────────────┬───────────────┬──────────────┘
                            │ produces       │ consumes
                            ▼                ▼
   ┌────────────────────────────┐   ┌────────────────────────────┐
   │ Committee session manager  │   │  Validation / chainstate    │
   │ (per height/round state):  │   │  ComputePosSeed, PosSchedule │
   │  proposals[], votes[],     │   │  block accept, finality gate │
   │  nonces[]/shares[],        │   └────────────────────────────┘
   │  certificate assembly      │
   └─────────────┬──────────────┘
                 │ emit / ingest
                 ▼
   ┌────────────────────────────────────────────────────────────┐
   │ net_processing: new NetMsgType handlers + relay rules (§4)  │
   │       posproposal · posvote · posshare · poscert           │
   └────────────────────────────────────────────────────────────┘
```

The engine is event-driven: it wakes on (a) a new certified tip (reset the
clock, recompute eligibility for the next height), (b) the lower-bound timer
firing (eligible leader may propose), (c) an inbound committee message, (d) the
upper-bound timeout (enter round-robin / escaping-stall), and (e) a new Bitcoin
anchor (consider an anchor-reshuffle proposal).

## 3. The per-height round state machine

One instance per height being worked, discarded when that height certifies or
the tip advances past it.

```
        new certified tip at h-1
                  │  (reset local clock; seed = ComputePosSeed(anchor(h-1), h))
                  ▼
            ┌───────────┐   not eligible
            │ SORTITION │──────────────► OBSERVE (relay only)
            └─────┬─────┘
       eligible (committee member and/or leader rank k)
                  ▼
   lower-bound n elapsed AND leader-rank delay k·δ elapsed
            ┌───────────┐
            │  PROPOSE  │  leader: build template, flood posproposal(VRF proof)
            └─────┬─────┘
                  ▼
            ┌──────────────────┐  collect proposals; keep lowest-VRF valid one
            │ COLLECT/PROPAGATE │  (anchor-weighted §6); relay-gate by eligibility
            └─────┬────────────┘
                  ▼  (lowest-VRF settled at proposal-timeout)
            ┌───────────┐  member emits posvote for the winner
            │   VOTE    │
            └─────┬─────┘
        ≥51 votes for one proposal       upper-bound timeout, <51
                  ▼                                  │
            ┌───────────┐                            ▼
            │ VALIDATE  │ full block check     ROUND-ROBIN: re-vote on store;
            │ (post-vote)│ (P6 step 10)        if anchor gap ≥3 ⇒ ESCAPING_STALL
            └─────┬─────┘  invalid ──► back to ROUND-ROBIN
                  ▼ valid
            ┌──────────────────────────┐
            │ SIGN (§7): members flood  │  BLS posshare; any node
            │ posshare; collect ≥51     │  aggregates whichever arrive
            └─────┬────────────────────┘
                  ▼  aggregate
            ┌───────────┐  assemble certificate; submitposblock internally;
            │ CERTIFIED │  flood poscert; tip advances ⇒ engine resets to SORTITION(h+1)
            └───────────┘
```

The **VALIDATE-after-VOTE** ordering is deliberate and from the paper (P6 step
10): full block validation is the expensive step, so a node defers it until a
proposal has actually gathered a lead in votes, avoiding validating every
competing proposal.

## 4. New P2P messages

Four message types, added to `NetMsgType` (`src/protocol.{h,cpp}`) and dispatched
in `net_processing.cpp::ProcessMessage`. With BLS aggregation (§7) the signing
half is a single, non-interactive share — there is no nonce/partial round to
gossip. All messages are **eligibility-gated**: a node relays a committee message
for height *h* only if the sender is provably in *h*'s committee (its VRF proof
verifies against the registry-derived schedule), which is the core anti-DoS lever
(§9).

| Message | Payload (sketch) | Relay rule |
|---|---|---|
| `posproposal` | height, parent, anchor ref, leader pubkey, **VRF proof**, block (or block header + txids for compact relay) | Relay only the **lowest-VRF valid** proposal seen for (height, anchor-tier); supersede on a strictly-better (lower-VRF, or anchor-weighted fresher) one; one-per-leader-per-round. |
| `posvote` | height, round-robin index, proposal id, voter pubkey, signature over the vote | Relay if voter ∈ committee and not already seen; dedupe per (voter, round index). |
| `posshare` | height, proposal id, member pubkey, **BLS signature share** over the proposal's `signhash` | Relay if member ∈ committee; one per member per proposal; each share is individually verifiable. |
| `poscert` | the certified block (header carries the BLS aggregate + a signer bitmask) | Standard block relay (`cmpctblock`/`block`); this is the existing accept path. |

A `posvote` and a `posshare` can be the *same* message in practice — a member's
share over the proposal is itself a vote for it — so the implementation may fold
voting into share emission and keep `posvote` only for the round-robin re-vote
index. Proposals should use **compact relay** (announce by id via `inv`, fetch
the body with `getdata`) to avoid flooding full blocks; votes and shares are
small and flood directly with dedup. Each message is bounded to the current and
next height; anything older or further ahead is dropped, capping memory.

## 5. Self-eligibility detection

No coordinator tells a node it is selected — it learns this locally, and
privately (the schedule needs the staking secret key, so it is not publicly
predictable, which is the anti-grinding property):

1. On a new certified tip at *h-1*, compute `seed = ComputePosSeed(anchor(h-1), h)`.
2. `order = PosSchedule(registry, seed)` gives the stake-weighted committee and
   the leader ranking for *h*.
3. For each staking key the node holds, compute its VRF output over `seed`; the
   node's slot/rank tells it whether it is (a) the rank-0 leader, (b) a backup
   leader (rank *k*), and/or (c) a committee member (slot < committee size).
4. The node arms the appropriate role(s) in the state machine. A node with no
   eligible key for *h* is an `OBSERVE`-only relayer for that height.

## 6. Timing model (Principle 10)

The paper is explicit and minimal: **no NTP, no network-adjusted time.** Each
validator counts local time from the last certified block it received.

- **Slot floor `n`, target 30 s, timestamp-retargeted.** A member will not
  countersign a proposal until `n` seconds have elapsed on its local clock since
  the previous certified block — the anti-fast-frequency floor. The **target is
  30 s** (not the paper's ~90 s UX figure), pinned by the ledger-growth-parity
  invariant: the block weight cap (`-con_maxblockweight = 200,000`) satisfies
  `200,000 / 30 s = 4,000,000 / 600 s`, so a saturated Sequentia ledger grows at
  exactly Bitcoin's saturated rate. The effective floor is **retargeted to hold
  the realized average cadence at that 30 s target**, exactly as the paper
  prescribes (P10: adjust *"based on the timestamps of blocks, similar to how
  Bitcoin deals with the change of difficulty every 2016 blocks"*). Retargeting
  does **not** contradict the parity rule — it is the mechanism that makes it hold
  in practice: the weight cap fixes the *weight* half of `weight / time`, and the
  retarget steers the *time* half to 30 s, since uncorrected drift in block
  intervals would otherwise break the equality. The weight cap stays fixed; the
  cadence is driven to it.

  The retarget is computed **deterministically from block-header timestamps** over
  an epoch aligned to the paper's ~2-week / 2016-Bitcoin-block boundary (the same
  clock its lowering-staking-requirement rule uses): at each boundary, compare the
  epoch's realized average inter-block time to the 30 s target and nudge `n` up
  (blocks ran fast) or down toward a floor (slow), clamped per step like Bitcoin's
  4× difficulty limit. Because every node derives the same `n` from the same
  on-chain timestamps, the floor stays synchronized network-wide **without being a
  hard validity rule** — consistent with P10's point that the bound is a local
  clock and not publicly enforceable: a block is never rejected for `n`; nodes
  simply will not countersign before it.
- **Leader-rank stagger `k·δ`, `δ` = 3 s (default).** The rank-0 leader proposes
  at the slot boundary; a rank-*k* backup waits an extra `k·δ` and proposes only
  if it has seen no valid lower-rank proposal, so backups fill in just for missing
  leaders and the common case stays a single proposal. `δ` = 3 s is a few times
  the network-propagation time of a ≤200,000-weight block under compact relay
  (sub-second to ~1–2 s), so a backup reliably observes the primary's proposal
  before competing, while still absorbing several missing leaders well inside the
  round-robin window below.
- **Upper-bound timeout `T` ≈ 45 s (default, ≈ 1.5 · n).** If no proposal has
  gathered 51 shares within `T` of the last certified block, the engine begins
  **round-robin re-voting** on the proposal store (P6 step 9), incrementing a
  round-robin index that namespaces the votes. `T − n` ≈ 15 s ≈ 5 · δ absorbs on
  the order of five missing/forked leaders plus share propagation before the
  recovery path engages. Round-robin is the *fast* recovery (seconds); the
  *slow* recovery, escaping-stall sub-quorum certification, only becomes
  available once the Bitcoin anchor has advanced +3 (≈ 30 min of Bitcoin), so the
  two operate on very different timescales.

`δ` and `T` are free local heuristics (configurable defaults). `n` is special:
its 30 s target is tied to the weight cap by the parity rule, and its retargeted
value is derived deterministically from on-chain timestamps so every node agrees
— but, like all the paper's bounds, compliance is soft (a local clock), never a
hard validity check.
- **Anchor reshuffle (P7).** On learning of a new Bitcoin block not referenced by
  the parent, an eligible node may propose a fresher-anchored block; its VRF is
  compared under an **anchor-weighting coefficient** that favours the fresher
  anchor. This is the *same* weight already used as the committee signing
  preference — a fixed local commit-timing weight (~0.3× quorum, +15 at 51/100)
  that steers convergence but **never counts toward the 51-signature finality
  threshold** ([`../04-proof-of-stake.md`](../04-proof-of-stake.md) §7). The
  autonomous layer is where this weight finally has a job: tipping which of two
  not-yet-certified proposals the committee converges on.

None of these bounds is consensus-enforced (they are local clocks); they are
incentive-shaped, exactly as the paper argues (propose too early → fewer fees;
too late → risk being replaced by a lower-VRF competitor).

## 7. Signature scheme: BLS aggregate (decided)

The autonomous committee certifies blocks with **BLS aggregate signatures
(BLS12-381), non-interactively.** MuSig2 (BIP327) is retained unchanged for the
single-host and coordinator paths, where it is ideal. This resolves what was the
proposal's pivotal open question.

### Why not MuSig2 for the autonomous path

MuSig2 is an *n-of-n*, *two-round interactive* aggregate. To realize 51-of-100 it
must aggregate *exactly* the 51 chosen signers, which on the wire means: fix the
signer subset, gossip round-1 `posnonce` from all 51, gossip round-2 `pospartial`
from all 51, then aggregate. In an open, lossy committee this is brittle: because
it is n-of-n over the chosen subset, a single member failing to send its nonce or
partial fails the whole aggregate, forcing the engine to pick a *different*
51-subset and restart both rounds. In a 100-member committee where ~30% may be
offline, subset selection becomes guesswork and restarts add latency. It is a
fine scheme for a *known, reliable* set (the coordinator path); it fights the
gossip model.

### Why BLS

BLS aggregation is **non-interactive**, which is exactly what the paper called for
(P6 step 11: *"any party can aggregate signatures after the broadcast without
communicating with the original signers"*). Each committee member signs the
proposal independently and floods a single `posshare`; **any** node aggregates
whichever ≥51 shares arrive — no subset pre-commitment, no second round, and the
aggregate is a single constant-size group element regardless of signer count.
This makes the signing half of the protocol robust to offline members by
construction: you collect whoever responds rather than betting on a fixed 51.

### On forward security — checkpoints are the accepted defense

BLS keys are long-lived and not forward-secure, so the paper's Principle 11
*posterior corruption* concern (old blocksigner keys sold and reused for a
long-range attack) is **not** addressed by the signature scheme itself. That is a
deliberate, accepted trade-off here: Sequentia's long-range defense is the
**Bitcoin-anchored checkpoint system** (a checkpoint consolidates after 2016
Bitcoin confirmations ≈ 2 weeks and is then irreversible locally), combined with
the rule that the **stake locktime exceeds the checkpoint depth** so that a
signer's keys still control bonded stake throughout the window in which their
signatures could rewrite history. Within that finalized horizon, leaked
historical keys cannot reorganize the chain. The paper's forward-secure option,
**Pixel**, is therefore **not adopted**; it would buy protocol-level forward
security at the cost of evolving-key operations for every staker (per-period key
update, mandatory secure deletion, heavier audit) — redundant given the
checkpoint guarantee. Pixel remains a *possible future upgrade* (it is "BLS plus
key evolution," so this BLS design is forward-compatible toward it), but it is
out of scope for this work.

### Implementation notes

- New `src/bls.{h,cpp}` wrapping a vetted BLS12-381 library (e.g. `blst`):
  sign, verify, aggregate-signatures, aggregate-verify, fast-aggregate-verify.
- Each staker commits a **BLS public key** alongside its stake (in or bound to
  the staking output) with a **proof-of-possession** to close the BLS rogue-key
  attack; the committee's expected signer set for a height is the BLS keys of the
  sortitioned members, so a node verifies the aggregate against the
  fast-aggregate of the present signers' keys plus a signer bitmask.
- This is a consensus change (a new certification scheme + the per-staker key
  commitment), so it is gated behind a chain flag, never active on the existing
  bundled chains until a planned activation, and shipped on testnet first.
- The wire change vs. the MuSig2 sketch in §4: the two-message `posnonce` +
  `pospartial` exchange collapses to a single `posshare` (member pubkey + BLS
  signature share), and `poscert` carries the aggregate + signer bitmask.

## 8. Equivocation, enforce-consensus, convergence

- **Double-signing.** A member emitting `posvote`/`posshare` for two distinct
  proposals at the same height is equivocating. There is *no slashing* by design
  (the paper, P7): the defence is enforce-consensus + checkpoints. The gossip
  layer should still propagate the conflicting pair as *evidence* (useful for
  monitoring and for the comparator/finality-gate which already reject a
  competitor below a certified block) and score the peer.
- **Enforce-consensus (LT1).** The session manager keeps, per height, the set of
  members who signed each certified-but-invalid block, so the honest remainder
  can assemble an alternative certificate (majority of the non-signers). This is
  the one place the manager forms a certificate at an *already-occupied* height.
- **Convergence.** Purely a fork-choice concern, already handled by the
  comparator + immediate-finality gate; the gossip layer only supplies the
  competing certified blocks and lets validation decide.

## 9. Anti-DoS and validation

The committee gossip is a new inbound surface, so every rule is designed to be
*cheap to reject and eligibility-gated*:

- **Eligibility gate.** Relay a `posproposal`/`posvote`/`posshare` only if the
  sender's pubkey is in the height's committee (VRF proof in the proposal; for
  votes/shares, the pubkey must match a committee slot). Non-committee chatter is
  dropped without further work.
- **Bounded windows.** Accept messages only for the current height and the next
  (anchor-reshuffle look-ahead); drop everything else. Memory is O(committee ×
  small) per height and freed on tip advance.
- **Per-member, per-round dedupe and rate limits.** One proposal per leader per
  round-robin index; one vote/share per member per round index; excess is
  misbehaviour-scored.
- **Validate lazily.** Cheap checks (eligibility, structural, signature-share
  verification) gate relay; full block validation runs once, post-vote (P6 step
  10), as already noted.
- **Compact proposal relay** (inv/getdata) bounds proposal bandwidth.

## 10. Concrete integration points

| Change | Where |
|---|---|
| Round engine + session manager + local clock | new `src/pos_net.{h,cpp}` |
| Message type constants | `src/protocol.{h,cpp}` (`NetMsgType`) |
| Receive/relay handlers | `src/net_processing.cpp` (`ProcessMessage` cases; relay in the send loop) |
| Start/stop the producer thread | `src/init.cpp` behind `-posproducer` / `-posgossip` |
| Internal template/submit (no RPC hop) | factor the bodies of `getposblocktemplate`/`submitposblock` (`src/rpc/mining.cpp`) into callable helpers the engine shares |
| Signature scheme (autonomous) | new `src/bls.*` (BLS12-381) + a per-staker BLS key commitment with proof-of-possession (§7); `src/musig.*` retained unchanged for the coordinator path |
| Eligibility / schedule | reuse `PosSchedule`, `ComputePosSeed`, `src/vrf.*` unchanged |

The RPCs stay as-is: they remain the coordinator path and the test surface, and
the engine calls the same underlying helpers, so there is one code path for block
assembly and acceptance.

## 11. Compatibility and rollout

- The coordinator/RPC path is untouched and remains supported; the autonomous
  engine is opt-in (`-posproducer`).
- The BLS certification scheme (§7) is a consensus change behind a chain flag,
  never active on the existing bundled chains until a planned activation;
  testnet first.
- A new functional test, `feature_pos_autonomous_committee.py`, spins up *N*
  nodes (no coordinator), each holding one staking key, and asserts they produce
  and certify a chain end-to-end, including the offline-member and
  anchor-reshuffle cases. This mirrors `feature_pos_distributed_committee.py` but
  with the orchestration on the wire instead of in the test harness.

## 12. Phased delivery

1. **Engine + self-eligibility + autonomous production (committee ≤ 1).**
   ✅ *Implemented* (`src/pos_producer.{h,cpp}`, `-posproducer`/`-posproducerkey`;
   test `feature_pos_autonomous_producer.py`). A node with one or more staking
   keys elects the best-ranked key each round, waits out the slot clock (with the
   soft cadence floor), and assembles/signs/submits a block with no coordinator;
   the produced blocks propagate and validate via the normal block-relay path, so
   no new wire message is needed at this phase. The shared `ProducePosBlock()`
   core also backs `generateposblock`. Proves the thread, the clock, sortition,
   and the accept path.
2. **Voting + aggregation to 51/100.** Add `posvote`, the chosen
   signature-share gossip (§7), and certificate assembly. Multi-node committee
   produces threshold-certified blocks with no coordinator.
3. **Anchor reshuffle + round-robin + enforce-consensus.** Add P7 fresher-anchor
   proposals with the anchor weight, the round-robin re-vote, and the LT1
   alternative-certificate path.
4. **Hardening.** DoS rules (§9), equivocation evidence, and propagation/latency
   tuning under realistic offline-member rates.

Each phase is independently testable and leaves the coordinator path working.

## 13. Decisions

**Resolved — signature scheme (§7).** The autonomous committee certifies with
**BLS aggregate signatures (BLS12-381), non-interactively**; MuSig2 is retained
for the single-host/coordinator path. Pixel / protocol-level forward security is
**not** adopted: Sequentia's accepted long-range defense is the Bitcoin-anchored
checkpoint system (2016-confirmation consolidation) together with a stake
locktime that exceeds the checkpoint depth.

**Resolved — timing (§6).** The slot **target is 30 s**, pinned by the
ledger-growth-parity invariant (`200,000 weight / 30 s = 4,000,000 / 600 s`) and
**held there by a timestamp-based retarget** as the paper prescribes (P10,
Bitcoin-difficulty-style, on the ~2-week / 2016-block epoch) — the retarget is
what makes parity hold in practice, not a departure from it. Leader-rank stagger
**`δ` = 3 s** and round-robin timeout **`T` ≈ 45 s (≈ 1.5 · n)** as local-clock
defaults.

**Resolved — gossip default (§2).** **`-posgossip` defaults on**: every full node
relays committee traffic, bounded by the eligibility gate; producing
(`-posproducer`) remains opt-in.

All design decisions for the autonomous committee are now settled; the remaining
work is implementation per the phased plan (§12).
