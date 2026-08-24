# Sequentia Core 24.7.0

This release finishes the Bitcoin half of automatic reward conversion. 24.6.0
shipped the feature and proved it on the live testnet by converting a reward on
the same chain; converting to native Bitcoin was built and unit-tested but had
never once run against a real parent chain. Running it revealed that it could
not have worked, and this release is what that discovery cost.

There is no consensus change here.

## The parent chain was never actually being read

Every read of the parent chain went for the payload of a JSON-RPC reply without
unwrapping the envelope around it. That finds nothing and reports nothing:

- the maker's Bitcoin lock could not be verified at all, so a cross-chain
  conversion would have stopped the first time it looked at the parent chain;
- `estimatesmartfee` never parsed either, so the fee rate quietly fell back to
  its 2 sat/vB default. A live run declined a conversion on the grounds that
  claiming would cost 350 satoshis, while the parent's own estimator wanted
  about a hundred times that. A guard that quotes a fee fifty times too low
  admits precisely the marginal conversions it exists to refuse.

All parent-chain reads now go through one place, and a reply carrying no result
is an error rather than an empty answer.

Where the fee estimate cannot be had at all — a freshly synced daemon, a chain
with no history — the node now assumes the **ceiling** rather than a default.
Assuming cheap is wrong in both directions at once: it admits swaps whose
proceeds cannot cover collecting them, and it underpays a claim that has to
confirm before the maker's timelock runs out. By the time a claim is being made
the asset is already gone, so declining to claim would forfeit the Bitcoin and
the asset both; overpaying a claim already judged worth making is much the
smaller loss.

## The refund could not refund most assets

A cross-chain swap's refund is the promise that a staker's asset comes back when
a maker walks away. It paid its fee in the policy asset while refunding a
different one, which leaves the transaction unbalanced in two assets at once:
short by the fee in the asset it holds, and inventing the fee in one it does
not. Such a transaction is rejected every time it is tried.

Staking rewards are mostly **not** the policy asset — that is what an open fee
market means — so this was broken for very nearly every swap it would ever have
been asked to rescue, and would only have been discovered at the worst possible
moment.

The refund now pays its fee in the asset it is refunding, when this node accepts
that asset for fees. When it does not, the fee comes from the wallet's own coins
and the asset comes back whole.

## Seeing a swap in flight

A staker mid-swap had an asset locked in a contract and no way to see it, and no
way to nudge one that was stuck.

- **`listrewardswaps`** shows the cross-chain conversions this wallet has in
  flight: what is locked, what is expected back, and how many blocks remain
  before an unfinished swap refunds itself. It does not print the keys that
  redeem either leg, which are in the same record.
- **`resumerewardswaps`** carries every unfinished swap as far as it can go now,
  rather than at the wallet's own two-minute pace. It is safe at any time and
  does nothing when there is nothing to carry.

## Both endings are now proved end to end

`feature_pos_reward_xchain.py` drives the whole cross-chain path against a real
parent: node0 runs in **Bitcoin mode**, so the claim is serialized and signed as
a Bitcoin transaction and the parent either accepts it or does not. Only the
negotiation is stubbed. The test covers the maker taking the asset and the
wallet claiming the Bitcoin from the revealed secret, and both refund paths —
the asset paying its own fee, and the fee coming from the wallet.

## Operational note

Converting to native Bitcoin requires the parent daemon to run with
**`-txindex`**: the wallet looks the maker's lock up by txid alone, which a
daemon that did not index it cannot answer. This is the same requirement pegins
already place on the parent. A parent that cannot answer now says so in the log
instead of failing silently.
