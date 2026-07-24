# Rail-crossing settlement: P2P-first, LSP-fallback, both directions

Design spec for completing the SeqDEX rail-crossing matrix. Principle: matching is
rail-blind (price/asset/size); settlement picks a mutually-supported rail per the
counterparties' capabilities — **direct peer-to-peer when they line up, the LSP
leg-bridge ONLY on a genuine mismatch** (offline / on-chain-only / passive covenant
maker). This applies symmetrically to both bridge directions.

## The two BTC-leg representations

A BTC<->asset swap has two legs bound by one preimage H. The asset leg is Sequentia
on-chain; the mismatch is always on the BTC leg.

| | BTC leg | on-chain HTLCs | coupled locktimes |
|---|---|---|---|
| **P2P submarine** | a bolt11 on H (pure Lightning) | 1 (asset only) | none — single T_seq gate |
| **LSP leg-bridge** | LSP terminates LN, originates an on-chain BTC HTLC | 2 (BTC + asset) | full W1/W2 coupling |

The LSP's second on-chain HTLC is exactly what forces the intricate coupled locktimes.
When the maker can accept BTC-LN, that HTLC never exists and the coupling disappears —
this is why **P2P is the safer default**, not a compromise.

## PAYER shape — Path A (FIRST-CLASS): direct P2P reverse submarine

Trade: taker BUYS asset, PAYS BTC over Lightning, RECEIVES asset on-chain. Maker rests
`LightningTerms{ln_direction=LnBTCForAsset=1}` advertising `maker_ln_node_pubkey`.
Drivers ALREADY EXIST: `RunMakerReverseSubmarine`/`RunTakerReverseSubmarine`
(seqdex `xdriver_submarine_reverse.go`); courier `XcSubTermsRequest`/`XcSubAssetLocked`/
`XcSubSettled` (`xcourier_submarine.go`).

Secret holder = MAKER (generates P, H=sha256(P)). Safe BECAUSE the BTC leg is a plain
invoice — Lightning forces the maker to reveal P to capture the payment.

1. Maker locks ONE on-chain HTLC (the asset): claim=taker-with-P, refund=maker after
   T_seq; mints a PLAIN bolt11 on H.
2. Maker -> `XcSubAssetLocked{hash_h, maker_refund_pub, seq_locktime, leg, bolt11}`.
3. Taker binds the leg to the signed offer: `VerifySEQLeg` proves the output is
   claim=taker's own SeqClaimKey on H, correct asset/amount/locktime.
4. Taker runs the anchor-depth gate `VerifySeqAnchorBuried >= min_anchor_depth` — never
   pays against a reorg-able asset HTLC.
5. Taker `PayInvoice(bolt11)` -> learns P. `OnPaid` persists P+leg before the claim.
6. Taker `ClaimSEQLeg(P)` before T_seq -> has the asset. Maker already has the BTC-LN.

Fund-safety: paying the maker's invoice is the ONLY way to obtain P, and paying it IS the
maker capturing BTC-LN. So the maker cannot capture BTC-LN without handing the taker the
key to a pre-locked, pre-verified, anchor-buried asset HTLC. Taker can't lose BTC-LN
without the asset (it verifies 3+4 before its single irreversible act); maker can't get
BTC-LN without delivering (getting paid = revealing P = taker claims before T_seq).

Timelocks (single chain, the ENTIRE coupling):
- (P1) anchorDepth(assetHTLC) >= min_anchor_depth   [before pay]
- (P2) seqTip + claimMargin < T_seq                  [before pay, re-checked]
- (P3) pay -> learn P -> claim, all strictly before T_seq
No T_btc, no hold expiry, no min-final-CLTV inequality — the BTC leg never touches chain.

## PAYER shape — Path B (FALLBACK): LSP payer-direction leg-bridge

Used when the matched maker is on-chain-only (FORWARD `CrossChainTerms`) or a passive
`CovenantTerms` fill — cannot accept BTC-LN. LSP terminates the taker's BTC-LN and
originates an on-chain BTC HTLC to the maker. Mirror of the built receiver bridge; reuses
`stepPayerLn` (leg-bridge.mjs:595-620, already unit-tested but never admitted).

Secret holder = TAKER (mints H, holds P). If the LSP minted H it could settle the taker's
hold without giving P to the taker -> taker loss. With the taker holding P, P becomes
public only when the taker claims the asset.

Structure (two on-chain HTLCs + one LN hold, all on the taker's H):
1. Taker mints H (holds P), hands H + its asset-claim pubkey to the LSP.
2. LSP ISSUES a BTC-LN HOLD invoice on H (inverse of /bridge/front). Taker pays -> HELD,
   not captured. Primitive: `lnrpc('holdinvoice', [H, amtMsat, ...])`; state via
   `holdinvoicelookup`; capture via `holdinvoicesettle`.
3. Only after the hold is HELD (stepPayerLn gate), LSP funds the on-chain BTC HTLC to the
   maker via the FORWARD handshake (claim=maker_btc_claim_pub-with-P, refund=LSP after
   T_btc) — `fundOnchain` (lsp-server.mjs:1916).
4. Maker funds the asset HTLC to the taker's claim pubkey on H, refund=maker after T_seq;
   relays `XcSeqLegLocked` which the LSP passes to the taker.
5. Taker verifies + claims the asset with P self-custody -> has the asset.
6. Maker reads P from the asset claim, claims the LSP's BTC HTLC -> maker paid.
7. LSP reads P (from the Sequentia asset-claim witness, primary; or its own BTC HTLC
   spend, backstop) and settles the held LN (`recoupSettle`) -> recoups exactly its front.

Timelocks (coupled, mirror of the receiver W1/W2):
- (B1) now < T_seq < T_btc  (maker needs runway to claim BTC after P reveals at ~T_seq;
  already enforced by the FORWARD driver, xdriver.go:613-618)
- (B2) hold settleable >= T_seq_wall + reorgMargin + settleMargin  (== `requiredTakerHold`,
  leg-bridge.mjs:471-490; sizes hold_expiry AND the incoming-HTLC min-final-CLTV from T_seq)
- (B3) T_btc matures inside the hold's remaining life (stepPayerLn holdBuffer) so a
  no-reveal ends double-no-loss (BTC refunds to LSP, hold expires to taker)

The (B3) hold buffer is SIZED, not guessed: `holdBuffer = refundFinalityConfs + refundConfirmBudget`
(6 + 12 = 18 BTC blocks), DERIVED in leg-bridge.mjs so the confirm budget and the finality depth
can never silently drift out of the window. It is the worst-case wall-clock window a no-reveal
refund gets: an adversarial maker pins T_btc to exactly `holdCLTV - holdBuffer` (the B3 ceiling), so
the slack must span the ENTIRE refund lifecycle — broadcast at T_btc, CONFIRM under RBF
(refundConfirmBudget = 12 blocks), then BURY to finality (refundFinalityConfs = 6). The honest fleet
rests T_btc well below that ceiling (seqdex `BtcLocktimeDelta` 180 vs a ~210-block hold, ~30 blocks
below), clearing both B1 (floor ~150, from the T_seq wall-clock + maker-claim runway) and B3
(ceiling ~192) with headroom. Coupled bump: `BtcLocktimeDelta` was raised 100 -> 180 so the honest
maker's T_btc clears the conservative B1 floor (100 was a wall-clock inversion under fast BTC) and
stays under the B3 ceiling at holdBuffer = 18.

RESIDUAL RISK (known, mitigated — not a logic bug). A FIXED hold window versus real on-chain finality
carries an irreducible residual: under SUSTAINED, multi-hour congestion where even a top-of-mempool,
RBF-bumped refund cannot CONFIRM + BURY inside holdBuffer, the LSP's refund stalls past the hold, the
hold fails back to the taker, and a maker claim can then take the LSP's BTC HTLC — a front loss borne
by the LSP, NEVER by the taker (whose BTC was only ever HELD) or the maker. We MITIGATE, not eliminate:
(a) a generous, reasoned window (refundConfirmBudget is ~4x the ~3-block RBF target, so only 12+ blocks
of top-fee starvation bites); (b) RBF escalation from the first post-refund tick (`refundBumpWithin`,
`sizeRefundFee`, the `refund-bump` action); (c) the LSP's concurrent-exposure caps bound the worst
case. This is the SAME known limitation every HTLC / Lightning system lives with — a fixed CLTV delta
vs. confirmation latency (an LN forwarding node can likewise lose an HTLC if it cannot confirm a
timeout-claim before the incoming CLTV under sustained congestion). Chasing it to ZERO would require an
UNBOUNDED window, itself a liveness / capital-lock failure. So it is sized generously and documented —
not treated as a bug to be closed.

P-source direction differs from the receiver bridge: there the LSP learns P from its own
LN settle (waitsendpay); here the LSP settles TOWARD the taker, so it must read P from a
chain (Sequentia asset claim primary, its BTC HTLC spend backstop). Front-time re-verify
(mirror of verifyFrontRouteExpiry): verify the ACTUAL committed incoming-HTLC CLTV covers
T_seq, never the invoice's requested min_final_cltv. Fail closed if unverifiable.

## Maker handshakes
- P2P payer (A): the bridge-maker.mjs:12-19 "can't fix both pubkey and H" constraint is a
  property of the maker-funded on-chain BTC HTLC (`CrossChainTerms`) ONLY. On the submarine
  path the maker funds NO BTC HTLC (bolt11), so it dissolves. P2P is fund-safe with any
  online maker; drivers already exist. Re-scope the code note to "why the LSP is the
  fallback for an on-chain-only maker," not "why the LSP is required."
- LSP payer (B): needs a NEW `runForwardBridgeTerms`/`openForwardBridgeSession` (mirror of
  `runReverseBridgeTerms`) — LSP funds the BTC HTLC + relays the maker's asset leg. Message
  types exist (`XcTerms`/`XcBtcLegFunded`/`XcSeqLegLocked`); maker side is `RunMakerForward`
  (xdriver.go:665), which mints from the taker's hash and never learns P. Verify-not-trust:
  the LSP verifies the maker's asset leg binds claim=the real taker's pubkey on the taker's
  H before relaying (analog of verifyMakerBtcHtlc).
- LSP vs covenant: LSP fills the covenant on-chain (seqdex `bridge` package) and runs the
  submarine hop with the taker.

## RECEIVER-direction P2P path (symmetry)
The receiver bridge is built LSP-only. Its P2P analog is the NORMAL submarine
(`xdriver_submarine.go`, LnAssetForBTC=0): taker sells asset on-chain, receives BTC-LN,
taker as secret holder. Needs: classify ln_direction=0 submarine asks as P2P-capable;
route to it when the bid-maker advertises btc_ln+interactive; wire the taker driver (mint
bolt11 on P, fund asset HTLC claim=maker, await the maker's pay -> maker claims). Single
on-chain HTLC -> same single T_seq gate. LSP receiver bridge stays as the mismatch fallback.

## Offer format — SettlementCapabilities (additive, signed)
The signed Offer uses `oneof settlement` (one variant per intent). Add a signed capability
descriptor so ONE resting intent advertises its full settlement surface:
```
message SettlementCapabilities {
  bool btc_onchain=1; bool btc_ln=2; bool asset_onchain=3; bool asset_ln=4;
  bool interactive=5;            // online + runs the handshake live (false => passive covenant)
  bool maker_can_hold_invoice=6; // node runs the holdinvoice plugin
}
```
Part of the maker-signed bytes (a relay can't forge it). A CovenantTerms offer IMPLIES
{interactive:false, asset_onchain:true, btc_*:false} regardless of stated caps (chain is
authoritative). Matching stays rail-blind; caps are read only at settlement.
Relay: extend `classifyRelayOffer` (unified-book.mjs:39-50) to recognize ln_direction 0/1
and surface `meta.caps` (generalize the existing `interactive` flag).

## Routing — chooseSettlementPath(match, offerCaps)
New pure function in settlement-router.mjs, consumed by swap.js review + LSP /swap dispatch:
```
plan = planSettlement(match)                       // rail-blind cross detect (unchanged)
if plan.happyCoincidence: return {path:'native'}   // rails coincide, no bridge
for the crossed (BTC) leg, takerBtcRail='ln':
  if offerCaps.interactive && offerCaps.btc_ln:
     return {path:'p2p-submarine', ln_direction: side==='buy' ? 1 : 0}
  else:
     return {path:'lsp-bridge', lnSide: side==='buy' ? 'payer' : 'receiver'}
```
`crossingShapeSupported` (bridge-driver.mjs:276-280) becomes "which shapes the LSP FALLBACK
settles"; extend to admit `btcLeg.bridge && lnSide==='payer' && native asset leg`.

## File-by-file
**seqdex:** offer.proto add SettlementCapabilities + Offer field (regen offer.pb.go);
validator.go validate caps vs ln_direction/trade_dir + covenant=>non-interactive; confirm
xdriver_submarine{,_reverse}.go reachable from the wallet courier; confirm RunMakerForward
accepts an LSP-funded BTC leg.
**LSP (sequentia-web-wallet/tooling/lsp):** unified-book.mjs classify ln_direction 0/1 +
surface caps; settlement-router.mjs add chooseSettlementPath; bridge-driver.mjs
crossingShapeSupported+describeCrossingSupport admit payer; bridge-maker.mjs add
runForwardBridgeTerms + verifyMakerAssetLeg; leg-bridge.mjs add a payer-side front-time gate
(analog of checkBridgeLocktimeOrdering/verifyFrontRouteExpiry verifying ACTUAL incoming-HTLC
CLTV covers T_seq); lsp-server.mjs new POST /bridge/hold (LSP ISSUES a BTC-LN hold on the
taker's H), observe() payer branch points s.lnRpc at the LSP's own node, prepareBridgeLegs
payer branch drives runForwardBridgeTerms, complete refundBridgeHtlcBtc (1981-1987 stub),
/swap route bridge:true BUY into runBridgedSwapJob + narrow the xsubbuy 422 to non-bridge.
**web swap.js:** call chooseSettlementPath; dispatch P2P submarine (both directions) vs LSP
bridge; bridgedTakePlan/startBridged BUY variant + payer bridgedSteps driver; STOP the inline
channel + startMixed for buy-btcln-assetchain; honest-disable Review when a shape REQUIRES
the bridge and the book is empty (never fall through to startMixed/422); disable Review up
front for a sub-asset BUY when subassetCapable is false.
**Ambra:** swap_route.dart stop degrading btcRail=='ln'&&assetRail=='chain' (return the real
mixed/bridge kind for BOTH directions); wire XchainReverseSwapScreen for the cross SELL; add
bridged-buy + bridged-sell screens posting bridge:true; COUPLE the same-chain rail toggles;
BUILD same-chain pure-LN (enable + wire LightningSwapScreen); lsp_client.swap send
node_key/counter_node_key/offer_id/maker_pubkey (self-custody + pin); pre-check /lnbook
liquidity before navigating.
