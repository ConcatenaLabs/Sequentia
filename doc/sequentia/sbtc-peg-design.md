# SBTC — an independent multisig BTC bridge for Sequentia, and the DEX silent-peg

Status: **design for build** (all product decisions made with the user 2026-07-19). This is
the canonical design for SBTC and its DEX integration. It is deliberately narrow:
**Sequentia uses NATIVE BTC**; SBTC exists only for the two use-cases below, and it is built
as an **ordinary application-level bridge — NO consensus code.**

## 0. Why SBTC exists (and why it is narrow)

Sequentia's identity is **native Bitcoin, not a Liquid-style pegged BTC**. Native BTC is the
distinct, privileged asset in every wallet (the only asset shown at 0 in a fresh wallet, top
of the send/receive dropdowns) and stays that way.

There is exactly one thing native BTC cannot do: **rest a DEX limit order while the user is
offline.** Bitcoin has no covenants; Elements does. A resting, partial-fillable, offline
limit order needs a covenant, which needs a Sequentia asset. So a BTC limit order is handled
by wrapping the user's real BTC into **SBTC** for the duration of the rest. SBTC is also
exposed publicly as a normal, unprivileged asset (e.g. confidential-tx wrapping).

**A BTC bridge cannot be trustless** — releasing real BTC "iff SBTC was burned" is only
enforceable on Bitcoin with covenants (which don't exist). So SBTC is a **trusted** bridge.
Committee-custody was ruled infeasible (Bitcoin can't verify the committee's BLS certs or
express its 126-of-250 threshold, and custody can't rotate per block). **Decision: a fixed
N-of-M operator multisig**, and — because that custody is a plain multisig, not the PoS
committee — it is built as an **independent bridge, not the consensus peg.**

## 1. No consensus, no native peg, no policy-asset entanglement

We do **not** enable Elements' native two-way peg (dormant, federation-based, and its minted
`pegged_asset` is coupled to the policy/subsidy asset). Instead:

- **SBTC is a normal REISSUABLE Sequentia asset**, issued exactly like GOLD/USDX/…, with its
  **reissuance token held by the bridge's N-of-M multisig**. It is distinct from tSEQ by
  construction (just another asset id), so the policy-asset coupling never arises, and
  **there is zero consensus / chainparams change.**
- The bridge is **no closer to consensus than any third-party bridge** anyone could deploy on
  Sequentia. `has_parent_chain` stays `false`; anchoring is untouched.

## 2. The SBTC bridge — an independent, application-level service

A standard **lock-and-issue** bridge with two custody roles, both held by the same fixed
**N-of-M operator set** (testnet: we run all N; break-and-fix is fine):

- On **Bitcoin (testnet4):** an N-of-M multisig address holding the reserve BTC.
- On **Sequentia:** the **reissuance token** for SBTC, so the operators mint/burn SBTC 1:1
  against the reserve.

Trust model: users trust the N operators to keep the reserve 1:1 and not abscond or
over-issue — the same trust as any custodial bridge, and the trust the user accepted in
choosing a fixed multisig. This is a *reserve*, not fronted market-maker inventory: SBTC is
minted only against a real BTC deposit and burned only when BTC is released.

## 3. Peg-in / peg-out (the bridge service)

- **Peg-in:** the user sends real BTC to the bridge's N-of-M multisig on testnet4. The bridge
  watches the deposit and, after K confirmations, **reissues SBTC 1:1** to the user's
  Sequentia address (operators co-sign the reissuance).
- **Peg-out:** the user sends SBTC to the bridge's Sequentia address; the bridge **burns it**
  and **releases the reserve BTC 1:1** from the multisig to the user's stated Bitcoin address
  (operators co-sign the Bitcoin tx).
- Idempotent + crash-safe (persisted processed-deposits / processed-burns sets); never mints
  unbacked SBTC, never double-spends a reserve UTXO, never releases more than was burned.
  This is the security-critical component; keep it simple and auditable. Both flows are
  exposed publicly (SBTC is a usable asset).

## 4. SBTC as an asset, and the PUBLIC bridge (Compages)

- Registry entry (ticker **SBTC**, subtitle "Pegged Bitcoin"), a normal **unprivileged**
  asset — one row among equals in the wallet. **Native BTC stays the privileged, distinct
  asset.**
- The **public wrap/unwrap BTC↔SBTC interface goes into Compages**, the cross-chain bridge
  product (today Ethereum↔Sequentia; expanding to Solana and other networks — Bitcoin is a
  natural addition), **NOT** a standalone wallet UI. Compages calls the sbtc-bridge
  `/pegin` and `/pegout`. This is for direct/public use (e.g. confidential-tx wrapping),
  separate from the DEX silent path below.

## 5. The DEX silent peg — an OPT-OUT for resting on-chain-BTC LIMIT orders ONLY

SBTC touches the DEX in exactly ONE narrow case, and even then optionally. **Precise scope
(do not overreach):**

- SBTC is used **only** when the user **pays with on-chain BTC AND** the order type is
  **LIMIT** (resting). **Market** orders and **any Lightning** leg **never** use SBTC — they
  are pure **native BTC** (interactive / LN).
- For that one case the composer shows a **"Keep resting while offline"** toggle, **default
  ON**, toggleable off, with the hint: *if on, your order rests as pegged BTC (SBTC) so it
  stays live while you're offline; your funds return as regular BTC if it fills or is
  cancelled.*
  - **ON (default):** the wallet silently **pegs in** the maker's real BTC → SBTC and rests
    the SBTC in a **covenant** (`CovenantTerms.asset_b = SBTC id`) on the `<asset>/BTC` book
    pair — partial-fillable, offline-liftable. On fill the taker's SBTC silently **pegs out**
    → real BTC. Transparent; the user places and receives native BTC.
  - **OFF:** the order does **not** use SBTC. It rests as a native-BTC HTLC (trustless, but
    time-bounded and needing the user online to refund) — the user trades the peg's
    convenience for staying fully native. If even that isn't wanted, the limit order is simply
    not offered as offline-resting.

The covenant / matcher / relay plumbing (partial fills, CrossRail, the settler) already
exists; the wallet-side toggle + peg-in/out calls are the new wiring.

### 5.1 Book routing — the SELECTED asset, NEVER the locked asset

SBTC is also a normal asset, so genuine SBTC pairs exist (SBTC/EURX, BTC/SBTC, …) with their own
books. The silent peg must NOT blur them into the native-BTC books. Two hard invariants:

- **A book is chosen by the asset the user SELECTED, not by what the covenant locks.** A BTC
  on-chain LIMIT order with the toggle ON locks SBTC in its covenant, but the user selected
  **BTC** — so it rests in the **BTC/<quote>** book (e.g. `EURX/BTC`) and must **NEVER** appear
  in the **SBTC/<quote>** book. The `SBTC/<quote>` book is for users who EXPLICITLY chose SBTC;
  mixing a silently-pegged BTC order into it would mis-sell parent-chain BTC liquidity as SBTC,
  and vice-versa. So an order carries its **book `pair`** (`EURX/BTC`) SEPARATELY from its locked
  **`asset_b`** (the SBTC id). Matching, depth, and the pair bar all read `pair`; only the
  covenant plumbing sees the SBTC id.
- **A `peg_out_on_fill` marker rides with the order.** TRUE only for a silently-pegged BTC order:
  on fill the settler pegs the covenant's SBTC back OUT to real BTC so the **TAKER receives
  parent-chain BTC**. A genuine SBTC order (user selected SBTC) has it FALSE — its SBTC transfers
  as SBTC, no peg. Both kinds lock SBTC in the covenant; they differ only in which book they rest
  in and whether the peg reverses on fill.

**BTC/SBTC is a real, if redundant, pair and is ALLOWED.** A BTC on-chain LIMIT maker there pegs
BTC → SBTC to rest (in the `SBTC/BTC` book, `pair` quote = BTC, `peg_out_on_fill` = true), and on
fill pegs SBTC → BTC to the taker while receiving the taker's SBTC — internally SBTC↔SBTC,
user-facing BTC↔SBTC. Do not special-case-block it.

## 6. Build order (bundled; ONE build/verify at the very end)

No consensus code. In order:

1. **SBTC asset + bridge service.** Issue the reissuable SBTC asset (reissuance token → the
   N-of-M multisig); the off-chain bridge service (built: `~/sbtc-bridge`). Register SBTC. (service; registry)
2. **SBTC in the wallets** as a normal unprivileged asset (one row among equals; NO public
   wrap/unwrap UI in the wallet). (wallet)
3. **Public wrap/unwrap → COMPAGES** (the cross-chain bridge product), calling the sbtc-bridge
   `/pegin` + `/pegout`, alongside its Ethereum (and coming Solana/others) bridges. (compages)
4. **DEX composer toggle + silent peg (narrow):** show a default-ON "Keep resting while
   offline" toggle ONLY for on-chain-BTC-pay + LIMIT orders; ON → peg-in on rest / peg-out on
   fill (covenant). Market + LN never touch SBTC. (wallet, relay)
5. **DEX terminal settlement rewrite** (rail-blind covenant book-walking; same-chain already
   works; a BTC LIMIT order with the toggle ON rests via an SBTC covenant, everything else is
   native BTC). Tier A control surface already committed on `terminal-rebuild`. (wallet)
6. **Verify EVERYTHING once** (bridge, wallet builds web + Ambra, Compages, all combos incl.
   peg-in/out + BTC market/LN native + BTC limit persist-on/off + partial fill), then ship.
   Do not rebuild/deploy mid-way.

## 7. Open items to confirm during build
- M and N for the operator multisig (testnet: e.g. 2-of-3 under our control).
- Reissuable vs fixed-supply SBTC (reissuable chosen: unbounded peg-ins, operators trusted
  not to over-issue — consistent with the multisig trust already accepted).
- Which existing bridge/service tooling to reuse (the seqob relay/settler stack is unrelated;
  this is a new BTC↔SBTC custody service, closest in spirit to a wrapped-asset bridge).
