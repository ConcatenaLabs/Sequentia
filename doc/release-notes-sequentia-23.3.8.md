# Sequentia Core 23.3.8

This release makes a Sequentia node survive its Bitcoin node going away, makes
the chain syncable from genesis again, and makes that sync take **nine minutes
instead of never**. Upgrading is recommended for every node; it is required for
any node that needs to be able to resync or reindex.

## The short version

Two independent defects made a node's dependency on Bitcoin far more brittle
than intended:

- a node that could not reach its Bitcoin daemon treated that as proof the
  blocks it was validating were **invalid**, and wrote that verdict to disk
  permanently;
- a consensus rule added in July was applied retroactively to blocks produced
  before it existed, so **no new node could sync the testnet from genesis**.

A third defect made the sync that did happen unusably slow: a development
self-check meant for regtest was left on for the public testnet, and it costs
more the longer the chain gets. Syncing from genesis went from four and a half
hours reaching height 1,445 — and still slowing — to the whole chain in nine
minutes.

A fourth made every node re-ask Bitcoin about thousands of blocks of settled
history every ten minutes, forever, and by default all at the same server.

All four are fixed. A further group of changes makes the case "Bitcoin Core is
not running" an explicit, readable choice rather than something the software
decided silently on the user's behalf.

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

- **Syncing is no longer quadratic.** `CTestNetParams` set
  `fDefaultConsistencyChecks = true`, the default for `-checkblockindex`, which
  runs `CheckBlockIndex()` — a full consistency walk of the **entire** block
  index — after every headers message and every tip update. Upstream sets that
  true only for regtest, where chains are a few dozen blocks long and the walk
  is free. On a real chain it costs O(blocks known so far) per headers message,
  so the sync is O(n²) and gets worse every day the chain lives.

  It is invisible from outside: the node is not stalled, logs no error, and sits
  at 100% CPU in the message-handler thread doing nothing a user would recognise
  as work. Measured on testnet at ~67k blocks, same machine and peer, the flag
  the only variable — with it on, 810 blocks and 16,384 headers in seven minutes
  and still slowing (171 headers/s at height 2,560 down to 3/s at height
  20,480); with it off, the whole header chain inside the first twenty seconds
  and **67,457 blocks in 541 seconds**, flat at ~125 blocks/s start to finish.
  The flat rate is the point: the per-batch O(n) term is gone, not merely
  smaller.

  Nothing about consensus or block validity depended on it. Every check on
  incoming blocks is unchanged; what stops is a development self-audit of the
  node's own index. The flag also governs the `-checkmempool` default, an
  equivalent per-transaction self-check, now likewise off on testnet. Developers
  who want either back pass `-checkblockindex=1` / `-checkmempool=1`; regtest
  keeps both on, which is where they earn their keep.

- **The anchor watcher no longer re-verifies Bitcoin history that cannot have
  changed.** It emptied both anchor-verdict caches on *every* parent-chain tip
  change, a plain extension included, and its walk — which deliberately has no
  depth floor — then re-derived every verdict from scratch. Since ~20 Sequentia
  blocks share one anchor, the number of distinct anchors grows with the number
  of Bitcoin blocks the chain spans: ~3,000 RPC calls per Bitcoin block on the
  current testnet, per node, growing every day, each a fresh TCP connection, and
  by default all aimed at the one public gateway. Yet appending to Bitcoin
  cannot alter its best chain below the old tip.

  A single header call now settles whether the move was an extension or a
  reorganization, and only the verdicts above a reorganization's fork point are
  dropped. Steady-state cost per Bitcoin block: **one call instead of three
  thousand**, and it no longer grows with the age of the chain. This drops
  re-verification of what cannot have changed; the walk still descends to
  height 1 on every tick and no depth floor is introduced.

  `getmainchainrpcstats` reports the calls a node has made to its Bitcoin
  daemon, in total and per method, and makes no call of its own so sampling does
  not perturb what it measures.

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

## Building

Nothing about how this release is built has changed. Build it exactly the way
you built the previous one — the notes below are the usual procedure for this
codebase, written down because they are easy to get wrong, not because this
release introduces a new requirement.

- **Build through the `depends` system**, as release builds always have.
  `depends` compiles Berkeley DB 4.8 from source, which is what lets a build
  open legacy wallets. A plain `./configure --without-bdb` produces a binary
  that cannot, and a node with a legacy wallet then stops at
  "Verifying wallet(s)". That has been true of every version, not just this one.
  The system BDB 5.3 is not a substitute: it can alter the format of existing
  wallets.
- **Re-run `configure` after checking out this tag**, or `make` will not pick up
  the version change and the binary will keep reporting the previous version.

## Recommended configuration

- **`mainchainrpctimeout=10`.** The parent-chain RPC is issued from
  `ConnectBlock` while holding `cs_main`, and the default is 900 seconds: a
  Bitcoin daemon that accepts TCP but does not answer can freeze the whole node
  for up to fifteen minutes. This is long-standing behaviour, not new in this
  release, but the fixes above make it easier to encounter.

## Build and continuous integration

Neither of these affects `elementsd`, `elements-qt` or `elements-cli`, which
have always built and run. They fix the ability to notice breakage.

- **`bench_bitcoin` and `test_elements-qt` link `libblst` again.** Both link
  `libbitcoin_node`, which calls into blst since the BLS committee work, but
  neither listed `$(LIBBLST)`, so both failed with
  `undefined reference to blst_p1_compress`. `bench_bitcoin` is in the CI
  matrix, so `build_and_test` was red on *every* pull request regardless of its
  contents — a one-line change failed it exactly like a seven-hundred-line one.
  A permanently red check reports nothing.

- **Three `util_tests` cases are green again.** `util_GetChainName`,
  `util_ArgsMerge` and `util_ChainMerge` had been red since the fork. The cause
  was one, not three: `CBaseChainParams::DEFAULT` is `test` here where Elements
  has `liquidv1`, and that constant also decides which network inherits the
  config file's unprefixed section. Rebuilding with the upstream default
  reproduces the upstream hashes exactly, which established there was no parsing
  regression hiding underneath before the expected values were touched.

## Known issues

- On testnet the block anchor can lag the Bitcoin tip by a couple of hours,
  because `-anchoravoidcontested` backs it away from heights a competing branch
  is contesting and Bitcoin testnet4 has competing tips almost permanently. The
  policy behaves as designed; testnet4 is what makes it degenerate.

## Verification

A node built from this release, syncing from genesis with anchor validation
enabled, reaches the network tip. The same test with the previous binary stops
permanently at block 1757.

A fresh node built from this release, with no `-checkblockindex` in its
configuration so the new default is what is being measured, syncs the whole
testnet — 67,457 blocks — in **541 seconds**, wall clock and monotonic clock
agreeing to one second. The block rate is flat from start to finish.

`feature_anchor_rpc_cost.py` measures the watcher's cost against the Bitcoin
daemon at two chain sizes and asserts it does not grow with the number of
distinct anchors. It was verified to fail on the previous behaviour.
