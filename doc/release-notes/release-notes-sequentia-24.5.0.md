# Sequentia Core 24.5.0

No consensus change. Nothing in this release alters what a node accepts, so it
needs no flag day and no coordinated cutover: run it when you like, beside
24.4.x nodes if you like.

What it adds is the wallet's other half. A Sequentia wallet has always held
Bitcoin — the addresses are Bitcoin-identical, so every receiving address is
also a Bitcoin testnet4 address — but the GUI could only ever count it. Now it
can spend it, and shows it where it belongs.

## Native Bitcoin, as one asset among the rest

`sendbtctoaddress` and `listbtctransactions` are new wallet RPCs. Sending scans
the parent chain for unspent outputs at this wallet's own addresses, builds a
plain Bitcoin transaction, signs it with the keys the wallet already holds, and
broadcasts it through the configured `-mainchainrpc` connection. There is no
peg, no federation and no bridge in the path: it is your Bitcoin, spent with
your key, on Bitcoin.

In the GUI, tBTC is not a separate tab or a separate mode. It is the top row of
the balances table and the default entry in the Send picker — the only
privilege native Bitcoin gets — and in the transaction history it is simply
another row, interleaved with everything else in date order. Sends quote the
concrete Bitcoin fee for approval before they go.

Spending BTC needs a node that can reach a Bitcoin node. Without one the
balance is still shown, and the Overview says why it cannot be spent rather
than reporting a plumbing error.

## The GUI generally

* An **empty wallet's Overview** no longer scatters its labels down the page
  with gaps between them. It was a layout fault, not a balance fault, and it
  had been there far longer than the balances table it appeared to be about.
* The **staking line names the wallet**, not just the node: with several
  wallets open, only the one holding the configured producer key is told the
  blocks are being produced with its key.
* The **transaction list is one chronology**. Bitcoin rows used to sort above
  every Sequentia row regardless of date.
* The date column shows the **time** again, and the leftover width goes to the
  address rather than to empty space.
* The **Assets and Staking pages fold**. Each card collapses to its title, with
  a one-line summary of its state beside it — "Stake registry — 14 staker(s)",
  "Block production — none of the last 100 blocks" — and opens when clicked.
  The card that answers the question you came with stays open; a staking-pool
  alert opens its own card and cannot be folded out of sight.
* Amounts in the fee dialog no longer call the smallest unit of a Sequentia
  asset a "sat". That name belongs to Bitcoin.

## One fix worth calling out on its own

A `-posproducerkey` persisted in `settings.json` was written to `debug.log` in
cleartext on every start — a staking private key, in the file people attach to
bug reports. Config-file and command-line args were already masked; settings
file entries now are too. **If you have ever run a producing node with the key
in `settings.json`, treat that key as exposed and rotate it**, and check any
debug log you have shared.
