# Sequentia Core 24.4.1

One change: the split payout mode's flag day on the live testnet moves from
height 106,500 to **height 102,150** -- about a day after this release instead
of four. The operators were asked how much runway they needed, which is the
question 24.4.0 should have asked before writing a height; the answer was one
day.

Moving a flag day down is safe here because nothing binds below it: a split
policy record is inert on every node until the height, on 24.4.0 and 24.4.1
alike, so the chain cannot have taken on any state the move would contradict.
The requirement is unchanged in kind: **every testnet node must be on a binary
carrying the flag -- now this one -- before height 102,150**, or it rejects the
first pot-paying coinbase and forks off.

Everything else about the mode is exactly as the 24.4.0 notes describe.
