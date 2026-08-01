# Sequentia Core 23.3.8

This release makes a Sequentia node survive its Bitcoin node going away, and
makes the chain syncable from genesis again. Upgrading is recommended for every
node; it is required for any node that needs to be able to resync or reindex.

## The short version

Two independent defects made a node's dependency on Bitcoin far more brittle
than intended:

- a node that could not reach its Bitcoin daemon treated that as proof the
  blocks it was validating were **invalid**, and wrote that verdict to disk
  permanently;
- a consensus rule added in July was applied retroactively to blocks produced
  before it existed, so **no new node could sync the testnet from genesis**.

Both are fixed. A third group of changes makes the case "Bitcoin Core is not
running" an explicit, readable choice rather than something the software decided
silently on the user's behalf.

## Fixed

- **An unreachable Bitcoin node no longer marks blocks invalid forever.**
  `pos-escape-stall-unverifiable` is a soft, retriable verdict — "I cannot check
  this right now", not "this block is wrong" — but the permanent-failure paths
  only excluded `BLOCK_MUTATED`, so it was written into the block index as a
  permanent failure and never retried. A node hit by this sat ~2300 blocks
  behind for hours while reporting itself fully synced (`blocks == headers`,
  `verificationprogress: 1`), kept proposing on a dead branch, and could only be
  recovered by an operator running `reconsiderblock`. Such a verdict now defers
  through the existing `fStall` path, the same mechanism already used when the
  parent chain is not ready for pegin checks, and is retried automatically.

- **The chain can be synced from genesis again.** The escaping-stall
  parent-chain time-gap rule, introduced after the 2026-07-17 finality
  partition, was being applied to blocks produced before it existed; every fresh
  sync stopped permanently at testnet block 1757 (dated 2026-07-06). The rule is
  now gated on an activation height, the standard soft-fork treatment.

  Running nodes were unaffected only because a block already in the chainstate
  is never revalidated — which also meant their state was not reproducible: a
  `-reindex`, a restore from backup or a disk failure would have left them
  unable to start. That is no longer the case.

- **`-poscheckpointdepth` is covered by the testnet consensus-flag guard.**
  Unlike the other PoS parameters it is not pinned on testnet — it is read at
  runtime and feeds the `bad-fork-prior-to-pos-checkpoint` rejection — so a
  conflicting value silently forked the node.

## Changed

- **Starting without a reachable Bitcoin node is now the user's decision.**
  Previously the outcome depended on `-server`: with it the node refused to
  start, without it (the typical GUI setup) it started anyway with anchor
  validation silently forced off for the whole session, even if Bitcoin came up
  moments later. Nobody chose either outcome. The node now asks: `elements-qt`
  offers two named choices, and `elementsd` and other non-interactive frontends
  still refuse to start, which is the right default when nobody is watching.

- **A node not following Bitcoin says so, and says what it costs.** A warning
  stays visible for the session, and the node counts and reports the one
  concrete thing it is taking on trust: sub-quorum (escaping-stall) blocks
  accepted without the parent-chain evidence that the committee really had
  stalled. Available through `getanchorstatus` and in the GUI.

- **Conflicting consensus flags are refused on the public testnet.** `-posbls`,
  `-poscommitteesize`, `-pospubliccommittee` and `-poscheckpointdepth` are
  network-wide rules: a node running different values computes a different
  quorum, rejects the network's blocks and forks in silence. A value matching
  the network is still accepted, so existing configurations keep working.
  Regtest and custom chains stay freely configurable.

- **Activation-height gates now share one convention:** `0` means a rule is not
  gated, a positive height means it is enforced from there. A chain launched
  with a rule already in place sets `1`, never `0`.

## GUI

- The "Bitcoin Core unreachable" dialog no longer elides its own buttons: the
  two named options were being cut to `rt without following Bitc` and
  `se and start Bitcoin Core fi`, leaving the user unable to read the choice
  they were being asked to make.
- Startup no longer shows `Awaiting mainchain RPC warmup (anchoring)`, which
  named an internal mechanism and was the only feedback for the minutes the
  check can take.
- Not reaching Bitcoin is no longer reported as a fault with troubleshooting
  advice attached — it is frequently a deliberate state.
- Only the anchor verdict is coloured as a warning; the block heights beside it
  are facts the node knows for certain and are no longer painted red.

## Documentation

- `-validateanchor=0` is documented as the master switch it is: it disables four
  mechanisms, and the consequential one is not validating other people's blocks
  but the node's ability to notice its own chain went bad.
- `CONTRIBUTING.md` now requires an activation height for every new consensus
  rule, recorded as a rule rather than as two one-off fixes.

## Recommended configuration

- **`mainchainrpctimeout=10`.** The parent-chain RPC is issued from
  `ConnectBlock` while holding `cs_main`, and the default is 900 seconds: a
  Bitcoin daemon that accepts TCP but does not answer can freeze the whole node
  for up to fifteen minutes. This is long-standing behaviour, not new in this
  release, but the fixes above make it easier to encounter.

## Known issues

- Building with `--with-gui` fails when linking `test_elements-qt`: `libblst`
  does not reach the link line. The daemon and the GUI both build; only the Qt
  unit-test binary is affected. Present on earlier versions too.
- Three `util_tests` cases (`util_GetChainName`, `util_ArgsMerge`,
  `util_ChainMerge`) fail. They are inherited from upstream and were never
  realigned after the Elements/Sequentia divergence; they fail identically on
  earlier versions.
- On testnet the block anchor can lag the Bitcoin tip by a couple of hours,
  because `-anchoravoidcontested` backs it away from heights a competing branch
  is contesting and Bitcoin testnet4 has competing tips almost permanently. The
  policy behaves as designed; testnet4 is what makes it degenerate.

## Verification

A node built from this release, syncing from genesis with anchor validation
enabled, reaches the network tip. The same test with the previous binary stops
permanently at block 1757.
