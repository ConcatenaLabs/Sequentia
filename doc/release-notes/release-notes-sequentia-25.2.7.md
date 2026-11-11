# Sequentia Core 25.2.7

25.2.7 is 25.2.6 plus changes to how the wallet pays a confidential address, a
cheaper check in the block producer, and fixes to the RPC, the command line and
the GUI. It changes no consensus rule: every block 25.2.6 accepts, 25.2.7
accepts, and the reverse, and the two relay each other's transactions. The fork
at height **163,000** is the same.

One change is in block production, described first. It changes how much work a
producer does before each attempt, not which blocks it makes. A committee still
upgrades all at once.

## The producer's certified-sibling check no longer walks the block index

Before each attempt a producer checks whether a certified sibling of the block
it would build is still to be restored after a parent-chain reorganization. The
check looked through the whole block index, under the node's main lock, whenever
the anchor watcher was tracking a block it had invalidated. Such an entry can
outlive its use: when Bitcoin reorganizes back after this chain has moved on to
another branch, the restored block's branch never reconnects, and the entry
stays until the node restarts. A producer in that state walked the whole index
on every poll and found nothing.

The check now decides from the tracked blocks alone whether such a sibling can
exist, and walks the index only when one can. Its answer is the same in every
case.

`getanchorstatus` adds `recoveryset`: the blocks the anchor watcher invalidated
because their parent-chain anchor was orphaned, and still tracks so that it can
restore them if the parent chain reorganizes back.

## A confidential payment is never sent in the clear unless the caller says so

Choosing a confidential address is the holder's choice for that transfer. A
blinded output needs another blinded output beside it when no input is blinded,
and where there was none the wallet used to send the output explicit, with at
most a warning. Now:

- `sendtoaddress`, `sendmany` and `send` blind such a payment. Change blinded
  beside the recipient stays rather than going to the fee, and a payment with no
  change gets a zero-value blinded OP_RETURN. A coin too small to pay for the
  blinded transaction is refused with "Insufficient funds".
- `fundrawtransaction` keeps such an output blindable in the same way, so
  `blindrawtransaction` can blind it. Only with `ignoreblindfail=true` passed is
  the output left explicit, with a warning, as before.
- `blindrawtransaction` fails when an output, or an issuance asked to be blinded
  (`blind_issuances`, true by default), cannot be blinded, and says how to fix
  it. Only with `ignoreblindfail=true` passed does it return the output, or the
  amount of an issuance without reissuance tokens, explicit. An issuance with
  reissuance tokens marked blind cannot be returned explicit at all. To keep
  issuances explicit, pass `blind_issuances=false`.
- `send` no longer fails with "Transaction is not yet fully blinded" for every
  confidential recipient paid from explicit coins.

`issueasset` with `blind=false`, and PSBTs from `walletcreatefundedpsbt`, which
the PSET blinder blinds later, work as before.

## Coin selection on a wallet that blinds by default

A wallet started with `-blindedaddresses=1` priced a lone change output as
blinded although it builds it explicit, and refused explicit payments with
"Insufficient funds" whose leftover a transparent wallet funds. It now prices
change as it builds it.

## RPC

- `getblockheader` adds `poscertifiedknown`: whether the node knows the answer
  `poscertified` gives. `poscertified` stays `false` while the answer is not
  known.
- `sequentia-cli` passes `getbtcfeerate <conf_target>` and
  `listrewardswaps <include_finished>` as numbers and booleans.

## GUI

The fee asset on a page with no preference of its own, such as the Supervision
page, defaults to the most valuable accepted holding rather than to the
Sequence token.
