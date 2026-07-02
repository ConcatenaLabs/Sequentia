# Where we are, and what is pending your decision

A short map so the open questions are clean while you think through the 4b corner cases.

## Settled / in place

- **4a** (equivocation prevention) is live on all 100 committee nodes: a producer holding a
  quorum-certified block at a height whose anchor is being restored after a reorg-of-reorg neither
  proposes nor backs an alternative there; it waits for the previous best chain to return.
- **VerifySeqLegSafe now also requires quorum certification**, not just anchoring (a swap leg's
  block must be immediately final AND anchored at/after the counter-leg). No minimum-anchor-depth
  wait; the anchor axis stays 0-conf, so real-time anchoring is intact.
- **The VRF is reliable** for sortition, leader selection, and voting, and the leader VRF score is
  already the implemented same-height fork-choice tiebreak (my earlier "unreliable" was a misread
  of a comment about VRF-as-a-sortition-seed, not fork choice).

None of the above depends on the pending item below.

## Pending your decision: the 4b reconciliation rule

The one open consensus question is whether (and how) to add an automatic convergence rule for the
RESIDUAL case, and its exact shape. To frame your corner-case thinking, here is the decision laid
out.

**The regime it targets.** 4a plus 4b's strictly-above rule already heal every ASYMMETRIC split
(the majority keeps producing quorum blocks above the fork; the minority yields; the minority can
never make another quorum). The residual is the SYMMETRIC case: roughly half the committee on each
side, neither ever reaching quorum, both sides advancing sub-threshold via escaping-stall. That is
governed by the fork-choice comparator, not by 4b and not by the finality gate (sub-quorum blocks
are never immediately final).

**Corner cases worth pressure-testing before you commit:**

1. **Is a rule even needed, or is the comparator enough?** In the symmetric split the comparator
   ties on signature power; the next key, the leader VRF score, breaks it deterministically the
   moment nodes see both chains, EXCEPT in two pathological cases: the VRF score is unset on both
   (VRF disabled, or a block with no valid VRF proof) or the same leader signed both (equivocation).
   Only there does the code drop to node-local first-seen, which cannot converge a partition. So the
   minimal fix may be just: replace that last first-seen step with a content-derived key (lowest
   block hash). Question for you: is lowest-hash acceptable as the ultimate backstop, given that the
   only party who can grind it is one that already controls ~half the committee (i.e. already in the
   compromised regime)?

2. **The reconvergence double-spend.** Any rule that lets a node abandon a chain can, in principle,
   reorg a swap that settled on the losing side. The efficient version of that attack is a
   majority-STAKE attack with ZERO Bitcoin hashpower (root cause B: both chains re-anchor to the
   same valid Bitcoin block, so anchoring cannot arbitrate and Bitcoin is left untouched so the
   Bitcoin leg stands). A cheaper sub-majority variant needs an induced stall. Question: what safety
   boundary is acceptable here, given we already assume honest-majority stake?

3. **The safety coupling is now in place.** Because VerifySeqLegSafe requires quorum certification,
   a swap can no longer settle on a sub-quorum (reorgable) block, which closes the reconvergence
   double-spend against swaps by construction, independent of whatever 4b rule you choose. Worth
   keeping in mind: the swap-safety hole and the convergence rule are now decoupled.

4. **A finality-overriding timer.** I would advise against any "after N Bitcoin blocks, force
   reconvergence" rule that can reorg an immediately-final block: it turns a rare, detectable,
   fail-safe stall into an attacker-inducible reorg of final history (inducible with zero Bitcoin
   hashpower via a partition), and it reopens the same-height comparison you deliberately withdrew.
   The deterministic-tiebreak fix in (1) handles the sub-quorum case without touching finality. But
   this is your call, and it is exactly the kind of corner case you are right to sit with.

**My recommendation, for when you are ready:** the smallest safe change is (1), the deterministic
content-derived backstop for the last comparator step, applied only where signature power AND the
VRF score both tie. Everything else (finality-overriding timers, same-height certificate
comparison) I would leave out. But I will not implement anything here until you have finished with
the corner cases and told me the shape you want.

## Next

Separately, we are about to open the Simplicity-in-the-DEX thread (covenant-enforced offers on the
Sequentia leg; the Bitcoin leg stays interactive). That is independent of the above and does not
touch the 4b decision.
