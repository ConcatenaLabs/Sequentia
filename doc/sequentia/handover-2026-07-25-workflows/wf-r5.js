export const meta = {
  name: 'courier-partial-r5',
  description: 'Fix sub-asset show-one-execute-another (settle the SAME offer or block Place), reverse candidate clamp, direction-aware floor/ceil for sell receive, em-dash microcopy; verify',
  phases: [ { title: 'Build' }, { title: 'Verify' } ],
}
const WEB = '/home/aejkohl/sequentia-web-wallet'
const IMPL = { type:'object', additionalProperties:false, required:['fixed','builds','build_output','fund_safety_notes','residual'], properties:{ fixed:{type:'array',items:{type:'object',additionalProperties:false,required:['what','how'],properties:{what:{type:'string'},how:{type:'string'}}}}, builds:{type:'boolean'}, build_output:{type:'string'}, fund_safety_notes:{type:'string'}, residual:{type:'string'} } }
const VERD = { type:'object', additionalProperties:false, required:['lens','pass','holes'], properties:{ lens:{type:'string'}, pass:{type:'boolean'}, holes:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','scenario','fix'],properties:{ severity:{type:'string',enum:['fund-loss','spec-violation','correctness','ux-leak']}, where:{type:'string'}, scenario:{type:'string',description:'under 400 chars'}, fix:{type:'string'} }}} } }

phase('Build')
const impl = await agent(`Web wallet ${WEB} (uncommitted diff already applied). Fix these, then rerun tests.

1) [CRITICAL - shows one price, executes another] swap.js requoteMixed SUB-ASSET branch (~L2503-2536) + startBuy/startSell (~L5357-5370). Round-4 made the DISPLAY render bp.offer (UBOOK best) but the settlement handle is still subassetOffers()[0], so startBuy re-derives the fill at the DIFFERENT sub-asset offer's price — the user sees '50 GOLD for 500000 sats' but receives 25 GOLD. FIX: the settlement MUST lift the SAME offer that is displayed. Find the sub-asset settlement offer whose id === bp.offer.id (subassetOffers merges the same relay UBOOK reads, so the matched offer is there when bp.offer is a sub-asset/LN-leg offer); carry dec.takeBtc/dec.takeAtoms as the authoritative fill into the {kind:'mixed'} LAST_QUOTE and into startBuy/startSell. When bp.offer is NOT a liftable sub-asset offer (e.g. the unified best is an on-chain maker the sub-asset path cannot deliver over LN), STILL show the same fill but DISABLE Place with the shared plain note (payerBridgeDisabledNote()) — never lift a different offer than shown, never give rail advice. Mirror requoteCross's show-fill-then-gate-Place. Add/keep a defense-in-depth refuse in startBuy/startSell if the lifted offer's price disagrees with the displayed dec by more than rounding.

2) [SPEC] Reverse fallback candidate clamp (xrswap.js fetchRQuote candidates ~L355-360 + openReverseFromComposer): clamp each reverse candidate to the requested slice exactly as the forward path does (xswap.js ~L92): take=min(reqSlice,candidate.base), wantBtc=proportionalBtcFloor(candidate.btc,take,candidate.base). Thread reqSeqAtoms through so a fallback fills the requested size, never the whole candidate offer.

3) [CORRECTNESS] sizeSubswapTake direction-aware proportional BTC (subswap.js ~L265,273): the taker PAYS BTC on a BUY (ceil, maker's favour) but RECEIVES BTC on a SELL (floor, maker's favour). Round-4 flipped it to ceil direction-blind, so a partial SELL Review overstates receive by <=1 sat (bridged/submarine sell Review swap.js ~L3343/3364 + live field ~L2389). Make takeBtc AND minBtc use CEIL for buy, FLOOR for sell — matching Go ProportionalBtc (buy) / ProportionalBtcFloor (sell). Keep every consumer's Review==execution.

4) [UX - house rule: NO em dashes] Replace em dashes in the NEW fail-closed strings (xrswap.js minSafeBtcReason/minSafeAssetReason, xswap.js dust-reason strings, subswap.js runLspPayerBridge throws, any new prettyErr copy) with a spaced hyphen ' - ' like the rest of the wallet copy.

Run: cd ${WEB} && node --check swap.js xswap.js xrswap.js subswap.js && node --test *.test.mjs tooling/lsp/*.test.mjs (report REAL). Extend swap-railblind.test.mjs: a chain/ln sub-asset buy LIFTS the SAME offer id it DISPLAYS (or blocks Place when the unified best is on-chain) — assert receive atoms at settlement == displayed dec.takeAtoms. Return the changes.`, { label:'build:r5', phase:'Build', schema: IMPL })

phase('Verify')
const LENSES = [
  { key:'display-eq-settlement', focus:'no rendered BTC<->asset path can DISPLAY one offer/fill and SETTLE a different one: the sub-asset branch lifts exactly the offer id it shows (or blocks Place with the plain note), and the receive atoms at settlement equal the displayed dec.takeAtoms; a fund-relevant show-one-execute-another is impossible' },
  { key:'sizing-and-dust', focus:'proportional BTC is CEIL for a buy (taker pays) and FLOOR for a sell (taker receives) everywhere it feeds a Review or live field (Review==execution, <=0 sat delta), and the pre-lock/pre-fund dust guards from round-4 still fail closed with 0 broadcasts on a sub-dust slice both directions; reverse fallback candidates are clamped to the requested slice (no whole-candidate overshoot)' },
  { key:'regress-and-vocab', focus:'nothing in the round-5 diff regressed the forward/reverse fund-safety or the test suite; no em dashes in any user-facing string; no banned machinery vocab or rail advice on any rendered sink; SBTC risk opt-in only on the on-chain-BTC limit' },
]
const verdicts = await parallel(LENSES.map(l => () =>
  agent(`ADVERSARIAL review of the uncommitted web-wallet round-5 diff. Try HARD to REFUTE: ${l.focus}. Trace the code, build a concrete failing input, cite file:line. Under 400 chars per hole; empty array if clean. Default pass=false on any fund-loss/misexecution, per-rail fill divergence, Review!=execution, overshoot, lost gate, or em dash. READ-ONLY.`,
    { label:`verify:${l.key}`, phase:'Verify', schema: VERD }).then(v=>({ ...v, key:l.key })).catch(()=>({ key:l.key, pass:false, holes:[{severity:'correctness',where:'agent-error',scenario:'no return',fix:'re-run'}] }))
))
return { impl, verdicts }
