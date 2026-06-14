# Sequentia Project blockchain platform
Sequentia is a Bitcoin sidechain dedicated to asset tokenization and decentralized exchanges.

https://sequentia.io/

Current code is based on Elements Version: 23.3.3

## SequentiaByClaude

This repository is a fork of the existing Sequentia project
(`SequentiaSEQ/SEQ-Core-Elements`), rebased onto Elements 23.3.3, implementing
the differences that make Sequentia a Bitcoin sidechain distinct from
Blockstream's Liquid. All four are implemented and tested as a proof of concept:

1. **Open "no-coin" fee market.** No mandatory native fee asset: any issued
   asset may pay transaction fees. Block producers configure which assets they
   accept and at what value via a static whitelist or a locally-run **price
   server** that auto-admits assets from exchange APIs once they cross
   operator-defined thresholds. Fees across assets are compared in an
   asset-independent *reference fee atom* unit. — see
   [`doc/sequentia/02-open-fee-market.md`](doc/sequentia/02-open-fee-market.md).
2. **Bitcoin anchoring.** Every block commits to a Bitcoin block at a
   monotonically non-decreasing height; the chain reorganizes if and only if
   Bitcoin reorganizes away the referenced block, giving immediate finality
   otherwise and friction-free cross-chain atomic swaps. — see
   [`doc/sequentia/03-bitcoin-anchoring.md`](doc/sequentia/03-bitcoin-anchoring.md).
3. **Proof-of-Stake consensus.** The theoretical paper's design, implemented
   in this repository: stake-weighted **private VRF sortition**, **committee
   certification** (sortitioned, majority quorum — immediate finality;
   optionally **MuSig2-aggregated** to paper-scale 100-member committees),
   **on-chain stake** with CSV-enforced unbonding, and **Bitcoin checkpoints**
   against long-range attacks. **The bundled Sequentia chain runs PoS by
   default**, bootstrapped from a genesis-seeded staking output with no
   `-staker` config (the staker set is entirely on-chain; see
   [`13-launch-and-bootstrap.md`](doc/sequentia/13-launch-and-bootstrap.md)).
   The signed-block "anyone-signs" PoC path is now the custom/regtest dev
   harness (`-con_pos=0`). — see
   [`doc/sequentia/06-proof-of-stake.md`](doc/sequentia/06-proof-of-stake.md)
   and [`07-vrf.md`](doc/sequentia/07-vrf.md).
4. **Bitcoin-identical addresses, opt-in confidential transactions.** The
   default address format is Bitcoin's, so a wallet can present one receiving
   address for both chains; confidential transactions are opt-in with a
   distinct format (Liquid blinds by default). — see
   [`doc/sequentia/08-addresses-and-ct.md`](doc/sequentia/08-addresses-and-ct.md).

The full design specification, the codebase-base decision and its rationale,
the implementation roadmap, and the new RPCs/options are in
[`doc/sequentia/`](doc/sequentia/00-overview-and-base-decision.md) — start with
[`00-overview-and-base-decision.md`](doc/sequentia/00-overview-and-base-decision.md).
To stand up the full system end-to-end (validating node, fee price server,
Bitcoin anchoring, and a single-host or distributed PoS committee), follow the
operator runbook [`09-running-sequentia.md`](doc/sequentia/09-running-sequentia.md)
with the reference config in
[`contrib/sequentia/`](contrib/sequentia/sequentia.conf.example).

### New RPCs and configuration

This fork adds (all gated on the relevant chain features):

- **Open fee market:** `getfeeexchangerates` / `setfeeexchangerates`,
  `setdynamicfeerates` / `getdynamicfeerates` / `cleardynamicfeerates`,
  `getfeeacceptancepolicy`; options `-con_any_asset_fees`,
  `-dynfeeratemaxage`; the price-server sidecar in
  [`contrib/price-server/`](contrib/price-server/).
- **Bitcoin anchoring:** `getanchorstatus`; options `-con_bitcoin_anchor`,
  `-validateanchor`, `-anchorminconf`, `-anchorpollinterval` (reuses the
  `-mainchainrpc*` connection).
- **Proof-of-Stake:** `getstakerinfo`, `getposschedule`, `getstakescript`,
  `generateposblock`, `getposblocktemplate` / `submitposblock` (distributed
  committee block production), `vrfprove` / `vrfverify`, the MuSig2 suite
  (`musigaggregatepubkey`, `musignonce`, `musigpartialsign`, `musigaggregate`,
  `musigverify`), `getcheckpointpayload` /
  `getcheckpointinfo`; options `-con_pos`, `-staker`, `-posslotinterval`,
  `-poscommitteesize`, `-posvrf`, `-posaggcommittee`, `-posunbonding`,
  `-poscheckpointdepth`, `-poscheckpoint` (configured static checkpoints).
- **Addresses/CT:** `-con_default_blinded_addresses` (custom chains);
  `-blindedaddresses` default is now chain-dependent.

### Tests

Sequentia-specific functional tests live in `test/functional/feature_*` (see
`feature_dynamic_fee_rates`, `feature_bitcoin_anchoring`,
`feature_anchor_swap_consistency`, `feature_ct_opt_in`, and the `feature_pos_*`
/ `feature_vrf` suites) and unit tests in `src/test/pos_tests.cpp` and
`src/test/vrf_tests.cpp` / `src/test/musig_tests.cpp`. Build with
`--enable-any-asset-fees` so the fee-unit strings the wallet tests expect
("rfa/vB") match.

## Installing Prerequisistes

### Install build tools
On Ubuntu (and probably Debian), you should be able to install the prerequisite
build tools with the following command:
```bash
sudo apt install ccache build-essential libtool autotools-dev automake pkg-config bsdmainutils python3
```
YMMV on other software distributions.

### Setup ccache
You may achieve speedups when building and rebuilding by using ccache,
that you may install and configure as follows:
```bash
sudo /usr/sbin/update-ccache-symlinks
echo 'export PATH="/usr/lib/ccache:$PATH"' | tee -a ~/.bashrc
source ~/.bashrc
```

## Configure and Build

### Prepare configuration

```bash
./autogen.sh
make -j$(nproc) -C depends NO_QT=1 NO_NATPMP=1 NO_UPNP=1 NO_ZMQ=1 NO_USDT=1
export CONFIG_SITE=$PWD/depends/x86_64-pc-linux-gnu/share/config.site NOWARN_CXXFLAGS='-Wno-deprecated -Wno-unused-result'
```

### Configure
```bash
./configure --enable-any-asset-fees --enable-debug --disable-bench --disable-tests --disable-fuzz-binary
```

Note that the `--enable-any-asset-fees` flag is an addition by Sequentia,
that will configure RPC documentation to denominate fee rates
using RFU and rfa instead of BTC and sat.

### Last But Not Least, Build
```bash
make -j$(nproc)
```

## Modes

The daemon supports several pre-set chains (note: the binary's default chain is
still `liquidv1`, inherited from Elements — pass `-chain=` explicitly):

* **Sequentia network**: `elementsd -chain=test` — the Sequentia chain:
  Proof-of-Stake by default (VRF sortition + aggregate committee, bootstrapped
  from a genesis-seeded staking output — doc 13), Bitcoin-anchored (requires a
  Bitcoin node via the `-mainchainrpc*` options), any-asset fees enabled,
  Bitcoin-identical addresses with opt-in confidential transactions.
* **Custom chains**: any other `-chain=` argument; regtest-like defaults
  (signed-block "anyone-signs" by default; opt into PoS with `-con_pos=1`),
  overridable by a rich set of start-up options. All Sequentia features are
  available here (`-con_any_asset_fees`, `-con_bitcoin_anchor`, `-con_pos`,
  `-posvrf`, `-con_genesis_stake`, `-con_default_blinded_addresses`, …) — this
  is what the functional tests use.
* Bitcoin modes (`-chain=main` / `-chain=regtest`), kept for parent-chain
  interoperability testing, and Liquid modes (`-chain=liquidv1` etc.),
  inherited from Elements.

## Confidential Assets and Transactions

Sequentia inherits Elements' asset issuance and Confidential Transactions
machinery, with one deliberate difference: **confidential transactions are
opt-in, not the default** (see
[`doc/sequentia/08-addresses-and-ct.md`](doc/sequentia/08-addresses-and-ct.md)).
Wallets hand out plain Bitcoin-format addresses by default — amounts and assets
are public, exactly like Bitcoin — and users who want confidentiality request a
confidential address explicitly (`getnewaddress "" "blech32"` or
`-blindedaddresses=1`). Note that confidential outputs cannot carry
proof-of-stake weight (their amounts are hidden).

Background on the inherited technology:

 * [Confidential Assets Whitepaper](https://blockstream.com/bitcoin17-final41.pdf)
 * [Elements Code Tutorial](https://elementsproject.org/elements-code-tutorial/overview)

## Inherited from Elements

Sequentia is built on the Elements platform; compared to Bitcoin itself,
Elements contributes the following (all retained here):
 * [Confidential Assets][asset-issuance]
 * [Confidential Transactions][confidential-transactions]
 * [Federated Two-Way Peg][federated-peg] — the machinery is inherited but,
   unlike Liquid's L-BTC, it plays **no special role in Sequentia**: the
   Sequentia chain is not configured with a parent-chain peg (no peg-in
   validation, no PAK enforcement), pegged BTC is never the fee currency
   (any asset can pay fees), and the network neither favours nor depends on
   any pegged asset. Any user may still use the inherited machinery to issue
   their own pegged BTC if they want one — largely unnecessary here, since
   Bitcoin anchoring enables real-time atomic swaps against *native* BTC
   (see [`doc/sequentia/03`](doc/sequentia/03-bitcoin-anchoring.md)), but
   potentially useful e.g. to hold BTC value under confidential transactions.
 * [Signed Blocks][signed-blocks]
 * [Additional opcodes][opcodes]

Previous elements that have been integrated into Bitcoin:
 * Segregated Witness
 * Relative Lock Time

Elements deferred for additional research and standardization:
 * [Schnorr Signatures][schnorr-signatures]

Additional RPC commands and parameters:
* [RPC Docs](https://elementsproject.org/en/doc/)

The CI (Continuous Integration) systems make sure that every pull request is built for Windows, Linux, and macOS,
and that unit/sanity tests are run automatically.

## License
Elements is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see http://opensource.org/licenses/MIT.

[confidential-transactions]: https://elementsproject.org/features/confidential-transactions
[opcodes]: https://elementsproject.org/features/opcodes
[federated-peg]: https://elementsproject.org/features#federatedpeg
[signed-blocks]: https://elementsproject.org/features#signedblocks
[asset-issuance]: https://elementsproject.org/features/issued-assets
[schnorr-signatures]: https://elementsproject.org/features/schnorr-signatures

## What is the Elements Project?
Elements is an open source, sidechain-capable blockchain platform. It also allows experiments to more rapidly bring technical innovation to the Bitcoin ecosystem.

Learn more on the [Elements Project website](https://elementsproject.org)

https://github.com/ElementsProject/elementsproject.github.io

## Secure Reporting
See [our vulnerability reporting guide](SECURITY.md)
