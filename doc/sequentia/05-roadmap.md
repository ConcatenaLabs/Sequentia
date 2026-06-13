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
- [x] **Merge Elements `23.3.3` downstream** (`git merge elements-23.3.3`):
      671 upstream commits, 23 conflicted files. Key resolutions kept the
      rfa (`CValue`) fee model through upstream's modified-fee refactor and
      discounted-CT additions; see the merge commit message. Re-greened:
      unit suites + the functional battery pass; binary reports v23.3.3.
      Also fixed a latent fork bug found in the process: uninitialized
      `initialFreeCoins`/`initial_reissuance_tokens` members made the
      testnet genesis hash nondeterministic.
      The pre-existing `key_io_tests` failures (fork-changed testnet address
      prefixes vs upstream vectors) were fixed by transcoding the
      `"chain":"test"` vectors in `src/test/data/key_io_valid.json` to the
      fork's prefixes (base58 111→52, 196→193, WIF 239→249, bech32 tb→tsq),
      preserving payloads. **The full `test_bitcoin` unit suite is green.**

## Milestone 2 — Finish challenge 1 (open fee market) — COMPLETE
1. [x] Audit fee floors for any-asset txs: all floors (mempool min, min-relay,
       block-min, RBF increments, prioritisation deltas) are compared in rfa;
       configured values are policy-asset atoms pegged 1:1 to rfa. Findings
       recorded in doc 02 §A.
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
8. [x] Robustness: R3 results that depend on the local parent daemon's view
       (anchor unknown/stale/unreachable) are rejected with
       `BLOCK_RECENT_CONSENSUS_CHANGE` — no peer punishment, not cached as
       invalid — so honest nodes whose bitcoinds are transiently out of sync
       cannot ban each other or permanently reject valid blocks. Only
       structural violations (R1/R2, height mismatch) are permanent.

## Milestone 4 — Integration & demo — COMPLETE
- [x] End-to-end demo of the two mechanisms together: federated chain anchored
      to a parent chain, dynamic fee whitelist fed by the price server against
      a mock exchange API, parent reorg correctly reorganizing the anchored
      chain.
- [x] Operator docs: `contrib/price-server/README.md`; anchoring options
      documented in `-help` (ELEMENTS category).
- [x] Cross-chain swap consistency demo:
      `test/functional/feature_anchor_swap_consistency.py` — BTC leg confirmed
      on the parent chain, SEQ leg confirmed in a Sequentia block anchored at
      `>=` the BTC leg's height (paper principle 7), then the parent
      reorganizes with the BTC leg double-spent away and the anchored chain
      automatically disconnects the SEQ leg's block: both legs revert
      together, with no extra timelocks. (The property is independent of the
      locking script, so HTLCs ride on top unchanged.)

## Milestone 4b — Bitcoin-identical addresses & opt-in CT
- [x] Restored the `--enable-any-asset-fees` configure stanza (defines
      `ANY_ASSET_FEES`, renaming the displayed fee units to RFU/rfa) that the
      elements-23.3.3 merge silently dropped, and made it the verified build
      configuration: with it, the fork-modified wallet tests
      (`wallet_basic`/`wallet_send`/`rpc_psbt` expect "rfa/vB") pass, and
      `amount_tests/ToStringTest` was made unit-constant-aware so the unit
      suite is green in both build modes.
- [x] The Sequentia `test` chain's default address parameters are Bitcoin
      testnet's (base58 111/196, WIF 239, tpub/tprv, bech32 `tb`), enabling the
      shared BTC/SEQ receiving-address wallet model; the confidential format
      stays distinct (blinded 70, blech32 `tsqb`). Upstream `key_io_valid.json`
      vectors restored (the earlier transcoding to custom prefixes is reverted)
      and green.
- [x] Confidential transactions are opt-in: new chain-level default
      `CChainParams::DefaultBlindedAddresses()` (false on Sequentia chains,
      true elsewhere) behind the existing `-blindedaddresses` option;
      per-call opt-in via `getnewaddress "" "blech32"`;
      `-con_default_blinded_addresses` for custom chains. Functional test
      `feature_ct_opt_in.py`. See doc 08.

## Milestone 5 — PoS consensus — COMPLETE
Originally deferred ("out of scope for the PoC"); since implemented in full,
per the theoretical paper and doc 04 §3. The detailed item list lives in
[doc 06 §"Implementation roadmap"](06-proof-of-stake.md) (all items checked):

- [x] Stake registry + deterministic stake-weighted leader schedule, enforced
      by consensus (`-con_pos`, `-staker`, `-posslotinterval`).
- [x] Private VRF sortition (RFC 9381-structured ECVRF over secp256k1,
      `-posvrf`; doc 07).
- [x] Committee certification: script multisig up to 16 members
      (`-poscommitteesize`) and paper-scale committees up to 100 via MuSig2
      aggregation (`-posaggcommittee`), including **distributed signing**
      across separately-hosted members (`getposblocktemplate` /
      `submitposblock` + the `musig*` RPC suite; doc 07 §6).
- [x] On-chain stake registration / unbonding (`getstakescript`,
      `-posunbonding`; height- or time-based CSV, doc 06 §5).
- [x] Minimum-stake blocksigner floor (`-posminstake`, whitepaper §3.3).
- [x] Long-range-attack defenses: dynamic Bitcoin checkpoints
      (`getcheckpointpayload` / `getcheckpointinfo`, `-poscheckpointdepth`,
      default 2016 BTC blocks) and operator-configured static checkpoints
      (`-poscheckpoint`).
- [x] No inflation: SEQ pre-mined at genesis, `con_blocksubsidy=0` (§3.9).
- [x] Operator runbook for deploying all of it (doc 09).

## Milestone 6 — Anchor-driven liveness & escaping-stall — IN PROGRESS
The last major consensus item before mainnet: the whitepaper's
Bitcoin-anchor-driven escaping-stall (§3.5/§3.8). Design and staged plan in
[doc 10](10-liveness-and-escaping-stall.md).

- [x] Consensus-view anchor-depth condition (`PosEscapingStallAllowed`, pure,
      from committed anchor heights; `pos_escaping_stall_gap`).
- [x] Escaping-stall sub-threshold certification (h+3 rule) in
      `CheckPosStakeRules` + producer RPCs (`feature_pos_escaping_stall.py`).
- [x] PoS same-height fork choice — more countersignatures wins, then lowest
      leader VRF score (`CBlockIndexWorkComparator` keys on `CBlockIndex`;
      `feature_pos_fork_choice.py`). Done via the comparator, not a header
      change (doc 10 §6).
- [ ] Retire the wall-clock slot gate for the anchor clock.
- [ ] Dynamic committee floor when participation is short.

The two open items are fidelity refinements over already-safe, deterministic
mechanisms — see [doc 10 §7](10-liveness-and-escaping-stall.md).

## Status for mainnet
The four challenges and the full PoS consensus are implemented, tested, and
adversarially reviewed (crypto, consensus, stake registry, fee market, P2P,
wallet/CT). The remaining work is enumerated and falls into three buckets,
none a regression or a safety/consensus-split gap on a correctly-configured
network:
- **Consensus refinements remaining** — Milestone 6 stages 4–5 (anchor clock,
  dynamic committee floor; doc 10 §7) and the fork/sibling-block storage
  hardening (doc 11 §1) — all fidelity/DoS-hardening over already-safe paths.
- **Launch / governance parameters** — genesis SEQ supply (400M) & distribution,
  the founding staker set, committee size, `-posminstake`, `-posunbonding` — set
  at launch, like any chain's founding constants (doc 12). The block weight cap
  (400,000) is now implemented (doc 11 §4).
- **Out-of-scope future subsystems** (beyond the four challenges) — asset ACLs
  (to be built with Simplicity), programmable accounts (deferred / maybe
  unnecessary given Simplicity), utreexo (after it matures in Bitcoin) — doc 11 §4.

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
