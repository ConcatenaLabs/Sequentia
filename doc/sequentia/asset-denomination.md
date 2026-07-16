# Asset denomination (precision) — protocol field and ecosystem integration

**Status:** Core node + desktop GUI now honour the per-asset denomination for display
(this branch). Wire format unchanged; **not** a consensus change. This document is the
canonical reference for the field and the integration contract every other component
(web wallet, Ambra, explorer, SeqDEX, bridges, registry/price server) shares.

**Audience:** the Sequentia core team and every wallet/service that displays, parses, or
prices asset amounts.

---

## 1. Why this matters now

On chain, an asset amount is an integer count of **atoms**. How many decimal places a
human sees is set by the asset's **denomination** (a.k.a. precision). SEQ uses 8, like
Bitcoin. The field has existed on chain since Mihailo's "Feature 1: Any asset fees"
(commit `34128e76e`, Sep 2024) but, until this branch, **the Core desktop GUI never read
it** — it formatted every asset with 8 decimals.

The reason nobody noticed: **every asset live on the testnet today is 8dp**, so the wrong
assumption and the right answer coincide. The moment an asset ships with a denomination
other than 8 (Alberto's `tADLT` is 2dp), any component that assumes 8 shows amounts off by
a factor of `10^(8 − d)` — and, worse, *parses user input* off by the same factor, which
moves the wrong number of atoms. The 2026-06 audit already flagged this as latent in
several places (AUDIT-2026-06 MED-4, MED-5). This document exists to close it everywhere,
consistently.

---

## 2. The field

`CAssetIssuance::nDenomination` — `uint8_t`, default `8`, serialized on chain:

```
assetBlindingNonce, assetEntropy, nAmount, nInflationKeys, nDenomination
```

(`src/primitives/confidential.h`). It is set once, at the asset's initial issuance, and is
**not consensus-critical**: no validation rule reads it and the fee/exchange-rate maths run
entirely in atoms. It is a display hint. **Valid range: `0..18`** (an amount is an int64
atom count, and `10^18 < 2^63`).

---

## 3. The shared contract: atoms ↔ units

This is the one piece of maths every component must implement identically.

```
1 unit of an asset with denomination d  =  10^d atoms

units = atoms / 10^d          (for display)
atoms = round(units × 10^d)   (when building a transaction from user input)
```

- SEQ: d = 8 → 1 SEQ = 100 000 000 atoms.
- d = 2 → 5000 atoms displays as `50.00`; the user typing `50` means 5000 atoms.
- d = 0 → whole numbers only; 5000 atoms is `5000` units.

On chain there are only atoms. Denomination changes **only where the decimal point is
drawn** at the human boundary. Balances, coin selection, fees, sums — all stay in atoms and
need no per-asset logic.

---

## 4. Supply ceiling and the escape hatch

`MAX_MONEY` bounds **atoms** (4e16 on the Sequentia mainnet chains = 400,000,000 SEQ).
`accept_unlimited_issuances = false` on every Sequentia chain, so issuance is capped at
that many atoms. Because the cap is in atoms, a lower denomination buys more **units**:

| denomination | max issuable units (4e16 atoms) |
|---|---|
| 8 | 400,000,000.00000000 |
| 4 | 4,000,000,000,000.0000 |
| 2 | 400,000,000,000,000.00 |
| 0 | 40,000,000,000,000,000 |

This is the intended way to issue more than 400M units, and it needs **no consensus
change** — provided every client honours the denomination. See §10 for the separate
question of uncapping atoms.

---

## 5. Source of truth

When a component needs an asset's denomination, resolve it in this order of authority:

1. **On-chain `nDenomination`** — the committed truth. Available to anyone who has the
   issuance transaction (cheap for your own issuances; needs `txindex` or an indexer for
   arbitrary assets). Authoritative; wins on conflict.
2. **Registry `precision`** — the Sequentia Asset Registry index entry is
   `[domain, ticker, name, precision, verified]` (`precision` is field index **3**). Easy
   and always available for verified assets, but it is the registry's word.
3. **Default 8** — when nothing else is known.

For assets issued by this software the chain and the registry agree by construction:
`issueasset` refuses a contract whose `precision` disagrees with `denomination`. Keep that
invariant when the registry is populated from any other path.

**Rule:** never default an unknown asset to precision 0 or 8 *and silently send* — that is
audit finding MED-5 (mis-parse). Discover the on-chain precision, or block with a warning.

---

## 6. The invariant that keeps the ecosystem consistent

> **Core's RPC amounts are, and stay, 1e8-scaled atoms** (`AmountFromValue` /
> `ValueFromAmount`). Do denomination scaling in the display/UI layer, never at the RPC
> boundary.

Every external client assumes 1e8 at the RPC today. If one wallet starts sending
denomination-scaled numbers into Core's RPC, Core will misread them. So:

- **Reading from Core:** RPC gives you atoms/1e8. To show units, divide by `10^d`. RPCs
  that already expose the field (`listissuances`, `decoderawtransaction`) report
  `denomination` next to the amount.
- **Writing to Core:** send amounts 1e8-scaled, as today. Convert the user's
  denomination-based input to atoms in your UI, then express those atoms as `atoms/1e8` for
  the RPC.

This is deliberately the same boundary Core's own GUI uses internally.

---

## 7. Component status and requirements

Legend: **Done** = verified in-tree · **Partial** = mechanism exists, gaps remain ·
**To confirm** = needs the owning team to check.

### 7.1 Core node + desktop GUI — `Sequentia` (this repo) — **Done**

- GUI formats and parses every non-SEQ asset at its denomination
  (`GUIUtil::formatAssetAmount` / `parseAssetAmount`), covering Overview, Send, the Assets
  page, the transaction list, and fee display. SEQ is unchanged (always 8).
- Precision resolution follows §5: `AssetMetadata` carries a precision with an authority
  tag (chain › registry › default). The registry fetcher now reads `precision`; the wallet
  records the on-chain `nDenomination` of assets it issued (so an unverified own asset like
  `tADLT` still displays correctly).
- The Assets page rescales `getbalance` output (which is 1e8-scaled like all RPC) to the
  asset's precision.
- Bug fixed: `issueasset denomination: 0` was silently rewritten to 8, making integer-only
  assets (the top of the ceiling table) unreachable. `0` is now honoured; denomination is
  validated to `0..18`.
- RPC amounts remain 1e8-scaled; `denomination` is reported alongside where already exposed.

Files: `assetsdir.{h,cpp}`, `assetregistry.cpp`, `qt/guiutil.{h,cpp}`, `qt/assetspage.cpp`,
`wallet/wallet.cpp`, `rpc/rawtransaction.cpp`, `wallet/rpc/elements.cpp`,
`primitives/confidential.h`, `validation.cpp`.

### 7.2 SWK web wallet — `sequentia-web-wallet` — **Partial**

Already precision-aware: it parses per-asset with `parseAtoms(value, m.precision)`
(AUDIT-2026-06 HIGH-1). Remaining gaps to close, from the same audit:

- **MED-4:** the wallet ⇄ swap write-back uses a fixed 8dp path → sub-8-precision assets are
  un-sendable (latent today because all live assets are 8dp; fails closed). Round to the
  per-asset precision throughout.
- **MED-5:** an owned-but-unlabelled asset defaults to precision 0 → send mis-parse. Resolve
  the on-chain precision, or block with a warning (per §5).
- Confirm `m.precision` is populated from the registry `precision` field (§5) and defaults
  to 8, not 0, when unknown.

### 7.3 Ambra wallet — `Ambra` (v0.10.x) — **To confirm**

Rust wallet with restricted-asset support (register / balances / receive / send). Confirm
every amount it renders or parses uses the per-asset denomination, and that its send path
converts user input → atoms at the asset's precision (not a fixed 8). Same source-of-truth
rule (§5): registry `precision`, on-chain fallback, default 8.

### 7.4 Block explorer — **To confirm**

- Decode `nDenomination` from each issuance and store it against the asset id. The explorer
  indexes every issuance, so it is the natural **authority for arbitrary assets' precision**
  and the best backend for the registry to source `precision` from.
- Render every amount of an asset as `atoms / 10^d`, not `atoms / 1e8`.
- Expose `denomination` in the asset/issuance API so light clients can resolve it without
  their own index.

### 7.5 SeqDEX — `~/seqdex` (daemon + markets + MM wallets) — **To confirm**

- Order amounts are atoms on chain; display and accept them at each asset's precision.
- A price is a ratio of two assets' **units**, so it depends on both denominations
  (`price = (base_atoms / 10^d_base) / (quote_atoms / 10^d_quote)`). Compute and display
  prices from units, not raw atoms, or cross-precision markets (the 6 cross-chain BTC↔asset
  and 15 same-chain markets) will be mispriced.
- Market-maker wallets must size orders in atoms derived from the correct precision.

### 7.6 Compages bridge (USDX) and other services — **To confirm**

Any service that mints, moves, or reports asset amounts (the bridge's USDX vault config,
reconciliation reports, dashboards) must apply §3 and §6. Bridges are especially sensitive:
a precision mismatch between the two sides is a silent 10^k accounting error.

### 7.7 Registry / price server — **To confirm**

- Keep `precision` in the index equal to the asset's on-chain `nDenomination`. Prefer
  sourcing it from the explorer's decoded issuance.
- Range `0..18`; treat missing/out-of-range as 8. Clients (Core included) already clamp
  this way.

---

## 8. Integration checklist

Every row must hold for a component to be denomination-correct.

| Check | Core GUI | Web wallet | Ambra | Explorer | SeqDEX | Bridge |
|---|---|---|---|---|---|---|
| Displays amounts at per-asset `d` | ✅ | ⚠️ MED-4 | ❓ | ❓ | ❓ | ❓ |
| Parses user input at per-asset `d` | ✅ | ⚠️ MED-5 | ❓ | n/a | ❓ | ❓ |
| Unknown precision → 8 (not 0), or warn | ✅ | ⚠️ MED-5 | ❓ | ❓ | ❓ | ❓ |
| Keeps RPC/wire amounts at 1e8 atoms | ✅ | ✅ | ❓ | ✅ | ❓ | ❓ |
| Prices computed from units, not atoms | n/a | n/a | n/a | n/a | ❓ | n/a |
| `precision` == on-chain `nDenomination` | ✅ | — | — | ❓ | — | ❓ |

✅ done · ⚠️ known gap (audit ref) · ❓ to confirm by the owning team · — n/a

---

## 9. Test asset for cross-client verification

`tADLT` (denomination 2) is the reference non-8 asset. A component is correct when a
`tADLT` balance of 5000 atoms shows as `50.00`, and typing `50` in a send builds a 5000-atom
output. Any component showing `0.00005000` or moving `5000 × 10^6` atoms has the bug.

---

## 10. Separate decision: uncapping issuance (do NOT do silently)

The denomination hatch (§4) needs no consensus change. If instead we ever want issuance
uncapped **in atoms** (beyond 4e16), that is a consensus decision: it means flipping
`accept_unlimited_issuances`, and it requires an overflow review of the fee/sum maths — the
per-transaction explicit-output total is summed across assets and checked against
`MAX_MONEY` in `CheckTransaction` (`consensus/tx_check.cpp`). Raise it explicitly; never
flip it as a side effect of denomination work.

---

## 11. Reference

- **Field:** `CAssetIssuance::nDenomination`, `uint8_t`, default 8, range 0..18.
- **Maths:** `units = atoms / 10^d`; `atoms = round(units × 10^d)`.
- **Authority:** on-chain `nDenomination` › registry `precision` (index field 3) › 8.
- **RPC boundary:** stays 1e8 atoms everywhere; scale in the UI layer.
- **SEQ:** always 8; never changes.
- **Audit context:** AUDIT-2026-06 HIGH-1, MED-4, MED-5.
