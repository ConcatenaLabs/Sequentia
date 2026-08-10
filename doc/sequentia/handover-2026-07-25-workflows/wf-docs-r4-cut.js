export const meta = {
  name: 'docs-r4-reduction',
  description: 'Cut the unsupportable quantitative claims from the anchoring and proof-of-stake chapters and settle the disputed anchor dynamics, keeping only what the tree or a stated method supports',
  phases: [ { title: 'Cut' }, { title: 'Verify' } ],
}

const NODE = '/home/aejkohl/SequentiaByClaude'

const RULES = `HOUSE RULES: NO em dashes. "Sequentia" is the network, NEVER abbreviated "SEQ" (SEQ is the ticker of the token named "Sequence"). Final publishable prose. Keep the file's existing wrap width.
FIRST PRINCIPLE: Bitcoin anchoring is supreme consensus law. Never present -validateanchor=0 as reasonable.

THIS IS A REDUCTION PASS, NOT AN EXPANSION. Three prior review rounds each produced NEW findings against numbers and simulations that were added to justify earlier numbers. That is a losing loop. The instruction now is: CUT anything that cannot be supported by (a) the source tree, or (b) a stated, reproducible method. A claim that is accurate but unverifiable by a reader is a DEFECT, not an asset. Prefer deleting a sentence to adding a caveat. Shorter and certainly true beats longer and arguable.`

const WORK = `TASKS.

1. [CUT] The trailing-anchor measurement in 03-bitcoin-anchoring.md (around lines 299-310). DELETE the percentile table and the competing-tips figure entirely: median 3, p75 5, p90 8-10, p99 15-23, max 26, "about 2,300 competing tips", the "slices" language, and the "Bitcoin testnet4 tip height observed at the same instant" method sentence. Reviewers correctly established that (i) no script, dataset or RPC recipe exists in the tree, (ii) the per-block historical parent tip is not observable retrospectively from a single session, so the stated method cannot produce those numbers, and (iii) getchaintips returns a CUMULATIVE inventory of every branch tip the node has ever seen, not a count of live contests, so the tips figure does not mean what the sentence says.
REPLACE with a short qualitative statement that is certainly true and needs no dataset: while the parent chain is contested the anchor can trail the parent tip by more than one block, this is routine on the public testnet whose parent chain forks frequently, and it is expected to be rare on mainnet. If you want to keep ONE number, keep only a single clearly-labelled illustrative observation with its date and heights (for example: on 2026-07-25 a Sequentia block committed anchor height 145,607 while Bitcoin testnet4 had already produced 145,609), because that one IS a directly observed data point rather than a distribution.

2. [SETTLE THE DISPUTE] 03-bitcoin-anchoring.md around lines 77-81, on what the anchor does while a contest is live. Round 2 review said it keeps advancing to the uncontested height; round 3 review said AnchorUncontestedHeight returns a rival's fork point which is FIXED while that rival stays in the window, so the anchor is static and the trail grows. READ THE CODE AND SETTLE IT: src/anchor.cpp:304-316 (AnchorUncontestedHeight), :335-343 (the back-off and the monotonic clamp). Consider both cases: a single rival that persists in the window, and the ordinary case where rivals appear and fall out of the window as the active tip advances. A directly observed data point: on 2026-07-25 an anchor sat at 145,607 while the parent tip was 145,609, and a later Sequentia block committed 145,623, so over that interval it did move. Write the accurate description covering both regimes, and if the behaviour genuinely depends on rival churn, SAY THAT plainly rather than picking one side.

3. [FIX] The remaining precise findings, each verified against source before writing:
   a. 04-proof-of-stake.md:283-287 overstates what pos_vrf_exprace asserts. src/test/pos_tests.cpp:397 allows the whale 0.74-0.86 (about 6 points) and :418-420 asserts only 0.27 < share < 0.33 and |s1-s15| < 0.03. Do not claim "a few tenths of a point". State the actual assertion bounds or drop the precision claim.
   b. 04-proof-of-stake.md:250-253 "anywhere from 9% to 24%": the span is tie-break dependent and understated (ties-to-first about 9%, uniform-random about 24%, raw-beta tiebreak about 34.5%). Simplest fix: DELETE the floored-ranking aside entirely. It is a hypothetical the code never implements and it has now consumed three review rounds. Keep the unfloored figure only if it is load-bearing.
   c. 04-proof-of-stake.md:224-226 "it takes the unweighted tiebreak nearly every time" is contradicted two sentences later (70% of entered rounds; the 98% figure applies to the 2% identities after the split). Make them agree.
   d. 04-proof-of-stake.md:244-249: the parenthetical says only the ordering key was swapped, but the offering time also comes from the legacy slot in that model (src/test/pos_tests.cpp:373-377). Correct or cut the parenthetical.
   e. 04-proof-of-stake.md:355-358: "that chain stays on the legacy election at every height" implies plain regtest runs a PoS election; CRegTestParams sets g_con_pos = false (src/chainparams.cpp:1130), so regtest has no PoS at all. Say that instead.
   f. 04-proof-of-stake.md:371-372 counts regtest among "bundled" chains while 03 uses "both bundled chains" for mainnet plus testnet. Make the term consistent across both files.
   g. 04-proof-of-stake.md:360-362 reads future tense about testnet nodes upgrading before 44300, but that activation is already past. Put it in the past tense.
   h. 03-bitcoin-anchoring.md:268-270 contradicts 03:285-287 and src/pos.h:663-669 on whether a block storm CAN supply the height gap in seconds. Read the source and make them agree.
   i. 03-bitcoin-anchoring.md:246-249 cross-references "the three rules of section 3 (R1/R2/R3)" but section 3 (03:108-121) names them Well-formedness, Monotonicity, and Bitcoin existence and best-chain membership, with no R1/R2/R3 labels. Either add the labels in section 3 or use the names at the cross-reference.
   j. 03-bitcoin-anchoring.md:203-205, 209, 246, 251-253: rewrap leftovers (orphan lines carrying a single word, a 94-character line) to the file's roughly 80-column wrap.`

const OUT = { type:'object', additionalProperties:false, required:['cuts','fixes','anchor_dynamics_verdict','residual'], properties:{
  cuts:{type:'array',items:{type:'string'}},
  fixes:{type:'array',items:{type:'object',additionalProperties:false,required:['item','what_changed','verified_against'],properties:{item:{type:'string'},what_changed:{type:'string'},verified_against:{type:'string'}}}},
  anchor_dynamics_verdict:{type:'string', description:'the settled answer on what the anchor does during a contest, with file:line'},
  residual:{type:'string'} } }

phase('Cut')
const done = await agent(`${RULES}\n\n${WORK}\n\nRepo ${NODE}, files doc/sequentia/03-bitcoin-anchoring.md and doc/sequentia/04-proof-of-stake.md (uncommitted edits from prior rounds are in the tree). PRESERVE the mainnet activation sentence at 04 around line 339 that begins "Mainnet is set to \`1\`" - it is correct. Verify every surviving claim against source.`, { label:'cut:r4', phase:'Cut', schema: OUT })

phase('Verify')
const VERD = { type:'object', additionalProperties:false, required:['lens','pass','issues'], properties:{ lens:{type:'string'}, pass:{type:'boolean'},
  issues:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','problem','fix'],properties:{ severity:{type:'string',enum:['inaccuracy','style-violation','omission','contradiction']}, where:{type:'string'}, problem:{type:'string',description:'under 250 chars'}, fix:{type:'string'} }}} } }

const verdicts = await parallel([
  { key:'nothing-unsupported-remains', focus:'NO quantitative or empirical claim survives that a reader cannot verify from the source tree or from a stated reproducible method; the percentile table and the competing-tips figure are gone; any surviving illustrative observation is dated, labelled as a single observation, and true' },
  { key:'claims-true-and-consistent', focus:'every surviving technical claim is true against src/anchor.cpp, src/pos.cpp, src/pos_producer.cpp, src/validation.cpp, src/chainparams.cpp and src/test/pos_tests.cpp; the anchor-during-contest description matches the code for BOTH the persistent-rival and rival-churn regimes; the two chapters do not contradict each other or themselves; the mainnet-is-1 sentence is intact' },
].map(l => () => agent(`ADVERSARIAL review of ${NODE}/doc/sequentia (git diff). ${RULES}\n\nTry HARD to falsify: ${l.focus}. Cite file:line. Under 250 chars per issue. Empty array only if genuinely clean. IMPORTANT: this was a REDUCTION pass, so do NOT propose adding new numbers, simulations or caveats; if something is wrong, prefer proposing a CUT. Default pass=false on any unsupported claim, contradiction, or em dash. READ-ONLY.`,
  { label:`verify:${l.key}`, phase:'Verify', schema: VERD }).then(v=>({ ...v, key:l.key })).catch(()=>({ key:l.key, pass:false, issues:[{severity:'omission',where:'agent-error',problem:'no return',fix:'re-run'}] }))))

return { done, verdicts }
