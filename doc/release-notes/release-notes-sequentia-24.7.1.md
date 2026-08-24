# Sequentia Core 24.7.1

A follow-up to 24.7.0, which fixed a cross-chain refund that could not refund
most assets. The fix was right about which asset pays; it was wrong about how
much, and that turns out to matter just as much.

There is no consensus change here.

## A refund's fee has to be sized, not guessed

The refund paid a flat 2,000 atoms. A flat atom count cannot be right for two
assets at once: the same 2,000 atoms is dust in a cheap asset and, in a valuable
one, a fee far above the going rate. This is not hypothetical on this chain —
one live testnet asset prices an entire staking reward at single-digit atoms.

The fee is now sized the way every other fee here is sized: a reference-unit
amount for the transaction's size, converted into whichever asset is paying, at
this node's own rate. The web wallet already did this; the node did not.

## And sized from the measured transaction, not an estimate of it

Sizing it from a guess at the transaction's length turned out to be the same
mistake one level down. A first attempt guessed 350 vB for a transaction that
came to 353, and the refund was rejected for a fee three atoms short.

Guessing high wastes a little. Guessing low is unrecoverable in the way that
matters: the refund is rejected, the asset stays locked, and the next pass
guesses exactly the same number and is rejected again — forever, without anyone
being told why. So the transaction is now built once to be measured and once to
be sent, and the fee comes from the measurement.

## Where the asset went

The swap record now keeps the refund's txid alongside the claim's, and
`listrewardswaps` reports it as `refund_txid`. "Where did my asset go" is the
obvious question to ask of a swap that ended in a refund, and the answer was not
being written down.

## Tests

`feature_pos_reward_xchain.py` now asserts against the refund transaction itself
rather than against balance deltas — the wallet is collecting block subsidies
while a refund is measured, and a balance delta cannot tell a subsidy from a fee.
It checks which asset carried the fee and that the amount tracks the
transaction's measured size at the node's relay rate, rather than pinning a
constant that would only record today's rate.
