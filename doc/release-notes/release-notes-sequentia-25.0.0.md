# Sequentia Core 25.0.0

Finality now survives committee members who sign two blocks at one height,
and a stake that leaves now stays locked until a Bitcoin checkpoint could have
secured what its keys signed. The second is a consensus rule and a testnet hard fork,
which is why the version moves to 25.

**Testnet hard fork at height 159,000.** From that block a node on 25.0.0
rejects a block that spends a staking output anywhere but into stake or an
unbonding output, which a node on 24.7.x still accepts. Every block producer
must run 25.0.0 before the testnet reaches it; a node that does not will follow
whichever branch an outdated producer extends, and will need `reconsiderblock`
after upgrading if it accepted a block the new rule rejects.

## Finality against equivocating committee members

### What was wrong

Two quorums of a majority-quorum committee overlap in only two members. Two
members who sign both of two same-height proposals can therefore certify both,
whenever the honest members are divided between them, and that division needs
no network partition: an attacker watching the signature shares in gossip
releases a competing proposal as the collection window closes, and signs both
blocks only when the division came out even. Because a quorum-certified block
was final the moment a node connected it, each half of the network kept the
block it saw first and refused the other, and the two branches kept growing.

### What changed

- A quorum-certified block becomes final only after standing on the active
  chain for an observation window (`-posfinalitydelayms`, default 3000 ms).
  Inside it a competing quorum-certified block, which only members who signed
  twice can produce, is judged by the ordinary comparator, so every node that
  holds both converges on the same one.
- A node that has verified a competing certificate whose block it has not yet
  received and judged keeps that height, and every block above it, undecided
  until it has, for at most `-posfinalityholdms` (default 30000 ms).
- Nodes without a producer verify, relay and act on `poscert` messages.
- `getposfinality` reports the finalized point and any certificate holding the
  next one.

None of this changes which blocks are valid; it changes when a node stops
reconsidering a height. Finality takes about ten seconds from the proposal, and
up to thirty more only while a competing certificate's block is awaited.

## Two-step unbonding

### What was wrong

A staking output counts as stake until it is spent, and its timelock runs from
its creation, so a stake older than the timelock could sign a block and leave in
the next one. Its keys were then worthless to their owner, and available to
anyone who wanted to rewrite recent history, long before a checkpoint secured
it.

### What changed

- Where the rule is active, a staking output may only be spent into stake or
  into an unbonding output of the same key. The unbonding output carries no
  weight and can be spent only once Bitcoin has advanced 2,016 blocks past the
  anchor of the block that created it, the depth a checkpoint needs to
  consolidate.
- `withdrawstake` performs the first step and `claimunbonded` the second. The
  fee of the first step may come out of the stake, up to 1% of it, and
  `bumpwithdrawstakefee` re-sends a pending first step within the same limit.
- `listunbonding` lists the wallet's unbonding outputs, how many Bitcoin blocks
  each still has to wait, and what can be claimed now.
- The wallet recognises a withdrawal that pays one of its staker keys' unbonding
  outputs, so a rescan or a restored backup finds the coins waiting there.
- GUI: the Staking tab says, before a withdrawal, how long the coins will wait;
  an Unbonding row shows what is waiting and when the next amount unlocks, with a
  Claim button; the transaction list shows the first step as "Unbonding" and the
  claim as "Unstake".
- Mainnet enforces the rule from its first block and the testnet from height
  159,000; custom chains take `-posunbondheight` and `-posunbonddepth`.

## Other changes since 24.7.13

### Upgrading

- **Burn transactions need upgraded nodes.** A node on 24.7.x refuses any
  transaction with more than one `OP_RETURN` output (`multi-op-return`). From
  25.0.0 a value burn, an `OP_RETURN` output with a null nonce, passes that
  limit, so a transaction with two burns, or a burn beside a data output,
  propagates only through upgraded nodes and is mined only by upgraded block
  producers. A burn with a nonce still counts as a data output: one beside
  another data output is refused by both versions.
- **Custom chains:** a node now refuses to start when
  `-con_coinbase_maturity` is set with `-con_coinbase_maturity_height=0`.
  Use 1 to apply the maturity from the first block.

### Mempool and block template

- Mempool admission already applied the chain's coinbase maturity. What
  changes is everything after admission. The pass that follows a reorg now
  applies the maturity in force at the new tip. At a height where the
  maturity rises, the mempool evicts the spends that become premature. And
  the block template leaves out any spend that is premature for the block it
  builds. Before, a spend admitted at exactly the maturity stayed in the
  mempool after a one-block rollback, or across a maturity boundary, and
  every block template then failed with
  `TestBlockValidity failed: bad-txns-premature-spend-of-coinbase`.
- `claimpoolrewards` honours the coinbase maturity and sweeps re-pots at
  once, and the GUI states the maturity.

### Wallet

- Issuance RPCs wait for the wallet to catch up with the chain, a blinded
  issuance no longer aborts the node, change stays explicit on a transparent
  wallet, and the change size is priced as it will be built.
- **Raw blinded issuance on a transparent wallet** needs a confidential
  output beside it: a confidential `asset_address` or `token_address` in
  `rawissueasset`, or a confidential `changeAddress` in
  `fundrawtransaction`. Without one, `blindrawtransaction` by default
  returns an issuance with no token unchanged, its amount explicit, and
  reports no error. One with a token fails, with an error that names the
  inputs' asset types rather than the missing output.
- The blinding dummy the wallet adds to a funded transaction (for a
  confidential `changeAddress` on a transparent wallet) now sits before
  the fee output, so `rawissueasset` accepts the funded transaction.

### Documentation

- A Simplicity developer page, and what `blindrawtransaction` does with a
  lone issuance.
