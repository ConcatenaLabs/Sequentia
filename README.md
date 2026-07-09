# Sequentia

Sequentia is a Bitcoin sidechain for asset tokenization and decentralized
exchange, built as a fork of [Blockstream Elements](https://github.com/ElementsProject/elements) 23.3.3.
This repository is the Sequentia node (`elementsd`): consensus, Bitcoin
anchoring, proof of stake, the open fee market, and the canonical protocol
documentation in [`doc/sequentia/`](doc/sequentia/README.md).

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
   `-posbls=0`), on-chain stake with CSV-enforced unbonding, and Bitcoin
   checkpoints against long-range attacks. No inflation: all Sequence tokens
   are pre-mined (`genesis_subsidy=0`); block reward = fees only. Staking
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

## Public testnet status

- Re-genesis on **2026-07-05**: genesis
  `ddd11d54c87a2bd94400fd31ce05d8e1110bb4b78e7103f738342086fc4ea92e`, a public
  BLS committee (20 producers at launch), parent chain **Bitcoin testnet4**.
- Issued testnet assets: GOLD, USDX, EURX, SILVR, OILX (all reissuable), plus
  demo assets such as BONDX (OpenAMP).
- Live services (all under https://sequentiatestnet.com):
  - `/` block explorer + `/api` REST API (electrs esplora API)
  - `/wallet` web wallet
  - `/bridge/` Compages Ethereum (Sepolia) bridge
  - `/emissio/` Emissio community rewards platform
  - `/openamp/v1/*` OpenAMP restricted-asset REST API
  - `/download/` binaries, Ambra APK, Fulmen AppImage
  - `/faucet` testnet faucet (tSEQ + assets)

## Connecting a node to the public testnet

The public testnet is the built-in `test` chain, which is also the binary's
**default chain** (`CBaseChainParams::DEFAULT` in `src/chainparamsbase.cpp`).
On `-chain=test` the node auto-configures the shared gateway with zero config
(`InitParameterInteraction` in `src/init.cpp`): it adds
`-addnode=159.195.15.140:18444` as a peer, points the anchor validation RPC
(`-mainchainrpc*`) at a shared Bitcoin testnet4 endpoint, and fetches asset
labels and reference prices from the public registry and price feed.

Two settings are **network-wide consensus rules** on the current chain and are
not yet defaults, so set them explicitly:

```ini
# elements.conf
chain=test

[test]
pospubliccommittee=1     # public fixed-size committee (the 2026-07-05 re-genesis runs this)
poscommitteesize=250     # committee cap 250, quorum 126
```

Then:

```bash
elementsd -daemon
elements-cli getblockhash 0      # ddd11d54c87a2bd94400fd31ce05d8e1110bb4b78e7103f738342086fc4ea92e
elements-cli getblockchaininfo   # watch it sync
elements-cli getanchorstatus     # "ok" once the testnet4 anchor RPC is reachable
elements-cli getposschedule      # the live committee and next-slot schedule
```

Prebuilt binaries for the current chain (rebuilt 2026-07-09 from this branch,
genesis verified) are on https://sequentiatestnet.com/download/, or build from
source as described below.

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

- **Open fee market:** `getfeeexchangerates` / `setfeeexchangerates`,
  `getfeeacceptancepolicy` (plus deprecated sidecar aliases
  `setdynamicfeerates` / `getdynamicfeerates` / `cleardynamicfeerates`);
  option `-con_any_asset_fees`; the price-server sidecar in
  [`contrib/price-server/`](contrib/price-server/) (this is the canonical
  price-server location; the node holds a single fee-asset whitelist that the
  sidecar keeps fresh).
- **Bitcoin anchoring:** `getanchorstatus`; options `-con_bitcoin_anchor`,
  `-validateanchor`, `-anchorminconf`, `-anchorpollinterval` (reuses the
  `-mainchainrpc*` connection).
- **Proof of stake:** `getstakerinfo`, `getposschedule`, `getstakescript`,
  `getblsregistration`, `generateposblock`, `getposblocktemplate` /
  `submitposblock` (coordinator-driven block production), `vrfprove` /
  `vrfverify`, the MuSig2 suite (`musigaggregatepubkey`, `musignonce`,
  `musigpartialsign`, `musigaggregate`, `musigverify`),
  `getcheckpointpayload` / `getcheckpointinfo`; options `-con_pos`, `-staker`,
  `-posslotinterval`, `-poscommitteesize`, `-posvrf`, `-posaggcommittee`,
  `-posbls` (BLS aggregate certification, default on the bundled chains),
  `-pospubliccommittee` (public fixed-size committee, run by the public
  testnet), `-posproducer` / `-posproducerkey` (the autonomous producer),
  `-posunbonding`, `-posminstake`, `-poscheckpointdepth`, `-poscheckpoint`.
- **Addresses/CT:** `-con_default_blinded_addresses` (custom chains);
  `-blindedaddresses` defaults to the chain's setting (off on Sequentia
  chains). Opt in per call with `getnewaddress "" blech32`.
- **Display/registry helpers:** `-assetregistryurl` (advisory asset labels
  from the shared registry), `-referencepricesurl` (per-asset USD prices for
  GUI display only).

## Tests

Unit tests: `src/test/pos_tests.cpp`, `src/test/vrf_tests.cpp`,
`src/test/musig_tests.cpp` (run with `make check`).

Sequentia functional tests live in `test/functional/`. Run one with
`test/functional/<name>.py`; run the suite with
`test/functional/test_runner.py`. A tour of the features:

| Test | Shows |
|---|---|
| `feature_any_asset_fee.py`, `feature_any_asset_fee_rates.py`, `feature_any_asset_fee_rbf.py`, `feature_any_asset_fee_scenarios.py`, `feature_dynamic_fee_rates.py` | fees in arbitrary assets, exchange-rate valuation, cross-asset RBF/CPFP |
| `feature_bitcoin_anchoring.py`, `feature_anchor_swap_consistency.py` | anchor validation, reorg-following, atomic-swap consistency across a Bitcoin reorg |
| `feature_pos_stake.py`, `feature_pos_min_stake.py`, `feature_vrf.py`, `feature_pos_vrf_committee.py` | on-chain staking, the 40,000-SEQ floor, VRF sortition |
| `feature_pos_bls_gossip.py`, `feature_pos_public_committee.py`, `feature_pos_distributed_committee.py` | the autonomous BLS gossip committee, the public bitfield committee, the manual MuSig2 flow |
| `feature_pos_finality.py`, `feature_pos_fork_choice.py`, `feature_pos_checkpoints.py`, `feature_pos_escaping_stall.py` | immediate finality, fork choice, Bitcoin checkpoints, the escaping-stall liveness valve |
| `feature_pos_genesis_bootstrap.py` | bootstrapping a chain from a genesis-seeded staking output |
| `feature_ct_opt_in.py` | transparent-by-default addresses with opt-in confidential transactions |

Build with `--enable-any-asset-fees` so the fee-unit strings the wallet tests
expect ("rfa/vB") match.

## Repository map

| Path | Contents |
|---|---|
| [`doc/sequentia/`](doc/sequentia/README.md) | The canonical Sequentia protocol documentation (start at its README index). |
| `src/` | The node. Sequentia-specific code: `src/pos.{h,cpp}`, `src/pos_producer.*` (proof of stake), `src/anchor.{h,cpp}` (Bitcoin anchoring), `src/exchangerates.{h,cpp}`, `src/policy/value.h`, `src/rpc/exchangerates.cpp` (open fee market), `src/vrf.{h,cpp}`, `src/musig.{h,cpp}`, `src/blst/` (crypto), `src/assetregistry.*`, `src/referenceprices.*` (display helpers), plus edits in `src/chainparams.cpp`, `src/validation.cpp`, `src/node/miner.cpp`. |
| [`contrib/sequentia/`](contrib/sequentia/) | Reference config, bootstrap tooling, the atomic-swap demo. |
| [`contrib/price-server/`](contrib/price-server/) | The fee price-server sidecar. |
| `test/functional/` | Functional tests (Sequentia ones listed above). |
| `doc/` (everything else) | Inherited Elements/Bitcoin documentation. |

Contributions: PRs against branch `claude/sequentia-bitcoin-sidechain-w6xady`
of https://github.com/GracedEternalKingCabbageMan/Sequentia (the default
branch). See [CONTRIBUTING.md](CONTRIBUTING.md).

## The Sequentia ecosystem

All repos live at https://github.com/GracedEternalKingCabbageMan/ and are public.

| Repo | One-liner |
|---|---|
| `Sequentia` | The Sequentia node (`elementsd` fork of Elements 23.3.3): consensus, anchoring, proof of stake, open fee market, plus the canonical protocol documentation in `doc/sequentia/`. (This repository.) |
| `SWK` | Sequentia Wallet Kit: a fork of Blockstream LWK, with Rust wallet library, CLI, and WASM bindings for building Sequentia (and Bitcoin testnet4) wallets. |
| `sequentia-web-wallet` | Proof-of-concept browser wallet built on SWK, live at https://sequentiatestnet.com/wallet. |
| `ambra` | Ambra: non-custodial dual-chain (Bitcoin testnet4 + Sequentia) mobile wallet, a Flutter UI over a Rust core built on SWK. |
| `fulmen` | Fulmen: desktop (Electron) wallet for SeqLN with a bundled Lightning node. |
| `seqln` | SeqLN: a Core Lightning fork that runs on Sequentia and Bitcoin from the same binary, with asset channels, any-asset payments, pure-Lightning swaps. |
| `seqdex` | SeqDEX: non-custodial atomic-swap DEX built on a P2P order book (seqob), same-chain swaps, and cross-chain BTC↔asset swaps made safe by Bitcoin anchoring. |
| `sequentia-explorer` | Sequentia block explorer frontend (esplora fork); the indexer lives in sequentia-electrs. |
| `sequentia-electrs` | The electrs fork: Rust indexer + Esplora REST API for Sequentia and its Bitcoin testnet4 parent chain. |
| `sequentia-registry` | Sequentia Asset Registry service (asset metadata). |
| `openamp` | OpenAMP: open-source restricted-asset issuance/transfer-approval service (an AMP2 equivalent) with opt-in confidentiality; zero consensus changes. |
| `compages` | Compages: centralized Ethereum (Sepolia) ↔ Sequentia bridge proof-of-concept. |
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
