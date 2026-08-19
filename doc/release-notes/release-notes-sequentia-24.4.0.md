# Sequentia Core 24.4.0

One thing: staking pools can commit to paying every delegator its exact
proportional share. **If you run a node against the live testnet, upgrade before
height 106,500** -- the new payout mode activates there, and a node that has not
upgraded by then rejects the first proportional-pool coinbase and forks off.

## The consensus change: the `split` payout mode

`direct` commits a pool to one address and `lottery` to a weighted draw; what was
missing was the arrangement most delegators actually expect -- everyone paid in
proportion, every time. The naive version -- splitting each block's reward as it
arrives -- fails at every activity level: on a busy chain one output per
delegator per fee asset per block does not fit in blocks, a small delegator's
single-block share is dust regardless, and on a quiet chain (today's testnet,
with no outside participants yet) most blocks carry no fees at all. So the mode
accumulates and batches.

Under `split`, a pool's block rewards accumulate in an on-chain POT, and anyone
may broadcast a CLAIM that distributes it: each delegator receives exactly its
floor-division share of each pot output it was eligible for, at the P2WPKH of its
own controller key. The claim is fully determined by the UTXO set, so
permissionless claiming is trustless -- and nobody's payout depends on the pool
operator staying interested.

The rules, all enforced by consensus (`bad-pot-claim`), and at mempool acceptance
too, so an invalid pot spend cannot poison block assembly:

- eligibility is per pot output, by creation height: weight counts only if the
  delegation record and the stake existed before the pot output did. Joining a
  pool one block before a claim earns exactly nothing from existing pots, with
  no minimum-age constant to tune;
- shares below 1,000 atoms roll into a fresh pot and accumulate, instead of
  being rounded away every block;
- each pot input reserves 1% to fund its own distribution, and a claim may
  withhold (fee plus the claimer's margin) at most 1/99 of what it delivers.
  That single invariant is the claim interval, the keeper incentive and the
  anti-grief rule at once: burning the pot requires delivering 99x the burn;
- commission is a bp/10000 chance, drawn from Bitcoin's proof of work, that a
  block pays the operator instead of the pot: exact in expectation, and immune
  to the retroactive-commission rug a claim-time lookup would allow;
- pot outputs are coinbase value and mature like any other coinbase reward.

`announcepayout "split"` announces it (the desktop wallet's presets use it for
the "share" arrangements, with the lottery now its own entry), `claimpoolrewards`
builds and broadcasts a claim and pays the claimer's margin to the claiming
wallet, and `listpools` shows every pool's accrued pot.

One sharp edge, stated plainly: leaving a pool forfeits your unclaimed share to
the delegators who stay. Claim before you leave; the wallets say so.

Design and reasoning: `doc/sequentia/split-payouts-design.md`.

## The flag day: height 106,500 on the testnet

To a node without the mode, a split policy record is an inert output, so the
first pot-paying coinbase splits the chain. Below the flag a 24.4.0 node treats
split records exactly as an old node does -- inert -- so upgrading early is safe
and cannot fork anything. Only the live testnet carries the gate (genesis-pinned,
like the Simplicity budget's); a fresh chain has the mode from genesis.

## Upgrading

Stop the node, replace the binary, start it again. Committee operators upgrade
every node in one pass, before height 106,500, then confirm behaviourally:

    contrib/sequentia/peer-stall-check.sh

A peer frozen just below a consensus-change height is running a binary without
that rule, whatever version it reports.
