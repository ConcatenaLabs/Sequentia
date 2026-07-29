export const meta = {
  name: 'rfq-client-cleanup-r2',
  description: 'Close the incomplete removals in SWK and the UI regression in Ambra left by the RFQ client sweep, without touching the live order-book cross takers',
  phases: [ { title: 'Fix' }, { title: 'Verify' } ],
}

const SWK = '/home/aejkohl/SWK'
const AMBRA = '/home/aejkohl/ambra'

const GUARD = `⛔ ABSOLUTE PROHIBITION, this has nearly caused a catastrophic deletion twice. The name "xchain" is OVERLOADED: it labels BOTH the retired RFQ REST rail AND the LIVE SeqOB order-book cross taker. The following are LIVE and must NOT be deleted, gutted, or "cleaned up" under any circumstances:
- web wallet xswap.js and xrswap.js (order-book cross takers over xcourier.js)
- Ambra app/lib/src/data/xchain_swap_service.dart and xchain_client.dart (courier cross rail; XSeqLeg and XchainMarket still used)
- seqdex daemon/pkg/xchain/ in its entirety
- SWK lwk_wollet/src/btc/xchain.rs "mod asyncr" - in particular asyncr::verify_seq_leg_safe, seq_tip_height, claim_deadline_ok, seq_broadcast, and wallet_async::find_htlc_funding. These are DELIBERATELY-KEPT ORPHANS: verify_seq_leg_safe is the anchor reveal FUND-SAFETY gate. A dead-code sweep must never take them.
What IS retired: the RFQ REST/gRPC rail only (/v1/xchain/markets, /quote, /propose, /swap, /reverse/*).
DECIDE BY REACHABILITY FROM A LIVE ENTRY POINT, NEVER BY NAME MATCH.

HOUSE RULES: no em dashes in comments or docs. "Sequentia" is the network and is never abbreviated as "SEQ"; SEQ is the ticker of the token named "Sequence".`

const IMPL = { type:'object', additionalProperties:false, required:['repo','fixed','builds','build_output','residual'], properties:{
  repo:{type:'string'},
  fixed:{type:'array',items:{type:'object',additionalProperties:false,required:['item','how'],properties:{item:{type:'string'},how:{type:'string'}}}},
  builds:{type:'boolean'}, build_output:{type:'string'}, residual:{type:'string'} } }

const VERD = { type:'object', additionalProperties:false, required:['repo','pass','issues'], properties:{
  repo:{type:'string'}, pass:{type:'boolean'},
  issues:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','problem','fix'],properties:{
    severity:{type:'string',enum:['breaks-order-book','build-break','incomplete-removal','correctness']}, where:{type:'string'}, problem:{type:'string',description:'under 300 chars'}, fix:{type:'string'} }}} } }

const WORK = [
  { name:'SWK', path:SWK,
    items:`1. ${SWK}/lwk_wollet/Cargo.toml:144-150 - the comment still claims the age crate "seals the cross-chain swap state at rest ... so the btc transports pull it explicitly", but seal_state/open_state are gone (grep "age::" in lwk_wollet/src/btc/ is empty). Drop "age" from the btc-async and btc-blocking feature lists and delete the stale rationale comment. The esplora feature keeps its own age dependency (clients/blocking/esplora.rs:1), so nothing else regresses. VERIFY that claim before editing.
2. ${SWK}/lwk_wasm/src/xchain.rs:92 - the doc on xchainSeqClaim still says "Returns the raw Elements tx hex for [\`Self::seq_broadcast\`]", but seq_broadcast was a method on the deleted XchainSwap, so there is no Self and the intra-doc link points at nothing. Reword to name lwk_wollet::btc::xchain::asyncr::seq_broadcast, or simply say the caller broadcasts the returned hex.
3. ${SWK}/SEQUENTIA-DUALCHAIN-PLAN.md - four live references to deleted types survive at lines 21, 42, 202, 232 (age-encrypted XchainSwapState, XchainSwap alongside the existing Wollet, XchainSwapState defines a cipher boundary, serializable XchainSwapState with at-rest), while only 138-139 was updated, so the document contradicts itself. Either update all of them or clearly mark the document as dated design history. Pick one and be consistent.
4. DOCUMENT THE KEPT ORPHANS so a future dead-code sweep cannot take them: add a brief comment at ${SWK}/lwk_wollet/src/btc/xchain.rs mod asyncr stating that verify_seq_leg_safe, seq_tip_height, claim_deadline_ok and seq_broadcast (and wallet_async::find_htlc_funding) are intentionally retained fund-safety surface for the order-book cross rail even though the in-repo wasm caller was removed. Same for read_seq_preimage if it is in the same situation.`,
    build:`cd ${SWK} && cargo check --workspace 2>&1 | tail -30 (report REAL output; if too slow, cargo check the affected crates and say so).` },
  { name:'Ambra', path:AMBRA,
    items:`1. REGRESSION introduced by the sweep: ${AMBRA}/app/lib/src/screens/xchain_swap_screen.dart:271 (and _arm at :79). At XStep.btcLocked the step now renders NO action button and _arm() starts no timer, so _refreshRefundReady() runs only once on entry. The removed _checkButton(_propose) used to re-arm it via _run's finally, so the CLTV refund button now stays "Refund (waiting for timeout)" until the user leaves and reopens the screen. FIX: either add w.add(_checkButton(_refreshRefundReady)) to the btcLocked case in _stepView, or poll _refreshRefundReady on a timer in _arm() whenever r.refundable. Choose whichever matches the file's existing idiom.
2. ${AMBRA}/app/lib/src/data/xchain_client.dart:25 - XchainMarket.fromJson is now unreachable (its only caller was the deleted XchainClient.markets(); the live composer builds XchainMarket directly at swap_screen.dart:517 from SeqObClient.crossMarketAssets()). Delete ONLY the fromJson factory. KEEP the XchainMarket class itself and the _big/_int/_dbl/_str helpers, which XSeqLeg.fromJson still uses.
3. ${AMBRA}/app/lib/src/data/subswap_service.dart:349, :1705, :1762 - three comments still name the deleted XchainReverseSwapService/RSwapStore. Reword them to reference XchainSwapService/SubswapStore. Also the secure-storage key "ambra.xchain.reverse.active" now has no reader; add a one-shot migration that reads it, logs a warning if non-empty, and clears it, so no user is left with an invisible orphaned record. (Note: that record's refund UI was ALREADY unreachable before this sweep, so the migration is the only thing that can surface it.)
4. DO NOT touch ambra_core FFI in this pass: xchain_read_seq_preimage (ambra_core/src/api/mod.rs:1389, binding api.dart:538) now has zero Dart callers, but removing it needs a flutter_rust_bridge regen plus a jniLibs rebuild, which must be its own commit. Leave it and note it in residual.`,
    build:`cd ${AMBRA}/app && flutter analyze 2>&1 | tail -30. Do NOT build an APK. ambra_core is unchanged in this pass so no cargo work is required; say so.` },
]

phase('Fix')
const out = await pipeline(WORK,
  (w) => agent(`${GUARD}\n\nREPO ${w.name} at ${w.path}. A prior RFQ client sweep left these incomplete removals and one regression. Fix each, and NOTHING else:\n\n${w.items}\n\nBUILD: ${w.build}\nReport honestly; do not claim a build you did not run. Return repo='${w.name}'.`,
    { label:`fix:${w.name}`, phase:'Fix', schema: IMPL }),

  (impl, w) => agent(`ADVERSARIAL review of the uncommitted changes in ${w.name} at ${w.path}. ${GUARD}\n\nWhat the fix agent reported:\n${JSON.stringify(impl)}\n\nTry HARD to prove the ORDER-BOOK DEX is broken or degraded: trace every order-book cross entry point and confirm it still resolves. Confirm NONE of the prohibited files were touched. Confirm the deliberately-kept fund-safety orphans still exist and are now documented. Check for dangling references, stale comments that still name deleted types, and any regression in kept UI. Verify the build claim where feasible.\nCite file:line, under 300 chars per issue. Empty array only if genuinely clean. Default pass=false on ANY order-book breakage, build break, or removal of a kept fund-safety symbol. READ-ONLY. Return repo='${w.name}'.`,
    { label:`verify:${w.name}`, phase:'Verify', schema: VERD }),
)

return { out }
