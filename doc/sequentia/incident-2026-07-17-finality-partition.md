# Incident 2026-07-17: the finality partition (and the fix)

Status: incident analysis + shipped fix (v23.3.6). Related design:
[`anchor-reorg-of-reorg-recovery-design.md`](anchor-reorg-of-reorg-recovery-design.md)
(this ships its "Change 4b"), [`04-proof-of-stake.md`](04-proof-of-stake.md) §6.

## Summary

On 2026-07-17 the Sequentia testnet split at block **25504** into two branches
that were **both quorum-certified with Bitcoin anchors that both ended up
canonical**. Every node then did exactly what the immediate-finality rule
prescribes — kept the first finalized branch it saw — and the network
partitioned permanently: a minority node stayed pinned at its finalized height
25558 while the rest of the committee kept extending a rival branch, with no
protocol path back. Anchoring could not arbitrate (both branches' anchors sat
on the same, converged Bitcoin chain) and no information was missing (the
pinned node received and soft-rejected the rival blocks every second).

Two changes fix the class:

1. **Finality reconciliation** (`-posreconcile`, default on): a node releases
   its finalized point for a rival branch that is provably the committee's —
   full-quorum certificates strictly above the local finalized height, anchors
   settled on the local Bitcoin best chain — after its own branch has received
   no certified extension for a patience window. This heals the partition
   automatically, from the minority side only.
2. **Escaping-stall real-time evidence** (`-posescapestallmtpgap`, consensus):
   a sub-quorum (escaping-stall) block must show one Bitcoin block interval of
   *median-time-past* between its anchor and its parent's, not just the anchor
   *height* gap — which a testnet4 difficulty-1 block-storm satisfies within
   seconds while the chain is fully alive. This removes the incident's first
   domino.

## The verified timeline

All data read off the affected node and the public explorer (heights
abbreviated; Bitcoin = testnet4):

| When (t = first divergence) | Event |
|---|---|
| t−30 s | Block 25503, quorum-certified 8/7, anchored at Bitcoin 144306. Chain healthy. |
| **t** | **Block 25504: 1 signature (leader-only escaping-stall)**, anchor jumped to the fresh, seconds-old Bitcoin tip 144311. The height gap (+5) satisfied the h+3 escaping-stall rule although **no stall existed** — a difficulty-1 storm had minted 5 Bitcoin heights in seconds. |
| t → t+27 min | The committee accepts the branch and certifies 25505–25558 (7-10 signatures each, all anchored at 144311). The finality point of nodes on this branch advances to 25558. |
| ~t+27 min | A transient testnet4 flap drops block 144311 from most members' Bitcoin views. Their anchor watchers invalidate 25504–25558 (correct: finality is modulo Bitcoin). |
| t+28 min (44 s after the last certified block) | The members re-mint 25504′ from 25503 — quorum-certified immediately, anchored at **144307, below the contested zone** (an anchor on the common trunk stays valid whichever storm branch wins). They keep anchoring 144307 for half an hour while the storm continues above. |
| later | Testnet4 converges — on the branch that **contains 144311**. The original branch's anchors are canonical again for everyone; the reorg-of-reorg recovery can mark those blocks valid again, but *valid ≠ active*: the members' finality now sits on 25504′-descendants. Both branches are quorum-certified with canonical anchors. |
| aftermath | The one node whose Bitcoin view never flapped (its watcher never fired) stays pinned at its finalized 25558, soft-rejecting the network's branch (`bad-fork-prior-to-pos-final`) forever, solo-producing 1/7 escaping-stall blocks the committee will never certify. The network is thousands of blocks ahead. |

## Root causes, in layers

1. **The first domino — a height gap is not a time gap.** The escaping-stall
   relaxation (whitepaper §3.8) keys on the anchor advancing
   `POS_ESCAPING_STALL_ANCHOR_GAP` (3) parent-chain *heights* past the
   parent's anchor. That equals ~30 minutes of real time on Bitcoin mainnet —
   the "genuine stall" it was designed to prove — but during a testnet4
   difficulty-1 block-storm heights advance in seconds. A leader-only block
   was accepted by consensus 30 seconds after a quorum-certified parent,
   seeding a rival branch while the chain was fully alive. (The producer's
   fresh anchor also outranked staler proposals in the committee's signing
   preference, pulling the committee onto the new branch.)

2. **Honest re-certification across time.** Quorum intersection prevents two
   disjoint quorums from certifying rivals *simultaneously*; it cannot prevent
   the same committee from legitimately re-certifying a *replacement* branch
   after its anchor watchers released finality during a transient parent-chain
   flap. The replacement was anchored below the contested zone, so it stayed
   valid under every storm outcome — while the original branch was tied to one
   specific outcome. When Bitcoin converged on that very outcome, both
   branches ended up anchor-canonical.

3. **No arbitration between rival finalities.** The immediate-finality gate
   deliberately rejects even a rival carrying MORE signatures (posterior
   corruption must stay impossible), and its only release valve was a Bitcoin
   reorg of the finalized block's anchor. With both branches anchor-canonical
   that valve can never open. The protocol had no notion of "certified first"
   (there is no global clock) and no rule for abandoning a branch the
   committee has provably left. This is case B of
   `anchor-reorg-of-reorg-recovery-design.md`, whose endorsed Changes 1-3+4a
   explicitly do not heal it; the reconciliation rule (Change 4b) was designed
   there and is shipped now.

## The fix

### Finality reconciliation (node-local fork-choice/release rule)

With `-posreconcile` (default on, requires `-validateanchor`):

* Rival branches that fork at/below the finalized point are **stored** instead
  of rejected at accept time: the finality gate moves to **activation time**
  (`FindMostWorkChain`), where such branches are skipped — stored but
  inactivatable. (This also closes a pre-existing hole: a rival indexed
  *before* local finalization could previously win on the plain comparator.)
* A monitor in the anchor-watcher thread releases the finalized point **only**
  when ALL of the following hold:
  1. the rival branch carries a **full-quorum certificate strictly above** the
     local finalized height (never a same-height certificate comparison), at
     least `-posreconcilemindepth` (3) heights above it;
  2. that certifying block's anchor is **on the local Bitcoin best chain** and
     at/below the currently **uncontested** parent height (`getchaintips`), so
     a release can never fire into a live Bitcoin fork;
  3. the rival forks no lower than the Bitcoin checkpoint floor (checkpoint
     gates are unchanged and still enforced at accept time);
  4. the local branch has received **no quorum-certified extension for
     `-posreconcilepatience` (600) seconds** — the committee has provably
     abandoned it. (Local steady clock: this is fork-choice behavior, not
     block validity; nodes may release at different moments and all converge
     on the same branch.)
* On release the local blocks are **not invalidated** — they were valid, they
  merely lost — the finalized pin is lowered for the approved branch only and
  ordinary fork choice adopts it. `getanchorstatus` exposes the monitor state
  (`reconcile: inactive / tracking / released`, rival certificate info,
  patience countdown).

Security: forging the release evidence requires a full committee quorum — a
stake majority, which already controls the chain going forward, so no new
attacker class. The majority side never releases (an abandoned minority branch
cannot produce new quorum certificates), so convergence is one-way and cannot
flap. Depth is bounded by the checkpoint floor. What changes for a payee: a
payment finalized on a node that is *already partitioned from the committee*
can be superseded after the patience window — during which the node is in a
visible alarm state; on the network's branch (the one every explorer and
counterparty follows) finality is as absolute as before.

### Escaping-stall MTP gap (consensus)

A block certified below quorum must now also satisfy
`MTP(block anchor) − MTP(parent anchor) ≥ -posescapestallmtpgap` (default 600 s,
one Bitcoin block interval). Median-time-past is the parent chain's own
real-time clock: a pure function of the (immutable) anchor hashes that every
validator reads identically from its Bitcoin daemon, and one that a height
race cannot fake without parent-chain hashrate. On mainnet the h+3 height gap
(~30 min) stays the binding constraint; the MTP gap bites exactly where the
incident lived — difficulty-1 storms. Verdicts: a violating block is invalid
(`bad-pos-escape-stall-too-soon`); an unverifiable one (Bitcoin daemon
unreachable) is soft-rejected and retried (`pos-escape-stall-unverifiable`),
mirroring the R3 anchor check. `-validateanchor=0` followers delegate this
check like every other anchor check. The producer honors the same rule, both
when self-certifying and in the ancestry-hold release.

**Deployment note:** the MTP gap is a consensus *tightening*. A mixed network
can diverge on a sub-quorum block minted during a storm (old nodes follow it,
updated nodes reject it) until producers are updated — the same rollout window
every tightening has. Escaping-stall blocks are rare and now doubly so;
updating the dominant producers closes the window.

## What this does deliberately NOT do

* No "heavier certificate wins": a rival with more signatures still never
  reorgs a finalized block (that guarantee is what makes real-time atomic
  swaps possible; see `04-proof-of-stake.md` §6 and the withdrawn first shape
  of Change 4 in the design doc).
* No same-height tie-break of any kind: near-symmetric splits where neither
  side can quorum-advance remain operator territory.
* No watcher hysteresis and no `anchorminconf` increase: anchor freshness for
  real-time swaps is untouched (Principle 7 rule III).
* Fix A (`-anchoravoidcontested`) is unchanged and still wanted on every
  producer. It cannot see a contest that has not reached its Bitcoin daemon
  yet (the incident's 25504 anchored a seconds-old tip), which is exactly why
  the MTP gate and the reconciliation exist as the next layers.

## Operator quick reference

* Am I partitioned? `elements-cli getanchorstatus` → `anchorstatus: "ok"`
  while the tip is stuck and `lastposfinalreject` is recent (pre-fix), or
  `reconcile.state: "tracking"/"released"` (post-fix). Bisect the fork point
  against the explorer with `getblockhash <h>` vs `/api/block-height/<h>`,
  then `getblockheader <hash>` and compare `poscountersigs`/`posquorum` and
  the anchors of the first divergent blocks.
* With v23.3.6+ the node heals itself ~10 minutes after the conditions are
  met; the pre-fix manual recovery (`invalidateblock` on the local first
  divergent block) is no longer needed.
