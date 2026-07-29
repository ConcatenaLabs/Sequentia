export const meta = {
  name: 'docs-anchor-exprace-r2',
  description: 'Correct the factual errors two reviewers found in the exp-race and anchor back-off documentation, then re-verify',
  phases: [ { title: 'Correct' }, { title: 'Verify' } ],
}

const NODE = '/home/aejkohl/SequentiaByClaude'

const RULES = `HOUSE WRITING RULES: NO em dashes. "Sequentia" is the network and is NEVER abbreviated as "SEQ" (SEQ is the ticker of the token named "Sequence", tSEQ on testnet). Final publishable prose, no placeholders. Match the surrounding doc/sequentia voice.
FIRST PRINCIPLE: Bitcoin anchoring is supreme consensus law; Sequentia reorgs whenever Bitcoin reorgs. Nothing may contradict that.
These are CONSENSUS documents. Every claim must be verified against the source before you write it. Where the two reviewers below disagree, go read the code and decide from the code, and say in your report which reading won and why.`

const ISSUES = `TWO INDEPENDENT ADVERSARIAL REVIEWS of the uncommitted doc changes agreed the maths is BACKWARDS and found further errors. Correct every item, verifying each against source.

ALREADY RESOLVED BY THE OWNER-SIDE CHECK, treat as settled fact: PosExpRaceActive is "params.pos_exprace_height > 0 && height >= params.pos_exprace_height" (src/pos.cpp:499), so 0 means DISABLED, not active-from-genesis. src/chainparams.cpp:415-419 sets mainnet pos_exprace_height = 0 with the comment "Exponential-race leader election: disabled until mainnet launches and a coordinated activation height is set (hard fork; see params.h). Mainnet is not live yet, so this stays 0 for now." Testnet is 44300 (src/chainparams.cpp:668). THE DOC MUST NOT IMPLY THE EXPONENTIAL RACE IS IN FORCE ON MAINNET. State plainly: active on the public testnet from 44300, and disabled on mainnet until a launch activation height is chosen.

1. [INACCURACY, both reviewers, the central error] 04-proof-of-stake.md:204-211. The doc blames the Sybil edge on ranking by "beta / w" and says splitting "slightly improved the odds". That is backwards: for score U*W/w, splitting w into k identities gives P(best < t) = 1-(1-t*w/(W*k))^k < t*w/W, so splitting strictly LOWERS the chance of the lowest slot. Reviewer simulations: 30% as one identity wins 21.3%, as two 15% identities 19.8%. The REAL edge came from elsewhere and the two reviewers describe it slightly differently: (a) slot 0 requires U < w/W, so a smaller identity's beta is conditionally smaller and wins the unweighted "a.cand->beta < b.cand->beta" tiebreak more often, and N identities are N draws (src/pos_producer.cpp:977); (b) the producer cadence floor max(slot,1) (src/pos_producer.cpp:762-764) collapses slots 0 and 1 into one offering time, and BackedForRound then ordered those candidates by RAW UNWEIGHTED beta, so N identities bought N draws. VERIFY BOTH AGAINST THE CODE and write the mechanism that is actually true; do not hand-wave.

2. [INACCURACY, both reviewers] 04-proof-of-stake.md:204,211. "almost, but not exactly, stake-proportional" and "The edge was small" badly understate the legacy defect. src/test/pos_tests.cpp:400-401 records the legacy election paying a 30% staker about 1.4x its share (roughly 42% of blocks), and a 5% staker about 8%, rising to about 1.5x when split into 15 identities. Give the measured numbers, and reserve "small" for the SPLIT DELTA, not for the overall deviation from proportionality.

3. [INACCURACY, both reviewers] 03-bitcoin-anchoring.md:75-77. "while a contest is live the anchor holds where it is, then jumps forward to the fresh tip" overstates the freeze and contradicts the median trail of 3 reported later in the same file. src/anchor.cpp:343 accepts the backed-off target whenever it is >= prev_anchor_height, so during an ordinary contest the anchor STILL ADVANCES, to the uncontested height (tip minus branchlen). Rewrite: while a contest is live the anchor follows the last uncontested height, trailing the parent tip by the depth of the contest, and holds at the parent block's anchor only when that height would fall below it; it jumps to the tip once the contest clears.

4. [CONTRADICTION, both reviewers] 03-bitcoin-anchoring.md:195-196 still says the committee falls back to "the lowest leader VRF among equally-fresh proposals", while 04 section 7 was updated in the same change set to the exponential-race score. From pos_exprace_height onward BackedForRound's secondary key is PosVrfScoreExp (src/pos_producer.cpp:960-977), raw beta only below it. Mirror the updated wording, and keep it consistent with item 1 above.

5. [INACCURACY] 03-bitcoin-anchoring.md:224,231-232. "Validity is exactly the three rules" and "no node may reject a block for anchoring further back than it would have chosen itself" are false for SUB-QUORUM blocks: PosEscapingStallAllowed (src/pos.h:664, gap 3) plus CheckEscapingStallMtpGap gate certification at src/validation.cpp:2325/2403/2441. Scope the claim to ANCHOR validity and add the exception: an escaping-stall block must additionally anchor at least +3 parent heights past its parent's anchor (and show the MTP gap), else it is rejected (bad-posvrf-agg-quorum / bad-pos-escape-stall-too-soon).

6. [OMISSION] 03-bitcoin-anchoring.md:234-241. "a longer wait, never a weaker guarantee" lists only the swap wait. The back-off ALSO delays reaching the +3 escaping-stall gap, so an under-quorum committee takes longer to escape a stall exactly when the parent chain is contested, which is the failure mode of the incident cited nearby. Add that sentence.

7. [OMISSION] 03-bitcoin-anchoring.md:226-228. The R3 gloss drops a guard: src/validation.cpp:4781 gates R3 on g_validate_anchor, so with -validateanchor=0 only R1 and R2 are enforced. Write R3 as needing a Bitcoin daemon: skipped when the anchor is unchanged from the parent, and skipped entirely with -validateanchor=0. (Do NOT present -validateanchor=0 as a reasonable thing to run; it defeats the supreme anchoring rule.)

8. [INACCURACY] 04-proof-of-stake.md:234-237. "exactly the same distribution as the undivided stake's single draw" holds in exact arithmetic only. src/pos.cpp:443 records the Q32 fixed-point evaluation as split-neutral to within 1% against a 256-bit reference (proportionality exact over 200k rounds). Keep "exactly" for the ideal exponential race, then state the implementation bound.

9. [INACCURACY] 04-proof-of-stake.md:251-252. "a sentinel above every real score" is not universal: PosExpScoreInf is (POS_VRF_MAX_SLOT+1)<<32, about 1048577 in Q32, while a real score reaches roughly 177*(W/w) in Q32, so a staker holding less than about 1/5900 of eligible weight can draw a real score above the sentinel. Drop the unqualified "every" and state the bound.

10. [INACCURACY] 03-bitcoin-anchoring.md:234-236. "can trail Bitcoin's tip by more than one Sequentia block" mixes units; the trail is measured in PARENT (Bitcoin) blocks, as the same paragraph then says (median 3, max 26). Use Bitcoin blocks.

ALSO (was left out of scope last round, do it now): doc/sequentia/05-operating-sequentia.md around lines 146 and 299 lists the anchor settings without -anchoravoidcontested or -anchorcontestwindow, so an operator reading only the operating guide never learns the two flags exist. Add them with their defaults and a one-line pointer to 03 section 5.`

const WRITE = { type:'object', additionalProperties:false, required:['corrections','disputed_resolved','residual'], properties:{
  corrections:{type:'array',items:{type:'object',additionalProperties:false,required:['item','what_changed','verified_against'],properties:{item:{type:'string'},what_changed:{type:'string'},verified_against:{type:'string'}}}},
  disputed_resolved:{type:'string', description:'which reading of the legacy Sybil edge won, and the source that settles it'},
  residual:{type:'string'} } }

phase('Correct')
const fixed = await agent(`${RULES}\n\n${ISSUES}\n\nRepo ${NODE}, files doc/sequentia/03-bitcoin-anchoring.md, doc/sequentia/04-proof-of-stake.md, doc/sequentia/05-operating-sequentia.md (uncommitted edits from the previous round are already in the tree; correct them in place). Verify EVERY claim against the source files named. Report which reading of item 1 the code actually supports.`, { label:'correct:docs', phase:'Correct', schema: WRITE })

phase('Verify')
const VERD = { type:'object', additionalProperties:false, required:['lens','pass','issues'], properties:{ lens:{type:'string'}, pass:{type:'boolean'},
  issues:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','problem','fix'],properties:{ severity:{type:'string',enum:['inaccuracy','style-violation','omission','contradiction']}, where:{type:'string'}, problem:{type:'string',description:'under 300 chars'}, fix:{type:'string'} }}} } }

const verdicts = await parallel([
  { key:'maths-and-election', focus:'the corrected account of the legacy election defect and the exponential race is now TRUE against src/pos.cpp, src/pos.h, src/pos_producer.cpp and src/test/pos_tests.cpp: the direction of the split incentive, the measured legacy deviation, the Q32 implementation bound, the sentinel bound, what the exp-race changed versus what it did not (committee membership, fork choice, cadence), and the activation status on testnet versus mainnet' },
  { key:'anchor-and-validity', focus:'the corrected anchor back-off account is TRUE against src/anchor.cpp, src/anchor.h and src/validation.cpp: that the anchor still advances to the uncontested height rather than freezing, the exact validity rules including the -validateanchor gate and the sub-quorum escaping-stall exception, the trail measured in Bitcoin blocks, the escaping-stall consequence, and that the ideal one-block-tracking statement is preserved as ideal and typical' },
  { key:'style-and-consistency', focus:'no em dashes anywhere in the added or edited lines; Sequentia never abbreviated as SEQ; the two documents do not contradict each other on the ranking key or on activation; the operating guide now lists the two flags; prose is final and publishable and matches the surrounding voice; nothing contradicts Bitcoin-anchoring supremacy or presents -validateanchor=0 as reasonable' },
].map(l => () => agent(`ADVERSARIAL re-review of the corrected documentation in ${NODE}/doc/sequentia (git diff). ${RULES}\n\nTry HARD to falsify: ${l.focus}. Read the actual source to check every claim; cite file:line. Under 300 chars per issue. Empty array only if genuinely clean. Default pass=false on ANY unverified or incorrect technical claim, or any em dash. READ-ONLY.`,
  { label:`verify:${l.key}`, phase:'Verify', schema: VERD }).then(v=>({ ...v, key:l.key })).catch(()=>({ key:l.key, pass:false, issues:[{severity:'omission',where:'agent-error',problem:'no return',fix:'re-run'}] }))))

return { fixed, verdicts }
