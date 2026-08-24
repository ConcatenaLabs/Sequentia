# Sequentia Core 24.6.0

No consensus change; runs beside 24.5.x and needs no flag day or committee
cutover. A minor rather than a patch because it adds RPCs and a capability the
node did not have: a staker's wallet can now sell what staking paid it, on its
own, including for native Bitcoin.

## Staking rewards are now something a wallet can see

Sequentia has no block subsidy. A staker earns the transaction fees of the
blocks it produces, and under the open fee market those arrive in whichever
assets the payers happened to choose — one coinbase output per asset. A pool
delegator is paid the same way, through its pool. Until now nothing in any
wallet could say which coins those were: rewards were simply balances, mixed in
with everything else.

`liststakingrewards` answers it, per asset and per output, classified by which of
the four ways of being paid produced it:

* `solo` — this wallet's own key was the elected leader and the coinbase paid it;
* `lottery` — a pool's per-block draw landed on this wallet's stake;
* `direct` — a pool paid the address it committed to under a direct policy;
* `split` — a share of a pool's pot, distributed by `claimpoolrewards`.

Maturity is reported rather than hidden: coinbase value is spendable only 100
blocks deep, and a staker wants to see income that is coming. `totals` separates
what is spendable now from what is still waiting.

The rule is decidable from wallet data alone — no chainstate lookup, no txindex —
because the light wallets have to reach the same verdict from the same facts. It
is implemented twice, here and in SWK, and both are pinned to the same cases so
a node wallet and a light wallet watching one staker's keys cannot drift apart
about that staker's coins.

## And something a wallet can act on

`setrewardautoconvert` is a standing instruction: sell each reward, as it
matures, for **one asset you pick**. Native Bitcoin is the default and the first
thing offered — it is what most stakers converting an unwanted tail of assets are
converting *to* — but it is not the only choice. Outside staking no asset is
privileged, and a staker who wants USDX, or more SEQ to stake with, is doing the
same thing for the same reason.

Off by default, always. Converting rewards is irreversible and a staker may have
chosen those assets deliberately, so nothing is sold because a version changed.
Once on, the wallet sells without asking again — that is the point of it — and
stops at the next check when it is turned off. `min_receive` is the floor a
batch must clear before anything is sold; below it a batch simply waits,
indefinitely if that is how long it takes. `max_slippage_bp` is how far from the
reference price a fill may land.

`convertrewards` runs a pass on demand, and dry-runs by default:
`getrewardautoconvert` reports what would happen and, for anything that would
not, why. "No market for this pair" and "not yet worth converting" are the
ordinary answers on a quiet chain, and both mean the rewards wait.

The Qt wallet carries all of this on the Staking tab.

## How a node trades at all

The node has no interactive DEX client, and for the same-chain case it does not
need one. A covenant offer is a Taproot output whose FILL leaf spends nothing and
signs nothing: it only *inspects* the transaction spending it, and passes when
that transaction pays the maker at the committed rate and returns any remainder
to an identical output. So the node builds one transaction and broadcasts it,
with the maker offline.

The relay is never trusted. Before anything is spent the node rebuilds the
maker's committed leaf byte for byte and looks the covenant up in its own UTXO
set: still unspent, explicit, the right asset, and hashing to the same Taproot
output. Having the chainstate is the one advantage a node has over a light wallet
here, and this is where it pays.

Selling for native Bitcoin *is* a conversation, so the node grew the parts to
have one: a minimal RFC 6455 WebSocket client, AES-256-GCM to open the courier's
sealed payloads, and the HTLC both legs are built on. The GCM implementation is
built on the tree's existing AES-256 — the only new arithmetic is GHASH — and is
pinned against the published NIST vectors. Nothing in consensus, in the P2P
protocol or in wallet storage uses it, and nothing should start.

**The order is the safety.** Nothing of the staker's moves until the maker's
Bitcoin is locked, confirmed and verified against the parent chain by this node.
The asset is funded only once this node's own Bitcoin anchor has reached the
height that lock confirmed at — the block that confirms the funding commits to an
anchor, and that number freezes on confirmation, so waiting afterwards can never
repair it. The lock is checked once more immediately before spending, because one
parent-chain reorg is all it takes for a lock that was confirmed at the first
check to be gone by the second. After funding, the asset is recoverable by
timelock whatever the maker does, and the Bitcoin is claimable the moment the
secret appears; both are driven by a resume pass on the scheduler, so a node that
restarts mid-swap finishes it rather than stranding it.

## Reaching a relay

`-seqoburl` points the wallet at a SeqDEX order-book relay, as a plain `http://`
URL. This node links no TLS library — Core dropped OpenSSL and neither depends
nor the release pipeline replaced it — and adding one for a wallet convenience
would be a dependency and reproducible-build change out of all proportion. What
crosses that wire is a signed public order book and end-to-end-sealed courier
payloads: nothing secret, and nothing trusted with funds. Operators who want the
traffic encrypted can run a relay locally or terminate TLS in front of the node.

Unset by default, and never contacted unless a wallet has conversion switched on.
`getseqdexstatus` reports the book and the courier separately, because a relay
that answers one and not the other converts rewards into Sequentia assets but
never into Bitcoin — worth being able to see rather than infer from nothing
happening.

## Notes

* `doc/sequentia/reward-autoconvert-design.md` is the specification, and covers
  the web wallet, the browser extension and Ambra as well as this one.
* New unit tests: the NIST GCM vectors, the HTLC script byte for byte, the
  covenant leaf rebuilt from a real resting offer against the output it is funded
  by on chain, order-book parsing against a real captured response, the courier's
  key agreement, and the conversion policy. New functional test:
  `feature_pos_rewards.py`.
