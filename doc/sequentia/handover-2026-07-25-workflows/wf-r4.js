export const meta = {
  name: 'courier-partial-r4',
  description: 'Fix the forward-partial fund-loss (min-slice dust guard), unify requoteMixed sub-asset branch onto UBOOK, clamp fallback candidates, floor the reverse receive display; adversarial verify',
  phases: [ { title: 'Build' }, { title: 'Verify' } ],
}
const WEB = '/home/aejkohl/sequentia-web-wallet'
const SEQDEX = '/home/aejkohl/seqdex'
const REF = `Go dust-floor to mirror EXACTLY (${SEQDEX}, phase3-pure-ln): daemon/internal/seqob/client/xminslice.go defines MinSafeBtcLegSats (~546 + 2x the per-leg spend fee) and the asset MinSafe floor; minSafeBtcErr/minSafeAssetErr (xdriver.go:390-397, reverse ~545-591) FAIL CLOSED when a slice's BTC leg or asset claim output would be sub-dust-after-fee. ProportionalBtc ceil / ProportionalBtcFloor floor: xdriver_subasset.go:263,290. Read xminslice.go for the exact constants + fee basis.`
const IMPL = { type:'object', additionalProperties:false, required:['fixed','builds','build_output','fund_safety_notes','residual'], properties:{ fixed:{type:'array',items:{type:'object',additionalProperties:false,required:['what','how'],properties:{what:{type:'string'},how:{type:'string'}}}}, builds:{type:'boolean'}, build_output:{type:'string'}, fund_safety_notes:{type:'string'}, residual:{type:'string'} } }
const VERD = { type:'object', additionalProperties:false, required:['lens','pass','holes'], properties:{ lens:{type:'string'}, pass:{type:'boolean'}, holes:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','scenario','fix'],properties:{ severity:{type:'string',enum:['fund-loss','spec-violation','correctness','ux-leak']}, where:{type:'string'}, scenario:{type:'string',description:'under 400 chars'}, fix:{type:'string'} }}} } }

phase('Build')
const impl = await agent(`Web wallet ${WEB} (uncommitted round-3/courier-partial diff already applied to swap.js/xswap.js/xrswap.js). Fix these four, then rerun tests. ${REF}

1) [FUND-LOSS] FORWARD min-slice dust guard (xswap.js runForwardCourier, AFTER fundBtc is computed ~L510 and BEFORE lockBtcLeg): fail CLOSED (bounceToComposer / session.fail pre-lock, NOTHING spent) when fundBtc is below the safe BTC-leg floor (mirror MinSafeBtcLegSats = 546 + 2x the leg spend fee) OR the takeSeq asset-claim output is below the asset dust floor. Add the same pre-fund guard to xrswap.js driveReverse BEFORE funding the SEQ leg (reverse: the asset leg + the floor BTC receive). Use the SAME constants/fee basis as the Go xminslice.go so JS and Go agree. This is the ONLY thing standing between an honest-maker post-lock 'amount_too_small' reject and an unrefundable sub-dust HTLC.

2) [SPEC] Unify requoteMixed's SUB-ASSET branch (swap.js ~L2498-2521) onto the unified book: match + preview from bridgedTakePlan(route) / UBOOK exactly like the submarine branch (~L2449-2496) — the SAME matched offer + renderMixedTake fill regardless of rail — and gate ONLY Place on sub-asset settleability (keep the existing sub-asset LAST_QUOTE {kind:'mixed'} dispatch + its takeBtcSats/takeAssetAtoms sizing, but source the offer/fill from bp, not subassetOffers()[0]). Fall back to subassetOffers only if UBOOK has no offer for the pair. Result: chain/chain and chain/ln render an identical matched offer + fill for the same market+size.

3) [SPEC] Clamp legacy-fallback candidates to the requested slice (swap.js requoteCross candidate build ~L2719 and/or xswap.js attempts build ~L445): each fallback candidate's take = min(requestedSlice, candidate.base_amount), fundBtc = proportionalBtcCeil(candidate.want, take, candidate.base) — so a fallback fills the requested size, never the whole candidate offer. Kills the overshoot when the primary maker times out.

4) [UX] Reverse confirm modal receive display (xrswap.js onOpen 'You receive' ~L460): show wantBtc (proportionalBtcFloor, what the seller actually receives), not the composer's ceil q.btc_amount. <=1 sat cosmetic today; make Review==execution.

Run: cd ${WEB} && node --check swap.js xswap.js xrswap.js && node --test *.test.mjs tooling/lsp/*.test.mjs (report REAL). Extend xswap-partial.test.mjs: a sub-dust slice is REFUSED pre-lock with 0 broadcasts (both directions); and swap-railblind: chain/ln sub-asset preview == chain/chain preview for the same market+size.`, { label:'build:r4', phase:'Build', schema: IMPL })

phase('Verify')
const LENSES = [
  { key:'dust-guard-fundsafe', focus:'a partial slice whose BTC leg or asset claim output would be sub-dust-after-fee is now REFUSED before any funding in BOTH directions (0 broadcasts), matching the Go minSafeBtc/minSafeAsset fail-closed; no locked-then-unrefundable path remains; whole takes + healthy partials unaffected; every prior gate intact' },
  { key:'railblind-complete', focus:'EVERY BTC<->asset rail combo (chain/chain, ln/chain, chain/ln, ln/ln) renders the SAME matched offer + fill from the ONE unified book for the same market+size — the sub-asset branch no longer reads a separate /book feed; fallback candidates never overshoot the requested slice; the rail only gates Place' },
  { key:'reverse-and-regress', focus:'the reverse (sell) partial Review shows the floor receive it actually pays out (Review==execution), binds slice-vs-slice, cannot be drained below dust; and NOTHING in the round-4 diff regressed the forward fund-safety, the 300+ test suite, or introduced banned vocab / rail advice' },
]
const verdicts = await parallel(LENSES.map(l => () =>
  agent(`ADVERSARIAL review of the uncommitted web-wallet round-4 diff. Try HARD to REFUTE: ${l.focus}. Trace the code, build a concrete failing input, cite file:line. Under 400 chars per hole; empty array if clean. Default pass=false on any fund-loss, per-rail fill divergence, overshoot, or lost gate. READ-ONLY. ${REF}`,
    { label:`verify:${l.key}`, phase:'Verify', schema: VERD }).then(v=>({ ...v, key:l.key })).catch(()=>({ key:l.key, pass:false, holes:[{severity:'correctness',where:'agent-error',scenario:'no return',fix:'re-run'}] }))
))
return { impl, verdicts }
