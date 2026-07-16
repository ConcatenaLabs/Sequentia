# Asset denomination (precision) — protocol field and ecosystem integration

**Status:** Core node + desktop GUI now honour the per-asset denomination for display, and
the price server now denominates fee rates correctly (both on this branch). Wire format
unchanged; **not** a consensus change. This document is the canonical reference for the field
and the integration contract every other component (web wallet, Ambra, explorer, SeqDEX,
bridges, registry) shares — §7 records each one's status from a 2026-07 review.

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

## 6a. Fees: the denomination lives in the exchange rate

Any-asset fees are the one place precision touches **consensus economics**, not just display,
so it gets its own rule.

The node values a fee paid in an asset as:

```
value_in_reference_atoms = fee_atoms * rate / 1e8      (exchangerates.cpp)
```

The node is **precision-blind** — it works purely in atoms and has no per-asset denomination.
Therefore the asset's precision must be carried in **`rate`**. Working the accounting through
(reference scale is 1e8), the correct fee rate is:

```
rate = price_per_unit * 1e8 * 10^(8 - d)
```

For `d = 8` this is the familiar `price * 1e8` (a no-op); for any other denomination it
rescales so the fee is valued correctly. Omitting the `10^(8 - d)` factor mis-values a fee
paid in a `d`-decimal asset by `10^(8 - d)` — e.g. **1,000,000× for a 2-decimal asset** — a
silent over/under-payment (the same family as AUDIT-2026-06 HIGH-9).

**The price server owns this.** It already fetches the registry index (which carries
`precision`); it now applies the `10^(8 - d)` factor when converting a price to a rate
(`scaled_rate()` in `contrib/price-server/price_server.py`). Because the node and every
wallet consume the *published rate* directly, fixing it in the price server fixes fee
valuation everywhere at once — wallets must **not** re-apply a precision factor to the rate
(the rate already carries it). A wallet converting a user-typed *fee-rate-per-vByte* from
units to atoms (×`10^d`) is a separate, correct step and does not double-count.

Trade pricing follows the same idea: a price is a ratio of two assets' **units**, so it
depends on both denominations — compute it from units (`atoms / 10^d`), never from raw atoms.

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

*(Statuses below reflect a 2026-07 read of each repo, not just the earlier audit.)*

### 7.2 Price server — `Sequentia/contrib/price-server` — **Done (this branch)**

Was the one economic bug: it published `rate = round(price * 1e8)`, discarding the
registry's `precision`, so fees in any non-8-decimal asset were mis-valued by `10^(8 - d)`
(§6a). Fixed: it now reads `precision` from the index and scales the rate via
`scaled_rate()`; `MAX_RATE` raised to `1e18` to leave headroom for the factor. A no-op for
all currently-listed (8dp) assets. This fixes fee valuation for **every** consumer of the
published rate (node, web wallet, Ambra) at once.

### 7.3 Asset Registry — `sequentia-registry` — **Done (correct)**

Publishes `precision` in the contract and in `index.minimal.json`
(`[domain, ticker, name, precision, verified]`), validated `0..8`. This is the shared
`precision` source for every client. Keep it equal to the on-chain `nDenomination` (they
agree for assets issued via `issueasset`/OpenAMP by construction).

### 7.4 SWK (wallet kit) — `SWK` — **Done (supports it)**

The Rust library under the web wallet, Ambra, and Fulmen exposes a `Precision` type
(`lwk_common::Precision`), so per-asset precision is a first-class parameter its consumers
pass through. Foundational support is present.

### 7.5 SWK web wallet — `sequentia-web-wallet` — **Good**

Precision-aware: `parseAtoms(str, d)` / `fmtAtoms(atoms, d)`, and reference/USD valuation
uses `units = atoms / 10^d`. `precisionKnown()` / `sendPrecision()` **block sending an asset
whose precision is unknown** (closing audit MED-5). Fees use the node's published rate
directly (now correct via 7.2). Remaining: confirm the swap write-back path rounds to
per-asset precision (audit MED-4; latent while all live assets are 8dp).

### 7.6 Ambra wallet — `ambra` — **Good**

Flutter UI over the Rust core; uses `parseAtoms` / `formatAtoms` with `label.precision`
throughout, including balances and send. The `×10^precision` in its fee-rate handling
converts the user's per-vByte fee rate from units to atoms — a correct, separate step, **not**
a re-correction of the published rate (§6a). Fees rely on the published rate (now correct via
7.2).

### 7.7 Explorer + indexer — `sequentia-explorer` / `sequentia-electrs` — **Good**

`electrs` decodes each asset's `precision()` and renders decimal supply as
`supply / 10^precision`. It currently sources precision from registry metadata and defaults
to 0 when absent. Enhancement: decode the on-chain `nDenomination` directly (the indexer sees
every issuance) so the explorer is the authority for arbitrary assets, and expose
`denomination` in the asset API for light clients.

### 7.8 SeqDEX — `seqdex` — **Precision-aware, but configured by hand**

Each market stores `BaseAssetPrecision` / `QuoteAssetPrecision`, so pricing/order sizing is
precision-aware. But these are **set manually per market** (CLI flags, default 8) with no
link to the registry or chain. **Recommendation: source each market asset's precision from
the registry (or on-chain `nDenomination`) at market creation**, leaving the flag as an
override — otherwise a market opened for a non-8 asset with the default 8 misprices silently.

### 7.9 OpenAMP — `openamp` — **Correct on issuance, one trap**

Sets the on-chain `Denomination` from the requested `precision` when issuing a restricted
asset, and serves precision in its records — good. But `if req.Precision == 0 { req.Precision
= 8 }` is the **same "0 means default" trap** fixed in Core: it makes an integer-only (0dp)
restricted asset unissuable. Treat "unset" distinctly from an explicit `0`.

### 7.10 Compages bridge — `compages` — **To confirm**

No explicit precision handling found. Its asset USDX is 8dp, so this is latent. If the bridge
ever mints/moves a non-8 asset, apply §3/§6a — a precision mismatch between the two sides is a
silent `10^k` accounting error.

### 7.11 SeqLN / Fulmen — `seqln` / `fulmen` — **Wire-level handled; display at the wallet**

SeqLN parses the issuance `denomination` byte via its `libwally-core` fork (branch
`sequentia-issuance-denomination`) so transaction ids/signatures are correct. Amount **display**
is the wallet's job (Fulmen) and should follow §3.

---

## 8. Integration checklist

Every row must hold for a component to be denomination-correct. Status as of 2026-07.

| Check | Core GUI | Price srv | Web wallet | Ambra | Explorer | SeqDEX | OpenAMP |
|---|---|---|---|---|---|---|---|
| Displays amounts at per-asset `d` | ✅ | n/a | ✅ | ✅ | ✅ | ✅ | ✅ |
| Parses user input at per-asset `d` | ✅ | n/a | ✅ | ✅ | n/a | ✅ | ✅ |
| Unknown precision → 8 (not 0), or block | ✅ | ✅ | ✅ | ❓ | ⚠️ →0 | ❓ | ⚠️ 0-trap |
| Wire/RPC amounts stay 1e8 atoms | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Fee **rate** carries precision | ✅ | ✅ | ✅¹ | ✅¹ | n/a | n/a | n/a |
| Prices/orders from units, not atoms | n/a | n/a | ✅ | ✅ | ✅ | ✅² | n/a |
| `precision` == on-chain `nDenomination` | ✅ | ✅ | — | — | ⚠️ | ⚠️ manual | ✅ |

✅ correct · ⚠️ gap/latent · ❓ to confirm · — n/a
¹ inherited from the price server's rate (7.2); must not re-apply a factor.
² per-market precision is correct in code but set by hand (7.8).

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
