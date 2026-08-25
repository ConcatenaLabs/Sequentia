# Sequentia Core 24.7.4

A testnet node now knows where the SeqDEX order-book relay is, the same way it
already knows where the parent chain, the asset registry and the price feed are.

There is no consensus change here.

## Reward conversion had nowhere to sell

Switching on automatic reward conversion and then being told
`No SeqDEX relay is configured, so nothing can be converted: set -seqoburl` is a
dead end that reads like a missing feature. Every other shared testnet service
is defaulted for `-chain=test` — the parent-chain anchor RPC, the asset registry,
the reference price feed — so that a fresh node or GUI works with no
`elements.conf` at all. The relay was simply never added to that list, because
nothing in the node needed it until reward conversion shipped.

It is now defaulted to `http://sequentiatestnet.com/seqob`.

Nothing is contacted because of this. The relay is read only once a wallet
actually has conversion switched on, which is off by default. What the default
removes is the dead end, not a choice: an explicit `-seqoburl` still wins, as it
does for every other one of these.

## Why that URL is plain http

The node links no TLS library and follows no redirects, so an `https://` relay is
unreachable to it and a `301` reads as a relay that is not there. The public site
redirects http to https for everything except the handful of routes the node
itself consumes — `/registry` and `/prices`, and now `/seqob`, which was added to
that list alongside this change.

This is the third time the same trap has cost time (see the comments around the
asset registry and price feed defaults), so it is worth stating plainly: a URL
the *node* fetches must be a route that answers over plain http without
redirecting. A browser reaching it over https proves nothing about whether the
node can.

## Verified

A node started with an empty data directory and no configuration at all reports:

    "relay": "http://sequentiatestnet.com/seqob",
    "book_readable": true,
    "markets": 36,
    "courier_reachable": true

— the book over http and the WebSocket courier both reachable, which between
them are everything a same-chain or cross-chain conversion needs.
