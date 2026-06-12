# Sequentia — Proof-of-Concept Design Specification

> **Status:** Living design document. This is the first specification produced
> for the *SequentiaByClaude* effort. It records (a) what Sequentia is, (b) the
> result of studying the Elements codebase, (c) the decision of which codebase to
> fork, and (d) the concrete engineering plan for the two priority features.

## 1. What Sequentia is

Sequentia ("Sequentia Network") is a **Bitcoin sidechain** built as a fork of
[Elements](https://github.com/ElementsProject/elements) (itself a fork of Bitcoin
Core). Elements is the technology that also powers Blockstream's *Liquid Network*.
Sequentia keeps Elements' UTXO model, Bitcoin Script, Confidential Assets and
asset-tokenisation machinery, but diverges from Liquid in three fundamental ways.
These are the project's three "challenges", in priority order:

1. **Open / "no-coin" fee market.** There is *no* mandatory native fee asset
   (Liquid forces fees to be paid in L-BTC). Instead a user may offer *any* asset
   issued on the network as a transaction fee. Block producers independently
   decide which assets they will accept and at what relative value, and then build
   the most valuable block they can from transactions paying in those assets.
   Producers configure acceptance two ways:
   - a **static whitelist** of `{asset → relative value}`, and
   - a **dynamic whitelist** maintained by a locally-run **price server** that
     queries external APIs (exchanges, DEX oracles) and auto-admits assets once
     they cross operator-defined thresholds (market cap, volume, volatility, …).

2. **Bitcoin anchoring.** Every Sequentia block carries a reference to a Bitcoin
   block whose height is `>=` the height referenced by the previous Sequentia
   block. Sequentia nodes therefore need a connection to a Bitcoin node. The chain
   reorganises *if and only if* Bitcoin reorganises away the referenced block,
   giving Sequentia **immediate finality as long as Bitcoin does not reorg**. The
   motive is friction-free cross-chain atomic swaps with Bitcoin — no extra
   reorg-protection timelocks.

3. **Proof-of-Stake consensus** (with voluntary Bitcoin checkpoints to resist
   long-range attacks), described in the theoretical paper. **Explicitly
   deprioritised** for the proof of concept: the PoC runs on a *strong federation*
   instead, which Elements already supports natively (see
   [`04-consensus-poc.md`](04-consensus-poc.md)).

This specification focuses on challenges **1** and **2**. Challenge 3 is scoped
only far enough to confirm the PoC consensus is already provided by Elements.

## 2. Result of studying the Elements / Sequentia codebases

The relevant findings (detailed in [`01-elements-architecture.md`](01-elements-architecture.md)):

- **Elements multi-asset + fee plumbing already exists.** Transactions, the UTXO
  set, and the mempool are asset-aware (Confidential Assets). Liquid restricts
  fees to `policyAsset`; the open fee market relaxes that restriction.

- **The existing Sequentia fork already implements challenge 1's *static*
  whitelist.** On the `master` branch of
  [`SequentiaSEQ/SEQ-Core-Elements`](https://github.com/SequentiaSEQ/SEQ-Core-Elements)
  there is a working implementation:
  - `src/exchangerates.{h,cpp}` — an `ExchangeRateMap` singleton mapping
    `CAsset → scaled rate`, plus JSON load/save (`exchangerates.json`).
  - `src/policy/value.h` — `CValue`, an asset-independent "reference fee atom"
    (rfa) unit used to make heterogeneous fees comparable in the mempool.
  - Mempool entries (`src/txmempool.h`) carry `nFeeAsset` and `nFeeValue` (rfa);
    the miner (`src/node/miner.cpp`) already orders packages by rfa value via
    `GetModFeesWithAncestors()`.
  - RPCs `getfeeexchangerates` / `setfeeexchangerates`
    (`src/rpc/exchangerates.cpp`).
  - Gated by the consensus flag `g_con_any_asset_fees` (set `true` for the
    Sequentia chain in `src/chainparams.cpp`).

  **What is missing from challenge 1:** the *dynamic* price server and its
  threshold-driven auto-admission. See [`02-open-fee-market.md`](02-open-fee-market.md).

- **Bitcoin-node connectivity already exists.** Elements talks to a trusted
  `bitcoind` over RPC for peg-in validation: `src/mainchainrpc.{h,cpp}`,
  `MainchainRPCCheck()` in `src/init.cpp`, and the `-mainchainrpc*` /
  `-validatepegin` settings. **The anchoring feature reuses this transport** to
  fetch Bitcoin block hashes/heights rather than inventing a new one. See
  [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md). Challenge 2 is *not* yet
  implemented in the Sequentia fork.

- **Strong-federation consensus already exists.** Elements "signed blocks"
  (`g_signed_blocks`, `consensus.signblockscript`, `CProof` /
  `DynaFedParams` / `m_signblock_witness` in `src/primitives/block.h`) provide a
  federated block-signing consensus out of the box. The Sequentia chain already
  enables it (`g_signed_blocks = true`). This satisfies the challenge-3 PoC
  requirement with no new consensus code.

## 3. Base decision: fork the existing Sequentia project

Per the project constraints the base must be **either** a fork of the existing
Sequentia project (an older Elements release, needing subsequent Elements
releases merged downstream) **or** a fresh fork of the latest Elements release.

**Decision: fork the existing Sequentia project** (`SequentiaSEQ/SEQ-Core-Elements`,
`master`), then track upstream Elements downstream.

### Rationale

| Factor | Fork existing Sequentia | Fresh latest Elements |
|---|---|---|
| Challenge 1 (static fee market) | **Already implemented & working** | Must be re-implemented from scratch (≈ the entire `exchangerates`/`CValue`/mempool-rfa subsystem) |
| Base Elements version | `elements-23.x` series | `elements-23.3.3` (latest) |
| Distance to latest Elements | Small — same `23.x` major series; downstream merge is a patch-level catch-up | N/A (already latest) |
| Risk | Inherit any Sequentia tech debt; must merge `23.x → 23.3.3` | Re-derive a known-correct subsystem and risk subtle divergence |
| Net work to reach PoC | Lower | Higher |

The existing fork is already on the **same `23.x` major series** as the latest
release (`23.3.3`), so "pulling subsequent Elements releases downstream" is a
manageable patch-level catch-up rather than a major-version jump — while it
preserves a substantial, working implementation of the highest-priority feature.
Starting fresh would discard that and force a re-implementation for no
compensating benefit.

### How this repository is set up as the fork

- This repo's history **is** `SequentiaSEQ/SEQ-Core-Elements@master` (full history,
  ~38.9k commits, sharing ancestry with Elements and Bitcoin Core). Shared
  ancestry is what makes `git merge` of future Elements releases tractable.
- Remotes are configured for downstream tracking:
  - `elements-upstream` → `https://github.com/ElementsProject/elements.git`
  - `sequentia-upstream` → `https://github.com/SequentiaSEQ/SEQ-Core-Elements.git`
- All SequentiaByClaude work happens on branch
  `claude/sequentia-bitcoin-sidechain-w6xady`.

To catch up to the latest Elements release later:

```sh
git fetch elements-upstream --tags
git merge elements-23.3.3        # resolve conflicts, then build & run the test suite
```

## 4. Document map

| Doc | Contents |
|---|---|
| [`01-elements-architecture.md`](01-elements-architecture.md) | The Elements subsystems that matter for these features, with file/line references. |
| [`02-open-fee-market.md`](02-open-fee-market.md) | Challenge 1: what exists (static whitelist) and the design for the new dynamic **price server**. |
| [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) | Challenge 2: block-header change, validation rules, reorg-following, Bitcoin RPC. |
| [`04-consensus-poc.md`](04-consensus-poc.md) | Challenge 3 PoC: strong federation via Elements signed blocks; path to PoS. |
| [`05-roadmap.md`](05-roadmap.md) | Milestones, ordering, and test strategy. |
</content>
</invoke>
