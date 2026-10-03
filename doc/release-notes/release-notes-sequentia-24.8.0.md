# Sequentia Core 24.8.0

Finality now survives committee members who sign two blocks at one height,
and a stake that leaves now stays locked until a Bitcoin checkpoint could have
secured what its keys signed. The second is a consensus rule, which is why the
minor version moves; it is off on the testnet until a cutover height is agreed,
so a node on 24.8.0 and a node on 24.7.13 agree about every testnet block.

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
- Mainnet enforces the rule from its first block. The testnet does not enforce
  it until a cutover height is set in `chainparams.cpp`; custom chains take
  `-posunbondheight` and `-posunbonddepth`.

## Other changes since 24.7.13

- mempool: the chain's coinbase maturity applies to mempool admission, and
  `claimpoolrewards` honours it; re-pots are swept at once and the GUI states
  the maturity.
- policy: value burns pass the one-`OP_RETURN` limit, and only a null-nonce burn
  is exempt from the data limit.
- wallet: issuance RPCs wait for the wallet to sync, a blinded issuance no
  longer aborts the node, raw issuance works with confidential change, change
  stays explicit on a transparent wallet, and the change size is priced as it
  will be built.
- doc: a Simplicity developer page, and what `blindrawtransaction` does with a
  lone issuance.
