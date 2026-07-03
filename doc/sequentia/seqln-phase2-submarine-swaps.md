# SeqLN Phase 2 — Submarine swaps (Sequentia asset on-chain ↔ BTC over Lightning)

Phase 1 shipped policy-asset channels (live open/route/mutual-close). Phase 2 is the first **cross-chain**
SeqLN deliverable, and per the fork spec it can ship before asset channels (Phase 3) because it needs no
asset-aware Lightning and no consensus/BOLT change.

Grounding: `seqln-core-lightning-fork-spec.md` §8, `seqdex-lightning-feasibility.md` §5 (Analysis D — the
full submarine-swap safety analysis), and the SeqDEX order-book design (`seqdex-orderbook-design.md`), whose
`CrossChainTerms` (=21) + `pkg/xchain` orchestrator already do the on-chain BTC↔asset version. Phase 2 is
that construction with the **BTC leg moved onto Lightning**.

## 1. Scope (v1 = Case A only)

- **Case A — Sequentia asset ON-CHAIN ↔ BTC over VANILLA Lightning.** The only v1 target. The Sequentia
  leg is an Elements HTLC output (asset-agnostic, already built for CrossChainTerms); the BTC leg is a
  plain BOLT11 payment on Bitcoin, so it works with any upstream LN node and needs no asset-LN.
- **Case B (Sequentia asset over LN ↔ BTC on-chain)** needs asset-aware channels (Phase 3); without them
  it collapses into the on-chain↔on-chain swap the DEX already does. Out of scope for Phase 2.
- Both directions of Case A:
  - **normal swap (on-chain → LN):** taker locks the Sequentia asset HTLC; the LN invoice payee holds the
    secret and reveals it by claiming, which lets the taker's counterparty claim the asset.
  - **reverse swap (LN → on-chain):** uses a **hold invoice** so the on-chain receiver generates and
    retains the preimage `P`. This is the important pattern for a DEX taker buying an asset with BTC-LN.

## 2. Mechanism (one shared SHA256 preimage)

`H = SHA256(P)`. Sequentia HTLC: claim path = `P` + receiver sig (spends immediately); refund path =
`OP_CHECKLOCKTIMEVERIFY` (CLTV) + funder sig. The same `H` gates the LN HTLC (BOLT11 payment_hash). Whoever
claims first reveals `P`; the other leg is then claimable with the same `P`. Both chains share SHA256, so
no new cryptography. PTLCs (scriptless, no hash reuse) are the long-term upgrade — deferred.

**The maker is the Boltz role** (spec §8): a well-capitalized, NON-custodial peer that runs an LN node
(Bitcoin) and is the Sequentia on-chain HTLC counterparty. The relay stays a pure matchmaker (couriers
opaque swap-session messages). No escrow, no accounts; worst case is a timelock refund.

## 3. Load-bearing safety (from feasibility §5 / DEX blocker B4)

1. **Timelock ladder:** the leg claimed SECOND (using the now-public `P`) must have the LONGER timelock.
   For asset→BTC-LN the maker's held (incoming) LN leg carries a longer CLTV than the outgoing leg it pays.
2. **Anchor-depth secret-reveal gate (the Sequentia-specific rule).** A Sequentia tx is final only to its
   **Bitcoin-anchor depth**, not its Sequentia confirmations. When the asset HTLC is claimed on-chain, `P`
   becomes public; if the counterparty acts on it and Bitcoin then reorgs out the claim's anchor, the asset
   claim un-happens while BTC has settled → atomicity broken. **Rule: the secret-revealing on-chain claim
   must reach Bitcoin-anchor depth (N > plausible reorg depth) BEFORE the other leg is settled.** Concretely
   the maker must NOT settle the held BTC-LN hold invoice until the taker's asset-claim tx is anchor-deep.
   `min_conf`/`min_anchor_depth = 1` is UNSAFE for this cross-leg point (contradicts real-time reorg
   following). Reuse the SeqLN §6.1 certified+anchor-buried backend + `getanchorstatus` for the depth check.
3. **Boltz-style 0-conf mitigations on the Sequentia leg:** reject RBF (explicit + inherited), never fund
   RBF-signaling txs, fee floor ≥ ~80% of estimate, per-pair 0-conf amount caps. (0-conf is only ever for
   the on-chain HTLC *funding*, never the cross-leg settlement, which waits for anchor depth.)

## 4. Reuse vs net-new

Reuse (already in SeqDEX): the Elements asset HTLC script + `LockSEQLeg`/`ClaimSEQ`/refund; the `pkg/xchain`
orchestrator and its `VerifySeqLegSafe` anchor-ordering check; the relay's opaque swap-session courier; the
order-book intent/`oneof settlement` plumbing; the same-preimage atomicity model.

Net-new for Phase 2:
- **`SubmarineTerms` settlement variant** in the offer schema (the reserved `=22` "Lightning" slot, sibling
  of `SameChainTerms=20` / `CrossChainTerms=21`): fields ~ `bytes payment_hash`, `bytes maker_refund_pub`,
  `uint32 onchain_cltv` (Sequentia HTLC refund height), `uint32 ln_cltv_delta`, `uint64 min_anchor_depth`
  (default > 1, NOT 1), `uint64 max_0conf_amount`, `Direction direction` (normal/reverse), `string ln_hrp`.
- **The BTC-LN leg driver:** create/pay the BOLT11 (normal) or issue a **hold invoice** and settle/cancel it
  on `P` (reverse). CLN provides hold invoices (the `holdinvoice`/`hodl` mechanism or the plugin); the maker
  runs a SeqLN (or any CLN) node on Bitcoin — this is where **SeqLN "same binary runs real BTC" is used for
  the first time** (`--network=testnet4`).
- **The stitching state machine** binding the LN hold invoice to the Sequentia HTLC by the shared `P`, with
  the anchor-depth gate before settle, and the refund/timeout path on both legs.

## 5. Build order (incremental, each verifiable)

1. **SeqLN on Bitcoin testnet4** — verify the same binary runs `--network=testnet4` against a Bitcoin node
   (dual-chain claim). Foundation for the maker's LN leg. (First step; concrete + testable now.)
2. **Hold invoice on the Bitcoin SeqLN node** — issue a hold invoice on a chosen `H`, confirm it stays
   pending until settle/cancel. (Needs a funded BTC channel with inbound liquidity for the receive role.)
3. **Sequentia asset HTLC round-trip** — lock a testnet asset to `H`+CLTV, claim with `P`, and separately
   exercise the CLTV refund. Reuse SeqDEX `pkg/xchain` primitives; assert on-chain.
4. **`SubmarineTerms` schema + relay courier** — add the =22 variant + opaque submarine-session messages.
5. **Maker stitching + anchor-depth gate** — the state machine; the maker settles the held BTC-LN invoice
   only after the taker's asset-claim reaches `min_anchor_depth` Bitcoin-anchor blocks.
6. **End-to-end + refund** — a non-custodial asset↔BTC-LN swap on testnets, and the refund path exercised.

## 6. Exit criterion

A non-custodial Sequentia-asset ↔ BTC-over-LN swap completed on testnets (both directions of Case A), with
the timeout/refund path exercised, and the anchor-depth secret-reveal gate enforced (the maker provably does
not settle the BTC-LN leg until the asset-claim is anchor-deep).
