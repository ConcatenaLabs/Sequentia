# Sequentia Core 25.2.0

25.2.0 fixes what the October 2026 security audit found, and carries a fork.
Part of the fixes change only what a node does locally and take effect as soon
as it runs. The rest changes consensus and takes effect from one height:
**163,000** on the testnet (`pos_hardening_height` and `pos_records_v2_height`);
from block 1 on mainnet, which has not launched.

**Every node must run 25.2.0 before the testnet reaches height 163,000**, the
block producers and committee members above all. At about a block a minute that
is around 8 October 2026, evening UTC, but the height decides, not the date.
From 163,000 the committee certificate carries a new field, stake record spends
are signed differently and blocks follow new rules, so a 25.0.x node and a
25.2.0 node refuse each other's blocks there. Upgrade the committee all at once,
as for every release that changes fork choice.

## What changes at 163,000

- **Committee seats by stake.** The public committee is no longer one place per
  staker. Its seats (at most `-poscommitteesize`, 250, and never more than the
  number of eligible stakers) are apportioned in proportion to stake, and the
  quorum is a majority of seats, not of members. With equal stakes nothing
  changes. One staker with most of the BLS-registered stake now certifies alone,
  and the chain needs it online to reach quorum. The certificate carries the
  seat total as a fourth push, and `poscountersigs` in `getblockheader` counts
  seats from this height.
- **Records need their key.** A delegation record must be created by a
  transaction that spends a coin of its controller, and a payout record by one
  that spends a coin of its signer; anyone could create either in another key's
  name before. The wallet does this by itself (`delegatestake`, `announcepayout`
  first pay a small amount to the key and spend it in the record's
  transaction). A delegation to the controller itself is refused, and a record
  may not be spent and re-created identically in one block.
- **Stake record spends sign the amount.** Spending a staking, unbonding,
  delegation or payout output now signs the segwit-v0 hash, which commits to the
  amount spent, and its scriptSig has one valid encoding: a signer that sees only
  the transaction cannot be lied to about the fee, and nobody can change its
  txid. The wallet signs for whichever rule the next block follows. A
  withdrawal, claim or record spend still waiting in the mempool when the chain
  reaches 163,000 is evicted: send it again (`withdrawstake`, `claimunbonded`).
- **No removal of a payout policy without notice.** The payout record in force
  cannot be spent. To change or end a policy, announce the next one (a direct
  payout to yourself, to stop sharing), wait out the notice, and then reclaim
  the old record. A record still inside its notice may be withdrawn.
- **Split pools pay in rounds, with no limit on delegators.** A split pool's
  rewards of one epoch (1,440 blocks, about a day) become a round, shared among
  those who stood behind the pool when the epoch began, in buckets of about 32
  delegators. Each claim pays one bucket of each round, so any delegator
  collects its share with a small transaction however large the pool, and nobody
  can block anyone else's payout. `claimpoolrewards` pays the bucket of the
  wallet's own delegated key by default; `listpools` lists open rounds; the GUI
  has a "Collect my pool rewards" button. Joining a pool during an epoch earns
  from the next one. Pots from before 163,000 are claimed as before.
- **Lottery.** The lottery draw is seeded from the anchor three blocks below,
  which the producer of the parent block cannot choose.
- **Anchors.** An anchor that repeats its parent's Bitcoin hash must repeat its
  height too; a block could claim a later Bitcoin height and advance every clock
  that reads it (unbonding, stalls, finality).
- **Supervised assets.** At most one key rotation per asset and role per block
  (two of them made the node unable to restart). No issuance on an input that
  spends a supervision record. No supervision declaration or record in a
  coinbase, which skipped the signature check and let a producer freeze any
  holder.
- **No stake weight for an uncompressed key**, which can never prove a VRF
  output and only diluted everyone else's slots.

Records, stake and policies created before 163,000 stay valid as they are.

## What changes at once (no fork)

- A bad committee certificate, which the block hash does not commit to, no
  longer marks an honest block invalid. Before, a peer could stall a node on a
  height for good by relaying an honest block with a garbled certificate.
- BLS points are decoded with their length: a crafted share, proof of
  possession or certificate made the node read past a buffer.
- Committee gossip is deduplicated on content, and shares must be signed with
  the member's registered key; a forged share or certificate sent first no
  longer shuts out the real one. Gossip no longer pays for signature checks and
  block downloads before classifying a message.
- The record rules are applied at mempool admission and in block assembly, so a
  dust transaction can no longer make every producer build an invalid block. A
  template that still fails becomes a block with no transactions instead of a
  missed slot.
- Parent-daemon errors, and "not found" from a bitcoind still syncing, are no
  longer read as an anchor being off Bitcoin's best chain; before, a bitcoind
  restart could make the watcher invalidate valid history. Checkpoints are kept
  per commitment and persist across restarts (`poscheckpoints.dat`). The parent
  RPC timeout defaults to 10 s.
- Fee and configuration fixes: relay ordering and saturated fee values, a
  1 MiB cap on the reference price reply, and consensus options that could fork
  a node in silence (`-posescapestallmtpgap` and others) refused on the testnet
  and mainnet.

## Upgrading

Stop the node, replace `sequentiad` and `sequentia-cli` (and `sequentia-qt`),
start it. Nothing to migrate: `poscheckpoints.dat` is created on first run. Check
that `sequentiad -version` reports v25.2.0.

## A node left behind at 163,000

A 25.0.x node refuses the first 25.2.0 block at 163,000 (its certificate has a
field the old code reads as trailing data) and marks it invalid. On an upgraded
node, its peer's `synced_headers` stays at 162,999; on the old node,
`getchaintips` lists the valid chain's block at 163,000 as `invalid`.

Recovery: upgrade to 25.2.0, restart, then

    sequentia-cli reconsiderblock <hash of the valid block at 163,000>

The invalid mark is on disk and upgrading does not clear it. If the old node had
also built blocks of its own at 163,000 or above, `invalidateblock` its own
first block there as well.
