# Sequentia Core 25.2.5

25.2.5 is 25.2.4 plus a change to how a block producer takes part in
certification, and one field on `listpools`. It changes no consensus rule: every
block 25.2.4 accepts, 25.2.5 accepts, and the reverse. The fork at height
**163,000** is the same.

## A producer that holds a quorum alone takes part in the round

From height 163,000 the public committee's seats are apportioned by stake, so a
staker with more than half of the stake holds a quorum of seats on its own
host. Such a host certified its own block at its own slot and never took part
in the gossip round, so it never countersigned another staker's proposal. Its
seats are part of every quorum, so no other proposal could be certified: that
staker produced every block, and every other staker, the staking pools
included, produced none.

A host now certifies alone only while no other host commands an eligible stake,
or while it has no peers. Otherwise it takes part in the round like any member
and countersigns the proposal the election backs, whoever proposed it. Blocks
are shared out in proportion to stake again, and a pool's delegators earn from
the blocks its weight entitles it to.

## `listpools` gives each signer's latest block

Each row carries `last_block_height` and `last_block_time`: the most recent
block the signer produced within the window. Both are absent when it produced
none there.

## Startup takes each producer key once

`startposproducer` and `startstaking` save the producer's keys to
`settings.json`; a key also listed in the configuration file was then held
twice after a restart. Startup now keeps one copy of each.

## Upgrading

Stop the node, replace `sequentiad`, `sequentia-cli` and `sequentia-qt`, start
it. Check that `sequentiad -version` reports v25.2.5. Nodes on 25.2.4 and 25.2.5
agree on every block. A block producer that holds a quorum of seats by itself is
the one whose behaviour changes; every committee member is upgraded at once,
as with any change to how the committee signs.
