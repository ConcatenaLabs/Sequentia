# Liveness & escaping-stall — design for the anchor-driven model

> **Status: implemented (and re-examined against the whitepaper, §7).** The
> **escaping-stall sub-threshold certification** (§3.8) and the **deterministic
> fork-choice preference** (more countersignatures, then lowest leader VRF
> score) are implemented and tested (`feature_pos_escaping_stall.py`,
> `feature_pos_fork_choice.py`, `pos_tests.cpp`). On re-reading the paper, the
> normal block timing it specifies is itself wall-clock (a per-node round
> timeout), so our timestamp slot-gate is *aligned*, not a simplification to
> replace (§7). The only open items are an underspecified optimisation (a
> dynamic committee floor) and an unmodelled liveness refinement (mid-round
> anchor reshuffle), both subsumed for safety/liveness by escaping-stall.

## 1. What exists today

The implemented PoS (doc 06/07) certifies a block when a committee quorum
(strict majority of `-poscommitteesize`) countersigns it, and gates *timing*
with a wall-clock rule: a block is rejected (`bad-posvrf-early`) if
`block.nTime < parent.nTime + slot · -posslotinterval`, where `slot` is the
leader's VRF sortition slot. This matches the whitepaper, whose normal-operation
timing is also a local wall-clock round timeout with the lowest-VRF participant
as proposer (§3.5); the timestamp gate plus the §6 lowest-VRF fork-choice
tiebreak realise that selection deterministically.

The whitepaper's *Bitcoin-anchor* consensus roles are: the sortition seed
(implemented), the escaping-stall path below (implemented), and a mid-round
leader reshuffle on a new parent block (a liveness refinement; §7). Earlier
drafts of this doc framed the wall-clock timing as a divergence to fix — that
was a misreading, corrected in §7.

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
| Sub-threshold (escaping-stall) certification | `CheckPosStakeRules` + `CheckProof` (`block_proof.cpp`): allow a named/aggregate committee below `PosQuorum` **if and only if** the escaping-stall anchor condition holds; otherwise keep requiring quorum. The aggregate-committee MuSig path must carry the actual signer set used, and the block must record it was certified under the stall rule (a coinbase marker, like `SEQVRF`/`SEQCMT`). |
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
~30-second chain never permits, and each further sub-threshold block needs
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

## 7. The remaining items — decided

Stage 3 (the fork-choice preference) is implemented — see §6. The three items
raised for review have been decided with the project owner:

- **Block timing ("stage 4") — already aligned, no change.** The whitepaper's
  *normal* block timing is itself wall-clock: "a locally-enforced wall-clock
  timeout that resets per block arrival, with the lowest-VRF participant serving
  as proposer" (§3.5). The Bitcoin anchor's consensus roles are the sortition
  **seed**, the **escaping-stall** (§3.8), and **keeping the tip referencing the
  freshest Bitcoin block** (next item) — not a replacement clock. Our timestamp
  slot-gate plus the §6 lowest-VRF tiebreak realise "lowest-VRF proposer at
  timeout." Nothing to change.

- **Keeping the tip on the freshest Bitcoin block (real-time swaps) —
  IMPLEMENTED via an anchor-freshness fork-choice key, chosen over the literal
  seed-reshuffle.** Motivation (owner): the Sequentia tip must reference the
  latest Bitcoin block so a swap's Sequentia leg confirms with
  `anchor ≥ the Bitcoin leg's height` promptly, giving timelock-free real-time
  cross-chain swaps (the anchoring already provides the reorg *safety*; this is
  about keeping the chains *synchronized*). "Real-time" is precise here — **no
  extra reorg-protection timelock**, not zero latency; see the definition box in
  [doc 03](03-bitcoin-anchoring.md). Rather than re-roll the sortition seed from each block's own
  anchor (the paper's literal mechanism — which adds proposer grinding and a
  deep sortition change), `CBlockIndexWorkComparator` now prefers, among
  equally-certified same-height blocks, the one with the higher (fresher)
  `m_anchor_height`, then the lower VRF score. This makes the canonical chain
  track Bitcoin's tip (a stale-anchor block loses to a fresher one), with **no
  grinding** (referencing an older Bitcoin block can only lose) and a tiny, safe
  change (the field already exists, set at acceptance, never mutated). Ordered
  *after* certification, so it never displaces a finalized block — a freshly
  arrived Bitcoin block is otherwise picked up within one block (~1 slot).
  Tested in `feature_pos_anchor_freshness.py`. Trade-off vs. the literal
  reshuffle: ~1-block *synchronization* lag instead of ~0, in exchange for no
  grinding and far less risk; the swap *safety* (and the no-extra-timelock
  property) is identical either way.

- **Dynamic committee floor — not implemented (owner: Option A).** The
  whitepaper leaves its trigger/curve undefined ("may not be finalized"), and
  its liveness purpose is already met by escaping-stall (sub-quorum down to one
  member). The static `-posminstake` floor stands.

**Net:** every consensus mechanism the whitepaper specifies for the PoS model is
now implemented — VRF sortition, committee certification + MuSig2 aggregation +
distributed signing, the anchor-seeded schedule, wall-clock round timing,
lowest-VRF + more-countersignatures + fresher-anchor fork choice, escaping-stall,
checkpoints, min-stake, unbonding. No specified mechanism remains open.
