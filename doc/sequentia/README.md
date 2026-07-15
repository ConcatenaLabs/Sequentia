# doc/sequentia - index

This directory is the canonical Sequentia protocol documentation, plus the
design notes and correspondence that produced it. It divides into three kinds
of document. Only the first kind is kept continuously in sync with the code;
everything else is labeled with its status below.

## 1. Current protocol documentation (living, code-accurate)

The numbered chapters are the definitive specification of the system as built,
verified against the source on this branch. Start at `00`.

| Chapter | Contents |
|---|---|
| [`00-overview.md`](00-overview.md) | What Sequentia is, the four defining properties, the reading guide. |
| [`01-architecture.md`](01-architecture.md) | The Elements substrate and how each property attaches to it; addresses and opt-in confidential transactions. |
| [`02-open-fee-market.md`](02-open-fee-market.md) | Reference-unit fee valuation, the per-producer whitelist, cross-asset RBF/CPFP, the price server. |
| [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) | The anchor commitment, validation and reorg-following, immediate finality, real-time cross-chain atomic swaps. |
| [`04-proof-of-stake.md`](04-proof-of-stake.md) | The full consensus: stake registry, VRF sortition, BLS committee certification (incl. the public fixed-size committee the testnet runs), liveness, fork choice, checkpoints, production. |
| [`05-operating-sequentia.md`](05-operating-sequentia.md) | The operator and wallet manual: joining the public testnet, fee policy, anchoring, producing blocks, the stake lifecycle, monitoring. |
| [`06-tokenomics-and-launch.md`](06-tokenomics-and-launch.md) | Sequence token (SEQ) supply, genesis construction, the genesis-seeded bootstrap, bundled and custom chains, governance vs engineering. |
| [`07-security-and-audit.md`](07-security-and-audit.md) | Security model, audit findings and their disposition, implementation status. |

Reference (current):

| Document | Contents |
|---|---|
| [`issuing-an-asset-guide.md`](issuing-an-asset-guide.md) | For issuers who are not web developers: finding your exact domain (the `www` question), what to type into Core, publishing the proof file on your site (WordPress included), checking it worked, and the usual questions. |
| [`asset-contracts-and-verification.md`](asset-contracts-and-verification.md) | The mechanism underneath: the contract committed into the asset id at issuance, the canonical hash, the domain proof, the registry, and why none of it can be added afterwards. |

Operating runbooks (current):

| Runbook | Status |
|---|---|
| [`runbook-windows-node.md`](runbook-windows-node.md) | Join the public testnet from a Windows machine, stake, issue assets, exercise the fee market. Matches the post-re-genesis (2026-07-05) chain. |
| [`demos/sequentia-testnet-runbook.md`](demos/sequentia-testnet-runbook.md) | Stand up a local `chain=test` committee with the bootstrap tooling. Written before the 2026-07-05 re-genesis: for the current public chain add `--public-committee` (i.e. `pospubliccommittee=1`, cap 250), and note `chain=test` now auto-adds the public gateway as a peer. |
| [`demos/100-node-bootstrap-runbook.md`](demos/100-node-bootstrap-runbook.md) | Historical demo: the 100-node mainnet-style bootstrap (pre-public-committee, anchored to Bitcoin testnet3 at the time). The tooling it drives is current (`contrib/sequentia/bootstrap-autonomous-testnet.py`). |
| [`regenesis-box-runbook.md`](regenesis-box-runbook.md) | Historical record: the box-side execution plan for the 2026-07-05 testnet re-genesis (executed; kept as the record of how the current chain was launched). |
| [`release-versioning.md`](release-versioning.md) | Policy: official releases are git-tagged at the build commit; private/test rebuilds keep the version number and identify via `-uacomment` instead. |
| [`build-windows-installer.md`](build-windows-installer.md) | Build the Windows setup executable (MinGW cross-compile + NSIS from Linux/WSL), including the bundled price server and its Python runtime. |

## 2. Design documents

Design notes, investigations, and audits. These record how decisions were
reached and may describe superseded iterations; the numbered chapters above,
not these, are authoritative for current behavior. Status of each:

**Consensus / node (this repository)**

| Document | Status |
|---|---|
| [`proposals/autonomous-committee.md`](proposals/autonomous-committee.md) | Implemented. The specification of the autonomous gossip-and-sign production layer (`-posproducer` + `-posbls`), including the liveness/safety arguments. |
| [`committee-regenesis-parameters.md`](committee-regenesis-parameters.md) | Implemented. The locked committee-design decisions behind the 2026-07-05 re-genesis (public fixed-size committee, cap 250, anchor-derived seed, bitfield certificate). |
| [`anchor-reorg-of-reorg-recovery-design.md`](anchor-reorg-of-reorg-recovery-design.md) | Implemented. Recovery when Bitcoin reorganizes back and forth; exercised by `feature_pos_reorg_of_reorg_recovery.py` and `feature_pos_parent_reorg_recovery.py`. |
| [`AUDIT-2026-06.md`](AUDIT-2026-06.md) | Audit record (2026-06), ecosystem-wide. Node-relevant dispositions are folded into `07-security-and-audit.md`. |
| [`escaping-stall-investigation-2026-06.md`](escaping-stall-investigation-2026-06.md) | Historical investigation of single-signer blocks on the pre-re-genesis testnet; led to fixes now in the code. |
| [`fee-asset-mempool-crash.md`](fee-asset-mempool-crash.md) | Incident record; the crash is fixed. |
| [`handoff-2026-06-28.md`](handoff-2026-06-28.md) | Historical session handover (order-book DEX + anchor consensus state as of 2026-06-28). |
| [`committee-seed-grind.py`](committee-seed-grind.py), [`committee-size-dist.py`](committee-size-dist.py) | Analysis scripts backing the committee-sizing and seed-grinding memos. |

**Ecosystem designs (implemented in other repositories)**

| Document | Status |
|---|---|
| [`openamp-design.md`](openamp-design.md) | Implemented in [`openamp`](https://github.com/GracedEternalKingCabbageMan/openamp); the daemon is live on the public testnet with the demo asset BONDX. Zero consensus changes in this repo. |
| [`seqdex-orderbook-design.md`](seqdex-orderbook-design.md) | Implemented in [`seqdex`](https://github.com/GracedEternalKingCabbageMan/seqdex) (the seqob order book). |
| [`cross-chain-orderbook-consolidation.md`](cross-chain-orderbook-consolidation.md) | Decision note for seqdex: cross-chain trading consolidated onto the order book (RFQ special-maker retired). |
| [`seqln-core-lightning-fork-spec.md`](seqln-core-lightning-fork-spec.md) | The operative SeqLN design spec; implemented in [`seqln`](https://github.com/GracedEternalKingCabbageMan/seqln). |
| [`sequentia-lightning-cln-spec.md`](sequentia-lightning-cln-spec.md) | Superseded by `seqln-core-lightning-fork-spec.md` (the earlier fork plan). |
| [`seqln-asset-channels-build-plan.md`](seqln-asset-channels-build-plan.md) | Implemented in seqln (asset-aware channels). |
| [`seqln-phase2-submarine-swaps.md`](seqln-phase2-submarine-swaps.md) | Implemented in seqln (submarine-swap primitives, both directions). |
| [`seqln-phase2-dex-integration.md`](seqln-phase2-dex-integration.md) | Design: mapping the submarine-swap primitives onto the SeqDEX order book. |
| [`seqln-step2-pure-ln-swaps-design.md`](seqln-step2-pure-ln-swaps-design.md) | Design for pure-Lightning asset↔BTC swaps; implemented in seqln per its milestone log. |
| [`seqln-tier2-hosted-channels-design.md`](seqln-tier2-hosted-channels-design.md) | Design for the hosted-channel signer split (thin-wallet non-custodial Lightning); daemon-layer milestones implemented in seqln, wallet integration in progress. |
| [`seqln-dex-instant-swap-latency.md`](seqln-dex-instant-swap-latency.md), [`seqln-dex-instant-swap-latency-followup.md`](seqln-dex-instant-swap-latency-followup.md) | Design notes on making Lightning-DEX swaps feel instant; the followup records the pure-LN endgame as built. |
| [`seqdex-lightning-feasibility.md`](seqdex-lightning-feasibility.md) | Exploratory feasibility study (Lightning on Sequentia); partly superseded by the seqln docs above. |
| [`seqdex-simplicity-assessment.md`](seqdex-simplicity-assessment.md) | Exploratory: what Simplicity covenants would open for SeqDEX. Simplicity is vendored but NEVER_ACTIVE on the bundled chains. |
| [`simplicity-dex-covenant-offers-design.md`](simplicity-dex-covenant-offers-design.md) | Design + regtest proof of covenant-enforced resting DEX offers; exploratory (gated off on the public chain). |
| [`ux-audit-spec-2026-07-02.md`](ux-audit-spec-2026-07-02.md) | UX audit and design-change spec across the ecosystem's user-facing surfaces; implementation tracked in the respective repos. |

## 3. Historical correspondence

The `alberto-*` files (Markdown, HTML, and PDF) are correspondence records
with the consensus reviewer: data notes, replies, and decision memos. They are
kept verbatim as historical records and are never edited; where a decision in
them became code, the numbered chapters reflect it.
