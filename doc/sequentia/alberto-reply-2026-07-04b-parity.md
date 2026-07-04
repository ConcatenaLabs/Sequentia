# 249, 250 or 251: does committee-size parity matter?

Short answer: no, it does not really matter, and one small correction to the memory: I never
suggested odd committee sizes. What the earlier note said is almost the reverse: under the
majority quorum, ODD sizes get one unit LESS safety margin than even ones, and I offered an
optional one-line quorum tweak to erase that difference. Here is the whole story, with exact
numbers, so the question can be closed.

## The numbers: 249, 250, 251 are statistically the same

Exact hypergeometric, population 50,000, majority quorum Q = K/2 + 1:

| K | Q | overlap | stall @35% | stall @45% | capture @33% | capture @40% |
| --- | --- | --- | --- | --- | --- | --- |
| 249 | 125 | 1 | 5.8e-7 | 5.6% | 2.7e-8 | 6.9e-4 |
| 250 | 126 | 2 | 7.6e-7 | 6.3% | 1.8e-8 | 5.4e-4 |
| 251 | 126 | 1 | 5.2e-7 | 5.6% | 2.4e-8 | 6.6e-4 |

Every stall and capture probability differs by at most a few tens of percent RELATIVE, at
absolute levels where it is meaningless: capture by a one-third coalition is once per roughly
40 to 60 years in all three cases, a 35%-sleepy stall once per 1.5 to 2 years in all three.
Nothing in these columns can justify choosing one of the three over another.

## The one thing parity does change

The overlap column is the only structural difference. Two quorums drawn from K members must
share at least 2Q - K members. At even K that is 2; at odd K it is 1. Concretely: for two
conflicting blocks to BOTH be certified, at least 2Q - K members must have signed both. So at
K = 250 a double-certification needs at least TWO double-signers; at 249 or 251, ONE
double-signer is enough, provided the remaining members split into two perfect halves behind
two different blocks.

How much that is worth after all the fixes: honest nodes can no longer double-sign at all
(the durable signing record closes the crash case, the share-lock closes the round-boundary
case), so the overlap margin only counts against a DELIBERATE equivocator, a malicious member
knowingly signing two blocks. The parity difference is then exactly this: does enabling a
split require one traitor or two, in a scenario that additionally needs a surgically timed
partition of everyone else. That is a thin margin against an already exotic attack, and
equivocation leaves compact cryptographic proof (two signed headers at the same height), so
the traitor is identified either way. It is real, but it is the least important line of
defence in the design.

## Below the cap, you are right

While the pool is smaller than the cap the committee is the whole pool, so its parity is
whatever the staker count happens to be that day; nobody chooses it. This growth phase is the
only place the optional tweak from the earlier note does anything: setting Q one higher when K
is odd keeps the overlap at 2 at EVERY size, for the price of one extra required signature at
odd sizes. As the table shows, that extra signature costs nothing measurable in stall
probability. It is a one-line rule; take it if you like uniform invariants ("any
double-certification requires at least two Byzantine signers, always"), skip it if you prefer
zero extra rules. I would take it, purely because it makes the security statement one sentence
with no parity footnote.

## Bottom line

- 249 vs 250 vs 251: statistically indistinguishable, so the choice is free.
- The cap should simply stay 250: your own formula, cap = (Q - 1) x 2, always lands on even
  numbers, and even sizes happen to carry the stronger overlap for free.
- The odd-size case only ever occurs by itself, below the cap, during growth; the optional
  odd-K quorum bump neutralises it for one extra signature, and is the only decision hiding in
  this question. Default if you say nothing: I include it.
