export const meta = {
  name: 'courier-partial-and-composer-r3',
  description: 'Port the forward+reverse PARTIAL cross handshake into the web JS courier (xswap.js/xrswap.js) + finish requoteCross full rail-blind unification; adversarial fund-safety verify',
  phases: [ { title: 'Build' }, { title: 'Verify' } ],
}

const WEB = '/home/aejkohl/sequentia-web-wallet'
const SEQDEX = '/home/aejkohl/seqdex'

const GO_REF = `GO PROTOCOL TO MIRROR (${SEQDEX}, branch phase3-pure-ln, partial cross fills landed in commit aebd427; box binaries are partial-capable and 42 GOLD makers rest allow_partial offers). Read these to mirror the wire protocol exactly:
FORWARD (buy asset with BTC on-chain, the taker LIFTS a resting ask):
 - daemon/internal/seqob/client/xdriver.go:349 taker sends an EMPTY XcTermsRequest (no amount).
 - xdriver.go:739-748 MAKER terms response carries the WHOLE offer (BtcAmount=p.BtcAmount, SeqAmount=p.SeqAmount = the whole offer_amount/want_amount). Terms are the offer's advertised RATIO, NOT the slice.
 - xdriver.go:360-367 taker terms guard validates WHOLE-vs-WHOLE (terms.SeqAmount==ExpectSeqAmount, terms.BtcAmount==ExpectBtcAmount = the whole signed offer) — it checks the RATIO, it does NOT abort on a partial.
 - xdriver.go:373-381 taker computes the slice: takeSeq=p.TakeSeqAmount; fundBtc=ProportionalBtc(ExpectBtcAmount, takeSeq, ExpectSeqAmount) (CEIL, maker's favour).
 - xdriver.go:470-471 taker's XcBtcLegFunded CARRIES the slice: SeqAmount=takeSeq, BtcAmount=fundBtc (+ the funded BTC leg).
 - xdriver.go:782-791 MAKER reads funded.SeqAmount (the slice); over-ask reject at :786-789 is takeSeq>p.SeqAmount (strictly >, genuine partials pass); recompute wantBtc=ProportionalBtc(p.BtcAmount, takeSeq, p.SeqAmount) and bind funded.Leg.Amount==wantBtc at :791; then LockSEQLeg(atomsToCoins(takeSeq),...) at :844-845 — locks EXACTLY the slice.
 - xdriver.go:499 taker binds the returned SEQ leg SLICE-vs-SLICE: locked.Leg.Amount==takeSeq.
REVERSE (sell asset for BTC on-chain): xdriver_reverse.go:158-163 taker ships the slice IN the XcTermsRequest (SeqAmount=takeSeq); :527-535 maker reads it, payBtc=ProportionalBtcFloor (floor, maker's favour); :610-611 maker terms (XcBtcLegLocked) carry the PARTIAL (BtcAmount=payBtc, SeqAmount=takeSeq); taker binds slice-vs-slice at :182,:186; maker binds taker asset leg slice-vs-slice at :640; over-ask reject :531.
ProportionalBtc (ceil) / ProportionalBtcFloor (floor): daemon/internal/seqob/client/xdriver_subasset.go:263,290.`

const IMPL_SCHEMA = { type:'object', additionalProperties:false, required:['area','fixed','builds','build_output','fund_safety_notes','residual'], properties:{
  area:{type:'string'}, fixed:{type:'array',items:{type:'object',additionalProperties:false,required:['what','how'],properties:{what:{type:'string'},how:{type:'string'}}}},
  builds:{type:'boolean'}, build_output:{type:'string'}, fund_safety_notes:{type:'string'}, residual:{type:'string'} } }
const VERDICT_SCHEMA = { type:'object', additionalProperties:false, required:['lens','pass','holes'], properties:{
  lens:{type:'string'}, pass:{type:'boolean'}, holes:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','scenario','fix'],properties:{
    severity:{type:'string',enum:['fund-loss','spec-violation','correctness','ux-leak']}, where:{type:'string'}, scenario:{type:'string',description:'under 400 chars'}, fix:{type:'string'} }}} } }

phase('Build')
const builds = await parallel([
  () => agent(`Web wallet ${WEB}. FUND-SAFETY-CRITICAL courier change. The JS courier CANNOT do a PARTIAL forward cross take today: xswap.js runForwardCourier (~L448-495) aborts on \`if (tSeq !== atSeq) throw\` / \`if (tBtc !== atBtc) throw\` (whole terms vs the slice the composer now passes), and its XcBtcLegFunded (~L523-532) omits SeqAmount. Port the forward + reverse PARTIAL handshake to mirror the Go client EXACTLY.

${GO_REF}

IMPLEMENT in xswap.js (FORWARD, runForwardCourier) — keep WHOLE takes byte-identical (atSeq==offer whole => takeSeq==whole, fundBtc==whole want, no behaviour change):
 1. The maker's Terms carry the WHOLE offer. Validate the OFFER (not the slice): tSeq==BigInt(offer.base_amount) AND tBtc==BigInt(offer.want_amount) (the maker quoted its advertised offer); keep the existing maker-keys-present, locktime-ordering (bl>sl), and fee-sanity guards. REMOVE the slice-equality abort.
 2. Slice: takeSeq=atSeq (the composer-requested slice, q.seq_amount); REJECT if takeSeq>tSeq (over-ask) or takeSeq<=0; fundBtc = ceil(tBtc*takeSeq/tSeq) (CEIL — maker never underpaid; add a ceilDiv helper if none exists). Set SWAP.seq_amount=takeSeq, SWAP.btc_amount=fundBtc; the confirmLockModal + lock use fundBtc (the slice's BTC), never tBtc.
 3. Lock the BTC leg for fundBtc.
 4. XcBtcLegFunded MUST include seq_amount:String(takeSeq), btc_amount:String(fundBtc) alongside the leg (the maker reads funded.SeqAmount to lock the slice and binds funded.Leg.Amount==ProportionalBtc(...)).
 5. On SeqLegLocked, bind the returned asset leg SLICE-vs-SLICE: seq_leg.amount==takeSeq (defense: the value gate verifyLeg must require seq_leg.amount>=takeSeq before the irreversible reveal — the taker must receive at least the slice it funded). Preserve every existing gate (anchor-bury, claim key on H, persist-before-broadcast, T_btc>T_seq).
IMPLEMENT in xrswap.js (REVERSE, sell asset for BTC): ship the slice in the TermsRequest (seq_amount=takeSeq); the maker's terms already carry the partial (tSeq==takeSeq, tBtc=floor proportional) — bind slice-vs-slice; over-ask + min handling; keep whole takes identical.
Add/extend a node --test unit (mock courier session) proving: a partial forward take validates whole-ratio terms, funds ceil-proportional BTC, sends SeqAmount in BtcLegFunded, binds seq_leg==takeSeq, and refuses if the maker locks < takeSeq; a WHOLE take is unchanged.
Run: cd ${WEB} && node --check xswap.js xrswap.js && node --test *.test.mjs (report REAL output). Return area='courier-partial'.`, { label:'build:courier-partial', phase:'Build', schema: IMPL_SCHEMA }),

  () => agent(`Web wallet ${WEB}, swap.js requoteCross (~L2550-2746). Round-2 unified the on-chain-pay preview onto bridgedTakePlan ONLY when \`!bp.crosses\` (L2596); when the best unified offer has an LN leg it falls to the legacy XBOOK path (L2638-2738) — matching a DIFFERENT offer than the Lightning-pay rail (still rail-siloed) and OVERSHOOTing to the whole offer (L2704-2715,2725, 'more than the Z you entered'). SPEC: ONE book, matched rail-blind; the rail only selects the invisible settlement; least UI complexity (no maker/bridge/rail/cross-chain/atomic vocab; NO rail advice). requoteMixed's submarine branch (L2449-2496) is the model: always render the unified match, gate ONLY Place.

IMPLEMENT:
 1. When haveUnified (UBOOK has a best offer for this pair), ALWAYS render bp.offer via renderMixedTake (the identical rail-blind fill both rails show) — remove the \`!bp.crosses\` gate on the DISPLAY. Keep the affordability gate on the sized take.
 2. Gate ONLY Place: for chain/chain, enable Place when the on-chain courier can settle THIS matched offer — i.e. \`!bp.crosses\` (a happy-coincidence on-chain maker => native courier, drive the existing X.openFromComposer courier xq UNCHANGED, now with the sized partial take). When bp.crosses (the matched best offer rests its asset over Lightning, which the on-chain courier cannot lift), STILL show the same fill but DISABLE Place with the shared plain note (payerBridgeDisabledNote() — 'This trade could not be placed right now — try again shortly.'); NEVER silently match a different XBOOK offer, NEVER give rail advice. This mirrors requoteMixed's show-fill-then-block-Place.
 3. Keep the XBOOK fallback ONLY for when the unified feed is genuinely unreachable (!haveUnified && offers.length) — and there, CAP the courier take to the requested size (route through the SAME sizeSubswapTake sizing or cap seq_amount to the request) so it NEVER overshoots/relabels the whole offer; drop the 'more than the Z you entered' wording. If capping the legacy courier is infeasible, prefer disabling Place with the plain note over an overshoot.
 4. The unified courier xq already passes the partial slice (seq_amount:takeAtoms) — leave that; the courier-partial change (other agent, xswap.js) makes the maker lock exactly it.
Run: cd ${WEB} && node --check swap.js && node --test *.test.mjs tooling/lsp/*.test.mjs (report REAL). Extend swap-railblind.test.mjs: when the best unified offer is a sub-asset (LN-leg) offer, BOTH the on-chain-pay preview and the Lightning-pay preview render the SAME matched offer + fill (Place may differ). Return area='composer-r3'.`, { label:'build:composer-r3', phase:'Build', schema: IMPL_SCHEMA }),
])

phase('Verify')
const LENSES = [
  { key:'forward-partial-fundsafe', focus:'the FORWARD partial courier is fund-safe: the taker funds fundBtc=ceil-proportional (never underpays the maker), validates the maker quoted the advertised whole offer (ratio) before funding, sends SeqAmount=takeSeq in BtcLegFunded, and REFUSES to reveal the preimage unless the maker locked seq_leg.amount>=takeSeq (the taker always receives at least the slice it funded); every pre-existing gate (T_btc>T_seq, anchor-bury, claim key bound to H, persist-before-broadcast) still holds; a WHOLE take is byte-identical to before' },
  { key:'reverse-and-overshoot', focus:'the REVERSE partial (sell) binds slice-vs-slice and cannot be drained below min/dust; and requoteCross NEVER overshoots to the whole offer on any rendered path (the old q.overshoot/wholeOffer whole-lift is gone or capped to the requested size), so no whole-offer string or over-lift survives on the on-chain-pay rail' },
  { key:'railblind-and-vocab', focus:'requoteCross shows the SAME matched offer + fill as requoteMixed for the SAME market+size regardless of the best offer rail (sub-asset best offer included) — the rail only gates Place, never swaps in a different XBOOK offer; no banned vocab (maker/bridge/rail/cross-chain/atomic/reorg/sat-vB) on any rendered sink; SBTC risk opt-in only on the on-chain-BTC limit; node --check + all tests pass' },
]
const verdicts = await parallel(LENSES.map(l => () =>
  agent(`ADVERSARIAL review of the uncommitted web-wallet diff (xswap.js/xrswap.js courier partial + swap.js requoteCross r3). Try HARD to REFUTE: ${l.focus}. Trace the actual code; construct a concrete failing input. Report every hole (under 400 chars each); empty array if genuinely clean. Default pass=false on ANY fund-loss path, per-rail fill divergence, whole-offer overshoot, or lost pre-existing gate. READ-ONLY. ${GO_REF}`,
    { label:`verify:${l.key}`, phase:'Verify', schema: VERDICT_SCHEMA }).then(v => ({ ...v, key:l.key })).catch(()=>({ key:l.key, pass:false, holes:[{severity:'correctness',where:'agent-error',scenario:'no return',fix:'re-run'}] }))
))
return { builds, verdicts }
