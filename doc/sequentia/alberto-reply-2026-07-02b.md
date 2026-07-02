# Reply: 4a deploy, VerifySeqLegSafe, the VRF tiebreak, 4b

Short answers to your five points.

## 1. Change 4a: deploy

Done, and already live. 4a is deployed to all 100 committee nodes (rolling restart earlier
today): a producer that holds a quorum-certified block at a height whose anchor is being
restored after a reorg-of-reorg neither proposes nor backs an alternative block there; it waits
for the previous best chain to be restored (bounded by a short recovery timeout, never a
deadlock). The same binary also fixes a real bug found alongside it: the persisted fork-choice
keys (countersignatures + leader VRF score) were serialized but never reloaded on startup, so
every restarted node saw its own chain as uncertified until the next quorum block.

## 2. VerifySeqLegSafe: quorum certification added

Implemented as you asked: the cross-chain swap gate now requires the Sequentia leg's block to be
quorum-certified (immediately final), IN ADDITION to the anchor-ordering check (the leg's block
anchors at a Bitcoin height >= the Bitcoin leg, and anchor status is ok). getblockheader now
reports the block's countersignatures + a "poscertified" flag; VerifySeqLegSafe ANDs it in. It is
feature-detected so a mid-upgrade node keeps the old behavior rather than breaking, and it needs a
node rebuild to activate. Anchoring binds the leg to Bitcoin; quorum certification means the
committee finalized it. Both now required.

## 3. min-anchor-depth: not added (agreed)

I am not adding any minimum-anchor-DEPTH wait to the gate. You are right that it would defeat the
whole point of Sequentia's real-time anchoring; the anchor axis stays 0-conf by design. The gate
is ordering + certification only, no depth buffer.

## 4. Tie-break, and the VRF question

First, a correction: I was wrong to call the VRF "unreliable" for the tiebreak. That was a misread
on my part. The only "proved unreliable" note in the code is narrow and about something else: it
rejects the leader's VRF score as the SEED for the next round's sortition, and only because that
score is set late (at block-accept, defaulting to "unset"), so it is not identically available at
the moment a child's seed must be derived. The seed is instead the parent's Bitcoin-anchor hash
plus the child height, both header fields fixed early. That comment says nothing about fork choice.

So, to answer you directly: the VRF IS reliable for leader selection, for committee sortition, and
for round voting (the seed is deterministic and anchor-derived, and each VRF output is unique and
verifiable, so a producer cannot grind it or claim a better slot than its true output). And the
leader VRF score IS already the implemented same-height fork-choice tiebreak, applied after
countersignature power (whitepaper 3.8; the code comment calls it "the ultimate truth"), precisely
because by acceptance time it is set and verified.

Therefore the convergence tiebreak should be: (1) most countersignature power; (2) on a tie, lowest
leader VRF score. Lowest block hash is worth adding only as an ULTIMATE fallback for the
pathological case where even the VRF score ties, which happens only when the score is unset on both
blocks (VRF disabled, or a block without a valid VRF proof) or the same leader signed both
(equivocation). In that corner today the comparator drops to node-local first-seen order, which is
the only non-deterministic step; a content-derived key (lowest hash) closes it. So: VRF first,
lowest-hash as the last-resort backstop, not as a replacement for VRF.

## 5. 4b reconciliation: waiting

Understood. I will not implement the 4b reconciliation/convergence rule until you have worked
through the adversarial corner cases. The pieces above (4a, VerifySeqLegSafe) do not depend on it.
