# Single-signer committee blocks on the testnet — investigation & recommendations

*For Alberto (consensus). Drafted 2026-06-24 from a live investigation of the public
testnet committee (100 nodes on one box, anchored to Bitcoin testnet4).*

---

## TL;DR

On the testnet we observed occasional **single-signer (sub-quorum) committee blocks
landing on the canonical chain** — visible in the block explorer, interleaved with
normal blocks, and recognisable at a glance by their much smaller weight (a near-empty
committee certificate). They are valid **escaping-stall** blocks (whitepaper §3.8).

Two *distinct* things are producing them:

1. **Genuine committee stalls (9–34 min)** — the 100-node committee really does fail to
   reach quorum for stretches, and escaping-stall correctly keeps the chain live. Root
   cause is environmental: Bitcoin **testnet4 is very reorg-heavy** and the demo box is
   resource-pressured. Not a consensus bug.
2. **False triggers during perfectly healthy operation** — when testnet4 emits ≥3 blocks
   inside a single ~30–60 s Sequentia slot, the anchor jumps +3, that *flips on*
   escaping-stall, and a producer then **self-certifies solo instead of waiting ~1 s for
   the quorum it could easily have gathered.** This is a producer-side behaviour, not a
   consensus rule, and it's cleanly fixable node-side.

**Safety:** none of these blocks grant false finality — a sub-quorum block carries fewer
than `quorum` countersignatures, so it never advances the immediate-final point; it is a
provisional tip until a real quorum block extends it. So there is no consensus-safety
problem here. The headline item for your call is whether to adopt the node-side
"collaborate-first" producer change (below), which removes case 2 entirely while keeping
the escaping-stall rule, the 3-block threshold, and *Bitcoin-as-the-source-of-time* all
unchanged.

---

## Relevant mechanics (as currently implemented)

- **Committee / quorum.** `poscommitteesize = 100` on the testnet; the certification
  quorum is a strict majority, `PosQuorum(m) = m/2 + 1 = 51` (`src/pos.cpp:138`).
- **Immediate finality.** The finalized point is the highest active-chain block whose
  countersignature count reaches quorum:
  `g_pos_immediate_final = highest f with f->m_pos_countersigs >= quorum`
  (`src/validation.cpp:3143-3152`; the `>= quorum` test is at `:3147`). The comment is
  explicit: *"Escaping-stall / leader-only (sub-quorum) tips simply leave no
  immediate-final point until a quorum block is connected."* → **sub-quorum blocks do
  not finalize.**
- **Escaping stall (whitepaper §3.8).** A block may be certified below quorum — down to
  **one** member — iff the parent (Bitcoin) chain has advanced
  `POS_ESCAPING_STALL_ANCHOR_GAP = 3` blocks past the parent block's anchor:
  `PosEscapingStallAllowed(parent_anchor_h, block_anchor_h)` (`src/pos.h:347`).
  Enforced in both certification paths: `min_members = escaping_stall ? 1 : quorum`
  (`src/validation.cpp:2236` for the MuSig/agg path, `:2292` for the BLS path). The
  measure is a Bitcoin **block count** — deterministic, every node agrees, and Bitcoin
  is the source of time by design.
- **Fork choice.** A quorum block outranks an escaping-stall block at the same height
  (`src/validation.cpp:132`).
- **Producer submit rule.** Each round the leader/members validate the proposal, make
  and flood their own share, then any node assembles + submits once it holds
  `min_members` shares: `if (m_collected.size() >= min_members) { assemble; submit }`
  with `min_members = escaping_stall ? 1 : quorum` (`src/pos_producer.cpp:1018`).

---

## What we observed (evidence)

Scan of the last 250 canonical blocks on `node000` (committee size 100, quorum 51):

- **~6 of 250 (~2.4%) are sub-quorum**, and *every* one has **anchor gap ≥ 3**
  (observed gaps: 3, 5, 6, 7, 9). Since validation rejects a sub-quorum block unless
  escaping-stall is allowed (gap ≥ 3), these are escaping-stall blocks on the canonical
  chain — there is no validation bypass.
- **Weight tells them apart:** normal blocks ≈ **15,394** wu (≈100 member commitments in
  the coinbase); the sub-quorum blocks ≈ **2,344** wu, `nTx = 1`.

Real Sequentia inter-block pause at each sub-quorum block (from SEQ's own timestamps;
normal block time is ~30–60 s):

| block | real SEQ gap | anchor jump | interpretation |
|-------|--------------|-------------|----------------|
| 6486  | 170 s (~3 min)   | +6 | borderline |
| 6571  | **2044 s (~34 min)** | +7 | **genuine stall** |
| 6581  | **60 s**         | +3 | **false trigger (healthy)** |
| 6594  | **1353 s (~22 min)** | +5 | **genuine stall** |
| 6621  | 520 s (~9 min)   | +9 | genuine stall |
| 6626  | **30 s**         | +3 | **false trigger (healthy)** |

So blocks 6581/6626 were **not** stalls at all (30–60 s is a normal slot); blocks
6571/6594 were real multi-minute quorum failures.

Environment at the time:
- **testnet4 is heavily reorg-prone:** `getchaintips` reported **1 active, 48 valid-fork,
  93 valid-headers** — i.e. ~48 fork branches seen on the parent chain.
- **Box pressure:** load average **~21**; **441 MB** free RAM (31 GB total, 21 GB used,
  no swap); running 100 × elementsd + bitcoind(testnet4) + electrs + explorer.
- testnet4 block intervals over the relevant span were wildly irregular: +83 min, +0 min
  (two blocks the same second), +3, +20 — avg ≈ 3 min/block but non-monotonic timestamps.

---

## Analysis

### Case 1 — genuine stalls (environmental)
The committee genuinely loses quorum for 9–34 min at times. The most consistent
explanation: testnet4's frequent reorgs (48 branches) repeatedly trip **anchoring
supremacy** — the anchor-watcher invalidates the SEQ blocks whose Bitcoin anchor was
orphaned, and the committee must rebuild, re-anchor, and re-reach quorum. On a box at
load ~21 with little free RAM, that recovery occasionally drags into the tens of minutes.
Escaping-stall does exactly its job here (liveness), with no false finality. This is a
**demo-environment** artifact (unstable parent + 100 nodes crammed on one host), not a
protocol defect; on a properly-resourced committee with a stable Bitcoin parent it would
not occur.

### Case 2 — false triggers (a producer behaviour worth fixing)
Blocks 6581/6626 happened during healthy operation: a single ~30–60 s slot in which
testnet4 *happened* to burst ≥3 blocks. That pushed the new block's anchor +3, which
made escaping-stall *allowed*, and the producer then took it. The reason it went **solo**
rather than collaborating:

> In the round, any sortition-eligible member that has signed (not only the round leader)
> validates the proposal, adds **its own** share to the collected set, floods it, and *in
> the same pass* checks `collected >= min_members`. With `min_members = quorum (51)` that
> test fails on 1 share, so it **waits** and the quorum assembles as peers' shares arrive
> — that wait is what forces collaboration. With escaping-stall allowed, `min_members = 1`,
> so the test passes immediately on its own share and it **submits before any peer share
> has had time to arrive** (with whatever it has collected — typically just itself, which
> is the weight-2344 single-member block we see).

So the node did not "prefer" solo by intent — the escape path simply lowers the submit
bar to 1 and fires at once. That's correct when the committee genuinely *can't* reach
quorum (waiting is pointless), but premature during a healthy burst where waiting ~1 s
would have produced a normal quorum block.

Note this is **specific to testnet4's min-difficulty bursts**. On Bitcoin **mainnet**, 3
blocks inside a ~30–60 s window is astronomically unlikely (10-min Poisson spacing), so
escaping-stall there only ever fires on a real ~30-min stall — the 3-block threshold is
correctly calibrated for the actual target chain.

---

## Recommendations

1. **Collaborate-first producer change (node-side, no consensus change) — recommended.**
   When escaping-stall is *allowed*, keep targeting a quorum within a short grace window
   (a couple of seconds — far below any real stall, far above healthy gossip latency) and
   only fall back to a sub-quorum/solo block if the quorum does not assemble. The
   escaping-stall *rule*, the 3-block threshold, the validation paths, and Bitcoin-as-time
   are all untouched — this only changes *when a producer chooses to escape*; it is low
   risk and self-contained (`src/pos_producer.cpp`). Effect:
   - healthy burst → it waits ~1 s, gets the quorum, emits a normal full-quorum block
     (just anchored +3) → **case 2 disappears**;
   - genuine stall → quorum never forms, so after the grace it escapes → **case 1
     liveness preserved**.

2. **Committee health (case 1) — operational, not code.** Reduce reorg-recovery stalls by
   relieving the environment: more box resources / fewer co-located services, and/or a
   smaller committee for the demo, and/or a steadier testnet4 source. Worth deciding how
   much we care for a demo, since escaping-stall already keeps the chain live and final
   safety is intact.

3. **Considered and rejected: a wall-clock / median-time-past stall threshold.** It would
   make the trigger robust to bursty parents, but it contradicts the design principle
   that **Bitcoin (block count) is Sequentia's source of time**, so we are not pursuing
   it. Recorded here only so the option is on the table for your judgement.

4. **Operational aside:** a laptop staking node running as a *producer* is the most likely
   node to win these solo races (it self-certifies its own share instantly) and, when it
   drops connectivity, to escape on its own. Keeping flaky/part-time nodes as
   stakers/validators rather than producers reduces both, independent of the above.

---

## Open questions for Alberto

- Is the **collaborate-first** producer behaviour desirable, or do you prefer the current
  "escape the instant it's allowed" (simpler, maximally liveness-biased)?
- Are you comfortable that sub-quorum escaping-stall blocks **never finalize** (they only
  ever sit as provisional tips until a quorum block extends them), i.e. that the weaker
  guarantee during a burst is acceptable and needs no further mitigation?
- For the demo testnet specifically: do we want to invest in committee-health
  (resources / committee size / parent stability), or accept occasional escaping-stall
  blocks as expected given the environment?
