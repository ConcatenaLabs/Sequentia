# Sequentia Core 25.2.6

25.2.6 is 25.2.5 plus changes to how a node relays its own transactions and how
its wallet builds and reports payments. It changes no consensus rule and no
block production: every block 25.2.5 accepts, 25.2.6 accepts, and the reverse.
The fork at height **163,000** is the same.

## A payment offered again reaches peers that saw it before

A node remembers, per connection, which transactions the peer has seen, and
never announced those to it again, even after the peer had dropped them. A
payment that every node had dropped, and that its wallet then resubmitted,
stayed in its sender's mempool and reached no peer for as long as the
connections lasted. A node now offers its own transactions to every peer
whenever it broadcasts them again: a wallet's resubmission, `sendrawtransaction`,
and the retry, every 10 to 15 minutes, of a transaction no peer has fetched yet.

## Payments carry no lock time

The wallet no longer sets an anti-fee-sniping lock time on a payment; its
`nLockTime` is 0 unless the caller sets one. Fee sniping needs a block producer
able to reorg the chain to take a block's fees, and a certified block is final
against every rival: the only reorgs come from the Bitcoin anchor, which no
producer controls. A payment locked at the height it was made at became
non-final whenever an anchor reorg cost the chain height, and every node then
dropped it until the chain grew back.

## The wallet resubmits a payment as soon as the node takes it again

A payment the node refuses (`non-final`, for example) is handed back to the node
as soon as the refusal lifts, parents first, instead of at the next periodic
resend.

## `gettransaction` says when a payment has not reached the network

`gettransaction`, `listtransactions` and `listsinceblock` give an unconfirmed
transaction:

- `refused`: the node's reason, while it refuses to take the transaction into
  its mempool;
- `unbroadcast`: `true` while the node holds it and no peer has fetched it in
  the minute since the wallet first saw it.

## Upgrading

Stop the node, replace `sequentiad`, `sequentia-cli` and `sequentia-qt`, start
it. Check that `sequentiad -version` reports v25.2.6. Nodes on 25.2.5 and 25.2.6
agree on every block, and relay to each other as before.
