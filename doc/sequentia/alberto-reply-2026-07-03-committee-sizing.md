# Committee sizing: the cap, the sampling tables, and 3A explained again

Reply to your 2026-07-03 note on `alberto-reply-2026-07-02-honest-splits` and
`alberto-datanote-2026-07-02-committee-size`. Five things: (1) the section-2 cap you
specified, and what it does; (2) the representativeness table you asked for; (3) the
stall-probability tables at 20%, 35%, and 45% sleepy, by the hypergeometric you named;
(4) dynamic-vs-fixed committee size and the value I suggest; (5) 3A explained again with
a worked example. All the numbers below come from `committee-sizing-tables.py` (exact
hypergeometric, checked in beside this note); nothing is a simulation or an estimate.

## 1. Section 2: the cap you specified is exactly the fix, and here is what it enforces

You said: committee capped at `(quorum - 1) * 2`, e.g. `(51 - 1) * 2 = 100`. That is the
right rule and it is precisely the missing invariant. What it buys, stated plainly:

If two different blocks at the same height each collect a quorum Q from a committee of N,
their two signer sets must overlap in at least `2Q - N` members. With `N <= (Q-1)*2` that
overlap is at least 2. So no two conflicting blocks can BOTH be certified unless at least
two members signed both. Honest members that keep a signing record never sign twice, so
with the cap in place a same-height double-certification cannot happen from honest nodes.

The bug today is that the cap is not actually enforced on the certifying set. Committee
membership is decided by a per-staker THRESHOLD, `PosVrfSlot(...) < committee_size`
(`pos.cpp:364`), so the number of eligible members is a random variable (Binomial), not a
fixed 100. The quorum stays pinned at 51 (`PosQuorum`, `pos.cpp:138`). Once the staker
pool exceeds 100, the eligible set often exceeds 100 (your datanote measured >= 102 on
about 40% of slots at 1000 stakers), and then two DISJOINT 51-quorums fit inside it. Your
cap closes this by construction.

The clean way to enforce it is a RANK cap rather than a threshold: the committee for a
slot is the `(quorum-1)*2` stakers with the lowest VRF slot values for that slot's seed,
ties broken deterministically (by pubkey). Every node computes the same set from the
registry plus the seed, so the eligible count is exactly `min(pool, cap)` on every slot,
never more. Note the schedule path already does this (`PosCommittee`, `pos.cpp:133`, takes
`min(pool, committee_size)` of the ranked schedule); the fix is to make the VRF/aggregate
sortition path (`PosVrfIsCommitteeMember`) agree with it instead of using the independent
threshold. This is consensus-level, so it cannot be switched on the live testnet (the
running chain would re-derive membership for its ~17.6k existing blocks under the new rule
and reject them). It folds into the planned re-genesis. Alternative to the rank cap, if
you prefer: derive the quorum from the ACTUAL eligible count each slot (`2Q > eligible`
holds by construction). The rank cap is simpler to validate and matches your `(quorum-1)*2`
framing directly, so that is what I recommend.

One consequence worth stating, because it answers your launch concern below: while the
staker pool is at or below the cap, the committee is the WHOLE pool, quorum is
`pool/2 + 1`, and `2Q = pool + 2 > pool`, so two quorums always intersect. The disjoint-
quorum split is impossible by definition until the pool grows past the cap. A small launch
is safe; the cap only starts doing work once you have more stakers than the cap.

## 2. Representativeness: how far the committee drifts from the staker distribution

Your goal: if 33% of stakers are in Europe, 33% in the US, 33% in Asia, the committee
should be close to 33/33/33. The relevant quantity is the standard deviation of a group's
SHARE of the committee, under sampling without replacement (hypergeometric), with the
finite-population correction. I report it in percentage points for a group that is `p =
1/3` of the population (your example). It scales as `sqrt(p(1-p))`, so the largest single
region, `p = 0.5`, is a x1.061 multiplier on these numbers; smaller regions are less. A
committee's share of a 33% region lands within about `+/- 1.96 * SD` of 33.3% on 95% of
slots.

I give SD, not raw variance, because it is in the same unit as the thing you care about (a
percentage of the committee); it is just the square root of the variance.

**Table 1. SD of a 33% region's committee share, in percentage points.** Rows are
committee size, columns are staker population. `n/a` means the committee cannot exceed the
population.

| committee \ population | 100 | 500 | 1000 | 10000 | 50000 |
| --- | --- | --- | --- | --- | --- |
| 20 | 9.48 | 10.34 | 10.44 | 10.53 | 10.54 |
| 30 | 7.24 | 8.35 | 8.48 | 8.59 | 8.60 |
| 50 | 4.74 | 6.33 | 6.50 | 6.65 | 6.66 |
| 80 | 2.37 | 4.84 | 5.06 | 5.25 | 5.27 |
| 100 | 0.00 | 4.22 | 4.47 | 4.69 | 4.71 |
| 250 | n/a | 2.11 | 2.58 | 2.94 | 2.97 |
| 500 | n/a | 0.00 | 1.49 | 2.05 | 2.10 |

Reading it:

- **Representativeness depends almost entirely on the committee SIZE, not the population.**
  Look along any row from population 1000 rightward: the number barely moves. Once the
  population is more than about 10x the committee, the finite-population correction is ~1
  and the SD is set by the sample size alone. This is the single most important fact for
  the dynamic-vs-fixed question in section 4.
- **The population only matters when the committee is a large fraction of it.** Down the
  left column (population 100), sampling 100-of-100 gives SD 0 (the committee IS the
  population), and 80-of-100 gives 2.37 rather than the 5.27 you get from a large pool.
  Taking a big slice of a small pool is what drives the error toward zero.
- **The knee is around 80 to 100.** At committee 100 a 33% region sits at 33.3% +/- 9.2pp
  on 95% of slots (roughly 24% to 42%). At 250 it tightens to +/- 5.8pp, at 500 to +/-
  4.1pp: real, but diminishing, and bought at 2.5x to 5x the nodes. Below 50 it gets loose
  fast (committee 30: +/- 16.9pp, so a region could be 16% or 50% of the committee).

There is a second reason representativeness matters, beyond fairness: **sleepiness is
correlated with geography** (a whole region's nodes are offline overnight in their
timezone; one cloud provider has an outage). A committee spread evenly across regions has
members awake in every timezone, so it is far more robust to that correlated sleep than a
committee that happened to concentrate in one region. The stall tables in the next section
assume INDEPENDENT sleepiness, which is optimistic; a representative committee is what
makes that assumption closer to true.

## 3. Stall probability, by the hypergeometric, at 20% / 35% / 45% sleepy

You are right that this is hypergeometric. Model: the population has `N` stakers of which
a fraction `s` are sleepy (offline); a committee of `n` is drawn without replacement; the
number of sleepy members is `Hypergeometric(N, round(s*N), n)`. A slot STALLS when the
awake members cannot reach quorum. Under your cap the quorum is `n/2 + 1`, so a slot
stalls exactly when at least half the committee (`ceil(n/2)`) is asleep. `n/a` where the
committee cannot exceed the population.

**Table 2. P(slot stall), 20% of stakers sleepy.**

| committee \ population | 100 | 500 | 1000 | 10000 | 50000 |
| --- | --- | --- | --- | --- | --- |
| 20 | 0.065% | 0.21% | 0.23% | 0.26% | 0.26% |
| 30 | 0.00038% | 0.014% | 0.018% | 0.023% | 0.023% |
| 50 | 0 | 4e-7 | 0.0001% | 0.0002% | 0.0002% |
| 80 | 0 | 2e-11 | 3e-10 | 2e-9 | 2e-9 |
| 100 | 0 | 1e-14 | 9e-13 | 2e-11 | 2e-11 |
| 250 | n/a | 0 | 6e-38 | 6e-27 | 3e-26 |
| 500 | n/a | 0 | 0 | 9e-54 | 4e-51 |

**Table 3. P(slot stall), 35% of stakers sleepy** (your central case).

| committee \ population | 100 | 500 | 1000 | 10000 | 50000 |
| --- | --- | --- | --- | --- | --- |
| 20 | 9.64% | 11.7% | 11.9% | 12.2% | 12.2% |
| 30 | 3.47% | 5.93% | 6.23% | 6.49% | 6.51% |
| 50 | 0.15% | 1.56% | 1.81% | 2.04% | 2.06% |
| 80 | 0 | 0.19% | 0.29% | 0.40% | 0.41% |
| 100 | 0 | 0.041% | 0.083% | 0.14% | 0.14% |
| 250 | n/a | 1e-12 | 1e-8 | 6e-7 | 8e-7 |
| 500 | n/a | 0 | 8e-24 | 1e-12 | 3e-12 |

**Table 4. P(slot stall), 45% of stakers sleepy** (the stress case).

| committee \ population | 100 | 500 | 1000 | 10000 | 50000 |
| --- | --- | --- | --- | --- | --- |
| 20 | 39.9% | 40.7% | 40.8% | 40.9% | 40.9% |
| 30 | 33.0% | 35.1% | 35.3% | 35.5% | 35.5% |
| 50 | 21.1% | 27.4% | 27.9% | 28.3% | 28.4% |
| 80 | 3.75% | 19.5% | 20.6% | 21.5% | 21.5% |
| 100 | 0 | 15.6% | 17.0% | 18.2% | 18.2% |
| 250 | n/a | 1.54% | 3.92% | 6.14% | 6.33% |
| 500 | n/a | 0 | 0.091% | 1.21% | 1.36% |

Reading them:

- **At 20% sleepy, everything from committee 50 up is effectively immune** (below one in a
  million per slot). Even committee 30 is one stalled slot in ~4300.
- **At 35% sleepy, committee 100 is comfortable: 0.14% per slot, about one stalled slot in
  700, and it self-heals** (the next proposer / next slot simply carries on; a stall is a
  brief liveness pause, not a split). Committee 50 is 2% (one in 50, noticeable), committee
  30 is 6.5% (one in 15, bad).
- **At 45% sleepy, no small committee is good.** Committee 100 stalls 18% of slots, 250
  stalls 6%, 500 stalls 1.4%. This is inherent: with a bare-majority quorum you are asking
  whether the majority is awake, and at 45% asleep only 55% are awake on average, so the
  sample crosses 50% often. Larger committees help (the sample concentrates near 45% and
  crosses 50% less), but 45% sleepy means nearly half the network is offline, which is an
  extreme you design to survive, not to run in.
- **The whole-population rows show a subtlety.** Committee 100 of a 100-staker pool never
  stalls at 45% (the committee IS the pool, exactly 45 asleep, 55 awake > 51). Sampling a
  large pool is what INTRODUCES the stall risk: with the whole small pool as committee
  there is no sampling variance, so 45% asleep is just 45%, safely under half. Growing the
  network past the cap is what first exposes you to draw-to-draw variance.

**Why bare-majority quorum, and not 2/3.** These tables use your `(quorum-1)*2` cap, i.e. a
51% quorum. If instead the quorum were the classic BFT 2/3 (67 of 100), a slot would stall
whenever more than a third is asleep. At 35% sleepy that is a coin-flip per slot (~55%),
which is unusable. So the sleepy-liveness requirement essentially FORCES the bare-majority
quorum; your cap is not just safe, it is close to the only workable quorum under real sleep
rates. The cost is that a bare majority tolerates only honest faults and a single
equivocator by itself; resistance to a genuinely malicious member signing two blocks has to
come from staking and slashing plus the durable per-height signing record, not from a
supermajority. That is a reasonable split of duties for a staked committee, but it is a
threat-model choice worth making on purpose.

## 4. Dynamic or fixed, and the value I suggest

Your instinct is right on both halves: there is a dynamic component, and there must be a
fixed cap. Put together they give one rule:

> **committee = min(staker_pool, CAP)**, quorum = committee / 2 + 1, with CAP fixed.

This is dynamic while the pool is small (below CAP you take the whole pool, so the
committee grows with the network and representativeness is perfect because you are not
sampling at all), and fixed once the pool exceeds CAP. It is also exactly the rank-cap fix
from section 1, so the safety fix and the sizing policy are the same mechanism.

Why the cap must be FIXED, not population-proportional: Table 1 showed representativeness
depends on committee size alone once the population is large. Growing the committee from
100 to 500 as the network grows would only tighten a 33% region from +/- 9.2pp to +/-
4.1pp, at 5x the signing and bandwidth cost per block. A population-proportional committee
spends linearly more resources for a benefit that has already flattened out. So: grow to
the cap, then stop.

**Suggested CAP: 100 (quorum 51).** Reasons:

- It is the knee of the representativeness curve (+/- 4.7pp SD, a 33% region within about
  +/- 9pp on 95% of slots).
- Stall is negligible at the realistic design point. At <= 35% sleepy it is at most 0.14%
  per slot and self-heals; it is not a split.
- It is the value the chain already uses, so re-genesis keeps the parameter rather than
  introducing a new one, and 100 BLS-aggregated signatures per block is cheap (the
  aggregate committee path already exists).
- It handles your launch case cleanly: with 40 or 60 stakers the committee is simply all
  40 or 60, fully representative and split-immune, and the cap only engages once you pass
  100 stakers. You can launch small and decentralize into the cap.

**If you want more margin against sustained near-half-offline conditions**, CAP = 128
(quorum 65) roughly halves the 45%-sleepy stall (18% down to 15%) and tightens
representativeness to +/- 4.2pp, for 28% more nodes. I would not go there unless you expect
to routinely run with 40%+ of stakers asleep; at 35% and below, 100 and 128 are both
effectively perfect and 100 is cheaper.

**Do not go below about 80.** Committee 64 to 80 is defensible only if you are confident
sleepiness stays under 30% and you accept +/- 5.3 to 5.9pp representativeness; it saves
nodes but gives up margin on exactly the two failure modes you are trying to minimize.
Below 50, both representativeness and 35%-sleepy liveness degrade sharply. The scan (at a
50,000 population, so the finite-population correction is ~1) makes the trade explicit:

| size | quorum | SD(33%) pp | stall@20% | stall@35% | stall@45% |
| --- | --- | --- | --- | --- | --- |
| 20 | 11 | 10.54 | 0.26% | 12.2% | 40.9% |
| 30 | 16 | 8.60 | 0.023% | 6.51% | 35.5% |
| 50 | 26 | 6.66 | 0.0002% | 2.06% | 28.4% |
| 64 | 33 | 5.89 | 8e-8 | 0.96% | 24.8% |
| 80 | 41 | 5.27 | 2e-9 | 0.41% | 21.5% |
| **100** | **51** | **4.71** | **2e-11** | **0.14%** | **18.2%** |
| 128 | 65 | 4.16 | 3e-14 | 0.034% | 14.7% |
| 160 | 81 | 3.72 | 2e-17 | 0.0067% | 11.6% |
| 250 | 126 | 2.97 | 3e-26 | 8e-7 | 6.33% |
| 500 | 251 | 2.10 | 4e-51 | 3e-12 | 1.36% |

## 5. 3A explained again, with a worked example

3A is a different disease from section 2, and the section-1 fixes do not touch it, which is
what makes it confusing. Here it is from the start.

**The setup.** A block is CERTIFIED in two steps that are separated in time. First the
committee members each emit a signature SHARE for the proposed block. Then one node (the
assembler) collects the shares into the finished certificate and broadcasts the completed
block. A member who emitted a share does NOT automatically hold the finished certificate;
it only knows it contributed a share. The certificate lives in the assembled block.

**The failure.** Take a committee of 4, {A, B, C, D}, quorum 3, and let H be the last
agreed height. At H+1:

1. Leader A proposes block X. A, B, C emit shares for X. Three shares is quorum, so X CAN
   be certified.
2. A assembles X with its certificate and broadcasts the finished block.
3. **A partition strikes exactly between the sharing and the assembly reaching everyone.**
   The finished X reaches A's side only. B, C, D never receive it. They each emitted a
   share, but none of them holds the finished certificate, so none of them knows X actually
   certified.
4. A's side sees X certified and finalizes it. A's finality floor is now H+1 = X.
5. B, C, D see H+1 as unfinished. After the Bitcoin anchor gap, the escaping-stall rule
   lets them treat H+1 as still open and mint a RIVAL block Y at H+1. They adopt Y (to
   them, Y is the only H+1 block that exists). At H+2 they sign a block Z on top of Y.
   Three signatures, so Z is fully certified at H+2. Crucially, signing Z at H+2 does NOT
   violate a one-signature-per-height rule, because H+2 is a NEW height.
6. Now B's chain is `... H, Y@H+1, Z@H+2` with Z certified, and A's chain is `... H, X@H+1`
   with X certified. Work equals height here, so B's chain (height H+2) outgrows A's chain
   (height H+1).
7. **Heal.** A is floor-pinned to X at H+1 and rejects B's taller chain because it forks
   below A's finalized block. B, C, D are on the Y chain with a certified Z at H+2 and
   reject X. Permanent split, and a CERTIFIED block, X, has been orphaned by three of the
   four honest members. No node ever signed two blocks at the same height.

**Why the earlier fixes miss it.** The durable per-height signing record (the section-1
fix) stops a node from signing two blocks at the SAME height. But B, C, D never do that:
they signed X's share at H+1, then signed Z at H+2, a different height. The double-signing
channel is not present, so the signing record has nothing to catch. Section 2's cap does
not help either: this needs only 4 members and one partition, it does not need the eligible
set to exceed the cap.

**The fix, in two parts.**

1. **Gossip the certificate as its own object.** The quorum certificate is small and
   self-verifying: the block hash, the set of signers, and their aggregate signature, a few
   hundred bytes. Gossip it independently of the full block body. Then, even when the
   assembled block X is lost to the partition, X's CERTIFICATE still floods to B, C, D. The
   moment they receive it they learn "H+1 is certified to X", pin their finality floor to X,
   and fetch X's body from any peer that has it. The certificate is the piece of
   information that has to cross the partition, and today only the full block carries it.
2. **Suppress escaping-stall at any height where a certified sibling is known.** Escaping-
   stall exists to recover from a genuine stall where nobody certified anything. A node that
   holds a certificate for H+1 (via the gossip in part 1) must NOT use escaping-stall to
   mint a rival at H+1. So B, C, D, once they have X's certificate, never mint Y in the
   first place.

**Same example, with the fix.** At step 3 the partition still drops the finished block X,
but X's certificate reaches B, C, D by gossip. They pin to X at H+1 (part 1) and
escaping-stall is suppressed there (part 2), so they never mint Y. They request X's body
and adopt it. No rival, no split. The worst case, if X's body is briefly unavailable, is
that B, C, D STALL at H+1 waiting for it, which is a recoverable liveness pause. The fix
converts a permanent safety split into, at worst, a temporary stall, which is the trade you
already accepted for the tiebreak.

To be explicit about scope: the section-2 cap and the 3A fix are independent. The cap stops
two disjoint quorums at the same height; certificate gossip stops a certificate from failing
to cross a partition. Neither fixes the other, and both are prevention-class: neither
reorders a certified block or weakens finality, so neither is the 4b you rejected.

## 6. 3B, briefly, plus a testing note

**3B (the round-advance re-vote), one paragraph so you have it while you re-read.** The
round number is a pure function of wall-clock time (`DriveRound`). If the round-0 leader
L1 gathers its quorum right at a round boundary, but L1's assembled block propagates slower
than one round, the nodes that have not yet seen L1 advance to round 1 by their clocks and
re-sign the round-1 leader L2 at the SAME height, while L1's round-0 shares are still live
and assemblable. If both cohorts reach quorum, both L1 and L2 certify at that height, and
the nodes that re-voted signed BOTH. That is a second way honest nodes double-sign, driven
by the clock, not by memory loss. The fix is a round-change lock: do not back a round-1
rival at a height while an earlier-round block there could still certify. Say the word and I
will write 3B up as its own worked example like section 5.

**Testing, since you want stakers > committee empirically.** You do not need another box.
Run more staker processes than the committee cap on the hardware you have: a sandbox with,
say, 60 stakers and committee 30 (or 100 stakers and committee 30, the config in your
datanote) already has pool > committee, so the disjoint-quorum window from section 2 is
open on about a third of slots and an induced partition can demonstrate the actual split.
The same sandbox is where you confirm the rank cap CLOSES it: with the fix, the eligible
set is `min(pool, cap)` every slot and the window probability drops to 0. This lines up
with your point that we have caught real issues only in empirical scenarios, and it fits
the re-genesis plan (a separate cluster now for observation, the fix folded into
re-genesis, not switched on the live chain).

---

Numbers: `doc/sequentia/committee-sizing-tables.py` (exact hypergeometric, no external
deps). Nothing here is implemented in consensus code yet; the rank cap, certificate gossip,
and round-change lock are all consensus-level and yours to weigh, same as before.
