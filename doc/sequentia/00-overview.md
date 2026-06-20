# Sequentia — Design & Operations Specification

Sequentia ("Sequentia Network") is a **Bitcoin sidechain** built on
[Elements](https://github.com/ElementsProject/elements), the Bitcoin Core
derivative that also powers Blockstream's Liquid Network. It inherits Elements'
UTXO model, Bitcoin Script, Confidential Assets, and asset-issuance machinery,
and adds four defining properties that distinguish it from Liquid.

This document set is the definitive specification of those properties and a
manual for operating the network. It describes the system as built.

## The four defining properties

1. **Open ("no-coin") fee market.** There is no mandatory native fee asset. A
   user may offer *any* issued asset as a transaction fee. Proposing a fee asset
   is permissionless; *accepting* it is each block producer's choice. Producers
   value heterogeneous fees against a common reference unit using an
   exchange-rate table — a static whitelist and/or a dynamically maintained one
   fed by a price server — and build the most valuable block they can from the
   transactions paying in assets they accept. SEQ, the staking asset, holds **no
   privileged fee status**: it is accepted 1:1 only as the default an
   unconfigured producer uses, and a producer may re-price it, refuse it, or make
   another asset the reference. See [`02-open-fee-market.md`](02-open-fee-market.md).

2. **Bitcoin anchoring.** Every Sequentia block references a Bitcoin block whose
   height is non-decreasing along the chain. A Sequentia node therefore maintains
   a connection to a Bitcoin node. The chain reorganizes *if and only if* Bitcoin
   reorganizes away a referenced block; otherwise Sequentia has **immediate
   finality**. This binds Sequentia's reorg risk to Bitcoin's, which is what makes
   real-time cross-chain atomic swaps against native BTC possible with no extra
   reorg-protection timelock. See [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md).

3. **Proof-of-Stake consensus.** Block production is a stake-weighted election
   with private VRF sortition; a committee certifies each block with a single
   aggregated signature (BLS12-381 by default; MuSig2 the `-posbls=0` fallback),
   giving the chain its immediate finality. Voluntary
   Bitcoin checkpoints resist long-range attacks. SEQ is the staking asset and the
   one thing that confers production eligibility. See
   [`04-proof-of-stake.md`](04-proof-of-stake.md).

4. **Bitcoin-identical default addresses, opt-in confidential transactions.**
   The default address format matches Bitcoin's, so a wallet can present one
   receiving address for both chains. Because a Bitcoin-format address carries no
   blinding key, confidential transactions are opt-in behind a visibly distinct
   address format — the inverse of Liquid's blinded-by-default model. See
   [`01-architecture.md`](01-architecture.md).

A consequence of (1) and (2): Elements' federated two-way peg is **inherited but
plays no role**. Sequentia configures no parent-chain peg and depends on no
pegged asset. Pegged BTC has no special fee status — it is proposed as a fee like
any other asset, subject to the same per-producer acceptance. Anyone may still
issue their own pegged BTC, but anchoring-based swaps against *native* BTC make
that largely unnecessary; the residual use case is holding BTC value under
confidential transactions.

## Built on Elements

Sequentia is based on Elements `23.3.3` and tracks it downstream, so future
Elements releases can be merged in. The four properties are layered on the
inherited stack: the open fee market on Elements' multi-asset plumbing, anchoring
on its Bitcoin-RPC transport, and Proof-of-Stake on its signed-block machinery.
[`01-architecture.md`](01-architecture.md) describes that substrate and how each
property attaches to it.

```sh
# Catch up to a later Elements release:
git fetch elements-upstream --tags
git merge elements-23.3.3        # resolve conflicts, then build and run the suite
```

## Reading guide

The chapters build from design through operation to review:

| Chapter | Contents |
|---|---|
| [`01-architecture.md`](01-architecture.md) | The Elements substrate and how the four properties attach to it: multi-asset/fee plumbing, the Bitcoin-RPC transport, signed blocks, addresses & confidential transactions, validation entry points. |
| [`02-open-fee-market.md`](02-open-fee-market.md) | The reference-unit fee valuation, the static and dynamic exchange-rate tables, per-producer acceptance, fee-replacement (RBF/CPFP) across assets, and paying fees in an arbitrary asset. |
| [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) | The anchor commitment, validation and reorg-following rules, immediate finality, and real-time cross-chain atomic swaps. |
| [`04-proof-of-stake.md`](04-proof-of-stake.md) | The full consensus: stake registry, VRF sortition and leader election, committee certification with BLS aggregation (MuSig2 the `-posbls=0` fallback), liveness (escaping-stall), fork choice and the finality gate, checkpoints, and the production layer. |
| [`05-operating-sequentia.md`](05-operating-sequentia.md) | The operator and wallet manual: configuration, the fee market and price server, anchoring, running a producer, the stake lifecycle, monitoring, the bundled tooling, and the launch checklist. |
| [`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md) | SEQ supply and distribution, the genesis-seeded bootstrap, the bundled chains, and what is a governance decision versus fixed in code. |
| [`07-security-and-audit.md`](07-security-and-audit.md) | The security model, the pre-mainnet audit and its resolved findings, features beyond the implemented scope, and implementation status. |
