# Sequentia Core 24.5.1

No consensus change. Fixes, one of which can strand a transaction, so this is
worth taking.

## A transaction built without fee estimates could be unrelayable

`DEFAULT_FALLBACK_FEE` has been 2000 atoms/kvB since the release that introduced
it, and it had never taken effect: a leftover Elements branch overwrote it with
`CFeeRate(100)` on every wallet load. So a wallet with no fee-estimation data —
a new node, a quiet chain — paid **exactly** `minrelaytxfee`.

Exactly the floor is not a margin, and on this chain it is worse than thin. A
fee is committed in the asset it is paid in, but relayed and mined against that
asset's value in reference units. The moment the asset is worth less than 1.0,
a transaction that paid the floor when it was built is *below* the floor, and
peers stop relaying it.

This was found the hard way. A stake deposit built on a node with no live
exchange rates paid 100 rfa/kvB at the seed rate of tSEQ = 1.0; when real rates
arrived and put tSEQ at $0.39, the same fee re-valued to 38 rfa/kvB. It went
unrelayed for 38 blocks and confirmed only because that wallet's own node
eventually produced a block and mined it. A CPFP child does not rescue this —
without package relay a peer must accept the parent on its own.

If you have a stuck transaction from an earlier version, bump it (`bumpfee`,
whose `fee_rate` is in atoms per vByte) rather than abandoning it.

## The price server could be pointed at nothing, and say nothing

The node GUI's **Fee acceptance → Launch price server** seeded the sidecar's
config by copying the shipped example verbatim, placeholder RPC endpoint
included. The sidecar then starts, fetches prices, computes rates correctly, and
publishes every one of them to a port with no node behind it — indefinitely,
because nothing about that looks like a failure.

* The GUI now writes the running node's own RPC endpoint and credentials into
  the config it creates, and warns before launching if an existing config points
  where this node is not listening.
* Reaching **no** node is now logged as an error naming the endpoint and its
  error. "published 6 rate(s) to 0/1 node(s)" reads like success and is not.
* Cookie authentication is re-read per call, so a node restart rotating the
  cookie no longer silently breaks every push from then on.
* `feed_aliases` gains `USDC.e` and `EURC.e`. Bridged stablecoins carry a `.e`
  suffix in the asset registry and none in the price feed, so without the alias
  they were never priced and never admitted — leaving a node's fee whitelist
  quietly short of assets with nothing to indicate it.

**If you run the price server, check it is actually reaching your node:** its
log should say `published N rate(s) to 1/1 node(s)`, and `getfeeexchangerates`
on the node should list more than the policy asset.

## The Fee acceptance window called tSEQ "bitcoin"

The fee RPCs key their maps by the node's internal asset names, and the policy
asset's internal name is inherited from Elements. The window printed that key
raw. It now shows the same label as the rest of the GUI, with the RPC key on
hover.
