# Liveness & escaping-stall — design for the anchor-driven model

> **Status: implemented.** The **escaping-stall sub-threshold certification**
> (§3.8, stages 1–2) and the **deterministic fork-choice preference** (more
> countersignatures wins, then lowest leader VRF score — stage 3) are both
> implemented and tested (`feature_pos_escaping_stall.py`,
> `feature_pos_fork_choice.py`, `pos_tests.cpp`). Stages 4–5 (retiring the
> wall-clock slot gate; a dynamic committee floor) remain deliberately deferred
> as fidelity refinements over already-safe mechanisms (see §7). §6 records the
> implementation.

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

The path is also abuse-proof: a `+3` anchor requires Bitcoin to have genuinely
produced three blocks (~30 min) since the parent's anchor, which a healthy
10-second chain never permits, and each further sub-threshold block needs
another `+3` of parent-chain progress — so the path self-limits to a genuine
stall and cannot be used to bypass committee certification.

**Implemented (stage 3) — the deterministic fork-choice preference.** Among
same-height (equal-work) blocks the chain now prefers more committee
countersignatures, then the lower leader VRF score (whitepaper §3.8) — so a
full-threshold block always beats an escaping-stall sub-threshold one. The key
realisation: this does **not** need `nChainWork` (a header-time quantity).
`CBlockIndexWorkComparator` (`src/validation.cpp`) already drives equal-work
reorgs via its secondary keys; we add the preference there, using two keys on
`CBlockIndex` set at acceptance and never mutated (so the candidate-set ordering
stays stable):
- `m_pos_countersigs` — the named committee size (coinbase `SEQCMT` count for
  the aggregate form; the listed members for multisig). More is better.
- `m_pos_vrf_score` — the top 64 bits of the leader's VRF `beta` over the slot
  seed. Lower is better. Registry-independent (just the leader key + seed), so
  deterministic across nodes.
Both are computed from the block body in `SetPosForkChoiceKeys` before the
block enters `setBlockIndexCandidates`, and persisted in `CDiskBlockIndex` so a
restarted node orders identically. Tested in `feature_pos_fork_choice.py` (a
2-member tip is reorganized onto a competing 3-member block at the same height)
and the full PoS/anchoring battery confirms no regression.

Stages 4–5 (retiring the wall-clock slot gate; a dynamic committee floor)
remain deferred — see §7.

## 7. Decisions on the remaining stages (4–5)

Stage 3 (the fork-choice preference) is implemented — see §6. The two remaining
stages are **fidelity refinements over mechanisms that are already safe and
deterministic** — neither is a safety or liveness gap — and are deliberately
deferred:

- **Stage 4 — replace the wall-clock slot gate with an anchor clock.** Deferred.
  The existing gate (`block.nTime ≥ parent.nTime + slot · interval`) compares
  *committed block timestamps*, so it is already deterministic across nodes
  (not node-local wall time), bounded by MTP and the 2-hour-future rule. An
  anchor-block-based clock is closer to the paper's letter but is a timing-model
  redesign with real risk for little safety gain; the escaping-stall rule
  (implemented) already provides the anchor-driven liveness guarantee that
  matters (the chain cannot freeze).

- **Stage 5 — dynamic committee floor (lower `-posminstake` when participation
  is short).** Deferred as largely redundant: its purpose is to keep a
  committee formable when stake participation is low, but the escaping-stall
  rule already lets the chain make progress with a sub-quorum (down to one
  member) when the committee cannot be filled. The static floor (doc 06 §5)
  plus escaping-stall covers the liveness need without adding a dynamic,
  state-dependent consensus parameter.

**Net:** the safety- and liveness-critical parts of the anchor-driven model
(escaping-stall sub-threshold certification, computed unforgeably from
committed anchor heights under `validateanchor=1`) are implemented and tested.
The deferred stages are convergence/fidelity polish to schedule with review.
