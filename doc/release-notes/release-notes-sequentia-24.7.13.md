# Sequentia Core 24.7.13

Block producers keep the Bitcoin anchor moving through a long-lived fork of
the parent chain, the fee whitelist has a single writer again, and the wallet
keeps its own record of its bitcoin instead of rescanning for it. No consensus
change: a node on 24.7.13 and a node on 24.7.12 agree about every block.

A committee should still move to it in one step. While the parent chain is
forked, producers on the two versions choose different anchors for the block
they propose, and a committee split between them certifies neither quickly.

## Anchoring under a parent fork

### What was wrong

When Bitcoin has rival branches, a producer anchors to the last block those
branches share, so that a Sequentia block never stands on ground that may
disappear. That is right while the latest Sequentia block is itself anchored
below the fork. Once the latest block is anchored above the fork point, the
chain is already committed to one branch: if that branch loses, Sequentia
reorganizes either way. Holding the anchor back then protects nothing and only
freezes it.

A frozen anchor also switches off the escaping-stall rule, which lets the chain
progress below quorum only when the anchor advances. So a long-lived parent
fork during a committee outage stopped the chain entirely.

This was reached on the testnet on 2026-10-01. Most of the committee was
offline from about 14:50 to 22:10 UTC. An escaping-stall block at 15:57
anchored at testnet4 height 154698; a rival testnet4 branch then appeared,
forking at 154697, and kept pace with the tip for six hours. Every producer
backed its target down to 154697, below the anchor already in the chain, so
the anchor held and no block could be made until a parent reorg pushed the
rival out of the contest window at 21:54.

### What changed

- A producer whose parent block is anchored above a rival's fork point ignores
  that rival and keeps following the branch the chain is committed to, for as
  long as that branch is Bitcoin's best chain.
- The commitment counts only while the parent daemon still reports the parent
  block's anchor at its height. If that anchor has been reorganized away, every
  rival counts as before and the reorg-following watcher takes over.
- Rivals that fork at or above the parent block's anchor are still backed away
  from, and finality reconciliation is unchanged.
- The log says which of two things happened when a producer reuses the parent
  block's anchor. It used to print `could not query mainchain daemon for a new
  anchor` for both an unreachable daemon and an anchor held on purpose, which
  sent operators looking for a broken Bitcoin connection that was fine. A held
  anchor now names the target and the anchor being kept.

`doc/sequentia/03-bitcoin-anchoring.md` and `05-operating-sequentia.md`
describe the rule.

## The fee whitelist has one writer

The fee whitelist is the set of assets a node accepts fees in, with the rate
for each. It has one source, the operator: rates set by hand with
`setfeeexchangerates`, or a price server that keeps them current under its own
feeds and admission thresholds.

- **The reference price feed no longer writes it.** A node started with
  `-referencepricesurl`, which is the default on the test chain, copied the
  prices it fetches for display into the whitelist for every asset the operator
  had not set. An asset the operator left out was accepted anyway, a cleared
  whitelist refilled at the next poll, and a price server's refusals were
  priced straight back in. The feed is display only again. On a node with no
  price server the wallet warns when a fee asset is not in the whitelist, and
  that warning is accurate: the node does refuse it.
- **On the mainnet chain an unconfigured node starts with an empty whitelist.**
  It accepts no fee asset, relays no transaction, and its wallet refuses to
  send, until the operator writes the whitelist or sets up a price server. It
  still syncs, validates and receives. The daemon says so at startup, a wallet
  send names the missing whitelist, and the desktop wallet prompts for it on
  first run. The testnet and custom chains keep the policy asset listed at 1:1,
  so nothing changes for nodes running today; `-con_seed_fee_whitelist=0`
  reproduces the mainnet behaviour on a custom chain.

## Wallet

- **The wallet keeps a record of its bitcoin.** Every balance read used to
  rebuild the answer with a scan of the whole parent UTXO set: 18.8 seconds a
  read on testnet4, once a minute per open wallet, and twice more for a send.
  The record sits beside the wallet as `parent_coins.json`, is filled once by a
  full scan and then advanced block by block, and a read takes 55 to 80 ms. A
  send marks the coins it committed and banks its own change, so a second send
  no longer collides with the first. When the parent chain is unreachable the
  record answers and says the figures are stale, rather than reporting zero.
  Unconfirmed incoming bitcoin is still not shown.
- **Bitcoin sends have fee controls**, backed by a new `getbtcfeerate` RPC, and
  can pay several recipients in one transaction.
- **Replacing and bumping a transaction is one window.** It opens with the
  original payment, and the fee is a table of rate and total in the paying
  asset and in the reference currency, any of which can be typed into. It
  states the minimum a replacement must pay, which
  `getmempoolcongestion` now reports as `incrementalrelayfee`, and the entry
  price as well when blocks are full. A transaction that dropped out of this
  node's mempool can be replaced.
- **Settlement is reported as the chain can prove it.** Where the committee
  certifies blocks, a transaction in a certified block is shown as settled
  instead of waiting for a count of confirmations.
- **A transaction the node refuses says so, and frees its funds.** A spend that
  consensus rejects, such as a supervised asset sent to a frozen script, was
  shown as an ordinary unconfirmed payment with its inputs counted as spent.
  It is now marked as rejected with the reason, and its inputs are spendable
  again. Policy and fee refusals keep the inputs locked.

## Deployment

No activation height and no consensus rule changes. Nodes on 24.7.12 and
24.7.13 accept each other's blocks. Switch a committee over in one step, the
way every committee upgrade is done.

An operator who relied on the reference feed to fill the fee whitelist needs
to set rates with `setfeeexchangerates` or run the price server after
upgrading, or the node accepts only the assets already written to its
whitelist.
