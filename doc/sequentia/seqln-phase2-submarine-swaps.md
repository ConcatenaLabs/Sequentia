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

## 5b. Implementation seam (mapped against the live seqdex code)

The cross-chain maker already exists in `~/seqdex/daemon` (branch `main`; SHARED with another session —
coordinate). Phase 2 plugs into it; do NOT rebuild the SEQ leg or the anchor gate.

- `pkg/xchain/orchestrator.go` — the 5-step swap: (1) Alice locks the BTC leg (longer CLTV), (2) Bob locks
  the SEQ leg (shorter CLTV), (3) **`VerifySeqLegSafe`** = the anchor-depth gate (SEQ leg's block
  `anchorheight >= Hp` AND `getanchorstatus == ok` AND quorum-certified; commit `444d26a` added the
  certification requirement), (4) Alice redeems SEQ with the preimage (reveals it on-chain), (5) Bob reads
  the preimage and redeems the BTC leg. The BTC leg is **pluggable via the `btcBackend` interface**
  (`pkg/xchain/btc_backend.go:26`): `LockBTCLeg` / `VerifyBTCLeg` / `ClaimBTCLeg` / `RefundBTCLeg`
  (`elementsBTCBackend`, and `NewSwapBitcoin` for real testnet4).
- **The catch:** that interface assumes an ON-CHAIN funded HTLC — `LockBTCLeg` returns a funded P2SH +
  block height, `ClaimBTCLeg`/`RefundBTCLeg` return txids. A Lightning leg has none of that: it's a hold
  invoice + an off-chain HTLC inside a channel, settled/cancelled by preimage. So a Lightning backend does
  NOT fit `btcBackend` cleanly.
- **Recommended approach:** generalise the parent leg to a `swapLeg` interface with two implementations —
  the existing on-chain-HTLC leg, and a new `lnLeg` (hold invoice). `lnLeg.Lock` = issue/observe a hold
  invoice on `H` (reverse) or prepare to pay a BOLT11 (normal); `lnLeg.Claim` = settle the hold invoice with
  the preimage; `lnLeg.Refund` = cancel/let it time out. The orchestrator's step order and the SEQ-leg
  anchor gate (`VerifySeqLegSafe`) stay IDENTICAL — the only Sequentia-safety rule is unchanged: the maker
  settles the BTC-LN hold invoice ONLY after Alice's SEQ redeem is anchor-deep (step 3 before step 5's
  LN settle). The `lnLeg` talks to a SeqLN/CLN node on Bitcoin (step 1 of §5, done) via its RPC.
- **Hold invoices:** core CLN has the primitives (`createinvoice`, the `htlc_accepted` hook,
  `preapproveinvoice`) but no turnkey hold command — the `lnLeg`/maker implements the hold via the
  `htlc_accepted` hook (accept-and-hold until preimage known, then resolve), à la Boltz. This is the main
  net-new LN plumbing.
- **Infra to test end-to-end:** a funded BTC(testnet4) channel with inbound liquidity for the receive role
  (hold-invoice payee). Two local SeqLN-on-Bitcoin nodes with a channel between them is the simplest
  self-contained harness once one has testnet4 coins.

## 6. Exit criterion

A non-custodial Sequentia-asset ↔ BTC-over-LN swap completed on testnets (both directions of Case A), with
the timeout/refund path exercised, and the anchor-depth secret-reveal gate enforced (the maker provably does
not settle the BTC-LN leg until the asset-claim is anchor-deep).
