# Implementation roadmap

Ordering favours low-risk, high-value first, and front-loads the work that other
work depends on.

## Milestone 0 — Baseline (this session)
- [x] Study Elements + the existing Sequentia fork; locate every relevant seam.
- [x] Choose the base: **fork the existing Sequentia project** (doc 00 §3).
- [x] Import the fork's full history into this repo; configure `elements-upstream`
      / `sequentia-upstream` remotes for downstream merges.
- [x] Write this design specification (`doc/sequentia/`).

## Milestone 1 — Build & green baseline
- [x] Build the imported tree (autotools, no-BDB/sqlite build); produces
      `elementsd` / `elements-cli`.
- [x] Run a representative functional subset; record the baseline. Note: the
      pre-existing `feature_any_asset_fee*.py` tests require a BDB (legacy
      wallet) build because they fund wallets via
      `-initialfreecoins`+`-anyonecanspendaremine`, which descriptor wallets do
      not honour; they fail in sqlite-only builds for that environmental
      reason (verified unrelated to the new work — `rpc_exchangerates.py` and
      the new tests pass).
- [ ] **Merge Elements `23.3.3` downstream** (`git merge elements-23.3.3`),
      resolve conflicts, re-green. (Same `23.x` series ⇒ patch-level catch-up.)

## Milestone 2 — Finish challenge 1 (open fee market)
1. [ ] Audit/normalise all fee floors to rfa for any-asset txs (doc 02 §A).
2. [x] Layer the `ExchangeRateMap` into static + dynamic with precedence,
       per-entry `source` + `updated_at`, and staleness drop
       (`-dynfeeratemaxage` + scheduler purge) (doc 02 §B.3).
3. [x] New RPCs: `setdynamicfeerates` / `getdynamicfeerates` /
       `getfeeacceptancepolicy` / `cleardynamicfeerates`.
4. [x] Price-server sidecar in `contrib/price-server/` (coingecko + generic
       JSON-API sources, threshold admission, quorum/spread/jump clamps, rate
       math, publisher, poll loop; clears its layer on shutdown). Verified
       end-to-end against a mock exchange API and a live node.
5. [x] Functional test `feature_dynamic_fee_rates.py`: dynamic admission, fee
       payment in a dynamically admitted asset, static precedence,
       withdrawal/clear, rejection of non-whitelisted fee assets.

## Milestone 3 — Challenge 2 (Bitcoin anchoring)
1. [x] `g_con_bitcoin_anchor` + anchor fields (`m_anchor_height`,
       `m_anchor_hash`) in `CBlockHeader` (both serialization branches, inside
       `SER_GETHASH` so the federation signs over them); mirrored in
       `CBlockIndex`/`CDiskBlockIndex`/`txdb`; new genesis hash for the
       anchored chain (doc 03 §2).
2. [x] Anchor helpers over `mainchainrpc` (`getblockcount`, `getblockhash`,
       `getblockheader`, `getbestblockhash`) + OK-result cache invalidated on
       parent tip change (`src/anchor.{h,cpp}`).
3. [x] Validation: R1 (anchor required), R2 (monotone heights; same height ⇒
       same hash), R3 (anchor on the parent chain's best chain, via the
       mainchain daemon; skipped when unchanged from parent block or with
       `-validateanchor=0`) in `ContextualCheckBlockHeader`.
4. [x] Block assembly anchors to the parent tip minus `-anchorminconf`-1,
       never below the previous block's anchor; falls back to the previous
       anchor if the daemon is unreachable.
5. [x] Parent-tip watcher (scheduler task, `-anchorpollinterval`) +
       reorg-following via `InvalidateBlock`/`ActivateBestChain`, with
       reconsideration if the parent chain reorganizes back.
6. [x] Settings `-con_bitcoin_anchor` / `-validateanchor` / `-anchorminconf` /
       `-anchorpollinterval`; startup probe reusing `MainchainRPCCheck` (parent
       genesis check made conditional). `getanchorstatus` RPC + anchor fields in
       `getblockheader`/`getblock`. Note: the spec'd `-anchormaxlag`/`-anchormaxlead`
       recency window is **deferred** (R3 + monotonicity bound staleness for the
       federated PoC).
7. [x] Functional test `feature_bitcoin_anchoring.py` (parent = second
       elementsd, RPC-identical to bitcoind for anchoring): happy path, anchor
       advance/monotonicity, parent reorg ⇒ anchored-chain reorg onto the
       surviving block, re-anchoring to the new branch, persistence across
       restart. Sandbox policies for `scheduler`/`msghand` extended with
       network access (they now legitimately call the mainchain daemon).

## Milestone 4 — Integration & demo (partially done)
- [x] End-to-end demo of the two mechanisms together: federated chain anchored
      to a parent chain, dynamic fee whitelist fed by the price server against
      a mock exchange API, parent reorg correctly reorganizing the anchored
      chain. (Cross-chain HTLC/atomic-swap walkthrough still to do.)
- [x] Operator docs: `contrib/price-server/README.md`; anchoring options
      documented in `-help` (ELEMENTS category).
- [ ] Cross-chain HTLC / atomic-swap demo flow.

## Milestone 5 (later) — PoS consensus
- [ ] Per the theoretical paper and doc 04 §3. Out of scope for the PoC.

## Risks / watch-items
- **Build resources.** A full Elements build is heavy (~4-core / 15 GB host
  used for the PoC build). CI should build with BDB so the pre-existing
  `feature_any_asset_fee*` legacy-wallet tests run.
- **Downstream Elements drift.** Keep merges small and frequent; never let the gap
  grow to a major version.
- **Header-format change (challenge 2)** means a fresh genesis — fine for the PoC,
  but coordinate the genesis/params change with any testnet already running.
- **Consensus correctness of reorg-following** is the subtlest part; treat the
  bitcoind best chain as the shared oracle and keep the invalidation logic
  deterministic across nodes (doc 03 §4).
</content>
