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
