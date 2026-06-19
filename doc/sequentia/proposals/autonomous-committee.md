# Design proposal — the autonomous gossip-and-sign committee

> **Status: proposal / RFC, not implemented.** This document specifies the
> design for Sequentia's autonomous (coordinator-free) Proof-of-Stake committee.
> It is a planning artifact, distinct from the as-built specification in
> [`../04-proof-of-stake.md`](../04-proof-of-stake.md). Where it proposes a
> change to a shipped mechanism (notably the signature scheme, §7) that is
> called out explicitly.

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
| 11. Aggregate the 51 signatures | Gossip the signature shares and aggregate (§7) | `posnonce`/`pospartial` or `posshare` |
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
`-posgossip` (relay/participate in committee messages) split so a pure validator
can relay committee traffic without producing:

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
   │   posproposal · posvote · posnonce · pospartial · poscert   │
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
            │ SIGN (§7): collect shares │  posnonce+pospartial (MuSig2)  OR
            │ until 51 signers present  │  posshare (non-interactive)
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

Five message types, added to `NetMsgType` (`src/protocol.{h,cpp}`) and dispatched
in `net_processing.cpp::ProcessMessage`. All are **eligibility-gated**: a node
relays a committee message for height *h* only if the sender is provably in *h*'s
committee (its VRF proof verifies against the registry-derived schedule), which
is the core anti-DoS lever (§9).

| Message | Payload (sketch) | Relay rule |
|---|---|---|
| `posproposal` | height, parent, anchor ref, leader pubkey, **VRF proof**, block (or block header + txids for compact relay) | Relay only the **lowest-VRF valid** proposal seen for (height, anchor-tier); supersede on a strictly-better (lower-VRF, or anchor-weighted fresher) one; one-per-leader-per-round. |
| `posvote` | height, round-robin index, proposal id, voter pubkey, signature over the vote | Relay if voter ∈ committee and not already seen; dedupe per (voter, round index). |
| `posnonce` | height, proposal id, member pubkey, 66-byte BIP327 public nonce *(MuSig2 path only)* | Relay if member ∈ the chosen signer subset; one per member per proposal. |
| `pospartial` | height, proposal id, member pubkey, 32-byte partial sig *(MuSig2 path)* **or** `posshare`: a 48/96-byte non-interactive signature share *(BLS/Pixel path, §7)* | As above; partials/shares are verifiable individually. |
| `poscert` | the certified block (header carries the 64-byte aggregate + the committee commitment) | Standard block relay (`cmpctblock`/`block`); this is the existing accept path. |

Proposals should use **compact relay** (announce by id via `inv`, fetch the body
with `getdata`) to avoid flooding full blocks; votes/nonces/partials/shares are
small and can flood directly with dedup. Each message is bounded to the current
and next height; anything older or further ahead is dropped, capping memory.

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

- **Lower bound `n`** (the slot interval): a member will not countersign a
  proposal until `n` seconds have elapsed on its local clock since the previous
  certified block. This is the anti-fast-frequency floor. The current code uses
  `-posslotinterval = 30`; the paper's worked target is ~90 s. *Decision point
  (§13):* which target, and whether it is fixed or auto-adjusted toward a target
  the way Bitcoin retargets difficulty.
- **Leader-rank stagger `k·δ`.** The rank-0 leader may propose first; a rank-*k*
  backup waits an extra `k·δ` before proposing, so backups only fill in when the
  primary is silent. This keeps the common case to a single proposal.
- **Upper-bound timeout `T`.** If a proposal has not gathered 51 votes within `T`
  of the last certified block, the engine begins **round-robin re-voting** on the
  proposal store (P6 step 9), incrementing a round-robin index that namespaces
  the votes.
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

## 7. The pivotal decision: interactive MuSig2 vs. non-interactive aggregation

This is the single most consequential choice, because it determines the shape of
the signing half of the gossip protocol.

**The shipped scheme is MuSig2 (BIP327)** — an *n-of-n*, *two-round interactive*
aggregate. To realize 51-of-100 it aggregates *exactly* the 51 chosen signers.
On the wire that means: fix the signer subset, gossip round-1 `posnonce` from all
51, then gossip round-2 `pospartial` from all 51, then aggregate. Two problems in
an open, lossy gossip setting:

1. **Brittleness to drop-out.** Because it is n-of-n over the chosen subset, if
   even one of the 51 fails to send its nonce or partial, the aggregate cannot
   complete — the engine must pick a *different* 51-subset and restart both
   rounds. In a 100-member committee where ~30% may be offline, subset selection
   becomes a guessing game and restarts add latency.
2. **No forward security.** MuSig2 keys are long-lived; the paper's Principle 11
   flags *posterior corruption* (old keys sold and reused for a long-range
   attack) and explicitly names **Pixel** (a forward-secure, BLS-based multisig)
   as the mitigation.

The paper anticipated exactly this. P6 step 11: *"a Pixel multi-signature scheme
could be used to support non-interactive aggregation so that any party can
aggregate signatures after the broadcast without communicating with the original
signers."* That is the natural fit for gossip: each member signs the proposal
independently and floods one `posshare`; **any** node aggregates whichever ≥51
shares arrive, with no subset pre-commitment and no second round.

Options:

- **A — Keep MuSig2, gossip both rounds.** *Pro:* zero new crypto; already in the
  tree; 64-byte signature. *Con:* the n-of-n brittleness above; two interactive
  rounds; not forward-secure. Best suited to a *known, reliable* committee — i.e.
  the coordinator path, not an open one.
- **B — BLS aggregate (non-interactive).** Each member BLS-signs independently;
  collect any 51 shares; aggregate. *Pro:* robust to drop-out (collect whoever
  responds), single round, anyone aggregates — matches the paper. *Con:* new
  curve (BLS12-381) and a per-staker BLS key registered alongside the stake;
  larger keys; pairing verification cost (amortized by aggregation).
- **C — Pixel (forward-secure BLS multisig).** Option B plus key evolution per
  height, giving forward security against posterior corruption. *Pro:* the
  paper's named choice; non-interactive *and* long-range-attack-resistant. *Con:*
  the most new machinery (key-update schedule, registry of evolving public keys).

**Recommendation.** Keep MuSig2 for the single-host/coordinator path (it is ideal
there). For the *autonomous* committee, adopt a **non-interactive aggregate**:
ship **Option B (BLS)** first because it removes the round-2 gossip and the
subset-restart problem outright, then layer **Pixel key-evolution (Option C)** as
the forward-secure upgrade once the gossip layer is proven. This is a consensus
change (a new signature scheme and a per-staker aggregate-key commitment), so it
must be gated behind a chain flag and shipped on testnet first. If we instead
keep Option A, the autonomous layer is viable but will be markedly less robust to
offline members and will leave the posterior-corruption gap that Principle 11
calls out.

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

- **Eligibility gate.** Relay a `posproposal`/`posvote`/`posnonce`/`pospartial`/
  `posshare` only if the sender's pubkey is in the height's committee (VRF proof
  in the proposal; for votes/shares, the pubkey must match a committee slot).
  Non-committee chatter is dropped without further work.
- **Bounded windows.** Accept messages only for the current height and the next
  (anchor-reshuffle look-ahead); drop everything else. Memory is O(committee ×
  small) per height and freed on tip advance.
- **Per-member, per-round dedupe and rate limits.** One proposal per leader per
  round-robin index; one vote/nonce/share per member per round index; excess is
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
| Signature scheme | reuse `src/musig.*` (Option A) **or** add `src/bls.*` + a staker aggregate-key commitment (Option B/C) |
| Eligibility / schedule | reuse `PosSchedule`, `ComputePosSeed`, `src/vrf.*` unchanged |

The RPCs stay as-is: they remain the coordinator path and the test surface, and
the engine calls the same underlying helpers, so there is one code path for block
assembly and acceptance.

## 11. Compatibility and rollout

- The coordinator/RPC path is untouched and remains supported; the autonomous
  engine is opt-in (`-posproducer`).
- If a non-interactive scheme (Option B/C) is chosen, it is a consensus change
  behind a chain flag, never active on the existing bundled chains until a
  planned activation; testnet first.
- A new functional test, `feature_pos_autonomous_committee.py`, spins up *N*
  nodes (no coordinator), each holding one staking key, and asserts they produce
  and certify a chain end-to-end, including the offline-member and
  anchor-reshuffle cases. This mirrors `feature_pos_distributed_committee.py` but
  with the orchestration on the wire instead of in the test harness.

## 12. Phased delivery

1. **Engine + self-eligibility + proposal gossip (happy path, committee = 1).**
   A single eligible node detects leadership, proposes, and certifies via the
   escaping-stall/sub-quorum path — fully autonomous, no signing committee yet.
   Proves the thread, the clock, sortition, `posproposal`, and the accept path.
2. **Voting + aggregation to 51/100.** Add `posvote`, the chosen
   signature-share gossip (§7), and certificate assembly. Multi-node committee
   produces threshold-certified blocks with no coordinator.
3. **Anchor reshuffle + round-robin + enforce-consensus.** Add P7 fresher-anchor
   proposals with the anchor weight, the round-robin re-vote, and the LT1
   alternative-certificate path.
4. **Hardening.** DoS rules (§9), equivocation evidence, and — if Option C —
   Pixel key evolution for forward security.

Each phase is independently testable and leaves the coordinator path working.

## 13. Decisions needed before coding

1. **Signature scheme (§7) — the pivotal one.** A (keep interactive MuSig2),
   B (non-interactive BLS), or C (forward-secure Pixel). Recommendation: B now,
   C later; keep MuSig2 for the coordinator path.
2. **Slot target.** Keep `-posslotinterval = 30 s`, or move toward the paper's
   ~90 s; fixed or retargeted toward a moving average.
3. **Leader-rank stagger `δ`** and the upper-bound timeout `T` (their ratio sets
   how aggressively backups and round-robin kick in).
4. **Forward security now or later** (couples to decision 1).
5. **Validator participation default.** Should a non-staking full node relay
   committee gossip by default (`-posgossip` on) to strengthen propagation, or
   stay quiet unless it produces?
