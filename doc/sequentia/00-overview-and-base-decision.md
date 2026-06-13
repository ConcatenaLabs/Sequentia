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
asset-tokenisation machinery, but diverges from Liquid in these fundamental ways
(the first three are the project's "challenges", in priority order):

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
   long-range attacks), described in the theoretical paper. Originally
   deprioritised in favour of a *strong federation* PoC (see
   [`04-consensus-poc.md`](04-consensus-poc.md)), now implemented in layers:
   public stake-weighted election ([`06-proof-of-stake.md`](06-proof-of-stake.md)),
   private VRF sortition, VRF committees and MuSig2 aggregation
   ([`07-vrf.md`](07-vrf.md)), on-chain stake, and Bitcoin checkpoints.

4. **Bitcoin-identical default addresses, opt-in confidential transactions.**
   The default address format is the same as Bitcoin's, so Sequentia wallet
   apps (which are intended to always also be Bitcoin wallets) can present one
   receiving address for both chains. Since a shared Bitcoin-format address
   cannot carry a blinding key, confidential transactions are **opt-in** with a
   visibly distinct address format — inverting Liquid's blinded-by-default
   behavior (see [`08-addresses-and-ct.md`](08-addresses-and-ct.md)).

A consequence of (1) and (2) worth spelling out: Elements' **federated two-way
peg is inherited but plays no special role here**. Sequentia is not configured
with a parent-chain peg, pegged BTC is never the fee currency, and the network
neither favours nor depends on any pegged asset (unlike Liquid's L-BTC). Any
user may employ the inherited machinery to issue their own pegged BTC, but
anchoring-based real-time atomic swaps against *native* BTC make that largely
unnecessary — the main residual use case is holding BTC value under
confidential transactions.

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

  The *dynamic* layer — the price server and its threshold-driven
  auto-admission (`setdynamicfeerates` etc., `contrib/price-server/`) — has
  since been implemented too. See [`02-open-fee-market.md`](02-open-fee-market.md).

- **Bitcoin-node connectivity already exists.** Elements talks to a trusted
  `bitcoind` over RPC for peg-in validation: `src/mainchainrpc.{h,cpp}`,
  `MainchainRPCCheck()` in `src/init.cpp`, and the `-mainchainrpc*` /
  `-validatepegin` settings. **The anchoring feature reuses this transport** to
  fetch Bitcoin block hashes/heights rather than inventing a new one.
  Anchoring is implemented (`src/anchor.{h,cpp}`, `-con_bitcoin_anchor`). See
  [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md).

- **Strong-federation consensus already exists.** Elements "signed blocks"
  (`g_signed_blocks`, `consensus.signblockscript`, `CProof` /
  `DynaFedParams` / `m_signblock_witness` in `src/primitives/block.h`) provide a
  federated block-signing consensus out of the box. The Sequentia chain already
  enables it (`g_signed_blocks = true`). This satisfied the challenge-3 PoC
  requirement with no new consensus code; the full Proof-of-Stake consensus
  (docs 06/07) has since been implemented on top of the same signed-block
  machinery, behind `-con_pos`.

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
| [`06-proof-of-stake.md`](06-proof-of-stake.md) | The implemented PoS consensus: stake registry, leader election, committees, on-chain stake, checkpoints. |
| [`07-vrf.md`](07-vrf.md) | Private VRF sortition (RFC 9381-structured ECVRF) and MuSig2 committee aggregation, incl. distributed signing. |
| [`08-addresses-and-ct.md`](08-addresses-and-ct.md) | Bitcoin-identical addresses and opt-in confidential transactions. |
| [`09-running-sequentia.md`](09-running-sequentia.md) | Operator runbook: deploying the full system end-to-end. |
| [`10-liveness-and-escaping-stall.md`](10-liveness-and-escaping-stall.md) | Design for the anchor-driven liveness / escaping-stall (the last pre-mainnet consensus item). |
