export const meta = {
  name: 'mainnet-exprace-from-launch',
  description: 'Make Sequentia mainnet launch with exponential-race leader election active from its first elected block, without changing the disabled-sentinel semantics other chains rely on',
  phases: [ { title: 'Change' }, { title: 'Verify' } ],
}

const NODE = '/home/aejkohl/SequentiaByClaude'

const CTX = `TASK FROM THE PROJECT OWNER (Andreas): "Mainnet (which isn't live yet) should launch with the new rule from the start." The new rule is the exponential-race leader election.

ESTABLISHED FACTS (verified, do not re-litigate):
- src/pos.cpp:499  bool PosExpRaceActive(params, height) { return params.pos_exprace_height > 0 && height >= params.pos_exprace_height; }  So 0 means DISABLED for this parameter, because of the "> 0" guard.
- src/chainparams.cpp:419  mainnet consensus.pos_exprace_height = 0, with a placeholder comment saying it stays 0 until a launch activation height is chosen. That is what must change.
- src/chainparams.cpp:668  testnet = 44300 (already activated live).
- src/chainparams.cpp:1406  regtest/custom reads -posexpraceheight with DEFAULT 0, i.e. OFF unless a test asks for it.
- CONVENTION TRAP: the sibling gate uses the OPPOSITE convention. src/chainparams.cpp:415 sets mainnet consensus.pos_coinbase_leader_height = 0 and its check at src/validation.cpp:2700 is a bare "nHeight >= pos_coinbase_leader_height", so 0 there means ACTIVE FROM GENESIS. Two adjacent parameters, opposite meanings for 0. This asymmetry is what makes mainnet currently read as configured while actually being off.

THE CHANGE. Mainnet must elect under the exponential race from its FIRST ELECTED BLOCK. Because 0 is the disabled sentinel for this parameter, the value must be 1 (genesis is height 0 and is not produced by leader election, so height 1 is the first block an election governs). Do NOT remove or weaken the "> 0" guard in PosExpRaceActive: that guard is what keeps the fork OFF by default on regtest and custom chains (src/chainparams.cpp:1406), and removing it would silently activate a consensus rule change on every such chain.

SEQUENTIA FIRST PRINCIPLES that constrain this: Bitcoin anchoring is supreme consensus law. No inflation, all SEQ pre-mined, block reward is fees only. Full-node sovereignty: block proposers cannot force consensus-rule changes on full nodes. This is a pre-launch parameter choice for a chain with NO history, so it is not a fork of anything live.

HOUSE RULES: no em dashes in comments or docs. "Sequentia" is the network and is never abbreviated as "SEQ"; SEQ is the ticker of the token named "Sequence".`

const IMPL = { type:'object', additionalProperties:false, required:['changed','builds','build_output','residual'], properties:{
  changed:{type:'array',items:{type:'object',additionalProperties:false,required:['file','what'],properties:{file:{type:'string'},what:{type:'string'}}}},
  builds:{type:'boolean'}, build_output:{type:'string'}, residual:{type:'string'} } }

phase('Change')
const impl = await agent(`${CTX}\n\nIn ${NODE}:\n\n1. Set mainnet consensus.pos_exprace_height = 1 (src/chainparams.cpp around :415-419) and REPLACE the placeholder comment with one that states: mainnet launches with the exponential race in force from its first elected block; the value is 1 rather than 0 because 0 is the DISABLED sentinel for this parameter (unlike pos_coinbase_leader_height directly above, where 0 means from genesis); and mainnet has no history so this is a launch parameter, not a fork of live consensus.\n\n2. Document the sentinel asymmetry at its source so nobody trips on it again: at the declaration/comment for pos_exprace_height (src/pos.h around :617) and/or the Consensus::Params field (src/consensus/params.h), state plainly that 0 disables the exponential race for this parameter, and that this differs from pos_coinbase_leader_height where 0 means active from genesis. Keep it brief and factual.\n\n3. Add a REGRESSION TEST so a future edit cannot silently return mainnet to the legacy rule: assert PosExpRaceActive(mainnet_params, 1) is true (and at a few higher heights), that testnet activates exactly at 44300 and not at 44299, and that regtest with no -posexpraceheight is INACTIVE at every height. Put it with the existing exp-race unit tests in src/test/pos_tests.cpp.\n\n4. Check whether any existing unit or functional test, or any doc, ASSERTS that mainnet is on the legacy rule or that pos_exprace_height is 0 on mainnet, and update it. In particular check test/functional/feature_pos_exprace.py and doc/sequentia/06-tokenomics-and-launch.md (the launch ceremony doc) and doc/sequentia/04-proof-of-stake.md. NOTE: 04-proof-of-stake.md is being edited concurrently by another task, so if you must touch it, make the SMALLEST possible edit to the mainnet-activation sentence only and say so in your report.\n\nBuild it: the tree is a Bitcoin Core fork, so a full build is slow. Prefer compiling the affected unit test target if the tree is already configured (look for a configured build directory or Makefile). If a full build is not feasible in reasonable time, at minimum verify the change compiles in isolation as far as you can and SAY SO HONESTLY in build_output rather than claiming a build you did not run.`, { label:'change:mainnet-exprace', phase:'Change', schema: IMPL })

phase('Verify')
const VERD = { type:'object', additionalProperties:false, required:['lens','pass','issues'], properties:{ lens:{type:'string'}, pass:{type:'boolean'},
  issues:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','problem','fix'],properties:{ severity:{type:'string',enum:['consensus-risk','inaccuracy','omission','style-violation']}, where:{type:'string'}, problem:{type:'string',description:'under 300 chars'}, fix:{type:'string'} }}} } }

const verdicts = await parallel([
  { key:'no-collateral-activation', focus:'the change activates the exponential race on MAINNET ONLY from height 1, and does NOT alter behaviour on the live public testnet (still exactly 44300), on regtest or custom chains (still OFF unless -posexpraceheight is passed), or at any other call site of PosExpRaceActive; the "> 0" guard is intact; enumerate every caller of PosExpRaceActive and every reader of pos_exprace_height and confirm each' },
  { key:'height-1-is-correct', focus:'height 1 is the correct value for from-launch activation: genesis at height 0 is not produced by leader election, no code path evaluates the election at height 0, and there is no off-by-one where the first elected block would still use the legacy rule; check the actual election call sites (validation.cpp, pos_producer.cpp, node/miner.cpp) and the time-gate, and confirm the new regression test would FAIL if the value were reverted to 0' },
  { key:'docs-and-consistency', focus:'the sentinel asymmetry is now documented where a future editor will see it; no test or document still claims mainnet runs the legacy rule or that its activation height is unchosen; no em dashes were added; Sequentia is not abbreviated as SEQ; nothing contradicts the first principles (anchoring supremacy, no inflation, full-node sovereignty)' },
].map(l => () => agent(`ADVERSARIAL review of the uncommitted change in ${NODE} (git diff). ${CTX}\n\nThis is a CONSENSUS parameter for a chain that will launch with real value, so hold it to that bar. Try HARD to falsify: ${l.focus}. Cite file:line and give a concrete failure. Under 300 chars per issue. Empty array only if genuinely clean. Default pass=false on ANY unintended behaviour change to testnet or regtest, any off-by-one, or any stale claim left behind. READ-ONLY.`,
  { label:`verify:${l.key}`, phase:'Verify', schema: VERD }).then(v=>({ ...v, key:l.key })).catch(()=>({ key:l.key, pass:false, issues:[{severity:'omission',where:'agent-error',problem:'no return',fix:'re-run'}] }))))

return { impl, verdicts }
