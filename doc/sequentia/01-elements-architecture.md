# Elements architecture — the parts that matter for Sequentia

This is a focused tour of the Elements/Sequentia subsystems the two priority
features touch. Line numbers refer to the imported Sequentia `master`
(`elements-23.x` series) and will drift as upstream is merged; treat the symbol
names as the stable reference.

## 1. Chain configuration — `src/chainparams.cpp`

A custom Elements chain is defined by a `CChainParams` subclass. The Sequentia
chain sets, among others:

```cpp
g_con_elementsmode      = true;   // Confidential-Assets / elements tx format
g_con_blockheightinheader = true; // block height is serialized in the header
g_con_any_asset_fees    = true;   // CHALLENGE 1: fees payable in any asset
g_signed_blocks         = true;   // CHALLENGE 3 PoC: federated signed blocks
consensus.signblockscript = CScript(0x51 /* OP_TRUE for dev */);
consensus.has_parent_chain = false;
```

Global feature flags (declared `extern` near the top of
`src/primitives/block.h` and in `src/chainparamsbase`/`src/chainparams`) are the
idiom Elements uses to switch consensus-relevant serialization on and off. **We
add one more such flag for anchoring** (see doc 03).

## 2. Assets, amounts and the fee asset

- `CAsset` (`src/primitives/transaction.h`) — a 256-bit asset id. `policyAsset`
  is the chain's distinguished asset (in Liquid, L-BTC). Asset labels ↔ ids are
  resolved by `gAssetsDir` (`src/assetsdir.{h,cpp}`), e.g.
  `GetAssetFromString()` / `GetLabel()`.
- `CAmount` — an `int64_t` count of atoms *of a specific asset*. Amounts in
  different assets are **not** directly comparable, which is the whole problem the
  fee market has to solve.
- A transaction's fee asset is obtained via `CTransaction::GetFeeAsset()`.

## 3. The reference-fee-atom (rfa) abstraction — challenge 1's core idea

Because the mempool must *order* transactions by economic value, and fees may be
in different assets, the existing Sequentia work introduces an asset-independent
unit:

- `CValue` (`src/policy/value.h`) — a thin `int64_t` wrapper denominated in
  **reference fee atoms (rfa)**. All cross-asset comparisons (mempool ordering,
  fee-floor checks, fee estimation) happen in rfa.
- `ExchangeRateMap` (`src/exchangerates.{h,cpp}`) — a singleton
  `std::map<CAsset, CAssetExchangeRate>` where each rate is scaled by
  `exchange_rate_scale = COIN (1e8)`. `policyAsset` is seeded at scale `1.0`.
  - `ConvertAmountToValue(amount, asset)` → rfa: `amount * rate / scale`
    (128-bit intermediate, saturating at `INT64_MAX`). An asset **absent from the
    map converts to 0 rfa** — i.e. "not accepted".
  - `ConvertValueToAmount(value, asset)` → atoms of `asset`.
  - JSON persistence in `<datadir>/exchangerates.json` via
    `LoadFromDefaultJSONFile` / `SaveToJSONFile`.

### Where rfa values flow

- **Mempool entry** (`src/txmempool.h`, `CTxMemPoolEntry`): stores
  `const CAsset nFeeAsset` and `CValue nFeeValue`, plus ancestor/descendant
  aggregates `nModFeesWith{Descendants,Ancestors}` as `CValue`.
  `GetModifiedFee()` returns rfa.
- **Acceptance** (`src/validation.cpp` ~`MemPoolAccept`): when
  `g_con_any_asset_fees` is set, `feeAsset` is taken from the tx and
  `m_modified_fees` is computed through `ConvertAmountToValue(...)`
  (lines ~893–919). This is the policy gate that lets a non-policy asset pay.
- **Re-valuation** (`src/txmempool.cpp` `CTxMemPool::RecomputeFees()`): walks
  every entry and recomputes `nFeeValue` from the current `ExchangeRateMap`.
  Called when rates change (e.g. from `setfeeexchangerates`). **The dynamic price
  server will call exactly this path** after it updates rates.
- **Mining** (`src/node/miner.cpp`): `addPackageTxs()` orders by
  `GetModFeesWithAncestors()` (rfa) so the assembled block is the highest-rfa-value
  block. The guard at line ~204 (`if (!g_con_any_asset_fees && feeAsset != ::policyAsset)`)
  is what restricts Liquid to policy-asset fees and is bypassed for Sequentia.
- **RPC/wallet**: `getfeeexchangerates` / `setfeeexchangerates`
  (`src/rpc/exchangerates.cpp`), and wallet fee logic
  (`src/wallet/spend.cpp`, `feebumper.cpp`) all value fees through the map.

This is the seam the **dynamic price server** plugs into: it does not need to
touch consensus — it only needs to *update the `ExchangeRateMap` and call
`RecomputeFees()`*, exactly as `setfeeexchangerates` already does.

## 4. The block header — challenge 2's insertion point

`CBlockHeader` (`src/primitives/block.h`) has **flag-gated serialization**. The
non-dynafed path serializes, in order: `nVersion`, `hashPrevBlock`,
`hashMerkleRoot`, `nTime`, then optionally `block_height`
(`g_con_blockheightinheader`), then either `proof` (`g_signed_blocks`) or
`nBits`+`nNonce`. The dynafed path additionally serializes `m_dynafed_params`
and the `m_signblock_witness`.

Two consequences for anchoring:

1. There is an established pattern for adding a header field behind a global
   flag and serializing it in **both** the dynafed and non-dynafed branches.
2. `SER_GETHASH` excludes the witness/solution from the block hash but **includes**
   the committed fields. New anchor fields must be in the hashed region so the
   federation signs over them (see doc 03 for the exact placement and the
   block-hash/back-compat implications).

## 5. Bitcoin-node connectivity — challenge 2's transport (already present)

Elements already maintains a trusted connection to a Bitcoin full node for
peg-in validation. We reuse it rather than build a second client.

- `src/mainchainrpc.{h,cpp}` — an `evhttp`-based JSON-RPC client to `bitcoind`.
  Auth via cookie or `-mainchainrpcuser/password`; host/port/timeout via
  `-mainchainrpchost/port/timeout`.
- `src/init.cpp` `MainchainRPCCheck()` — startup probe of the mainchain daemon;
  wired to `-validatepegin` (default on when the chain `has_parent_chain`).
- Callers today live in `src/pegins.cpp`.

For anchoring we add thin helpers (e.g. `getblockhash <height>`,
`getblockheader <hash>`, `getblockchaininfo`) over this same client and a small
cache, then consult them in header validation and block assembly (doc 03).

## 6. Consensus / validation entry points

- `CheckBlockHeader` / `ContextualCheckBlockHeader` (`src/validation.cpp`) —
  context-free and context-dependent header checks. The anchoring monotonicity
  rule (`btc_height(X+1) >= btc_height(X)`) and the "referenced Bitcoin block is
  on the bitcoind best chain" rule live here.
- `CChainState::ActivateBestChain` / `InvalidateBlock` — reorg machinery. The
  "reorg Sequentia when Bitcoin reorgs" behaviour is implemented by invalidating
  Sequentia blocks whose anchor is no longer on Bitcoin's best chain (doc 03).
- `CBlockIndex` (`src/chain.h`) — per-block index; we extend it with the cached
  Bitcoin anchor height/hash for fast monotonicity checks without deserialising.
</content>
