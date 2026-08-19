export const meta = {
  name: 'docs-anchor-exprace-r3',
  description: 'Close the remaining accuracy findings in the anchoring and proof-of-stake chapters, including provenance for the measured anchor-trail figures',
  phases: [ { title: 'Correct' }, { title: 'Verify' } ],
}

const NODE = '/home/aejkohl/Sequentia'

const RULES = `HOUSE RULES: NO em dashes. "Sequentia" is the network, NEVER abbreviated as "SEQ" (SEQ is the ticker of the token named "Sequence"). Final publishable prose. Match the surrounding doc/sequentia voice.
FIRST PRINCIPLE: Bitcoin anchoring is supreme consensus law; Sequentia reorgs whenever Bitcoin reorgs. Never present -validateanchor=0 as a reasonable option.
These are CONSENSUS documents: verify every claim against source before writing it. Two prior review rounds already corrected a backwards account of the election maths, so do not trust a plausible story, check it.`

const ISSUES = `REMAINING FINDINGS from the third adversarial review. Fix each, verifying against source.

1. [INACCURACY, raised twice] 04-proof-of-stake.md:330-332 (and check 06-tokenomics-and-launch.md): "Custom and regtest chains read the height from -posexpraceheight" is WRONG for regtest. Only CCustomParams::UpdateFromArgs reads the flag (src/chainparams.cpp:1413); CreateChainParams maps REGTEST to CRegTestParams (src/chainparams.cpp:2297-2298, class at :1063), which never reads it and leaves pos_exprace_height at its 0 default, so -chain=regtest -posexpraceheight=10 is SILENTLY IGNORED. Write: custom chains (including elementsregtest, the functional-test default, which is why feature_pos_exprace.py works) read -posexpraceheight and default to 0; plain -chain=regtest ignores the flag and stays at 0.

2. [INACCURACY] 03-bitcoin-anchoring.md:253-254: bad-posvrf-agg-quorum is the legacy MuSig2-aggregate path ONLY (src/validation.cpp:2328). BOTH bundled chains default to the BLS committee (src/chainparams.cpp:465-466 mainnet, 717/738 testnet), where a sub-quorum block lacking the height gap is rejected as bad-posbls-agg-quorum. Name bad-posbls-agg-quorum as the code the bundled chains actually emit, with bad-posvrf-agg-quorum as the MuSig2 equivalent. Verify which code bad-pos-escape-stall-too-soon belongs to as well.

3. [INACCURACY] 03-bitcoin-anchoring.md:69-70: "whose tip is within -anchorcontestwindow blocks of the active tip" reads two-sided, but the filter is ONE-SIDED: AnchorUncontestedHeight skips a branch only when tip_height + w < active_tip_height (src/anchor.cpp:311), so a branch at or ABOVE the active tip always counts. Rewrite as: no more than -anchorcontestwindow blocks BELOW the active tip, with a branch at or above it always counting.

4. [INACCURACY] 03-bitcoin-anchoring.md:226-229 and 05-operating-sequentia.md:317-322: -anchorcontestwindow is NOT producer-only. The finality-reconciliation release gate reads it via GetMainchainUncontestedHeight (src/anchor.cpp:546, helper at :186-206) to refuse releasing finality for a rival anchored above the uncontested height. Keep "the back-off itself is production policy and never a validity rule", but add that the same window is also consulted by finality reconciliation, and say what that means for an operator who changes it.

5. [OMISSION] 03-bitcoin-anchoring.md:246-257: the median-time-past half of the escaping-stall rule is ALSO disabled by -validateanchor=0 (CheckEscapingStallMtpGap returns ALLOWED when !g_validate_anchor, src/anchor.cpp:229-232). Say so: the time evidence rides on the same Bitcoin daemon and is delegated with it, so under -validateanchor=0 only the anchor-height gap survives.

6. [INACCURACY] 04-proof-of-stake.md:319-321: "true when consensus.pos_exprace_height is non-zero" contradicts line 327 of the same section six lines later. PosExpRaceActive tests > 0 (src/pos.cpp:499), so a NEGATIVE value is non-zero yet disabled. Write "greater than zero".

7. [CONTRADICTION] 04-proof-of-stake.md:274-280 versus 357-361: "The candidate ordering key is now PosVrfScoreExp" and "merging cannot admit a candidate that outranks the lowest score" are unqualified, but BackedForRound's PRIMARY key is anchor height (src/pos_producer.cpp:974-975), so a fresher-anchored candidate outranks a lower score. Qualify BOTH sentences with "among candidates carrying the same anchor height".

8. [INACCURACY] 04-proof-of-stake.md:206-208 and 229-231: two quantitative problems. (a) The "ranking purely by floor(U*W/w)" figures (about 22% / about 19%) do not reproduce: with the floor the result is entirely tie-break dependent (about 9% if ties go to the first candidate, as the test's strict < resolves them, or about 24% on random tie-breaking). Either rank by U*W/w with NO floor and quote about 21% whole versus about 19% split, or keep the floor and STATE the tie-break rule that produces the quoted numbers. (b) "15 draws against a threshold 15 times smaller are slightly worse" understates it: field entry falls from 60% to 1-(1-0.04)^15 = about 46%, a 14 point drop, while the same passage calls the 3 point split delta "small". Fix the inverted quantifiers.

9. [INACCURACY] 04-proof-of-stake.md:269-272: "split-neutral to within 1%, with proportionality exact over 200,000 simulated rounds (src/pos.cpp)" overclaims. A Monte Carlo cannot show exactness (standard error about 0.1pp), and no 256-bit reference harness exists in the tree; the only support is the comment at src/pos.cpp:442-443. Attribute EXACTNESS to the real-arithmetic result only, and say the Q32 evaluation agrees with a 256-bit reference within sampling error, citing the in-tree evidence (src/test/pos_tests.cpp pos_vrf_exprace, 30,000 rounds).

10. [OMISSION, provenance for MY OWN numbers] 03-bitcoin-anchoring.md:266-274: the testnet4 anchor-trail figures carry no method, window or artifact, unlike every other empirical claim in these chapters. THE MEASUREMENT WAS TAKEN IN THIS SESSION, so here is the provenance to state: measured on 2026-07-25 against the live public testnet; roughly 10,000 Sequentia blocks in the height range about 39,000 to 48,953; the trail computed as each block's committed anchorheight against the Bitcoin testnet4 tip height at the same instant; distribution median 3 parent blocks, p75 5, p90 8 to 10, p99 15 to 23, maximum observed 26; Bitcoin testnet4 carried about 2,300 competing tips per getchaintips during the window; and the committee in this testbed shares ONE bitcoind, so every producer computes the identical back-off, which is why the ordering rule never promotes a fresher candidate here. State the window and method, present the p99 as a range across sampling slices (or give a single figure and say how it was sliced), and mark it explicitly as a testnet4 observation that is NOT expected to characterise mainnet. If you judge the figures cannot be stated honestly at this precision, give the median and the qualitative shape and drop the tail percentiles rather than inventing rigour.`

const WRITE = { type:'object', additionalProperties:false, required:['corrections','residual'], properties:{
  corrections:{type:'array',items:{type:'object',additionalProperties:false,required:['item','what_changed','verified_against'],properties:{item:{type:'string'},what_changed:{type:'string'},verified_against:{type:'string'}}}},
  residual:{type:'string'} } }

phase('Correct')
const fixed = await agent(`${RULES}\n\n${ISSUES}\n\nRepo ${NODE}, files doc/sequentia/03-bitcoin-anchoring.md, doc/sequentia/04-proof-of-stake.md, doc/sequentia/05-operating-sequentia.md, doc/sequentia/06-tokenomics-and-launch.md (uncommitted edits from prior rounds are in the tree; correct in place). NOTE: another task recently set the mainnet activation sentence at 04-proof-of-stake.md around line 339 to "Mainnet is set to 1 ..." which is CORRECT and must be preserved; do not revert it. Verify every claim against source.`, { label:'correct:r3', phase:'Correct', schema: WRITE })

phase('Verify')
const VERD = { type:'object', additionalProperties:false, required:['lens','pass','issues'], properties:{ lens:{type:'string'}, pass:{type:'boolean'},
  issues:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','problem','fix'],properties:{ severity:{type:'string',enum:['inaccuracy','style-violation','omission','contradiction']}, where:{type:'string'}, problem:{type:'string',description:'under 300 chars'}, fix:{type:'string'} }}} } }

const verdicts = await parallel([
  { key:'claims-true', focus:'every technical claim across the two chapters is TRUE against src/pos.cpp, src/pos_producer.cpp, src/anchor.cpp, src/validation.cpp and src/chainparams.cpp: the regtest-versus-custom flag reading, the reject-code names for the bundled BLS committee, the one-sided contest-window filter, the -validateanchor consequences for both halves of the escaping-stall rule, the greater-than-zero activation predicate, and the anchor-height-primary ordering qualification' },
  { key:'numbers-honest', focus:'no quantitative claim overstates its evidence: the legacy-election percentages reproduce under a stated tie-break rule, exactness is claimed only for real arithmetic and not for Monte Carlo, the anchor-trail figures carry a stated window and method and are marked as a testnet4 observation not a mainnet prediction, and no figure is presented with more precision than its sampling supports' },
  { key:'style-and-integrity', focus:'no em dashes in any added or edited line; Sequentia never abbreviated as SEQ; the mainnet-is-1 activation sentence is intact; the chapters do not contradict each other; -validateanchor=0 is presented only as a hazard; and nothing contradicts Bitcoin-anchoring supremacy' },
].map(l => () => agent(`ADVERSARIAL re-review of ${NODE}/doc/sequentia (git diff). ${RULES}\n\nTry HARD to falsify: ${l.focus}. Read the source to check every claim; cite file:line. Under 300 chars per issue. Empty array only if genuinely clean. Default pass=false on any unverified claim, overstated number, or em dash. READ-ONLY.`,
  { label:`verify:${l.key}`, phase:'Verify', schema: VERD }).then(v=>({ ...v, key:l.key })).catch(()=>({ key:l.key, pass:false, issues:[{severity:'omission',where:'agent-error',problem:'no return',fix:'re-run'}] }))))

return { fixed, verdicts }
