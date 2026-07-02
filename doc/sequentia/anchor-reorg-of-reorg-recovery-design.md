# Anchor reorg-of-reorg recovery — design

Status: design complete; Changes 1-3 ready to implement (Alberto-endorsed direction);
Change 4 is consensus-rule and gated on Alberto.

## Problem (two distinct root causes that share one downstream surface)

### A. Reorg-of-reorg recovery gap (the bug Alberto endorsed fixing)

Bitcoin goes A -> B -> A (the previous best chain becomes best again; common on testnet4).

- On B: `AnchorWatchTask` section 2 (`src/anchor.cpp` ~295-375) detects the parent reorg and
  `InvalidateBlock`s the A-anchored Sequentia blocks (`BLOCK_FAILED_VALID/CHILD`, persisted via
  `m_dirty_blockindex`), recording them ONLY in the in-memory `g_anchor_invalidated` set
  (`anchor.cpp:51,366`).
- When A returns: recovery exists ONLY in section 1 (`anchor.cpp` ~219-256), gated on
  `tip_changed` and driven exclusively by that ephemeral set.

The asymmetry is the defect: **invalidation re-derives "what is bad" from Bitcoin ground truth
every tick; recovery depends on a non-persisted worklist.** `BLOCK_FAILED` flags survive a
restart (reloaded by `LoadBlockIndex`), but `g_anchor_invalidated` does not. So a restart between
B-invalidation and A-restoration, a coalesced/missed parent flap, or a peer-sourced failure
leaves the previously-valid-and-finalized A-anchored blocks `BLOCK_FAILED` forever.

Two compounding holes:
- `ResetBlockFailureFlags` (`src/validation.cpp` ~3929-3961) only re-queues blocks that already
  `HaveTxsDownloaded` and never restores `pindexBestHeader`, so a recovery chain known only as
  headers cannot reconnect.
- `pindexBestHeader` is set purely by chainwork; the only corrective (`InvalidChainFound`,
  `validation.cpp` ~1687-1706) fires only when the invalidated block is an ANCESTOR of best-header.
  An orphaned-anchor headers-only sibling fork therefore permanently pins best-header.

Alberto's required behavior: "consider FINAL AGAIN the blocks already produced and validated
pinned to the ULTIMATE best bitcoin chain" — re-validate + re-finalize them; do NOT mint a single
fresh block on top (that would let a Bitcoin-side-valid atomic swap be double-spent on the
Sequentia side, and wastes block space).

### B. Committee-equivocation finality split (the live 96/4; NOT the same bug)

Two DIFFERENT blocks at one height, BOTH anchored to the SAME (valid) Bitcoin block — quorum
reached among disjoint committee subsets. The minority's finalized block has an intact anchor, so
the immediate-finality gate's release valve `final_anchor_orphaned` (`validation.cpp` ~4335) stays
false and the gate rejects the longer rival chain (`bad-fork-prior-to-pos-final`); worse,
`AcceptBlockHeader` bails before `AddToBlockIndex` (~4596-4599) so the rival chain is never stored
("headers-only"). Anchoring genuinely cannot break this tie. **The endorsed fix does NOT heal
this**; it needs a separate finality tie-break rule (Change 4 = Alberto's call).

A reorg-of-reorg is the typical UPSTREAM trigger that produces an equivocation split; once both
survivors re-anchor to the returned chain, mechanism B freezes the residue.

## Changes

### Change 1 — re-derive the recovery worklist from on-disk ground truth (`src/anchor.cpp`)
Make recovery symmetric with invalidation. Each tick, snapshot under `cs_main` the directly-
invalidated anchored blocks by scanning `m_chainman.m_failed_blocks` for entries with
`(nStatus & BLOCK_FAILED_VALID)` and non-null `m_anchor_hash`; capture
`(hash, m_anchor_height, m_anchor_hash)`. OUTSIDE `cs_main`, run `CheckMainchainAnchor` on each;
for any returning **OK only** (never NOT_FOUND/STALE/NO_CONNECTION), re-take `cs_main`,
`ResetBlockFailureFlags(pindex)`, erase from the hint set, `ActivateBestChain`. Run every tick
(like section 2), not only on `tip_changed`. Keep `g_anchor_invalidated` as a non-authoritative
hint. Never hold `cs_main` across the bitcoind RPC.

### Change 2 — seed the recovery hint at startup (`src/node/blockstorage.cpp`)
In `BlockManager::LoadBlockIndex` (where it already re-derives `BLOCK_FAILED_CHILD` and
`pindexBestHeader`), collect entries with `(nStatus & BLOCK_FAILED_VALID)` and non-null
`m_anchor_hash` and seed them via a new `SeedAnchorInvalidated()` exported from `anchor.h`. Pure
in-memory; no disk-schema change. Belt-and-suspenders given Change 1's ground-truth scan.

### Change 3 — restore best-header + re-request bodies (`src/validation.cpp`)
Extract the skip-invalid best-header recompute into a runtime `RecalculateBestHeader()` (highest
`IsValid(BLOCK_VALID_TREE)` by `CBlockIndexWorkComparator`). Call it (a) at the end of
`ResetBlockFailureFlags` so recovery RAISES best-header onto the now-valid branch, and (b) inside
`InvalidChainFound` replacing the narrow ancestor-only reset so an orphaned-anchor sibling fork can
no longer pin best-header. Existing net path then re-issues `getheaders` and fetches bodies;
`ActivateBestChain`/`UpdateTip` reconnect and re-finalize the WHOLE restored chain.

### Change 4 — REVISED 2026-07-01 after Alberto's review (see alberto-96-4-answers-2026-07-01.md)

The original shape here (yield to a strictly-better-certified SAME-HEIGHT sibling) is
**WITHDRAWN**: comparing certificates at the same height reopens the posterior-corruption
channel (accumulate signatures after the fact, reorg a finalized block), which the gate
deliberately forbids (`validation.cpp:4314-4318`, and Alberto's 2026-06-28/30 analysis).
Replaced by two parts:

- **4a — PREVENTION (node-local, no consensus change; deploy gated on Alberto's ack).**
  The producer/countersigner must not treat height h as vacant while the node holds a
  quorum-certified block at h (`m_pos_countersigs >= quorum`, persisted on the index) that
  is awaiting, or has received, a favorable anchor verdict after a parent-chain move. It
  neither proposes nor backs a rival at h; bounded patience (about one block interval) so
  production can never deadlock; a negative verdict (anchor genuinely off the best chain)
  releases the guard immediately. Closes the fresh-mint-vs-watcher-tick race that
  manufactured the live 96/4 (proposer fires instantly after a rollback,
  `pos_producer.cpp:536-555, 613-631`, while recovery needs a watcher tick + bitcoind RPC).
- **4b — RECONCILIATION (consensus rule, Alberto's call).** A node abandons its finalized
  block ONLY for a rival branch that (i) forks no lower than the Bitcoin checkpoint floor,
  (ii) is anchor-valid throughout on Bitcoin's current best chain, and (iii) contains a
  quorum-certified block at a height STRICTLY ABOVE the local finalized height. Never a
  same-height certificate comparison. Converges the live 96/4 (the minority yields to the
  majority's quorum progress; the majority never yields because a 4-of-100 branch can never
  produce another quorum certificate).
- **Residual (Alberto to choose):** near-symmetric splits where neither side can
  quorum-advance; operator-only, or a last-resort same-height tie-break (more sigs, then
  lower VRF) after N Bitcoin blocks without quorum progress.
- **Footnote:** the finality gate is accept-time only (`ContextualCheckBlockHeader`); a
  rival indexed BEFORE local finalization can still win via the comparator. The 4b
  predicate can also be enforced at activation time to make the gate symmetric.

## Safety (Changes 1-3 preserve anchoring supremacy by construction)
They only ever CLEAR `BLOCK_FAILED` on a block whose stored anchor returns
`CheckMainchainAnchor == OK` on a LIVE RPC (>=1 confirmation on the parent best chain AND height
match). Never on NOT_FOUND/STALE/HEIGHT_MISMATCH/NO_CONNECTION — no un-finalizing on ignorance.
Section 2 keeps re-deriving bad-ness every tick, so if Bitcoin re-orphans an anchor we just
reconsidered, the next tick re-invalidates it. `RecalculateBestHeader` uses `IsValid(BLOCK_VALID_TREE)`,
so best-header can never be raised onto a failed branch. `UpdateTip` recomputes the finalized point
purely from the resulting active chain — finality follows Bitcoin, never overrides it.

## Test
New `test/functional/feature_pos_reorg_of_reorg_recovery.py` (modeled on
`feature_pos_finalized_anchor_reorg.py`): finalize A-anchored blocks; force parent A->B and assert
they go `BLOCK_FAILED` + tip rolls back; restore parent to A and assert auto-recovery **(4a)
without restart and (4b) WITH a restart between** (proves Change 1+2); a headers-only variant
asserting `pindexBestHeader` restore + body re-request + reconnect (Change 3); a double-spend
assertion (a tx mined only in the restored chain is present and not double-spendable). Separately a
skip/xfail `feature_pos_finality_split_recovery.py` reproducing the 96/4 (documents B; asserts the
heal once Change 4 lands).

## Open questions for Alberto
1. Change 4: is un-finalizing a quorum-certified block EVER admissible in favor of a same-anchor,
   strictly-better-certified sibling, and what is the exact deterministic predicate (countersigs
   only? plus VRF? plus chainwork)?
2. For the live split: does the majority block carry MORE countersigs than the minority, or did
   both merely cross quorum from disjoint subgroups (true equivocation, equal certification)? If
   equal, the comparator falls to VRF/first-seen and an explicit equivocation rule is needed.
3. Change 1 scope: reconsider only watcher-invalidated (`BLOCK_FAILED_VALID` + anchor present), or
   also peer-learned `BLOCK_FAILED_CHILD` / soft anchor-stale? (Design scopes to the former.)
4. Re-derive from `nStatus` each tick (design's choice, no schema change) vs persisting the set?
5. Until Change 4 lands, the stuck minority needs the `invalidateblock` band-aid; confirm that is
   the accepted interim and automation must not auto-invalidate without authorization.

## Implementation notes (what was actually built)

Changes 1-3 are implemented; Change 4 is left for Alberto. Two refinements came out of an
adversarial review and differ from the first sketch:

- **Provenance marker instead of inferring from flags.** The first cut seeded the recovery
  worklist from `(BLOCK_FAILED_VALID && anchor present)`. That is ambiguous: `invalidateblock`
  and consensus connect-failures set the exact same persisted state, and every anchored block
  has an anchor. A restart would then have RESURRECTED operator-/consensus-invalidated blocks
  (anchor still OK -> reconsidered -> reconnected) — silently undoing the documented finality-
  split recovery (operators `invalidateblock` the stuck tip, [[sequentia-finality-split-stall]])
  and breaking the standard cross-restart persistence of `invalidateblock`. Fix: a node-local
  `BLOCK_FAILED_ANCHOR = 512` nStatus bit (chain.h), OUTSIDE `BLOCK_FAILED_MASK`, set ONLY by the
  watcher (`CChainState::MarkAnchorInvalid`, called in section 2 after `InvalidateBlock`) and
  cleared in `ResetBlockFailureFlags`. `LoadBlockIndex` seeds only marked blocks.
- **Negative anchor cache.** Section 1 now runs every tick (for coalesced flaps + the post-
  restart seed), so to avoid a per-tick `getblockheader` RPC storm against bitcoind (the
  documented #1 stall vector) a `g_anchor_stale_cache` mirrors `g_anchor_ok_cache`: definitive
  off-best-chain verdicts (STALE/NOT_FOUND/HEIGHT_MISMATCH, never NO_CONNECTION) are cached and
  cleared on the same `tip_changed`, so each orphaned anchor is RPC-checked at most once per
  parent-tip epoch. Section 1 also breaks on NO_CONNECTION (no hammering a down daemon).
- **`RecalculateBestHeader` once per batch.** Moved out of the generic `ResetBlockFailureFlags`
  (which would have added a 2nd O(n) scan per call and changed `reconsiderblock`) to a single
  call after section 1's reconsider loop.

Lock order is `cs_main -> g_anchor_mutex` (consistent; `g_anchor_mutex` is file-local and never
held while taking `cs_main`). Test: `test/functional/feature_pos_reorg_of_reorg_recovery.py`
asserts BOTH recovery across a restart (original tip hash restored verbatim) AND that a manual
`invalidateblock` is NOT resurrected across a restart. All existing anchor/finality/reconsider
functional tests still pass.

### Change 4a implementation (2026-07-01; deploy gated on Alberto's ack)

- `AnchorCertifiedSiblingPending(chainman, tip_hash, child_height)` (`anchor.{h,cpp}`): returns
  the hash of a child of the tip at the next height that lies on a watcher-invalidated branch
  (recovery-set root with `BLOCK_FAILED_ANCHOR` provenance; manual/consensus invalidations are
  skipped) that is NOT in `g_anchor_stale_cache` (not confirmed off the parent's best chain
  this parent-tip epoch) and that carries a quorum certification AT OR ABOVE the child
  (`max(m_pos_countersigs) >= PosQuorum`, so a sub-quorum escaping-stall block with a certified
  descendant is still protected, per adversarial review; a branch that is sub-quorum throughout
  is deliberately unguarded — it never held finality). Recovery-set entries now live until
  their branch actually RECONNECTS (section 1 erases on `ActiveChain().Contains`), so the guard
  covers the whole un-fail-to-reconnect window. Snapshots under `g_anchor_mutex`, then takes
  `cs_main`; strictly sequential, never nested.
- `PosProducer::Step()` evaluates it after the round-state reset: while pending, neither
  proposes nor calls `DriveRound` (no rival backed or signed), re-polling at 500 ms, bounded by
  `-posanchorrecoverywait` (default 30 s; 0 disables) so an unreachable parent daemon can only
  delay production. A plain forward reorg never holds: the invalidation walk caches the stale
  verdict before the producer sees the rollback.
- Test `test/functional/feature_pos_certified_sibling_guard.py`: 3 unit-weight stakers,
  committee 3, quorum 2 (a committee=1 chain names no certificate members, so it can never
  exercise the guard). Reproduces the boot race deterministically (the first watcher tick is
  delayed by a large `-poscheckpointscan` window over a 3000-block parent chain while the
  producer's slot is due immediately); asserts the hold log, verbatim restoration, no rival
  branch, resumed production, and stale-verdict liveness. The negative control
  (`-posanchorrecoverywait=0`) reproduces the pre-guard rival branch and fails the test.

### Found by the 4a test: fork-choice keys were not reloaded from disk (FIXED)

`m_pos_countersigs` / `m_pos_vrf_score` were serialized in `CDiskBlockIndex` (`chain.h`) but
never copied back in `CBlockTreeDB::LoadBlockIndexGuts` (`txdb.cpp`). Every restarted node
therefore saw ALL historical blocks as countersigs=0 / vrf=unset: no immediate-final point
until the next quorum block connected after boot (the finality gate was down for pre-restart
history in that window), a neutered same-height comparator for historical blocks (a fresh
post-restart rival outranked any pre-restart block), and the 4a guard blind after a restart.
Fixed by copying the two fields on load; on-disk values were always written correctly, so
existing datadirs heal on the next restart with no reindex.

### Adversarial review outcome (3 independent reviewers, 2026-07-01)

Verdicts: 3 x ship-with-nits. Addressed: the sub-quorum-root coverage gap (max-certification
at/above the child, above); the erase-before-reconnect window (entries live until connected);
`g_validate_anchor` assigned before the producer thread starts (`init.cpp`; was an
unsynchronized cross-thread read); the hold timer moved to a steady clock (NTP-step-proof);
`-posanchorrecoverywait` clamped to [0, 3600]; `AssertLockNotHeld` at the helper entry; the
test's guard patience decoupled from checkpoint-scan latency (120 s). Deliberate and
documented, not changed: rival proposals/shares are still recorded and relayed during a hold
(only OUR proposal/signature is withheld); `generateposblock` (RPC) bypasses the guard — it is
an explicit operator action, like `invalidateblock`. Accepted residual: a flaky parent RPC
during a genuine departure can hold production for the full bounded patience (latency wart,
no supremacy violation); a reviewer verified empirically that the committed test FAILS on the
pre-guard binary (rival minted 22 ms after producer start) and PASSES on the guarded one.

## Live evidence (2026-06-27 ~23:26-23:45Z)
A real testnet4 reorg-of-reorg exercised this end to end: bitcoind advanced to 141991 while the
SEQ anchor `0000…e06c41` left Bitcoin's best chain; the watcher logged
`Parent chain reorganization detected: invalidating block 508b2ccd… (and descendants)`, the SEQ
chain rolled back ~386 blocks (10220 -> ~9834), and then stuck at 9835 with
`Anchor-reorg recovery: peer=N best known block … is in an invalidated (orphaned) …` and 354
invalid + 62 headers-only tips — the asymmetric-recovery + best-header-pin holes, live.
