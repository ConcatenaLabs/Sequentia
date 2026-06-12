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
- [ ] Build the imported tree in a real build environment (depends/autotools or
      the documented build path); produce `sequentiad` / `sequentia-cli`.
- [ ] Run the existing functional suite; record the green baseline.
- [ ] **Merge Elements `23.3.3` downstream** (`git merge elements-23.3.3`),
      resolve conflicts, re-green. (Same `23.x` series ⇒ patch-level catch-up.)

## Milestone 2 — Finish challenge 1 (open fee market)
1. [ ] Audit/normalise all fee floors to rfa for any-asset txs (doc 02 §A).
2. [ ] Layer the `ExchangeRateMap` into static + dynamic with precedence,
       per-entry `source` + `updated_at`, and staleness drop (doc 02 §B.3).
3. [ ] New RPCs: `setdynamicfeerates` / `getdynamicfeerates` /
       `getfeeacceptancepolicy` / `cleardynamicfeerates`.
4. [ ] Price-server sidecar in `contrib/price-server/` (sources, thresholds, rate
       math, publisher, poll loop) + mock-API tests (doc 02 §B.4).
5. [ ] Functional tests: two-asset chain, dynamic admission/withdrawal, mining
       order by rfa (doc 02 §C).

## Milestone 3 — Challenge 2 (Bitcoin anchoring)
1. [ ] Add `g_con_bitcoin_anchor` + `BitcoinAnchor` fields to `CBlockHeader`
       (both serialization branches, in `SER_GETHASH`); mirror in `CBlockIndex`;
       new genesis (doc 03 §2).
2. [ ] Anchor helpers over `mainchainrpc` (`getblockhash`, `getblockheader`,
       `getbestblockhash`) + small cache (doc 01 §5).
3. [ ] Validation rules R1–R4 in `CheckBlockHeader` /
       `ContextualCheckBlockHeader` (doc 03 §3).
4. [ ] Block assembly sets the anchor to a mature recent Bitcoin tip (doc 03 §3 R4).
5. [ ] Bitcoin-tip watcher + reorg-following via `InvalidateBlock` /
       `ActivateBestChain` (doc 03 §4).
6. [ ] Settings `-anchorbtc` / `-anchormaxlag` / `-anchormaxlead` /
       `-anchorminconf`; startup probe (doc 03 §5).
7. [ ] Functional tests against regtest bitcoind: happy path, reorg cascade,
       guard rejections (doc 03 §7).

## Milestone 4 — Integration & demo
- [ ] End-to-end demo: Sequentia federated chain anchored to a regtest bitcoind,
      with two fee assets and a running price server; show a cross-chain HTLC /
      atomic-swap flow surviving (and correctly reorging on) a Bitcoin reorg.
- [ ] Operator docs: running `sequentiad` + bitcoind + price server.

## Milestone 5 (later) — PoS consensus
- [ ] Per the theoretical paper and doc 04 §3. Out of scope for the PoC.

## Risks / watch-items
- **Build resources.** A full Elements build is heavy; needs a real build host
  (not feasible inside the ephemeral session that produced this spec). Milestone 1
  is the gate everything else depends on.
- **Downstream Elements drift.** Keep merges small and frequent; never let the gap
  grow to a major version.
- **Header-format change (challenge 2)** means a fresh genesis — fine for the PoC,
  but coordinate the genesis/params change with any testnet already running.
- **Consensus correctness of reorg-following** is the subtlest part; treat the
  bitcoind best chain as the shared oracle and keep the invalidation logic
  deterministic across nodes (doc 03 §4).
</content>
