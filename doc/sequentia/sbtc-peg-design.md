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

## 4. SBTC as a wallet asset

- Registry entry (ticker **SBTC**, subtitle "Pegged Bitcoin"), a normal **unprivileged**
  asset — one row among equals. **Native BTC stays the privileged, distinct asset.**
- A public "wrap / unwrap BTC" affordance (peg-in / peg-out) for direct use (confidential txs
  etc.), separate from the silent DEX path below.

## 5. The DEX silent peg — resting on-chain-BTC LIMIT orders

The one place the bridge is **silent** (transparent): a maker rests a BTC limit order
bringing **real** parent-chain BTC AND the taker wants **real** parent-chain BTC.

1. Maker places a BTC limit bid. The wallet silently **pegs in** the maker's real BTC → SBTC
   and rests the SBTC in a **covenant** (`CovenantTerms.asset_b = SBTC id`) on the
   `<asset>/BTC` book pair. The order rests, partial-fillable, offline-liftable.
2. A taker fills (fully or partially). The maker is credited the asset; the taker receives
   the SBTC (the covenant FILL).
3. The taker's SBTC is silently **pegged out** → real BTC to the taker's parent-chain
   address. Neither party need notice SBTC was involved.

A **market** taker paying real BTC settles interactively (online) and needs no bridge. A
maker who wants to hold SBTC directly simply skips the peg-out. The covenant / matcher /
relay plumbing (partial fills, CrossRail, the settler) already exists.

## 6. Build order (bundled; ONE build/verify at the very end)

No consensus code. In order:

1. **SBTC asset + bridge service.** Issue the reissuable SBTC asset (reissuance token → the
   N-of-M multisig); build the off-chain bridge service (watch testnet4 deposits → reissue
   SBTC; watch Sequentia SBTC returns → release BTC). Register SBTC. (new service; registry)
2. **SBTC in the wallets** as a normal unprivileged asset + a public wrap/unwrap affordance. (wallet)
3. **Silent DEX integration** (peg-in on rest, peg-out on fill) into the covenant flow. (wallet, relay)
4. **DEX terminal settlement rewrite** (rail-blind covenant book-walking; BTC via SBTC
   covenants). The Tier A control surface is already committed on `terminal-rebuild`. (wallet)
5. **Verify EVERYTHING once** (bridge, wallet builds web + Ambra, all combos incl. peg-in/out
   + silent resting BTC limit + partial fill), then ship. Do not rebuild/deploy mid-way.

## 7. Open items to confirm during build
- M and N for the operator multisig (testnet: e.g. 2-of-3 under our control).
- Reissuable vs fixed-supply SBTC (reissuable chosen: unbounded peg-ins, operators trusted
  not to over-issue — consistent with the multisig trust already accepted).
- Which existing bridge/service tooling to reuse (the seqob relay/settler stack is unrelated;
  this is a new BTC↔SBTC custody service, closest in spirit to a wrapped-asset bridge).
