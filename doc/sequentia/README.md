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
| [`supervised-assets.md`](supervised-assets.md) | For issuers and operators: what supervision is and is not, the operational and recovery keys and why there are two, issuing, freezing, unfreezing, pause, key rotation, publishing records without being front-run, and the RPC reference. |
| [`supervised-assets-implementation.md`](supervised-assets-implementation.md) | Implementation notes for supervised assets: the consensus rules as coded, the record format, activation (testnet height 94,600), and the tests. Companion to the proposal below. |

Operating runbooks (current):

| Runbook | Status |
|---|---|
| [`runbook-windows-node.md`](runbook-windows-node.md) | Join the public testnet from a Windows machine, stake, issue assets, exercise the fee market. Matches the post-re-genesis (2026-07-05) chain. |
| [`hard-fork-and-restart-runbook.md`](hard-fork-and-restart-runbook.md) | Operator runbook for the testnet box: watching a consensus activation, the all-at-once committee cutover, wallet capture and reload, and the activation-height table. |
| [`demos/sequentia-testnet-runbook.md`](demos/sequentia-testnet-runbook.md) | Stand up a local `chain=test` committee with the bootstrap tooling. Written before the 2026-07-05 re-genesis: for the current public chain add `--public-committee` (i.e. `pospubliccommittee=1`, cap 250), and note `chain=test` now auto-adds the public gateway as a peer. |
| [`demos/100-node-bootstrap-runbook.md`](demos/100-node-bootstrap-runbook.md) | Historical demo: the 100-node mainnet-style bootstrap (pre-public-committee, anchored to Bitcoin testnet3 at the time). The tooling it drives is current (`contrib/sequentia/bootstrap-autonomous-testnet.py`). |
| [`regenesis-box-runbook.md`](regenesis-box-runbook.md) | Historical record: the box-side execution plan for the 2026-07-05 testnet re-genesis (executed; kept as the record of how the current chain was launched). |
| [`release-versioning.md`](release-versioning.md) | Policy: official releases are git-tagged at the build commit; private/test rebuilds keep the version number and identify via `-uacomment` instead. |
| [`build-windows-installer.md`](build-windows-installer.md) | Build the Windows setup executable (MinGW cross-compile + NSIS from Linux/WSL), including the bundled price server and its Python runtime. |

## 2. Design documents

Design notes, investigations, and audits. These record how decisions were
reached and may describe superseded iterations; the numbered chapters above,
not these, are authoritative for current behavior. Status of each:

**Cross-cutting protocol specs (living, code-accurate)**

| Document | Status |
|---|---|
| [`asset-denomination.md`](asset-denomination.md) | Canonical reference for the per-asset denomination (precision) field and the integration contract every wallet/service shares (Core, web wallet, Ambra, explorer, SeqDEX, bridges, registry). Core + desktop GUI implemented on this branch; other components tracked with a checklist. |

**Consensus / node (this repository)**

| Document | Status |
|---|---|
| [`proposals/autonomous-committee.md`](proposals/autonomous-committee.md) | Implemented. The specification of the autonomous gossip-and-sign production layer (`-posproducer` + `-posbls`), including the liveness/safety arguments. |
| [`committee-regenesis-parameters.md`](committee-regenesis-parameters.md) | Implemented. The locked committee-design decisions behind the 2026-07-05 re-genesis (public fixed-size committee, cap 250, anchor-derived seed, bitfield certificate). |
| [`committee-public-selection-impl-spec.md`](committee-public-selection-impl-spec.md) | Implemented. The implementation spec for the public fixed-size committee (Option A, confirmed 2026-07-03): deterministic membership, the bitfield certificate, quorum. |
| [`split-payouts-design.md`](split-payouts-design.md) | Implemented (flag day: testnet height 102,150, Sequentia Core 24.4.0). The third pool payout mode: proportional rewards through a claimable on-chain pot. |
| [`supervised-assets-proposal.html`](supervised-assets-proposal.html) / `.pdf` | The proposal that argued for supervised assets; the implementation notes above describe what shipped. |
| [`proof-of-stake.html`](proof-of-stake.html) / `.pdf` | "Proof of Stake on Sequentia": the standalone rendering of the proof-of-stake design for readers outside the repository. `04-proof-of-stake.md` is the maintained text. |
| [`incident-2026-07-17-finality-partition.md`](incident-2026-07-17-finality-partition.md) | Incident record: the finality partition of 2026-07-17 and the fix shipped in 23.3.6. |
| [`gui-intro-dialog-texts-2026-07-10.md`](gui-intro-dialog-texts-2026-07-10.md) | Record of the first-run dialog copy changes in the desktop GUI (`src/qt/intro.cpp`). |
| [`anchor-reorg-of-reorg-recovery-design.md`](anchor-reorg-of-reorg-recovery-design.md) | Implemented. Recovery when Bitcoin reorganizes back and forth; exercised by `feature_pos_reorg_of_reorg_recovery.py` and `feature_pos_parent_reorg_recovery.py`. |
| [`AUDIT-2026-06.md`](AUDIT-2026-06.md) | Audit record (2026-06), ecosystem-wide. Node-relevant dispositions are folded into `07-security-and-audit.md`. |
| [`escaping-stall-investigation-2026-06.md`](escaping-stall-investigation-2026-06.md) | Historical investigation of single-signer blocks on the pre-re-genesis testnet; led to fixes now in the code. |
| [`fee-asset-mempool-crash.md`](fee-asset-mempool-crash.md) | Incident record; the crash is fixed. |
| [`handoff-2026-06-28.md`](handoff-2026-06-28.md) | Historical session handover (order-book DEX + anchor consensus state as of 2026-06-28). |
| [`committee-seed-grind.py`](committee-seed-grind.py), [`committee-size-dist.py`](committee-size-dist.py), [`committee-sizing-tables.py`](committee-sizing-tables.py) | Analysis scripts backing the committee-sizing and seed-grinding memos. |
| [`bls-share-verify-bench.cpp`](bls-share-verify-bench.cpp) | Micro-benchmark of BLS share verification, backing the committee-size decision. |

**Ecosystem designs (implemented in other repositories)**

| Document | Status |
|---|---|
| [`openamp-design.md`](openamp-design.md) | Implemented in [`openamp`](https://github.com/GracedEternalKingCabbageMan/openamp); the daemon is live on the public testnet with the demo asset BONDX. Zero consensus changes in this repo. |
| [`opendamp-design.md`](opendamp-design.md) | OpenDAMP, network-enforced restricted assets through Simplicity covenants. The covenants and the issuance path are implemented in the `openamp` repository (`opendamp/`); this repository's part is the Simplicity execution-budget ×4 flag day at testnet height 101,810 (24.3.0). |
| [`sbtc-peg-design.md`](sbtc-peg-design.md) | Implemented in [`sbtc-bridge`](https://github.com/GracedEternalKingCabbageMan/sbtc-bridge) as an application-level custody bridge (not the consensus peg). One divergence from this design: the shipped bridge does not burn returned SBTC, it holds it as float and reissues only the shortfall, so circulating (not total issued) supply equals the reserve. |
| [`bridged-usdc-standard.md`](bridged-usdc-standard.md) | The unified bridged-stablecoin standard (one `USDC.e` fed from several chains, precision 6, issued supervised). Implemented in [`compages`](https://github.com/GracedEternalKingCabbageMan/compages). |
| [`ux-audit-spec-2026-07-02.md`](ux-audit-spec-2026-07-02.md) | UX audit and design-change spec across the ecosystem's user-facing surfaces; implementation tracked in the respective repos. |

A design document belongs to the repository whose code it describes, so most of
them are **not** kept here:

| Looking for | It is in |
|---|---|
| The SeqDEX and SeqOB designs — order-book and terminal specs, the covenant-offer design, the rail-crossing P2P/LSP spec, the Lightning feasibility and Simplicity assessments, the instant-swap latency notes | [`seqdex`](https://github.com/GracedEternalKingCabbageMan/seqdex), under `docs/`. Its `test/regtest/` also holds the regtest proofs of the covenant order book, which run against a node built from this repository. |
| The SeqLN designs — the Core Lightning fork spec, asset channels, submarine and pure-Lightning swaps, Tier-2 hosted channels | [`seqln`](https://github.com/GracedEternalKingCabbageMan/seqln), under `doc/seqln-design/`. |
| How Fulmen bundles a SeqLN node | [`fulmen`](https://github.com/GracedEternalKingCabbageMan/fulmen), under `docs/`. |
| The Pignus lending design (the loan covenant, the oracle set, Bitcoin collateral) | [`pignus`](https://github.com/GracedEternalKingCabbageMan/pignus), `docs/pignus-design.md`. The covenant builder and its functional tests (`test/functional/pignus_*.py`, `feature_pignus_*.py`) live here because they run against this node. |
| The Levo launchpad covenant and sale flow | [`levo`](https://github.com/GracedEternalKingCabbageMan/levo), under `doc/`. |
| The seqcj CoinJoin design and threat model | [`seqcj`](https://github.com/GracedEternalKingCabbageMan/seqcj), `docs/DESIGN.md`. |

What stays here is the protocol: anchoring, proof of stake, the fee market, and
the consensus rules those other projects build on.

## 3. Historical correspondence

The `alberto-*`, `reviewer-*` and `letter-to-alberto-*` files (Markdown, HTML,
and PDF) are correspondence records with the consensus reviewer: data notes,
replies, and decision memos in both directions. They are kept as historical
records and are not revised after the fact (beyond removing names that do not
belong in public copy); where a decision in them became code, the numbered
chapters reflect it.
