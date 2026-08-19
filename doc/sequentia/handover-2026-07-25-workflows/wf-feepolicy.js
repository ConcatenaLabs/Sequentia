export const meta = {
  name: 'fee-whitelist-from-node-policy',
  description: 'Determine the Sequentia node actual mempool accept policy for fee assets, whether /feerates already is that policy, and how the wallet fee list should be sourced from it',
  phases: [ { title: 'Investigate' }, { title: 'Recommend' } ],
}

const NODE = '/home/aejkohl/Sequentia'
const WEB = '/home/aejkohl/sequentia-web-wallet'

const CTX = `SEQUENTIA FIRST PRINCIPLES (do not contradict):
- SEQ/Sequence has EQUAL standing with every issued asset. No privileged native unit. The "no-coin / open fee market" principle: fees are payable in ANY accepted Sequentia-issued asset, and block proposers choose what they accept.
- The any-asset fee exchange rates are CORRECT and must not be "inverted": rate = (asset_price / SEQ_price) * 1e8 = the asset value in SEQ-sats * 1e8; asset_fee_atoms = ceil(policy_fee * 1e8 / rate). A valuable asset paying FEWER atoms is correct.
- Fee-rate units are the chosen fee asset's OWN units per vByte, NEVER "sat/vB" (sat is Bitcoin-only).
- BTC is the NATIVE parent-chain asset, not a Sequentia-issued asset.

THE QUESTION (from the project owner, Andreas). The web wallet DEX fee selector decides which assets a user may pay fees in. Today it filters with acceptedFee(hex), which is TRUE when C.feeRates[asset].rate > 0, and feeRates is loaded from the endpoint GET /feerates, which currently returns a ticker to rate map (observed live: EURX, bitcoin, SILVR, SBTC, OILX, GOLD, USDX).

Andreas reasoned, and I agree: it is not enough that SOME block proposer somewhere accepts an asset at some rate. The transaction must first be ACCEPTED INTO THE MEMPOOL AND RELAYED by the node the wallet broadcasts through. If that node will not accept the fee asset, the transaction never propagates and no proposer ever sees it. He has directed that the wallet's fee list come from the SERVING NODE's actual mempool accept policy.

WHAT MUST BE ESTABLISHED: whether /feerates already IS that policy (in which case they are identical by construction and the honest answer is "already correct, here is the proof"), or whether they are two different things that merely happen to coincide today (in which case there is a real gap to close).`

const SCHEMA = { type:'object', additionalProperties:false, required:['lens','findings','conclusion'], properties:{
  lens:{type:'string'},
  findings:{type:'array',items:{type:'object',additionalProperties:false,required:['claim','evidence'],properties:{claim:{type:'string'},evidence:{type:'string',description:'file:line or command output'}}}},
  conclusion:{type:'string'} } }

phase('Investigate')
const inv = await parallel([
  () => agent(`${CTX}\n\nLENS 1 - THE NODE'S ACTUAL POLICY. In the Sequentia node source ${NODE} (a Bitcoin Core / Elements fork), find EXACTLY where a transaction paying its fee in a non-policy asset is accepted or rejected at MEMPOOL ACCEPTANCE, and what determines the accepted set.\nAnswer with file:line:\n(a) The mempool acceptance path for fee assets. Look in src/validation.cpp (AcceptToMemoryPool / PreChecks / CheckFeeRate), src/policy/, src/exchangerates.{cpp,h}, and anything named for fee assets or exchange rates. What is the precise predicate?\n(b) Is the accepted set a CONFIGURED WHITELIST (a startup arg or conf entry, e.g. something like -acceptedasset / -feeasset / -exchangerates), or is it implicitly "any asset for which this node currently holds an exchange rate", or something else? Name the mechanism and its default.\n(c) How is that set POPULATED at runtime? A config file, a startup arg, an RPC that sets rates, a feed? Name the RPCs (for example anything like getexchangerates / setexchangerates) and whether the set can change while the node runs.\n(d) Is there ALREADY an RPC or REST endpoint that reports the node's accepted fee assets (or its exchange-rate table) that a wallet could query? Give its exact name and output shape.\n(e) Does the RELAY decision differ from the MINING decision? That is, could a node relay a transaction whose fee asset it would not itself mine, or refuse to relay one a proposer would happily mine? This is the crux of the owner's question.\nRead-only. Cite file:line for every claim.`, { label:'lens:node-policy', phase:'Investigate', schema: SCHEMA }),

  () => agent(`${CTX}\n\nLENS 2 - WHAT ACTUALLY SERVES /feerates, AND WHAT THE WALLET DOES WITH IT. Trace the whole path end to end.\nAnswer with file:line or config evidence:\n(a) On the box (ssh seq), what serves GET /feerates? Check the Caddy config at /etc/caddy/Caddyfile (the route may be a path prefix, a handle block, or proxied through another service, so search thoroughly), the LSP at ${WEB}/tooling/lsp/lsp-server.mjs, the wallet service, and the price server. Identify the actual producer of that JSON.\n(b) Is that producer reading the NODE's exchange-rate table / accept policy, or is it reading a PRICE FEED (for example a price_server that fetches market prices)? This is the decisive question. Show the code that builds the map.\n(c) In the wallet ${WEB}: how is C.feeRates loaded and cached (see index.html around the /feerates fetch and the swk.feeRatesCache localStorage entry), and how does acceptedFee(hex) in swap.js consume it? Note the ticker-vs-hex keying.\n(d) CAN THEY DIVERGE? Construct a concrete scenario: an asset that appears in one and not the other, or a rate present but not accepted. Say plainly whether divergence is possible today and whether it would be silent.\n(e) Which node does the WALLET actually broadcast through (the dexnode, the explorer node, an LSP-proxied node)? Its policy is the one that matters. Identify it on the box.\nRead-only apart from harmless read-only RPCs and config reads.`, { label:'lens:serving-path', phase:'Investigate', schema: SCHEMA }),
])

phase('Recommend')
const OUT = { type:'object', additionalProperties:false, required:['already_correct','explanation','divergence_possible','recommendation','node_change_needed','open_questions'], properties:{
  already_correct:{type:'boolean', description:'true if /feerates already IS the serving node accept policy'},
  explanation:{type:'string'},
  divergence_possible:{type:'string'},
  recommendation:{type:'string', description:'exact change, with file:line, to source the wallet fee list from the serving node accept policy'},
  node_change_needed:{type:'boolean', description:'true if the node must expose something it does not expose today'},
  open_questions:{type:'array',items:{type:'string'}} } }

const rec = await agent(`${CTX}\n\nNODE POLICY: ${JSON.stringify(inv[0])}\n\nSERVING PATH: ${JSON.stringify(inv[1])}\n\nDecide and recommend.\n1. Is /feerates ALREADY the serving node's accept policy? If yes, say so plainly and give the proof, and state what (if anything) should still change (for example a comment, a test pinning the invariant, or a rename so the next person does not have to re-derive this).\n2. If they are two different things, describe exactly how they can diverge and how silently, then give the concrete fix: what the node should expose (if anything), what should serve it, and how acceptedFee() should consume it. Prefer using an EXISTING node RPC over inventing a new mechanism.\n3. Keep the open fee market intact: the answer must NOT end up privileging the policy asset or any single asset, and must not reintroduce a "native asset" concept. SEQ has equal standing.\n4. Note the interaction with a queued wallet fix: the fee SELECTOR is being changed so that paying BTC on-chain locks the fee to BTC, paying over Lightning locks it to the asset being paid, and only paying an on-chain Sequentia asset offers a CHOICE. Your recommendation applies to that choice list. Say whether the source change is safe to land in the same patch.\nBe honest if the evidence is inconclusive rather than guessing.`, { label:'recommend', phase:'Recommend', schema: OUT })

return { inv, rec }
