# Bitcoin-identical addresses & opt-in confidential transactions

A further intended difference between Sequentia and Liquid/Elements, alongside
the open fee market (doc 02) and Bitcoin anchoring (doc 03):

1. **The default address format is the same as Bitcoin's.** Sequentia wallet
   apps are intended to always also be Bitcoin wallets and, by default, to
   present *one* receiving address valid for both chains — cycling to a fresh
   address whenever a transaction is received on *either* chain, to discourage
   address reuse. (The cycling behavior is wallet-app logic, out of scope for
   the node; the node's job is to make the formats line up.)
2. **Confidential transactions are opt-in, not opt-out.** A shared
   Bitcoin-format address cannot carry a blinding key, so the Liquid default of
   handing out blinded addresses is inverted: Sequentia wallets hand out plain
   Bitcoin-format addresses by default, and confidential addresses use a
   visibly distinct, opt-in format.

## 1. Address formats

| | Sequentia (default, unblinded) | Sequentia (opt-in confidential) |
|---|---|---|
| **Mainnet (future)** | identical to Bitcoin mainnet: base58 `1…`/`3…` (0/5), WIF 128, `xpub`/`xprv`, bech32 `bc1…` | distinct blinded base58 prefix + a Sequentia blech32 HRP |
| **Testnet (the current `test` chain)** | identical to Bitcoin testnet: base58 `m`/`n`/`2…` (111/196), WIF 239, `tpub`/`tprv`, bech32 `tb1…` | blinded base58 prefix 70, blech32 `tsqb1…` |
| **Custom/regtest chains** | unchanged Elements defaults (`ert…`), with `-con_default_blinded_addresses=0` to simulate Sequentia behavior in tests | blech32 `el1…` |

Notes:

- CT addresses **cannot** be Bitcoin-compatible (they embed a blinding pubkey),
  which is exactly why CT must be opt-in for the shared-address story to work.
  The confidential format stays deliberately distinct so a sender always knows
  whether an output will be blinded.
- The format change is purely an encoding of the same script types; it does not
  affect consensus, the genesis block, or how scripts execute. A Sequentia
  `tb1…` address and a Bitcoin `tb1…` address with the same key are the same
  scriptPubKey on both chains — which is what lets one address serve both.
- The fork previously used custom prefixes (base58 52/193/249, bech32 `tsq`) on
  the `test` chain; these are replaced. `src/test/data/key_io_valid.json`'s
  `"chain":"test"` vectors are upstream's Bitcoin-testnet vectors again (they
  had been transcoded to the custom prefixes; that transcoding is reverted).

## 2. Opt-in confidential transactions

Elements already has the right machinery; only the *default* changes:

- `CChainParams::DefaultBlindedAddresses()` (new): chain-level default for the
  existing `-blindedaddresses` option. `true` (historical Liquid/Elements
  behavior) everywhere except Sequentia chains, where it is `false`. Custom
  chains configure it with `-con_default_blinded_addresses` (default 1, so all
  existing Elements tests keep their semantics).
- `getnewaddress` / `getrawchangeaddress` consult
  `-blindedaddresses` with that chain default
  (`src/wallet/rpc/addresses.cpp`). Outside elements mode blinding stays
  impossible regardless of the flag.
- **Opt-in paths** (unchanged upstream behavior, now meaningful as the *only*
  ways to get CT on Sequentia):
  - per-call: `getnewaddress "" "blech32"` force-blinds even when the default
    is unblinded;
  - per-node: `-blindedaddresses=1` restores blind-by-default for a wallet that
    wants it.
- Sending is driven by the destination, as in Elements: paying a confidential
  address produces a blinded output; paying a Bitcoin-format address produces a
  transparent output. Nothing changes in send logic.

Implications worth stating plainly: by default Sequentia amounts and assets are
**public**, exactly like Bitcoin. Users who want confidentiality must use the
confidential address format end-to-end, and mixed transactions reveal whatever
is unblinded (standard Elements semantics).

## 3. Wallet-app guidance (out of node scope, recorded for context)

A conforming Sequentia wallet app:

1. derives one keychain usable on both chains (the formats are identical, so
   one descriptor serves both);
2. presents a single receiving address for BTC and SEQ;
3. cycles to the next address when a transaction arrives on **either** chain
   (address reuse across chains is otherwise invisible to single-chain wallet
   logic);
4. treats confidential Sequentia addresses as a separate, explicit "private
   receive" flow.

## 4. Status

- [x] `test` chain address parameters = Bitcoin testnet (base58 111/196, WIF
      239, `tpub`/`tprv`, bech32 `tb`); confidential format distinct
      (blinded 70, blech32 `tsqb`). Upstream key_io vectors restored and green.
- [x] `DefaultBlindedAddresses()` chain default; Sequentia `test` chain =
      unblinded by default; `-con_default_blinded_addresses` for custom chains;
      `-blindedaddresses` help updated.
- [x] Functional test `feature_ct_opt_in.py`.
- [ ] Sequentia mainnet chain params (when defined) mirror Bitcoin mainnet.
</content>
