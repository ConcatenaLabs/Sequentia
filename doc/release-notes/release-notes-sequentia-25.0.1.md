# Sequentia Core 25.0.1

25.0.1 corrects defects in 25.0.0 that would have stopped or split the testnet
at the two-step-unbonding fork. The fork itself is unchanged: from height
**159,000** a block may spend a staking output only into stake or into an
unbonding output of the same key, and that height is the same as in 25.0.0.

**Every node must run 25.0.1 before the testnet reaches height 159,000**, the
block producers above all. 25.0.0 must not be the version that crosses that
height: a single withdrawal broadcast at the wrong moment stops every 25.0.0
producer there (below).

## Producers no longer stop at the activation height

### What was wrong

The mempool admits a transaction for the next block. A staking output spent
straight to an address is valid in every block below 159,000, so such a
withdrawal admitted at height 158,998 and not mined in block 158,999 stayed in
the mempool, invalid for block 159,000, and nothing removed it. Every
producer's template for 159,000 carried it and failed validation
(`TestBlockValidity failed: bad-unbond-required`), every producer skipped its
slot, and the chain stopped until the producers were restarted. Any staker
with a matured stake could cause it on purpose, at no cost, and an honest one
by accident with a badly timed `withdrawstake`.

A parent-chain reorg did the same to a claim (`bad-unbond-premature`): when the
anchor of the block that confirmed an unbonding output was reorganized away on
Bitcoin, that block was disconnected and the unbonding transaction returned to
the mempool, while the claim spending it stayed.

### What changed

- Connecting the block before the activation height evicts from the mempool
  every transaction the rule refuses in the next block, with its descendants.
- The end of a reorg judges the rule again, at the tip the reorg ends on.
- The block template leaves out any transaction the rule refuses.
- A block a producer cannot assemble is reported in the default log
  (`no block at height ..., block assembly failed: ...`), once per height and
  reason. In 25.0.0 that line needed `-debug=validation`, so such a stop was
  silent.

The wallet keeps an evicted withdrawal or claim as an unconfirmed transaction
that is in no mempool, and goes on counting its inputs as spent. Release them
with `abandontransaction <txid>`, then withdraw or claim again: from 159,000
`withdrawstake` builds the two-step form.

None of this changes which blocks are valid. It changes which transactions a
node keeps and offers.

## Fork choice no longer depends on certificate size

### What was wrong

Among blocks at the same height, 25.0.0 preferred the one whose certificate
named more committee members. The block hash excludes the BLS certificate, and
any node holding a quorum of signature shares can assemble one, so a single
block reaches different nodes with certificates of different sizes, and each
node kept the count of the first it saw. Two nodes holding the same two
certified siblings could rank them oppositely, finalize opposite blocks when
the observation window closed, and stay split, each refusing the other's
branch with `bad-fork-prior-to-pos-final`. Members signing two blocks at one
height could bring this about using honest signature shares taken from
gossip.

### What changed

Same-height blocks are ordered by whether they are certified (the certificate
names at least the quorum), then by the leader's VRF score, then by block hash.
The number of members a certificate names is never compared: every valid
certificate of a block that is not escaping a stall reaches the quorum, so
whether a block is certified is the same on every node. A certified block is
still final against every sibling once its observation window has passed, and
finality still never blocks a reorg that follows Bitcoin.

Between two blocks that are both below the quorum (escaping-stall blocks),
the one whose certificate names more members no longer wins; the VRF score and
then the hash decide, as for certified blocks.

This changes which branch a node follows when same-height siblings compete,
not which blocks are valid; it is why the version moves.

## A finalized block stays final

### What was wrong

25.0.0 kept the time each quorum block was first seen for its observation
window only for blocks within 100 of the tip, and every finality pass gave a
block it had no record of a fresh window. Once the newest quorum block was more
than 100 blocks below the tip, which is what a committee stall does (escaping-
stall blocks carry no quorum), it was back in its window on every pass and
nothing was final. A rival branch forking below it was then adopted: in a test,
25.0.0 reorganized a finalized block away for a longer branch whose only
quorum block was its own block at that height, where 24.7.13 refused it with
`bad-fork-prior-to-pos-final`. After a restart the same held until a quorum
block was again within 100 of the tip. In that state each pass, run on every
block and every 250 ms, also walked the chain down to its first block.

### What changed

- A finalized block, and every ancestor of it, stays final. Its window is never
  opened again, whatever the distance to the tip.
- The window opens when a quorum block joins the active chain after the initial
  block download. A block connected during that download or loaded from disk
  counts as observed, so a restarted node finds its finalized block final again
  before it connects to any peer.
- Only Bitcoin moves the finalized point back, as before: when the anchor
  watcher invalidates a finalized block whose anchor was reorganized away, its
  ancestors stay final and the node follows the branch Bitcoin leaves standing.
- A pass examines only the blocks connected since the previous one and those
  still in their window.

None of this changes which blocks are valid.

## Withdrawal fees are sized in the asset that pays them

### What was wrong

`withdrawstake`, `claimunbonded` and `bumpwithdrawstakefee` computed the fee in
reference units and paid that number of SEQ atoms, without the exchange rate.
The fee was right only while SEQ was priced at one reference unit per atom: at
0.25 a withdrawal paid a quarter of the node's fee rate, at 4 four times it, and
at 0.01 a claim fell below the relay minimum and was refused. None of the three
let the fee be paid in another asset, although consensus accepts that.

### What changed

- Each fee is the node's fee rate over the transaction's size, converted into
  the paying asset at the node's exchange rate, as every other send does.
- By default the fee is still paid in SEQ out of the coins being moved. Under
  two-step unbonding a stake may pay at most 1% of the staking outputs it spends
  that way (a consensus rule: a 10 SEQ partial withdrawal from a 100 SEQ output
  may pay up to 1 SEQ). A fee above that, or a node that does not accept SEQ, is
  refused with a message that names `fee_asset`.
- `fee_asset`, on all three, pays the fee in that asset, any the node accepts
  and SEQ included, from the wallet's other transparent coins; the whole stake
  or claim then arrives. `bumpwithdrawstakefee` keeps the pending withdrawal's
  fee asset unless told otherwise, and its replacement pays the original's fee
  value plus the incremental relay fee, judged at the node's exchange rates.
- Under two-step unbonding `withdrawstake` refuses an `address` instead of
  ignoring it: the coins go to an unbonding output, and `claimunbonded` takes
  the address.

## Custom chains enforce two-step unbonding from genesis

A fresh custom chain (such as `elementsregtest`) now enforces two-step unbonding
from its first block, as the mainnet does, instead of only when
`-posunbondheight` was given. `-posunbondheight` still moves the height on a
custom chain, and `-posunbondheight=0` turns the rule off, which is how a test
reproduces the one-step withdrawal the testnet allows below 159,000. A custom
chain whose history already holds a one-step withdrawal must be started with
`-posunbondheight=0`, or a height above that withdrawal, to sync or reindex.
The testnet and the mainnet are not affected: their heights are fixed in code.
