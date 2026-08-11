export const meta = {
  name: 'strip-rfq-clients',
  description: 'Remove the retired RFQ/TDEX xchain CLIENT code from SWK and Ambra without touching the live order-book DEX',
  phases: [ { title: 'Map' }, { title: 'Remove' }, { title: 'Verify' } ],
}

const CTX = `CONTEXT. The RFQ cross-chain rail (the seqdex daemon "XchainService": /v1/xchain/markets, /quote, /propose, /swap, /reverse/quote, /reverse/open, /reverse/submit, served by seqdexd on :9945) is RETIRED. It has no reverse-proxy route and no clients reach it. The project owner (Andreas) has ordered the server-side code deleted, and asked to also remove the client-side implementations that were built against it: "Get rid of anything we aren't using, just make sure not to break the order book DEX."

THE LIVE SYSTEM THAT MUST KEEP WORKING: the SeqOB ORDER-BOOK DEX. Rail-blind matching over one unified order book, settled by the seqob WS courier (relay on :9955 and :9965), covering same-chain covenant CLOB trades, cross-chain BTC to asset HTLC swaps, sub-asset (asset over Lightning) swaps, pure-Lightning swaps, and the LSP leg-bridge. Anything these depend on MUST NOT be touched.

⚠ THE TRAP, ALREADY HIT ONCE ON THE SERVER SIDE: the name "xchain" is OVERLOADED. It labels BOTH the retired RFQ rail AND the shared cross-chain HTLC machinery the live order-book courier is built on. On the server, daemon/pkg/xchain/maker.go carries a header comment saying it is "used by the daemon's XchainService" and yet is called by every live courier driver; deleting it by name would have broken the running system. Expect the same overloading in these repos. Files like seqob_xchain_seqleg.rs, btc/htlc.rs and seqdex_htlc.rs are strong candidates for SHARED order-book machinery, not RFQ. NEVER delete on a name match. Establish reachability from real entry points.

METHOD: decide by REACHABILITY, not by naming. RFQ-only means: reachable only from code that calls the retired HTTP/gRPC xchain endpoints listed above. If a symbol is also reached from the order-book path (the covenant CLOB, the seqob courier, the LSP bridge, sub-asset, pure-LN, submarine, or the wallet order-book UI), it STAYS.`

const MAP_SCHEMA = { type:'object', additionalProperties:false, required:['repo','delete_list','keep_list','shared_traps','entry_points','risk'], properties:{
  repo:{type:'string'},
  delete_list:{type:'array',items:{type:'object',additionalProperties:false,required:['path','why_rfq_only'],properties:{path:{type:'string'},why_rfq_only:{type:'string'}}}},
  keep_list:{type:'array',items:{type:'object',additionalProperties:false,required:['path','used_by'],properties:{path:{type:'string'},used_by:{type:'string'}}}},
  shared_traps:{type:'array',items:{type:'string'},description:'files whose NAME says xchain but which the order-book DEX depends on'},
  entry_points:{type:'string',description:'how the order-book DEX is entered in this repo, so reachability was judged against something real'},
  risk:{type:'string'} } }

const DEL_SCHEMA = { type:'object', additionalProperties:false, required:['repo','removed','wiring_edits','builds','build_output','residual'], properties:{
  repo:{type:'string'},
  removed:{type:'array',items:{type:'string'}},
  wiring_edits:{type:'array',items:{type:'string'}},
  builds:{type:'boolean'}, build_output:{type:'string'}, residual:{type:'string'} } }

const VERD_SCHEMA = { type:'object', additionalProperties:false, required:['repo','pass','issues'], properties:{
  repo:{type:'string'}, pass:{type:'boolean'},
  issues:{type:'array',items:{type:'object',additionalProperties:false,required:['severity','where','problem','fix'],properties:{
    severity:{type:'string',enum:['breaks-order-book','build-break','incomplete-removal','correctness']}, where:{type:'string'}, problem:{type:'string',description:'under 300 chars'}, fix:{type:'string'} }}} } }

const REPOS = [
  { name:'SWK', path:'/home/aejkohl/SWK', branch:'sequentia',
    build:'Rust workspace. Build with: cd /home/aejkohl/SWK && cargo check --workspace 2>&1 | tail -40 (add --all-targets if quick). The wasm crate is lwk_wasm. Report REAL output; if a full build is too slow, cargo check the affected crates and SAY SO.',
    notes:'Known xchain-named files: lwk_wollet/src/btc/xchain.rs, lwk_wasm/src/xchain.rs, lwk_wollet/src/btc/mod.rs, lwk_wollet/src/seqdex_htlc.rs, lwk_wasm/src/seqdex_htlc.rs, lwk_wasm/src/lib.rs, lwk_wollet/src/btc/htlc.rs, lwk_simplicity/tests/seqob_xchain_seqleg.rs. SWK is the wallet kit behind the sequentia-web-wallet; its order-book DEX support and its dual-chain BTC support MUST survive. Remember principle: every Sequentia wallet is DUAL-CHAIN (a full Bitcoin parent-chain wallet AND Sequentia side by side), so generic BTC/HTLC machinery is almost certainly shared, not RFQ.' },
  { name:'Ambra', path:'/home/aejkohl/ambra', branch:null,
    build:'Flutter app over a Rust core (ambra_core) via flutter_rust_bridge. Build with: cd /home/aejkohl/ambra && cargo check --manifest-path <the ambra_core Cargo.toml> 2>&1 | tail -40, and for the Dart side cd app && flutter analyze 2>&1 | tail -40. Report REAL output. Do NOT attempt a full APK build. If generated bridge files (frb_generated*.dart, api.dart) reference removed Rust symbols they must be regenerated or hand-corrected consistently; note honestly if regeneration was not run.',
    notes:'Known xchain-named files: app/lib/src/screens/xchain_swap_screen.dart, app/lib/src/screens/xchain_reverse_swap_screen.dart (these two look like RFQ UI), plus app/lib/src/screens/swap_screen.dart, my_orders_screen.dart, lightning_swap_screen.dart and app/lib/src/rust/api.dart + frb_generated.{io,web}.dart (these are very likely ORDER-BOOK or shared, treat with suspicion). Ambra is the Flutter mobile wallet; its order-book DEX and its Lightning swaps MUST survive. Also note a pending separate task will port composer parity into Ambra later, so leave the order-book swap surface healthy.' },
]

phase('Map')
const results = await pipeline(REPOS,
  (r) => agent(`${CTX}\n\nREPO ${r.name} at ${r.path}${r.branch ? ' (branch ' + r.branch + ')' : ''}.\n${r.notes}\n\nMAP THE BOUNDARY, READ-ONLY. Do not edit anything in this stage.\nDetermine, by REACHABILITY from real entry points:\n(a) DELETE list: code reachable ONLY from the retired RFQ xchain endpoints. For each, state why it is RFQ-only (which endpoint it calls, and that nothing in the order-book path reaches it).\n(b) KEEP list: anything the order-book DEX, the covenant CLOB, the seqob courier, sub-asset, pure-LN, submarine, the LSP bridge, or the dual-chain BTC wallet depends on. Name what uses it.\n(c) SHARED TRAPS: files whose NAME contains xchain (or looks RFQ-ish) but which the live order-book DEX actually depends on. This is the single most important output; getting it wrong breaks the running product.\n(d) ENTRY POINTS: state how the order-book DEX is actually entered in this repo, so your reachability judgement is anchored to something real.\nReturn repo='${r.name}'.`, { label:`map:${r.name}`, phase:'Map', schema: MAP_SCHEMA }),

  (map, r) => agent(`${CTX}\n\nREPO ${r.name} at ${r.path}. A read-only boundary map was produced:\n${JSON.stringify(map)}\n\nNOW REMOVE exactly the DELETE list and nothing else. Honour the KEEP list and the SHARED TRAPS absolutely: if you find yourself about to touch anything on those lists, STOP and explain in residual instead.\nAlso remove any now-dead wiring: unused imports, module declarations (mod xchain; etc), exported bindings, dead config/constants, dead UI routes or navigation entries pointing at removed screens, and dead test files that only exercised removed code. Do not refactor anything unrelated and do not rename surviving symbols.\nBUILD IT: ${r.build}\nIf the build cannot be completed in reasonable time, say so honestly in build_output rather than claiming a build you did not run. Report repo='${r.name}'.`, { label:`remove:${r.name}`, phase:'Remove', schema: DEL_SCHEMA }),

  (del, r) => agent(`ADVERSARIAL review of the uncommitted deletion in ${r.name} at ${r.path}. ${CTX}\n\nWhat the removal agent reported:\n${JSON.stringify(del)}\n\nTry HARD to prove the ORDER-BOOK DEX IS BROKEN by this change: trace every order-book entry point (covenant CLOB, seqob courier, cross-chain HTLC, sub-asset, pure-LN, submarine, LSP bridge, dual-chain BTC wallet) and confirm each still resolves to code that exists. Check for dangling imports, dangling module declarations, dangling FFI/wasm bindings, dangling generated-bridge references, dead UI routes that now point nowhere, and anything the removal agent deleted that the map put on the KEEP list. Also check the removal was COMPLETE (no orphaned files left behind that only the deleted code used).\nVerify the build claim independently where feasible. Cite file:line, under 300 chars per issue. Empty array only if genuinely clean. Default pass=false on ANY order-book breakage, build break, or dangling reference. READ-ONLY. Return repo='${r.name}'.`, { label:`verify:${r.name}`, phase:'Verify', schema: VERD_SCHEMA }),
)

return { results }
