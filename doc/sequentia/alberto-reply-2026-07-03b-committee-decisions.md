# The committee change: four decisions left, and my recommendation on each

Thanks for confirming Option A. That was the load-bearing call, and it settles the shape of the
whole change: the committee becomes the public top-K of the schedule the chain already computes,
the certificate collapses from ~32 KB to ~300 bytes, and stalls drop sharply. Before I cut the
consensus code there are four things still open. Each has my recommendation so you can approve
quickly; only the first actually gates the code, the rest can ride the re-genesis batch. The full
tables behind all of this are in the committee-sizing memo I sent earlier; this is just the
decision list.

## Decision 1 (this one gates the code): where the committee seed comes from

Making the committee public has one side effect worth closing. The committee for a height is
derived from a seed, and the seed is derived from the block's Bitcoin anchor. Under the old
private sortition a leader could shift the next seed by its choice of anchor but could not SEE
which choice helped it, because each member's eligibility was secret. Now that the committee is
public, a leader can compute the committee for every anchor it is allowed to pick and choose the
one that seats the most of its own coalition. It is a small grind (bounded by how many recent
Bitcoin blocks are valid to anchor to, a handful), but at a one-third coalition it can turn a
capture that should take ~50 years into one that takes closer to ten.

Two ways to handle it:

- **1a. Leave the seed as it is (from the parent block's anchor) and accept the small grind.**
  No new code. Fine at a large committee, weaker at a small one.
- **1b (recommended). Seed the committee from an OLDER anchor, two blocks back instead of the
  parent's.** That anchor is already fixed before the current leader acts, so the current leader
  cannot grind it at all. The only cost is that the committee becomes knowable two blocks ahead
  instead of one, which is the same kind of lookahead you already accepted (and still well under
  Ethereum's one epoch).

My recommendation is 1b. It removes the grind almost entirely for one extra block of advance
notice. If you would rather keep the minimum lookahead and lean on committee size for the margin,
1a with the size in Decision 2 is acceptable.

## Decision 2: the committee size cap

You asked for the smallest safe committee, and your instinct was right that small is better. The
one thing that changed my own answer while measuring is that the cost of a larger committee
almost disappears under Option A: the certificate is now a fixed ~300 bytes no matter how many
members sign, so "bigger" no longer means "heavier blocks." What still scales with size is safety
against a malicious coalition, and that, not sleepy nodes, is what sets the floor. At scale:

| cap (quorum) | tolerates a coalition up to | a one-third coalition captures a block about | nodes |
| --- | --- | --- | --- |
| 100 (51) | ~26% | every 2 days | 100 |
| 200 (101) | ~32% | every 2.4 years | 200 |
| 250 (126) | ~34% | every ~50 years | 250 |

My recommendation is **250**. It is the smallest size that reaches the classical one-third
attacker bound with real margin, and since the certificate no longer grows with the committee,
the only price is running 250 signing keys instead of 100, which the network reaches only as it
decentralises anyway (below 250 stakers the committee is simply everyone, exactly your safe-launch
case). If you weigh node count more heavily, 200 keeps a 32% tolerance for fewer members; 100 is
the floor I would defend, and only if you are comfortable that a coalition approaching a quarter of
the stake is out of scope. The value is a single constant and does not change any of the code, so
you can also leave it to me to set at 250 and revisit at re-genesis.

## Decision 3: fold in the two honest-split fixes now, or later

The cap closes the disjoint-quorum split. It does not close the other two honest splits from the
last memo, and neither is fixed by anything already shipped:

- **3A (certificate lost to a partition).** Fix: gossip the ~300-byte certificate as its own
  message and stop a node from minting a rival at a height where it has seen one. Since the
  certificate is now tiny, this is cheap and natural.
- **3A residual + 3B (the round-boundary re-vote).** Fix: one rule, do not sign a second block at
  a height you have already share-signed until the first can no longer be certified.

Both are prevention-class, node-local, and do not touch finality or reorder any block (neither is
the reconciliation rule you rejected). My recommendation is to **do both now**, in the same
re-genesis batch, so the committee change and the honest-split fixes land together and get tested
together rather than reopening this later. If you would rather see the cap alone first, they can
follow, but they are small next to the committee change.

## Proceeding unless you object

These follow directly from Option A and I will build them as part of it unless you want to weigh
in:

- **BLS keys move into the staker registry.** This is the mechanism behind the 300-byte
  certificate you liked: each member's signing key is registered once, with a one-time proof, so
  it no longer has to ride in every block. It is a small addition to the staking record, folded
  into re-genesis.
- **The quorum is a majority of the ACTUAL committee**, so a launch with 60 stakers runs 31-of-60
  rather than the impossible 51-of-60 today.

## Testing, per your directive

Exactly as you asked, with more stakers than committee seats, and on the box we already have (no
new hardware). The plan: a deterministic test that reproduces the disjoint-quorum split under
today's rule and shows the cap closing it (this is the acceptance test), then a ~150-staker
sandbox at a small cap with induced partitions to watch a real split happen and then not happen,
tagged by region to check the committee tracks the staker distribution, and run at 20/35/45%
of nodes stopped to confirm the stall rates.

## The short version

If you are happy with the recommendations, the one word I actually need is on Decision 1 (seed
from two blocks back, yes or no), and I will start the build against cap 250 with both honest-split
fixes included. The rest you can confirm, adjust, or leave to me and re-genesis. Nothing is in the
consensus code yet; the change is a separate, tests-first branch, and it does not touch the live
chain.
