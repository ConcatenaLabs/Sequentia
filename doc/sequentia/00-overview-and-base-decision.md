# Sequentia — Design Specification

> **Status:** Living design document for the *SequentiaByClaude* effort. It
> records (a) what Sequentia is, (b) the Elements subsystems it builds on, and
> (c) how the priority features are implemented.

## 1. What Sequentia is

Sequentia ("Sequentia Network") is a **Bitcoin sidechain** built on
[Elements](https://github.com/ElementsProject/elements) (itself a fork of Bitcoin
Core). Elements is the technology that also powers Blockstream's *Liquid Network*.
Sequentia keeps Elements' UTXO model, Bitcoin Script, Confidential Assets and
asset-tokenisation machinery, but diverges from Liquid in these fundamental ways
(the first three are the project's "challenges", in priority order):

1. **Open / "no-coin" fee market.** There is *no* mandatory native fee asset
   (Liquid forces fees to be paid in L-BTC). Instead a user may offer *any* asset
   issued on the network as a transaction fee. A fee is **permissionless to
   propose** but **subject to per-producer acceptance**: a transaction is
   included only if a block producer is willing to accept that asset *and* at the
   rate the fee is posted. Producers independently decide which assets they will
   accept and at what relative value, then build the most valuable block they can
   from transactions paying in those assets. Producers configure acceptance two
   ways:
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
   long-range attacks), described in the theoretical paper. It is built in
   layers: a stake-weighted election
   ([`06-proof-of-stake.md`](06-proof-of-stake.md)), private VRF sortition, VRF
   committees and MuSig2 aggregation ([`07-vrf.md`](07-vrf.md)), on-chain stake,
   and Bitcoin checkpoints. It runs on top of Elements' inherited signed-block
   machinery (see [`04-consensus-poc.md`](04-consensus-poc.md)).

4. **Bitcoin-identical default addresses, opt-in confidential transactions.**
   The default address format is the same as Bitcoin's, so Sequentia wallet
   apps (which are intended to always also be Bitcoin wallets) can present one
   receiving address for both chains. Since a shared Bitcoin-format address
   cannot carry a blinding key, confidential transactions are **opt-in** with a
   visibly distinct address format — inverting Liquid's blinded-by-default
   behavior (see [`08-addresses-and-ct.md`](08-addresses-and-ct.md)).

A consequence of (1) and (2) worth spelling out: Elements' **federated two-way
peg is inherited but plays no special role here**. Sequentia is not configured
with a parent-chain peg, and the network neither favours nor depends on any
pegged asset (unlike Liquid's L-BTC). Pegged BTC has no special fee status,
positive or negative: it can be proposed as a fee exactly like any other
non-policy asset, subject to the same per-producer acceptance (asset + rate).
(SEQ is special **only** in that it is the asset that unlocks block-production
eligibility — staking. For *fees* it has no privileged status: like any asset it
is accepted 1:1 only as the default an unconfigured producer uses, and a producer
may re-price it, refuse it, or make a different asset the 1:1 reference.) Any user
may employ the inherited machinery to issue their
own pegged BTC, but anchoring-based real-time atomic swaps against *native* BTC
make that largely unnecessary — the main residual use case is holding BTC value
under confidential transactions.

## 2. The Elements subsystems Sequentia builds on

The relevant Elements groundwork (detailed in
[`01-elements-architecture.md`](01-elements-architecture.md)):

- **Elements multi-asset + fee plumbing.** Transactions, the UTXO set, and the
  mempool are asset-aware (Confidential Assets). Liquid restricts fees to
  `policyAsset`; the open fee market relaxes that restriction.

- **Open fee market (challenge 1).** Built on the Elements multi-asset plumbing:
  - `src/exchangerates.{h,cpp}` — an `ExchangeRateMap` singleton mapping
    `CAsset → scaled rate`, plus JSON load/save (`exchangerates.json`).
  - `src/policy/value.h` — `CValue`, an asset-independent "reference fee atom"
    (rfa) unit used to make heterogeneous fees comparable in the mempool.
  - Mempool entries (`src/txmempool.h`) carry `nFeeAsset` and `nFeeValue` (rfa);
    the miner (`src/node/miner.cpp`) orders packages by rfa value via
    `GetModFeesWithAncestors()`.
  - RPCs `getfeeexchangerates` / `setfeeexchangerates`
    (`src/rpc/exchangerates.cpp`).
  - Gated by the consensus flag `g_con_any_asset_fees` (set `true` for the
    Sequentia chain in `src/chainparams.cpp`).

  The *dynamic* layer — the price server and its threshold-driven
  auto-admission (`setdynamicfeerates` etc., `contrib/price-server/`) — sits on
  top. See [`02-open-fee-market.md`](02-open-fee-market.md).

- **Bitcoin-node connectivity.** Elements talks to a trusted `bitcoind` over RPC
  for peg-in validation: `src/mainchainrpc.{h,cpp}`, `MainchainRPCCheck()` in
  `src/init.cpp`, and the `-mainchainrpc*` / `-validatepegin` settings. **The
  anchoring feature reuses this transport** to fetch Bitcoin block
  hashes/heights rather than inventing a new one. Anchoring is implemented
  (`src/anchor.{h,cpp}`, `-con_bitcoin_anchor`). See
  [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md).

- **Block-signing machinery (challenge 3).** Elements "signed blocks"
  (`g_signed_blocks`, `consensus.signblockscript`, `CProof` /
  `DynaFedParams` / `m_signblock_witness` in `src/primitives/block.h`) replace
  Bitcoin's proof-of-work with a header block signature. The Sequentia chain
  enables it (`g_signed_blocks = true`) and drives the per-block signing rule
  from a stake-weighted election rather than a fixed signer set. The full
  Proof-of-Stake consensus (docs 06/07) is implemented on top of this signed-block
  machinery, behind `-con_pos`.

## 3. Base: built on Elements

Sequentia is built on [Elements](https://github.com/ElementsProject/elements),
which gives it the entire Bitcoin-Core/Elements stack — the UTXO model, Bitcoin
Script, Confidential Assets, asset issuance, the trusted `bitcoind` RPC
transport, and the signed-block machinery — to build the four challenges on.

### Tracking upstream Elements

- The repository is based on Elements `23.3.3` and tracks upstream so future
  Elements releases can be merged downstream. Shared ancestry with Elements and
  Bitcoin Core is what makes `git merge` of future releases tractable.
- The `elements-upstream` remote
  (`https://github.com/ElementsProject/elements.git`) is configured for
  downstream tracking.

To catch up to a later Elements release:

```sh
git fetch elements-upstream --tags
git merge elements-23.3.3        # resolve conflicts, then build & run the test suite
```

## 4. Document map

| Doc | Contents |
|---|---|
| [`01-elements-architecture.md`](01-elements-architecture.md) | The Elements subsystems that matter for these features, with file/line references. |
| [`02-open-fee-market.md`](02-open-fee-market.md) | Challenge 1: the static whitelist and the dynamic **price server**. |
| [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) | Challenge 2: block-header change, validation rules, reorg-following, Bitcoin RPC. |
| [`04-consensus-poc.md`](04-consensus-poc.md) | Challenge 3: Proof-of-Stake on Elements' inherited signed-block machinery. |
| [`05-roadmap.md`](05-roadmap.md) | Milestones, ordering, and test strategy. |
| [`06-proof-of-stake.md`](06-proof-of-stake.md) | The implemented PoS consensus: stake registry, leader election, committees, on-chain stake, checkpoints. |
| [`07-vrf.md`](07-vrf.md) | Private VRF sortition (RFC 9381-structured ECVRF) and MuSig2 committee aggregation, incl. distributed signing. |
| [`08-addresses-and-ct.md`](08-addresses-and-ct.md) | Bitcoin-identical addresses and opt-in confidential transactions. |
| [`09-running-sequentia.md`](09-running-sequentia.md) | Operator runbook: deploying the full system end-to-end. |
| [`10-liveness-and-escaping-stall.md`](10-liveness-and-escaping-stall.md) | Design for the anchor-driven liveness / escaping-stall (the last pre-mainnet consensus item). |
| [`11-security-and-hardening-notes.md`](11-security-and-hardening-notes.md) | Deferred hardening items + analyses from adversarial review, and whitepaper features beyond the implemented scope. |
| [`12-tokenomics-and-genesis.md`](12-tokenomics-and-genesis.md) | SEQ supply (400M, no inflation), the genesis bootstrap, and what's a launch decision vs. fixed in code. |
