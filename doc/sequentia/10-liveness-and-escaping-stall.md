# Liveness & escaping-stall — design for the anchor-driven model

> **Status: partially implemented.** The **escaping-stall sub-threshold
> certification** (§3.8, stages 1–2 below) is implemented and tested
> (`feature_pos_escaping_stall.py`, `pos_tests.cpp pos_escaping_stall_gap`).
> What remains is the *deterministic fork-choice preference* (full-threshold
> beats sub-threshold; lowest-VRF tiebreak — stages 3–5), which is a finality
> refinement, **not** a safety requirement (see §6). This doc specifies the
> whole model; §6 records exactly what is done vs. pending.

## 1. What exists today

The implemented PoS (doc 06/07) certifies a block when a committee quorum
(strict majority of `-poscommitteesize`) countersigns it, and gates *timing*
with a **wall-clock** rule: a block is rejected (`bad-posvrf-early`) if
`block.nTime < parent.nTime + slot · -posslotinterval`, where `slot` is the
leader's VRF sortition slot. This is the doc-06 base-layer liveness, carried
into the VRF model, and doc 06 §5 flags it as a deliberate PoC simplification.

Two things diverge from the whitepaper:

1. **Liveness is wall-clock, not anchor-driven.** The paper derives the
   liveness clock from the Bitcoin anchor's progression, not node wall time.
2. **There is no escaping-stall path.** If a quorum of the sortitioned
   committee is unavailable, the chain simply stalls; the paper allows a
   *sub-threshold* block once Bitcoin has advanced far enough.

## 2. The whitepaper rules (§3.8)

> If the last certified Sequentia block references a Bitcoin block at a depth of
> 4 Bitcoin blocks in the past (height `h`), a new Sequentia block can be
> certified **without reaching the countersignature threshold** if it
> references the hash of a Bitcoin block at height `h + 3`.

So the liveness clock is the parent chain: when the chain has not advanced
while Bitcoin has moved on (the last certified block's anchor is now 4 deep),
a block that re-anchors forward (to `h + 3`) may be certified by **fewer than
quorum** committee members — escaping the stall.

**Fork choice among the resulting candidates** (§3.8):

1. A block with **more countersignatures** beats one with fewer (so a
   full-threshold block always beats an escaping-stall block).
2. On an **equal** count, the block whose leader has the **lowest VRF score**
   wins; the other is orphaned.
3. An escaping-stall (sub-threshold) block **loses** to any block that meets
   the normal threshold at the same height.

**Committee floor (§3.8, related):** if participation drops so far that a
100-member committee cannot be formed, the minimum-stake requirement is lowered
to keep the committee populated. (Interacts with `-posminstake`, doc 06 §5.)

## 3. Mapping onto the code

| Rule | Where it lands |
|---|---|
| Anchor-depth liveness (replace wall-clock `bad-posvrf-early`) | `CheckPosStakeRules` (`validation.cpp`): compute the last-certified block's anchor depth on the parent chain (via `src/anchor.{h,cpp}` / `m_anchor_height`) instead of comparing `nTime`. |
| Sub-threshold (escaping-stall) certification | `CheckPosStakeRules` + `CheckProof` (`block_proof.cpp`): allow a named/aggregate committee below `PosQuorum` **iff** the escaping-stall anchor condition holds; otherwise keep requiring quorum. The aggregate-committee MuSig path must carry the actual signer set used, and the block must record it was certified under the stall rule (a coinbase marker, like `SEQVRF`/`SEQCMT`). |
| Fork choice (countersig count, then VRF score; stall < threshold) | the active-chain comparison. Elements signed-block "work" is height (first-seen wins ties); this must change to a PoS-specific comparator that orders same-height blocks by (is-threshold, countersig count, lowest VRF). This is the subtlest and highest-risk part — it touches `CBlockIndex`/`CChainState::ConnectTip` fork selection. |
| Committee floor lowering | `PosIsEligibleStake` / the registry: a dynamic floor when the eligible set can't fill a committee. |

## 4. Open design questions (decide before coding)

1. **Anchor-depth determinism.** "Depth 4 of the last certified block's anchor"
   must be computed identically on every node. The node already follows the
   parent chain (anchor watcher); the depth must be read from a *consensus*
   view of the parent (the block's own committed anchor heights), not a local
   RPC snapshot, or honest nodes will disagree. Likely: derive depth purely
   from anchor heights committed in the SEQ chain (`tip.m_anchor_height` vs the
   new block's `m_anchor_height`), avoiding any live parent query in the
   validity rule.
2. **`h + 3` with the existing monotonic-anchor rule.** Anchoring already
   requires non-decreasing anchor heights (doc 03). The escaping-stall block
   "references `h + 3`" must be reconciled with that rule (it is a forward
   jump, which monotonicity already permits).
3. **Sub-threshold MuSig.** The aggregate path commits to the *exact* member
   set (the challenge's `agg_key` must equal the aggregate of the named set).
   An escaping-stall block has a *smaller* set; the block must name exactly the
   members who signed, and quorum enforcement becomes conditional. Keep the
   "named set == agg_key" invariant; only relax the count.
4. **Fork-choice rewrite risk.** Changing same-height selection is the riskiest
   change in the codebase (determinism across nodes, reorg safety). It needs
   its own focused testing (competing-block scenarios) before anything else.
5. **Wall-clock retirement.** Whether to drop the `nTime` slot-gate entirely or
   keep it as a secondary bound. The paper uses the anchor clock; the wall-clock
   gate can likely be removed once the anchor clock is in.

## 5. Implementation plan (staged, each independently testable)

1. **Consensus-view anchor depth.** A pure function: given a block index and
   its committed anchor height, the parent-chain depth of that anchor relative
   to the new block's committed anchor — computed only from SEQ-committed data.
   Unit-test it.
2. **Escaping-stall certification rule** in `CheckPosStakeRules` /`CheckProof`:
   permit sub-threshold certification under the depth-4/`h+3` condition, with a
   coinbase marker; keep quorum required otherwise. Functional test: a stalled
   committee produces a valid sub-threshold block only after the anchor
   advances.
3. **PoS fork choice**: same-height ordering by (threshold?, countersig count,
   lowest VRF). This is the deep change — gate it behind extensive
   competing-block functional tests across multiple nodes.
4. **Replace the wall-clock gate** with the anchor clock; retire
   `bad-posvrf-early` (or redefine it in anchor terms).
5. **Dynamic committee floor** (lowering `-posminstake` when participation is
   short). Lower priority; the static floor (doc 06 §5) is the base.

Each stage is a separate commit with its own tests; stage 3 (fork choice)
should not merge until the competing-block tests are exhaustive, because it is
the one change that can split consensus if it is non-deterministic.

## 6. Implemented vs. pending

**Implemented (stages 1–2):**

- `PosEscapingStallAllowed(parent_anchor_height, block_anchor_height)`
  (`src/pos.h`): the pure, deterministic `h + 3` condition (gap
  `POS_ESCAPING_STALL_ANCHOR_GAP = 3`), computed only from SEQ-committed anchor
  heights — no live parent query in the validity rule, so all nodes agree.
- `CheckPosStakeRules` (aggregate-committee path): the named-member quorum is
  relaxed to a single member when the stall condition holds; the full
  strict-majority quorum is required otherwise. The block stays a valid
  aggregate-committee block (every named member sortition-eligible, `agg_key`
  equal to the aggregate of the named set) — only the *count* relaxes.
- Producer RPCs (`generateposblock`, `getposblocktemplate`) build sub-quorum
  aggregate blocks and let consensus validate the stall condition.
- Tests: `pos_escaping_stall_gap` (unit) and `feature_pos_escaping_stall.py`
  (end-to-end over a parent chain: full-quorum accepted; sub-quorum rejected
  with no anchor advance; accepted after a +3 advance; rejected again).

**Why this is safe without the fork-choice preference.** A sub-threshold block
is *validly certified* — its named members are sortition-eligible and its
aggregate signature is valid — so a full-vs-sub fork at the same height is an
ordinary first-seen signed-block fork, with no double-spend beyond normal
reorg semantics. It is also abuse-proof: a `+3` anchor requires Bitcoin to have
genuinely produced three blocks (~30 min) since the parent's anchor, which a
healthy 10-second chain never permits, and each further sub-threshold block
needs another `+3` of parent-chain progress — so the path self-limits to a
genuine stall and cannot be used to bypass committee certification.

**Pending (stages 3–5), with the architectural blocker:** the whitepaper
prefers a full-threshold block over a sub-threshold one at the same height
(then lowest VRF). Encoding that in fork choice means reflecting *certification
strength* in `nChainWork` — but `nChainWork` is fixed at **header**-acceptance
time, while the countersignature count lives in the **coinbase body**. So a
faithful deterministic preference requires either surfacing the count into the
header (a format change / new genesis) or recomputing chain work at connect
time. Until then, full-vs-sub same-height forks converge by first-seen like any
signed-block fork — a finality nicety deferred, not a safety gap. Stage 5 (the
dynamic committee floor) builds on the static `-posminstake` floor (doc 06 §5).
