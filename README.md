# Sequentia

Sequentia is a Bitcoin sidechain for asset tokenization and disintermediated
exchanges, built as a fork of [Blockstream Elements](https://github.com/ElementsProject/elements) 23.3.3.
This repository is the Sequentia node, released as **Sequentia Core**: the
`sequentiad` daemon, `sequentia-cli`, and the `sequentia-qt` desktop GUI,
covering consensus, Bitcoin anchoring, proof of
stake, the open fee market, and the canonical protocol documentation in
[`doc/sequentia/`](doc/sequentia/README.md).

Website: https://sequentia.io/ · Public testnet: https://sequentiatestnet.com
· Development company: Concatena Labs.

**Everything here is testnet software.** There is no Sequentia mainnet; the
`-chain=sequentia` mainnet parameters exist in the code but carry a placeholder
genesis that must be regenerated at a real launch.

## How Sequentia differs from Elements/Liquid

Four defining properties, all implemented and tested in this repository:

1. **Open ("no-coin") fee market.** There is no privileged native fee asset.
   Fees may be proposed in ANY issued asset; block producers choose which
   assets they accept and at what rate (a whitelist kept fresh by hand or by a
   bundled price-server sidecar). The Sequence token (SEQ; tSEQ on testnet)
   has equal standing with every issued asset everywhere except staking.
   See [`doc/sequentia/02-open-fee-market.md`](doc/sequentia/02-open-fee-market.md).
2. **Bitcoin anchoring is supreme.** Every Sequentia block references a
   Bitcoin block header at a non-decreasing height; if Bitcoin reorganizes
   away an anchor, Sequentia reorganizes with it, in real time, no exception.
   Otherwise a committee-certified block is final. This real-time
   reorg-following is what makes cross-chain atomic swaps and Lightning swaps
   safe without extra reorg-protection timelocks.
   See [`doc/sequentia/03-bitcoin-anchoring.md`](doc/sequentia/03-bitcoin-anchoring.md).
3. **Proof of stake.** Stake-weighted private VRF leader sortition plus
   committee certification with BLS12-381 aggregation (MuSig2 legacy,
   `-posbls=0`, custom chains only), on-chain stake with CSV-enforced
   unbonding, and Bitcoin checkpoints against long-range attacks. Stake can
   be delegated to a staking pool, and a pool commits on-chain to how it pays
   delegators (direct, lottery, or proportional split from a claimable pot).
   No inflation: all Sequence tokens are pre-mined (`genesis_subsidy=0`);
   block reward = fees only. Staking
   minimum: 40,000 SEQ. The public testnet runs the **public fixed-size
   committee** (`-pospubliccommittee`, cap 250, quorum 126) with compact
   bitfield certificates.
   See [`doc/sequentia/04-proof-of-stake.md`](doc/sequentia/04-proof-of-stake.md).
4. **Transparent by default, confidentiality opt-in.** This deliberately flips
   the Elements/Liquid default (`m_default_blinded_addresses=false` in
   `src/chainparams.cpp`). Default addresses are unblinded and use Bitcoin's
   own bech32 format (`tb1...` on testnet), so one address can serve a
   dual-chain Bitcoin+Sequentia wallet. Confidential addresses are opt-in and
   use blech32 with HRP `tsqb` (testnet) / `sqb` (mainnet params); the CT
   machinery still exists, it is just not the default.
   See [`doc/sequentia/01-architecture.md`](doc/sequentia/01-architecture.md).

A consequence of (1) and (2): Elements' federated two-way peg is inherited but
plays no role. Sequentia configures no parent-chain peg and depends on no
pegged asset; anchoring-based atomic swaps against native BTC replace the peg's
main use.

Consensus rules added since the fork. Each is active from genesis on every
fresh chain, and height-gated only on the already-running testnet:

- **Simplicity and tapscript introspection.** Full
  [Simplicity](https://blockstream.com/simplicity.pdf) (tapleaf version
  `0xbe`) and Elements' transaction-introspection opcodes (tapleaf `0xc4`),
  activated on the testnet via BIP9 at height 89,856. This is what the
  covenant-based on-chain order book builds on.
- **60-second block spacing as a consensus rule.** A minimum-spacing hard
  fork at height 93,800, with coinbase maturity and the leader time-gate
  rescaled at the same height. Fresh chains enforce it from block 1.
- **Supervised assets.** An issuer may opt an asset into supervision at
  issuance (and only then: an unsupervised asset can never become
  supervised), gaining on-chain freeze/unfreeze of individual outputs.
  Consensus-enforced from height 94,600 on the testnet. See
  [`doc/sequentia/supervised-assets-implementation.md`](doc/sequentia/supervised-assets-implementation.md).
- **Simplicity execution budget ×4.** The per-input Simplicity budget is
  quadrupled from height 101,810, so the covenant order book's
  larger programs fit. See
  [`doc/sequentia/opendamp-design.md`](doc/sequentia/opendamp-design.md).
- **Split pool payouts.** A staking pool can commit to proportional payouts
  from an on-chain pot that any delegator may claim, from height 102,150.
  See
  [`doc/sequentia/split-payouts-design.md`](doc/sequentia/split-payouts-design.md).

## The public testnet

- Genesis `ddd11d54c87a2bd94400fd31ce05d8e1110bb4b78e7103f738342086fc4ea92e`,
  a public BLS committee, parent chain **Bitcoin testnet4**, 60-second blocks.
- Issued testnet assets: GOLD, USDX, EURX, SILVR, OILX (all reissuable),
  `USDC.e` (unified bridged USDC, precision 6, fed by the Compages bridge),
  plus demo assets such as BONDX (OpenAMP).
- Live services (all under https://sequentiatestnet.com):
  - `/` block explorer + `/api` REST API (electrs esplora API)
  - `/wallet/` web wallet
  - `/dex/` the SeqDEX site (Lightning, on-chain, confidential, and
    channel-marketplace trading, driven by the Ambra browser extension)
  - `/bridge/` Compages bridge (Ethereum Sepolia, Bitcoin testnet4 via
    sbtc-bridge, and Solana devnet; one unified `USDC.e`)
  - `/emissio/` Emissio community rewards platform
  - `/openamp/v1/*` OpenAMP restricted-asset REST API
  - `/registry/` the asset registry (the node's default `-assetregistryurl`)
  - `/pools/` the staking pool board
  - `/levo/` Levo launchpad; `/lending/` Pignus lending
  - `/download/` node binaries (Linux tarball, Windows installer), the Ambra
    APK and Chromium extension, Fulmen and Seqognito builds, `pignus-cli`,
    all with signed `SHA256SUMS`
  - `/faucet` testnet faucet (tSEQ + assets)

## Connecting a node to the public testnet

The public testnet is the built-in `test` chain, which is also the binary's
**default chain** (`CBaseChainParams::DEFAULT` in `src/chainparamsbase.cpp`).
On `-chain=test` the node auto-configures the shared gateway with zero config
(`src/init.cpp`): it adds the two known seed peers
(`159.195.15.140:18444`, `13.140.162.77:18444`) as `-addnode` entries in
`AppInitMain`, and `InitParameterInteraction` points the anchor validation RPC
(`-mainchainrpc*`) at a shared Bitcoin testnet4 endpoint and fetches asset
labels and reference prices from the public registry and price feed.

The committee parameters the live chain runs (public fixed-size committee,
cap 250, quorum 126) are baked-in defaults on `chain=test`; a non-default
`-pospubliccommittee` or `-poscommitteesize` is refused at startup rather than
allowed to fork silently. Joining is zero-config:

```bash
sequentiad -daemon
sequentia-cli getblockhash 0      # ddd11d54c87a2bd94400fd31ce05d8e1110bb4b78e7103f738342086fc4ea92e
sequentia-cli getblockchaininfo   # watch it sync
sequentia-cli getanchorstatus     # "ok" once the testnet4 anchor RPC is reachable
sequentia-cli getposschedule      # the live committee and next-slot schedule
```

Prebuilt binaries (a Linux tarball and a Windows installer, published
automatically from each release tag) are on
https://sequentiatestnet.com/download/, or build from source as described
below. Every consensus change above shipped as a release, so an older binary
stops following the chain at the first activation height it does not know.
Always run the newest tag.

To stake and produce blocks, see the operator manual
[`doc/sequentia/05-operating-sequentia.md`](doc/sequentia/05-operating-sequentia.md)
and, for a hand-held Windows walkthrough,
[`doc/sequentia/runbook-windows-node.md`](doc/sequentia/runbook-windows-node.md).

## Building

On Ubuntu/Debian:

```bash
sudo apt install ccache build-essential libtool autotools-dev automake pkg-config bsdmainutils python3
./autogen.sh
make -j$(nproc) -C depends NO_QT=1 NO_NATPMP=1 NO_UPNP=1 NO_ZMQ=1 NO_USDT=1
export CONFIG_SITE=$PWD/depends/x86_64-pc-linux-gnu/share/config.site
./configure --enable-any-asset-fees --disable-bench --disable-fuzz-binary
make -j$(nproc)
```

`--enable-any-asset-fees` is a Sequentia addition: it makes RPC documentation
denominate fee rates in the reference fee unit (RFU/rfa) instead of BTC/sat.
Fee-rate units in Sequentia are always the chosen fee asset's own units per
vByte, never "sat/vB".

The recipe above builds the daemon and CLI only; to also build the
`sequentia-qt` GUI, build `depends` without `NO_QT=1` and drop nothing else.

Full platform build docs are the inherited Elements/Bitcoin ones:
[`doc/build-unix.md`](doc/build-unix.md), [`doc/build-osx.md`](doc/build-osx.md),
[`doc/build-windows.md`](doc/build-windows.md).

## Chains ("modes")

| `-chain=` | What it is |
|---|---|
| `test` (**default**) | The public Sequentia testnet: PoS with the autonomous BLS committee, anchored to Bitcoin testnet4, any-asset fees, Bitcoin-testnet address format, published throwaway founder key. |
| `sequentia` | The future Sequentia mainnet parameters: same consensus, Bitcoin-mainnet address format, distinct network magic. Its genesis founder key is a **placeholder**; the node refuses to start on it without `-allowplaceholdergenesis`. |
| custom (any other name) | Regtest-like config-derived chains, e.g. `elementsregtest`: signed-block "anyone-signs" by default, opt into every Sequentia feature (`-con_pos`, `-con_bitcoin_anchor`, `-con_any_asset_fees`, `-posvrf`, `-pospubliccommittee`, `-con_genesis_stake`, `-con_default_blinded_addresses`, ...). This is what the functional tests use. |
| `main`, `regtest`, `liquidv1`, ... | Inherited Bitcoin-Elements/Liquid chains, kept for the test harness and parent-chain interop. |

## Sequentia RPCs and options

Added by this fork (each gated on the relevant chain feature):

- **Open fee market:** `getfeeexchangerates` / `setfeeexchangerates`
  (with `persist=false` for automated price-server pushes) /
  `getfeeacceptancepolicy`;
  option `-con_any_asset_fees`; the price-server sidecar in
  [`contrib/price-server/`](contrib/price-server/) (this is the canonical
  price-server location; the node holds a single fee-asset whitelist that the
  sidecar keeps fresh).
- **Bitcoin anchoring:** `getanchorstatus`; options `-con_bitcoin_anchor`,
  `-validateanchor`, `-anchorminconf`, `-anchorpollinterval` (reuses the
  `-mainchainrpc*` connection).
- **Proof of stake:** `getstakerinfo`, `getposschedule`, `getposslot`,
  `getposrecentblocks`, `getstakescript`, `getblsregistration`,
  `startposproducer`, `generateposblock`, `getposblocktemplate` /
  `submitposblock` (coordinator-driven block production), the staking wallet
  (`registerstake`, `liststakeutxos`, `withdrawstake`,
  `bumpwithdrawstakefee`), delegation and payout addresses
  (`getdelegationinfo` / `getdelegationscript`, `getpayoutinfo` /
  `getpayoutscript`), `vrfprove` / `vrfverify`, the MuSig2 suite
  (`musigaggregatepubkey`, `musignonce`, `musigpartialsign`,
  `musigaggregate`, `musigverify`),
  `getcheckpointpayload` / `getcheckpointinfo`; options `-con_pos`, `-staker`,
  `-posslotinterval`, `-poscommitteesize`, `-posvrf`, `-posaggcommittee`,
  `-posbls` (BLS aggregate certification, default on the bundled chains),
  `-pospubliccommittee` (public fixed-size committee, run by the public
  testnet), `-posproducer` / `-posproducerkey` (the autonomous producer),
  `-posunbonding`, `-posminstake`, `-poscheckpointdepth`, `-poscheckpoint`.
- **Supervised assets:** `getsupervisedassets` / `getsupervisedassetid`,
  `getassetfreezes`, and the freeze/unfreeze record flow
  (`buildsupervisionrecord`, `getsupervisionrecordhash`,
  `getsupervisionunfreezehash`, `setsupervisionunfreezesig`,
  `addsupervisionrecordoutput`, `submitsupervisionrecord`,
  `getsupervisionsubmissions`, `decodesupervisionscript`); implemented in
  `src/supervision.{h,cpp}`.
- **Addresses/CT:** `-con_default_blinded_addresses` (custom chains);
  `-blindedaddresses` defaults to the chain's setting (off on Sequentia
  chains). Opt in per call with `getnewaddress "" blech32`.
- **Display/registry helpers:** `-assetregistryurl` (advisory asset labels
  from the shared registry), `-referencepricesurl` (per-asset USD prices for
  GUI display only).

## Tests

Unit tests: `src/test/{pos,pos_compact,bls,vrf,musig,exchangerates,supervision,sequentia_chainparams}_tests.cpp`
(run with `make check`).

Sequentia functional tests live in `test/functional/`. Run one with
`test/functional/<name>.py`; run the suite with
`test/functional/test_runner.py`. A tour of the features:

| Test | Shows |
|---|---|
| `feature_any_asset_fee.py`, `feature_any_asset_fee_rates.py`, `feature_any_asset_fee_rbf.py`, `feature_any_asset_fee_scenarios.py`, `feature_any_asset_fee_no_default.py` | fees in arbitrary assets, exchange-rate valuation, cross-asset RBF/CPFP |
| `feature_bitcoin_anchoring.py`, `feature_anchor_swap_consistency.py`, `feature_pos_parent_reorg_recovery.py`, `feature_pos_reorg_of_reorg_recovery.py` | anchor validation, reorg-following, atomic-swap consistency across a Bitcoin reorg, recovery when Bitcoin reorganizes back and forth |
| `feature_pos_stake.py`, `feature_pos_min_stake.py`, `feature_pos_vrf.py`, `feature_pos_vrf_committee.py` | on-chain staking, the 40,000-SEQ floor, VRF sortition |
| `feature_pos_delegation.py`, `feature_pos_payout.py`, `feature_pos_stake_vesting.py`, `feature_pos_withdrawstake.py` | stake delegation, reward payout addresses, staking-only vesting, the unbonding flow |
| `feature_pos_bls_gossip.py`, `feature_pos_public_committee.py`, `feature_pos_distributed_committee.py` | the autonomous BLS gossip committee, the public bitfield committee, the manual MuSig2 flow |
| `feature_pos_finality.py`, `feature_pos_fork_choice.py`, `feature_pos_checkpoints.py`, `feature_pos_escaping_stall.py`, `feature_pos_block_spacing.py` | immediate finality, fork choice, Bitcoin checkpoints, the escaping-stall liveness valve, the 60-second minimum spacing |
| `feature_pos_pools.py` | staking pools: pool creation, delegating into a pool, the committed payout modes |
| `feature_pos_exprace*.py`, `feature_pos_finalized_anchor_reorg.py`, `feature_pos_certified_sibling_guard.py`, `feature_anchor_unreachable_parent.py` | the exponential-race sortition rule, anchor reorgs across a finalized block, the certified-sibling guard, an unreachable parent-chain RPC |
| `feature_supervised_assets.py`, `feature_supervised_zero_supply.py`, `feature_supervised_mempool_load.py`, `feature_supervised_reorg_resurrection.py` | supervised issuance and the end-to-end freeze/unfreeze flow, plus its edge cases (zero-supply issuance, mempool reload, frozen outputs across a reorg) |
| `feature_fee_estimation.py` | per-asset fee estimation |
| `feature_pos_genesis_bootstrap.py` | bootstrapping a chain from a genesis-seeded staking output |
| `feature_ct_opt_in.py` | transparent-by-default addresses with opt-in confidential transactions |

Build with `--enable-any-asset-fees` so the fee-unit strings the wallet tests
expect ("rfa/vB") match.

## Repository map

| Path | Contents |
|---|---|
| [`doc/sequentia/`](doc/sequentia/README.md) | The canonical Sequentia protocol documentation (start at its README index). |
| `src/` | The node. Sequentia-specific code: `src/pos.{h,cpp}`, `src/pos_producer.*` (proof of stake), `src/anchor.{h,cpp}` (Bitcoin anchoring), `src/exchangerates.{h,cpp}`, `src/policy/value.h`, `src/rpc/exchangerates.cpp` (open fee market), `src/supervision.{h,cpp}`, `src/supervision_submit.*` (supervised assets), `src/vrf.{h,cpp}`, `src/musig.{h,cpp}`, `src/blst/` (crypto), `src/assetregistry.*`, `src/referenceprices.*` (display helpers), plus edits in `src/chainparams.cpp`, `src/validation.cpp`, `src/node/miner.cpp`. |
| [`contrib/sequentia/`](contrib/sequentia/) | Reference config, bootstrap tooling, the atomic-swap demo. |
| [`contrib/price-server/`](contrib/price-server/) | The fee price-server sidecar. |
| `test/functional/` | Functional tests (Sequentia ones listed above). |
| `doc/` (everything else) | Inherited Elements/Bitcoin documentation. |

Contributions: PRs against branch `master`
of https://github.com/ConcatenaLabs/Sequentia (the default
branch). See [CONTRIBUTING.md](CONTRIBUTING.md).

## The Sequentia ecosystem

All repos live at https://github.com/ConcatenaLabs/ and are public.

| Repo | One-liner |
|---|---|
| `Sequentia` | The Sequentia node, Sequentia Core (fork of Elements 23.3.3): consensus, anchoring, proof of stake, open fee market, supervised assets, plus the canonical protocol documentation in `doc/sequentia/`. (This repository.) |
| `SWK` | Sequentia Wallet Kit: a fork of Blockstream LWK, with Rust wallet library, CLI, and WASM bindings for building dual-chain Sequentia + Bitcoin testnet4 wallets. |
| `sequentia-extension` | Ambra for Chromium: the non-custodial browser extension wallet on SWK, dual-chain, with Lightning, OpenAMP restricted assets, pool delegation, and the `window.sequentia` provider that the SeqDEX site and Levo connect through. |
| `sequentia-web-wallet` | Proof-of-concept browser wallet built on SWK, live at https://sequentiatestnet.com/wallet. |
| `ambra` | Ambra: non-custodial dual-chain (Bitcoin testnet4 + Sequentia) mobile wallet, a Flutter UI over a Rust core built on SWK. |
| `fulmen` | Fulmen: desktop (Electron) wallet for SeqLN with a bundled Lightning node. |
| `seqln` | SeqLN: a Core Lightning fork that runs on Sequentia and Bitcoin from the same binary, with asset channels, any-asset payments, pure-Lightning swaps. |
| `seqdex` | SeqDEX: non-custodial order-book DEX. P2P signed resting orders, atomic same-chain asset swaps, and cross-chain BTC swaps made safe by Bitcoin anchoring. |
| `seqdex-web` | The standalone SeqDEX website: Lightning, on-chain, and confidential trading plus a channel marketplace, driven by the extension wallet. Live at https://sequentiatestnet.com/dex/. |
| `sequentia-explorer` | Sequentia block explorer frontend (esplora fork); the indexer lives in sequentia-electrs. |
| `sequentia-electrs` | The electrs fork: Rust indexer + Esplora REST API for Sequentia and its Bitcoin testnet4 parent chain. |
| `sequentia-registry` | Sequentia Asset Registry service (asset metadata). |
| `openamp` | OpenAMP: open-source restricted-asset issuance/transfer-approval service (an AMP2 equivalent) with opt-in confidentiality; zero consensus changes. |
| `SeqPal` | SeqPal: tokenization-as-a-service proof of concept, issuing restricted security tokens through OpenAMP. |
| `compages` | Compages: centralized, operator-run bridge proof-of-concept from Ethereum (Sepolia), Bitcoin testnet4 (the public front for sbtc-bridge) and Solana devnet, feeding one unified `USDC.e`. Live at https://sequentiatestnet.com/bridge/. |
| `sequentia-pool-board` | The public board of staking pools (weight, delegators, production reliability, committed payouts). Live at https://sequentiatestnet.com/pools/. |
| `levo` | Levo: a launchpad where staked Sequence sets the allocation ceiling and a covenant holds project tokens from lock to delivery. Live at https://sequentiatestnet.com/levo/. |
| `pignus` | Pignus: non-custodial collateralised lending, loan terms compiled into a taproot covenant. Live at https://sequentiatestnet.com/lending/. |
| `seqcj` | seqcj: CoinJoin coordinator (Chaumian blind-signature credentials over confidential transactions), with a BTC lane through sbtc-bridge. |
| `seqognito` | Seqognito: a desktop mixing wallet on SWK, CoinJoin over confidential transactions, everything over Tor. |
| `sbtc-bridge` | Independent application-level BTC-to-SBTC custody peg: SBTC is pegged 1:1 to BTC held in an N-of-M multisig reserve, an ordinary unprivileged asset. Not Elements' consensus peg. |
| `emissio` | Emissio: community rewards platform, earning Sequence tokens (SEQ) for testnet contributions. |
| `libwally-core` | libwally fork with the Sequentia transaction-parsing patch (issuance denomination byte) used by SeqLN. |

## Inherited from Elements

Sequentia retains Elements' Confidential Assets and Confidential Transactions
machinery (opt-in here), asset issuance, signed blocks, and additional opcodes.
Background: the [Confidential Assets whitepaper](https://blockstream.com/bitcoin17-final41.pdf)
and the [Elements project](https://elementsproject.org). Upstream Elements RPC
documentation: https://elementsproject.org/en/doc/.

## License

Released under the terms of the MIT license. See [COPYING](COPYING) or
http://opensource.org/licenses/MIT.

## Secure reporting

See [our vulnerability reporting guide](SECURITY.md).
