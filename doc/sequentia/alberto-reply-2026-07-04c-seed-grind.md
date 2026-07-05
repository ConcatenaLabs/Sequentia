# Correction: burying the committee seed (1B) does not reduce grinding

I owe you a correction. In the implementation spec I recommended decision 1B, seeding the
committee from an anchor two blocks back, to remove the grind that a public committee opens,
and you confirmed 1B. Building it, I proved it does not work: burying the seed by any fixed
depth just moves the same grind from the leader of the block before the target to the leader
of the block that many blocks before it. The lever is one producer freely selecting a single
seed input, and shifting which producer holds it changes nothing. So I did not ship 1B; the
seed is unchanged (the current rule, 1A). This does not change anything else we have built or
any number I have sent you: the cap-250 capture figures already assumed this grind. But the
spec's reasoning was wrong and you approved it on my recommendation, so here is exactly what
is true, why, and the honest options. Numbers are from `committee-seed-grind.py`, checked in
beside this note.

## 1. What the grind is, and why the public committee is what turns it on

A Sequentia block anchors to a Bitcoin block, and the committee for the next height is
seeded from that anchor's hash. A producer's one freedom is which recent Bitcoin block to
anchor to. The consensus rules bound that freedom tightly (anchor heights only advance, and
the anchor must be a real block on Bitcoin's best chain), so in practice a producer chooses
among a handful of fresh Bitcoin blocks, call it k, usually two to a few. Each choice yields
a different seed and therefore a different committee at the next height.

This lever has always existed. The code that derives the seed even says so, and says why it
was considered harmless: it calls the producer's anchor choice "bounded, VRF-mitigated
grinding." The mitigation was the private committee. Under the old private sortition, a
producer could shift the next seed but could not SEE the resulting committee, because
membership was each staker's secret VRF output; grinding was pointless because you could not
tell which anchor helped you.

Option A, the public committee you approved, removes that mitigation. The committee is now a
public function of the seed and the staker registry, so a producer can compute the committee
for every anchor it could choose and pick the one that seats the most of its own coalition.
The grind that was dormant becomes live. This is a real and expected consequence of going
public, and it is what decision 1 in the spec was meant to answer.

## 2. Why burying the anchor does not answer it

The seed for height H is a hash of the anchor of some earlier block and H. Move that earlier
block from one-back to two-back and the producer of the two-back block now chooses the anchor
that sets H's committee. That producer can compute H's committee for each of its anchor
choices just as well as the one-back producer could: the registry is public and barely moves
over two blocks. So the grinder is always "the producer of the block that supplies the seed,"
whichever block that is, and its power is the same: best of k choices, available only on a
block it leads, which it does with probability equal to its stake share. The capture
probability has no dependence on the burial depth at all. The script prints it four ways, for
depth one, two, three and five, and they are byte-identical (3.6e-8 per height at cap 250
against a one-third coalition); the formula simply contains no depth term.

Two related dead ends, so we do not revisit them: mixing several past anchors into the seed
(RANDAO-style) also fails here, because a producer that controls even one of the mixed inputs
and varies it k ways still gets k selectable seeds; and burying deeper (three, five, ten
blocks) is not only equally ineffective, it is worse, because it makes the committee knowable
further ahead, which only helps a denial-of-service attacker pick its targets.

## 3. How big it actually is, and why the cap already covers it

The grind multiplies the base capture probability by roughly one plus (stake share times
k-minus-one): a small factor at realistic k. What matters is that at the cap we chose it stays
in the range the cap was picked to give.

Committee cap 250 (the recommendation), population 50,000, per height and as time to the first
captured block at 30-second spacing:

| coalition stake | no grind (k=1) | grind k=4 | grind k=8 |
| --- | --- | --- | --- |
| 25% | 3e11 yr | 3e11 yr | 3e11 yr |
| 33% | 53 yr | 27 yr | 16 yr |
| 40% | 15.5 h | 7 h | 4 h |

The grind roughly halves the time-to-capture for a one-third coalition, from once in ~50
years to once in ~20, which is still decades and still well inside the budget I gave you when
recommending 250. For contrast, at cap 100 the same one-third coalition captures roughly once
every one to two days with or without the grind, which is exactly why I recommended against
100 for a one-third threat in the first place. So this finding does not move the cap
recommendation: 250 absorbs the grind; 100 was already too weak on the base numbers. Above
40% nothing sampling-based holds, grind or no grind, as before.

## 4. Options going forward

- **A (recommended): accept it, keep cap 250, ship the seed as built (1A).** The residual is
  a one-in-decades event for a coalition approaching the classical one-third bound, it was
  already priced into the numbers behind 250, and the seed stays simple and reorg-safe. This
  is what the implementation is on now.
- **B (a real fix, as separate future work): seed the committee from the leader's own VRF
  output.** A leader cannot select its VRF output without changing whether it is the leader at
  all, so this collapses the k choices to one and removes the grind entirely. The catch is in
  our own code: this exact input was tried before and reverted as unreliable, because the
  leader's VRF score is set after the block index is built, so nodes transiently disagree on
  the seed. Making it work is real, isolated consensus work; I would scope it as its own
  change, not fold it into the re-genesis, and it is not needed to make 250 safe.
- **C (partial): remove the producer's anchor choice for seeding.** Derive the committee seed
  from a Bitcoin block at a fixed height offset rather than the producer's chosen anchor, so
  there is nothing to select among. This reduces k toward one but couples committee membership
  to Bitcoin reorgs and wants care; I mention it for completeness and do not recommend it over
  A.

My recommendation is A now, with B tracked as the eventual grinding-resistance upgrade if we
ever want to push the safe committee size below 250 or harden against coalitions past a third.

## 5. Status, so nothing is ambiguous

The public committee, certificate gossip, the share-lock, and the timing controls are all
built and tested on the unchanged 1A seed, flag-gated so the live chain is untouched, on the
implementation branch. Nothing needs redoing because of this finding. The only thing that was
wrong is the spec's decision 1, and it is corrected here. What I need from you is one word:
accept A (and I will note B as tracked future work), or tell me to build B or C before the
re-genesis.

Numbers: `doc/sequentia/committee-seed-grind.py`. The grinding lever and its old mitigation
are documented in-tree at `PosSeedForChild` (src/pos.cpp); the anchor-choice bound is the
monotonic-advance rule in `ContextualCheckBlockHeader` (src/validation.cpp).
