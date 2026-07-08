# Architecture - the Elements substrate

Sequentia is based on Elements `23.3.3` and tracks it downstream. Its four
defining properties (see [`00-overview.md`](00-overview.md)) are not a rewrite of
the node; each one attaches to a subsystem Elements already provides. The open
fee market rides Elements' multi-asset plumbing; Bitcoin anchoring rides its
Bitcoin-RPC transport; Proof-of-Stake rides its signed-block machinery; and the
Bitcoin-identical address story is an encoding default over the same scripts.

This chapter describes that substrate and the seam each property plugs into. The
full design of each property lives in its own chapter, cross-linked below. Symbol
names are the stable reference; line numbers drift as upstream is merged.

## The inherited stack

Sequentia inherits Elements' core as-is:

- **UTXO model and Bitcoin Script.** Outputs are unspent transaction outputs
  guarded by scripts; script execution is unchanged. The address-format work
  (below) is a re-encoding of the same script types, not a new script semantics.
- **Confidential Assets.** Elements' multi-asset, confidential-amount transaction
  format, enabled by `g_con_elementsmode = true`.
- **Asset issuance.** Anyone may issue an asset; assets are identified by a
  256-bit `CAsset` id (`src/primitives/transaction.h`) and labelled through
  `gAssetsDir` (`src/assetsdir.{h,cpp}`, via `GetAssetFromString()` /
  `GetLabel()`). `policyAsset` is the chain's distinguished asset.

A Sequentia chain is a `CChainParams` subclass in `src/chainparams.cpp` that
flips the relevant global feature flags, for example:

```cpp
g_con_elementsmode        = true;   // Confidential-Assets / Elements tx format
g_con_blockheightinheader = true;   // block height serialized in the header
g_con_any_asset_fees      = true;   // fees payable in any asset
g_signed_blocks           = true;   // header block signature instead of PoW
consensus.has_parent_chain = false; // no federated peg
```

These `extern` feature flags (declared near the top of `src/primitives/block.h`
and in `src/chainparamsbase` / `src/chainparams`) are the idiom Elements uses to
switch consensus-relevant serialization on and off. Anchoring adds one more such
flag (see [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md)).

Because `has_parent_chain` is false, Elements' federated two-way peg is inherited
but plays no role: Sequentia configures no parent-chain peg and depends on no
pegged asset (see [`00-overview.md`](00-overview.md)).

## Multi-asset and fee plumbing - the open fee market substrate

A `CAmount` is an `int64_t` count of atoms *of a specific asset*. Amounts in
different assets are not directly comparable, yet the mempool must order
transactions by economic value when fees may be paid in different assets. The
substrate resolves this with an asset-independent unit.

### The reference fee atom (rfa)

- **`CValue`** (`src/policy/value.h`) is a thin `int64_t` wrapper denominated in
  **reference fee atoms (rfa)**. All cross-asset comparisons - mempool ordering,
  fee-floor checks, fee estimation - happen in rfa.
- **`ExchangeRateMap`** (`src/exchangerates.{h,cpp}`) is a singleton
  `std::map<CAsset, CAssetExchangeRate>`. Each rate is scaled by
  `exchange_rate_scale = COIN (1e8)`; `policyAsset` is seeded at scale `1.0`.
  - `ConvertAmountToValue(amount, asset)` → rfa, computed as
    `amount * rate / scale` (128-bit intermediate, saturating at `INT64_MAX`).
    An asset **absent from the map converts to 0 rfa** - i.e. "not accepted".
  - `ConvertValueToAmount(value, asset)` → atoms of `asset`.
  - The map is persisted as JSON in `<datadir>/exchangerates.json` via
    `LoadFromDefaultJSONFile` / `SaveToJSONFile`.

A transaction's fee asset is obtained via `CTransaction::GetFeeAsset()`.

### Where rfa values flow

| Stage | Location | Behaviour |
|---|---|---|
| Mempool entry | `CTxMemPoolEntry` (`src/txmempool.h`) | Stores `const CAsset nFeeAsset` and `CValue nFeeValue`; ancestor/descendant aggregates `nModFeesWith{Descendants,Ancestors}` are `CValue`. `GetModifiedFee()` returns rfa. |
| Acceptance | `MemPoolAccept` (`src/validation.cpp`) | With `g_con_any_asset_fees`, `feeAsset` is taken from the tx and `m_modified_fees` is computed via `ConvertAmountToValue(...)`. This is the policy gate that lets a non-policy asset pay. |
| Re-valuation | `CTxMemPool::RecomputeFees()` (`src/txmempool.cpp`) | Walks every entry and recomputes `nFeeValue` from the current `ExchangeRateMap`. Invoked when rates change (e.g. from `setfeeexchangerates`). |
| Mining | `addPackageTxs()` (`src/node/miner.cpp`) | Orders by `GetModFeesWithAncestors()` (rfa), so the assembled block is the highest-rfa-value block. The guard `if (!g_con_any_asset_fees && feeAsset != ::policyAsset)` - which restricts Liquid to policy-asset fees - is bypassed for Sequentia. |
| RPC / wallet | `getfeeexchangerates` / `setfeeexchangerates` (`src/rpc/exchangerates.cpp`); wallet fee logic (`src/wallet/spend.cpp`, `feebumper.cpp`) | All value fees through the map. |

This plumbing is the foundation; it makes no policy decisions of its own beyond
"absent asset = 0 rfa = not accepted". The seam a dynamic price server plugs into
is `RecomputeFees()`: a price server updates the `ExchangeRateMap` and calls that
path - exactly as `setfeeexchangerates` does - without touching consensus. The
single exchange-rate whitelist, per-producer acceptance, cross-asset
fee replacement, and paying fees in an arbitrary asset are the full design in
[`02-open-fee-market.md`](02-open-fee-market.md).

## The Bitcoin-node RPC transport - the anchoring substrate

Elements already maintains a trusted connection to a Bitcoin full node, used for
peg-in validation. Anchoring reuses this transport rather than introducing a
second client.

- **`src/mainchainrpc.{h,cpp}`** - an `evhttp`-based JSON-RPC client to
  `bitcoind`. Authentication is by cookie or `-mainchainrpcuser` /
  `-mainchainrpcpassword`; host, port and timeout come from `-mainchainrpchost`,
  `-mainchainrpcport`, `-mainchainrpctimeout`.
- **`MainchainRPCCheck()`** (`src/init.cpp`) - a startup probe of the mainchain
  daemon, wired to `-validatepegin` (default on when the chain
  `has_parent_chain`).
- The existing callers live in `src/pegins.cpp`.

Anchoring layers thin helpers (`getblockhash <height>`,
`getblockheader <hash>`, `getblockchaininfo`) over this same client plus a small
cache, then consults them in header validation and block assembly. The anchor
commitment, the validation and reorg-following rules, and immediate finality are
described in [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md).

## Signed-block machinery - the Proof-of-Stake substrate

Elements replaces Bitcoin's proof-of-work with a **header block signature**.
Sequentia keeps that plumbing untouched and changes only the rule that decides
who may sign a given block, driving it from a stake-weighted election.

- **Enablement.** `g_signed_blocks = true` for the Sequentia chain.
- **The signing rule is a script.** `consensus.signblockscript` is the
  blocksigning challenge. In stock Elements it is a *fixed* federation script
  (e.g. an `OP_CHECKMULTISIG` over functionary keys). Sequentia computes the
  challenge **per block** from its stake-weighted election instead.
- **The signature lives in the header.** Either `CProof` (legacy) or
  `m_signblock_witness` alongside `DynaFedParams` (dynamic federation), in
  `src/primitives/block.h`.

### Header serialization and the hashed region

`CBlockHeader` (`src/primitives/block.h`) has flag-gated serialization. The
non-dynafed path serializes, in order: `nVersion`, `hashPrevBlock`,
`hashMerkleRoot`, `nTime`, then optionally `block_height`
(`g_con_blockheightinheader`), then either `proof` (`g_signed_blocks`) or
`nBits` + `nNonce`. The dynafed path additionally serializes `m_dynafed_params`
and the `m_signblock_witness`.

`SER_GETHASH` excludes the witness/solution from the block hash but **includes**
the committed fields. The block signature is therefore taken over the committed
header. Two consequences follow:

1. There is an established pattern for adding a header field behind a global flag
   and serializing it in **both** the dynafed and non-dynafed branches - the
   pattern anchoring uses for its anchor field.
2. The Bitcoin anchor is placed in the hashed region, so the producer signs over
   it and a block's anchor cannot be altered without invalidating the signature.

### How Proof-of-Stake uses it

Proof-of-Stake changes exactly one thing: the blocksigning challenge is computed
per block from a stake-weighted, anchor-seeded election (and, with `-posvrf`,
private VRF sortition), rather than inherited as a fixed federation script. The
signature itself rides the existing `proof.solution` / `m_signblock_witness`
plumbing unchanged. Committee quorums reach paper-scale 100-member committees via
signature aggregation (BLS12-381 by default, MuSig2 the `-posbls=0` fallback)
carried in the dynafed/signed-block witness. The full
consensus - stake registry, sortition and leader election, quorum certification,
liveness, fork choice, the finality gate and checkpoints - is in
[`04-proof-of-stake.md`](04-proof-of-stake.md).

The fee market and anchoring compose cleanly with this layer: the elected leader
is the block producer, whose `exchangerates.json` and price policy decide which
fee assets it accepts; consensus only checks that fees were paid, not how they
were valued.

## Addresses and confidential transactions

Two address-layer choices distinguish Sequentia from Liquid/Elements:

1. **The default address format is identical to Bitcoin's.** A Sequentia wallet
   app is intended to also be a Bitcoin wallet and, by default, to present *one*
   receiving address valid for both chains, cycling to a fresh address whenever a
   transaction is received on *either* chain to discourage reuse. The cycling is
   wallet-app logic, out of scope for the node; the node's job is to make the
   formats line up.
2. **Confidential transactions are opt-in, not opt-out.** A shared Bitcoin-format
   address cannot carry a blinding key, so Liquid's blinded-by-default model is
   inverted: Sequentia wallets hand out plain Bitcoin-format addresses by default,
   and confidential addresses use a visibly distinct, opt-in format.

### Address formats

| | Sequentia (default, unblinded) | Sequentia (opt-in confidential) |
|---|---|---|
| **Mainnet (future)** | identical to Bitcoin mainnet: base58 `1…`/`3…` (0/5), WIF 128, `xpub`/`xprv`, bech32 `bc1…` | distinct blinded base58 prefix + a Sequentia blech32 HRP |
| **Testnet (the `test` chain)** | identical to Bitcoin testnet: base58 `m`/`n`/`2…` (111/196), WIF 239, `tpub`/`tprv`, bech32 `tb1…` | blinded base58 prefix 70, blech32 `tsqb1…` |
| **Custom / regtest chains** | unchanged Elements defaults (`ert…`), with `-con_default_blinded_addresses=0` to simulate Sequentia behaviour in tests | blech32 `el1…` |

Key properties:

- A confidential address **cannot** be Bitcoin-compatible, because it embeds a
  blinding pubkey - which is exactly why confidential transactions must be opt-in
  for the shared-address story to hold. The confidential format stays
  deliberately distinct so a sender always knows whether an output will be
  blinded.
- The format change is purely an encoding of the same script types. It does not
  affect consensus, the genesis block, or how scripts execute. A Sequentia
  `tb1…` address and a Bitcoin `tb1…` address with the same key are the same
  scriptPubKey on both chains, which is what lets one address serve both.
- The `test` chain's vectors in `src/test/data/key_io_valid.json` for
  `"chain":"test"` are Bitcoin-testnet vectors.

### Opt-in confidential transactions

Elements already provides the machinery; only the *default* changes:

- **`CChainParams::DefaultBlindedAddresses()`** is the chain-level default for the
  existing `-blindedaddresses` option. It is `true` (historical Liquid/Elements
  behaviour) everywhere except Sequentia chains, where it is `false`. Custom
  chains configure it with `-con_default_blinded_addresses` (default 1, so all
  existing Elements tests keep their semantics).
- `getnewaddress` / `getrawchangeaddress` consult `-blindedaddresses` against
  that chain default (`src/wallet/rpc/addresses.cpp`). Outside elements mode,
  blinding is impossible regardless of the flag.
- **Opt-in paths** (standard Elements behaviour, and the only ways to get
  confidential transactions on Sequentia):
  - per-call: `getnewaddress "" "blech32"` force-blinds even when the default is
    unblinded;
  - per-node: `-blindedaddresses=1` restores blind-by-default for a wallet that
    wants it.
- Sending is driven by the destination, as in Elements: paying a confidential
  address produces a blinded output; paying a Bitcoin-format address produces a
  transparent output. Send logic is unchanged.

By default, Sequentia amounts and assets are **public**, exactly like Bitcoin.
Users who want confidentiality use the confidential address format end-to-end;
mixed transactions reveal whatever is unblinded, per standard Elements semantics.

### Wallet-app behaviour (out of node scope)

A conforming Sequentia wallet app derives one keychain usable on both chains (the
formats are identical, so one descriptor serves both), presents a single
receiving address for BTC and SEQ, cycles to the next address when a transaction
arrives on either chain, and treats confidential Sequentia addresses as a
separate, explicit "private receive" flow. This is wallet-app logic; the node
only guarantees the formats line up.

## Validation entry points

Where these properties hook into block and transaction validation:

- **`CheckBlockHeader` / `ContextualCheckBlockHeader`** (`src/validation.cpp`) -
  context-free and context-dependent header checks. The anchoring monotonicity
  rule (`btc_height(X+1) >= btc_height(X)`) and the "referenced Bitcoin block is
  on the bitcoind best chain" rule live here.
- **`MemPoolAccept`** (`src/validation.cpp`) - the any-asset fee gate, valuing
  fees through the `ExchangeRateMap` (see the fee-plumbing table above).
- **`CChainState::ActivateBestChain` / `InvalidateBlock`** - reorg machinery.
  "Reorg Sequentia when Bitcoin reorgs" is implemented by invalidating Sequentia
  blocks whose anchor is no longer on Bitcoin's best chain.
- **`CBlockIndex`** (`src/chain.h`) - the per-block index, extended with the
  cached Bitcoin anchor height/hash for fast monotonicity checks without
  deserialising.

The signed-block signature check (`CheckProof` and the Proof-of-Stake
`CheckChallenge` / `CheckPosStakeRules` split) is covered in
[`04-proof-of-stake.md`](04-proof-of-stake.md); the anchor checks above are
detailed in [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md).
