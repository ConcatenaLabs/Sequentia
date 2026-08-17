# Sequentia Core 24.2.0

This release carries a consensus change that has been live on the testnet since
2026-08-16 and, until now, shipped under 24.1.0's version string. That omission is
the reason for this release: **a peer running an earlier 24.1.0 build forked off the
testnet at height 96,277 and nothing in `getpeerinfo` could distinguish it from an
upgraded node.** If you run any node against the live testnet, upgrade.

## The consensus change: a supervised asset may be issued with zero supply

Supervised assets (24.1.0) originally required an issuance to create a positive
amount. That refused a legitimate and important shape: issuing the asset first with
zero units and minting into it later through its reissuance token, which is exactly
how a bridged asset is brought up so that its supply can never exceed its proven
reserves at any moment, including the first.

The rule now accepts a supervised issuance whose asset amount is zero, provided it
still creates the reissuance token that consensus requires of every supervised
asset. Nothing else changes.

**This relaxes validation.** Every block that was valid before this change is still
valid, so there is no activation height and the rule is active from genesis on every
chain, including regtest and a future mainnet. That is the correct shape for a
relaxation and it is why no gate was added.

**It is still a hard fork for the live network.** Transactions that every earlier
node rejects are now valid, so an earlier node rejects any block containing one. The
live testnet has contained such blocks since height 96,277 (2026-08-16 21:56 UTC,
the supervised issuances of `USDC.e` and `EURC.e`), which means:

- an earlier node cannot follow the testnet past 96,276 and never will;
- upgrading fixes it with no operator surgery: the node accepts 96,277 onward and
  resyncs from its peers.

## Why the version number moved, and what we changed about the process

The relaxation merged without a version bump, and the first transaction exercising
it was produced seven hours later. Both binaries then reported
`/Sequentia Core:24.1.0/`, so the fork was invisible in peer information and stood
for roughly a day before anyone read `synced_headers`.

Two changes stop the next one:

- `CONTRIBUTING.md` now requires a version bump in the same pull request as any
  change to consensus validation, relaxations included, and requires that no
  transaction exercise a new rule on a live chain until every node reports the
  bumped version.
- `contrib/sequentia/peer-stall-check.sh` reports the behavioural signature a
  version string cannot: a peer whose `synced_headers` is frozen while the bytes we
  send it keep climbing, because we are repeatedly offering a chain it refuses. It
  exits non-zero when any peer is stalled, so it belongs on a timer as well as in
  the cutover checklist.

## Upgrading

Stop the node, replace the binary, start it again. Committee operators upgrade every
node in one pass, as with any consensus release: mixed binaries select different
anchors and fragment share-locks even when they agree about asset ids.

After the cutover, confirm it behaviourally rather than by version alone:

    contrib/sequentia/peer-stall-check.sh

Every peer should report as following the chain. A peer frozen just below a
consensus-change height is running a binary without that rule, whatever version it
reports.
