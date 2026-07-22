# SeqDEX gap-closure plan (audit of 2026-07-22)

Status: canonical execution plan. Produced by a 53-agent audit (8 subsystem auditors, each
blocker/major finding adversarially re-verified against code and branches, plus a read-only
live-box probe and a completeness critic) against the canonical spec set:
`seqdex-terminal-spec.md`, `seqdex-orderbook-design.md`, `cross-chain-orderbook-consolidation.md`,
`sbtc-peg-design.md`, `simplicity-dex-covenant-offers-design.md`, `seqln-phase2-dex-integration.md`.

When this plan and the terminal spec disagree, the spec wins. Every claim below carries
file evidence in the audit record; severities were confirmed or corrected by an independent
verifier pass, and several initial findings were REFUTED and are not listed here.

---

## 1. Verdict

The web same-chain terminal is genuinely close to spec. The relay core is solid. The gap
is concentrated in four places:

1. **BTC pairs are still pre-rebuild.** Whole-offer takes (the forbidden "buy 10, get 43"),
   market remainders that auto-rest as maker orders, no partial cross fills at the protocol
   level, and the SBTC silent-peg loop broken by a one-sided quote path.
2. **The rail-blind bridged take is broken at four layers** and can produce a FALSE SUCCESS
   in which the LSP executes an unrelated custodial swap with its own funds while the user
   is told "Bridged swap settled".
3. **Matching is advertised but mostly unsettled.** The matcher correctly plans rail-blind
   crosses; only covenant-vs-covenant actually settles. Everything else is an ownerless
   advisory match.
4. **Ambra is a spec-generation behind web**, and the whole terminal-rebuild branch has
   never run on-device.

Plus an operational layer: on the box today, several audited code paths are not what runs
(stale LSP process, covenant watcher and trade log disabled on the production relay, two
LN asset nodes down, the SBTC bridge down, a stranded E2E HTLC awaiting refund), and the
pos_exprace hard fork at height 44300 (~1 to 2 days away) will restart everything and
recreate exactly this degradation unless a runbook exists first.

## 2. Verified working (do not rebuild)

- **Web same-chain terminal (main):** rails start unselected with placement gated (spec
  section 6.5/10), Market genuinely walks the book with partial fills, slippage floor and
  never-rest (`takeMarketWalk`), Limit rests a partial-fillable covenant with
  persist-before-broadcast, the three empty states are distinguished, live WS book with
  reconnect, compact Active-trades strip, honest whole-offer disclosures everywhere they
  still exist, reference-currency input is first-class and bidirectional.
- **Branch state (web):** `terminal-rebuild` is FULLY MERGED into `main` (merge 3c578da6),
  zero commits ahead; retire the branch. Main is ~35 commits ahead of it.
- **Relay/matcher core (seqdex):** price-time priority with FIFO, integer/overflow-safe
  fill math, rail-blind candidate filtering with CrossRail tagging, pure plan+emit design
  (no phantom trade-truth), E2E-encrypted opaque courier, durable cross/LN/covenant offers
  across maker WS drops, signed-cancel replay defense, canonical signing without the float
  footgun, mandatory bounded expiry.
- **Anchor gates on every secret-revealing path** (forward taker, reverse maker + resume,
  submarine both directions, with self-derived block hashes). Anchoring supremacy is
  honored at settlement; the gaps are in book/trade bookkeeping, not in the gates.
- **Covenant CLOB machinery:** plan/fill/remainder re-rest with min_lot floors end to end,
  chain watcher with reorg awareness (when enabled), both-covenant settler run loop.
- **Leg-bridge fund-safety findings 1/2/4/5** present and tested in the files (63/63 unit
  tests pass); findings 3/4/5 are STAGED on the box but not in the running :9981 process.
- **SBTC scope discipline:** the keep-resting toggle appears only for on-chain-BTC-pay +
  LIMIT, default ON; market/LN never touch SBTC; genuine SBTC books and BTC books verified
  not to leak into each other; web+Ambra maker paths and Compages wrap/unwrap complete;
  bridge peg-in/peg-out E2E-proven live 2026-07-20.
- **Ambra branch state:** `terminal-rebuild` is strictly 5 commits ahead of main, jniLibs
  fresh for all 3 ABIs; the 5 commits are additive (no regression to the verified-at-parity
  cross-HTLC/covenant/key-derivation layers).
- **Uncommitted seqdex working tree** = a finished-looking sub-asset SELL crash-recovery
  state-file feature (tests pass, commit-ready). Land it (P1.6).

## 3. Confirmed gaps, by theme

Severity tags are post-verification. "(V)" = independently confirmed; "(A)" = confirmed
with corrections (the corrected form is what is stated here).

### T1. BTC pairs are not first-class (spec sections 2.4, 2.7, 4, 10)

- BLOCKER (V): cross market take is the whole-offer courier; disclosure exists but the user
  cannot take 10 of a 43 (`requoteCross`/`reviewCross`/xswap). Pure-LN and sub-asset SELL
  are whole-offer too; sub-asset BUY has partial end-to-end but the composer's quote painter
  overwrites the typed BTC with the whole offer, making the slice path unreachable (A).
- BLOCKER (V): a cross MARKET order's unfillable remainder is auto-RESTED as a maker order
  (`reviewCross`), directly violating the closed section-10 decision; same-chain gets it right.
- BLOCKER-level protocol root (A): `xlift`/`xsell` hard-code whole-offer TakeAmount, the
  cross maker posts `AllowPartial:false` and quotes full-size terms, and both drivers abort
  on non-whole terms. Partial cross fills need maker + driver + CLI changes. The proven
  pattern already exists in the sub-asset T8 code (`ProportionalBtc`, remainder re-rest).
- MAJOR (V): Limit mode is silently ignored on LN and mixed routes (whole-offer market
  execution at the maker's price while the UI implies a resting order).
- MAJOR (V): no dedicated price field anywhere (limit price only exists as the ratio of two
  amounts); section 6.4 requires a real price input plus a sweep estimate/slippage bound.
- MAJOR (V): IOC/FOK/post-only absent everywhere; relay-side, an `allow_partial=false`
  incoming that cannot fully fill RESTS instead of FOK-canceling, and post-only needs relay
  support to be race-free (A).
- MAJOR: book rows are individual offers with no price-level aggregation on any surface.

### T2. The rail-blind bridged take is broken at four layers (spec section 5)

- BLOCKER (A, the worst finding): the wallet client wrapper `seqlnSwap` (seqln.js:443)
  destructures a fixed field whitelist that silently DROPS `bridge:true`, `btc_node_key`,
  maker-rail fields, `btc_sats`/`asset_atoms` and `taker_seq_refund_pub`. The LSP therefore
  never enters the bridged branch for a wallet request and routes by taker rails into the
  custodial submarine path: with no submarine maker resting it fails in the wrong subsystem
  with a misleading error; with one resting the LSP executes an UNRELATED custodial swap
  with its own funds and the wallet reports "Bridged swap settled" (false success, LSP
  liquidity drained, user got nothing).
- BLOCKER (V): `bridgedTakePlan` ignores the composer amount entirely (whole best offer)
  and reviews in raw atoms/sats with no mismatch warning.
- MAJOR (A): only ONE crossing shape is wired in the LSP (taker sells asset on-chain,
  receives BTC over LN, vs an on-chain reverse maker) and even that shape is reachable only
  from the test harness; `startBridged` has no phase 2 (no self-custody asset HTLC fund, no
  hold registration, no `/bridge/asset`, no anchor-ordering wait). Every other shape fails
  closed AFTER a Review that promised bridging unconditionally.
- MAJOR (V) fund-safety, leg-bridge: no cross-leg locktime-ordering check. A malicious
  maker can set a short T_btc that passes the 6-block runway gate, let the LSP front, refund
  its own BTC HTLC, then claim the asset with P: full-front loss. Not triggerable by the
  honest fleet (T_btc ~16h vs T_seq ~2h) but must close before third-party makers exist.
- MAJOR (V) fund-safety, leg-bridge: `/bridge/asset` accepts on a job whose driver already
  died (maxTicks exhausted, terminal `failed`); the taker can then fund + relay the asset,
  the maker claims it, and no front ever happens. Taker loses the asset absent manual
  operator recoup. Fix: reject on terminal jobs; make maxTicks exhaustion `interrupted`
  (resumable) instead of `failed`.
- MAJOR (V): relay cross-session fragility: role binding is per-WS-connection, no re-attach
  (fresh WS gets 403), sessions in RAM; an hours-long cross settlement dies with any socket
  blip or relay restart. Needs authenticated re-attach-by-session_id + server keepalive.

### T3. The deployed submarine mixed path is custodial (spec section 5 non-custodial)

- BLOCKER (V): `xsubbuy`/`xsublift` (the fallback for two of the four mixed shapes) run
  entirely on LSP-owned wallets/nodes; no user key, address or invoice in the contract.
  The sub-asset BUY and SELL flows ARE non-custodial; only the plain submarine shapes are
  not. These paths must be replaced with self-custody equivalents (the sub-asset pattern)
  or retired into the leg-bridge.
- BLOCKER (V): the book is not ONE: pure-LN offers live on a separate rail-keyed relay
  (:9965) excluded from the unified book, and per-rail requotes gate Review on rail-matched
  books before the rail-blind path can run; a taker's toggle still determines WHICH
  liquidity is reachable.
- MAJOR (V): `MIXED_MAX_0CONF` is compared in three different units (spec: sats; LSP env:
  atoms; wallet sends display units), so the sync-vs-async `/swap` gate misbehaves for
  essentially every trade, and over-cap submarines run synchronously holding the HTTP
  request. One source of truth needed (LSP `/status` advertises the cap in sats).
- MAJOR (V): the JIT pay-leg channel open funds from a 0-conf deposit but does not open
  the channel `mindepth=0`, so it reaches usable only after a Sequentia confirmation while
  the copy promises near-instant. (Check the seqln zeroconf option; else fix the open or
  the copy.)

### T4. Matches are advertised, not settled (matching engine)

- MAJOR (V): only covenant-vs-covenant maker crosses settle (via the settler's own poll).
  Interactive, LN-vs-LN, cross-vs-cross and covenant-vs-LN matches are emitted and dropped;
  crossed books (bid >= ask) persist. The `From.matched` session id is not even registered
  with the router.
- MAJOR (V): `cmd/seqob-bridge run` is a stub; the covenant-vs-Lightning silent bridge has
  no live owner in seqdex (the box's LSP leg-bridge serves interactive rails, not covenant
  crosses).
- Decision needed (critic P2.14): ownerless advisory matches for pure-LN/submarine pairs
  (sub-asset already has a never-auto-cross guard). Either extend the guard or build
  settlement owners; do this BEFORE more matcher work.

### T5. Reorg and finality bookkeeping (Principle 1 in the books, not the gates)

- MAJOR (V): interactive-fill reorg-undo unwired: `NoopReorgWatcher`, logging-only
  onReopen; and `anchorConfs` is hard-coded 0 at SettleAck so a maker-set
  `min_anchor_depth>0` order can never reach FILLED (stuck PARTIAL at zero active).
  Covenants ARE reorg-reconciled when the watcher runs; interactive fills are not.
- MAJOR (A): the trade feed records only interactive same-chain SettleAcks + covenant
  watcher fills. Cross-chain, submarine, pure-LN, sub-asset settlements are never recorded:
  every BTC/LN-only pair reports last_price 0 with an empty chart. (The active_amount half
  is mostly mitigated by maker cancel/requote loops; the feed half is the real gap. Maker
  loops should send SettleAcks, which the relay already accepts for those sessions.)
- No reversal hook subtracts a reorged fill from /trades //candles (append-only ring+log).
- MAJOR (V): the WS book stream interleaves confidential and transparent namespaces to
  every subscriber (REST keeps them disjoint); a live transparent book can render blinded
  offers and vice versa.
- MAJOR (V): oversell accounting absent (adversarial review M1): a maker can rest N offers
  against the same coins; interactive depth can be phantom; the design-doc "serialize lift
  sessions per offer" is not implemented (funds safe, sessions wasted).
- Minor but load-bearing (A): no app-level WS keepalive and no delta sequence numbers; a
  client that misses a >256-event burst renders a silently stale book with no re-snapshot
  signal.

### T6. SBTC silent peg: loop broken at the taker, binding missing (sbtc-peg-design 5/5.1)

- BLOCKER (V): the web reverse QUOTE path (`fetchRQuote`/`findReverseOffer`) reads only the
  `(asset,'BTC')` orientation, so a pegged covenant resting under `('BTC',asset)` is
  invisible to the quote even though the book RENDERS it: Review stays disabled and the
  silent-peg loop cannot complete from the web terminal when the pegged bid is the only
  reverse liquidity (the normal state).
- MAJOR (V): nothing anywhere validates that a BTC-advertised covenant actually locks the
  SBTC asset (`peg_out_on_fill` was replaced by inference). A malicious maker can rest a
  covenant locking a worthless asset advertised as BTC; the taker pays real EURX/GOLD and
  receives junk. Add the asset_a==SBTC check to both wallet taker paths (and ideally the
  relay validator), and update the spec to the implemented `advertise_sell_as` mechanism
  WITH this binding requirement.
- BLOCKER (V): Ambra's pegged-covenant take fills the whole resting offer regardless of
  typed size (the FFI already accepts `takeAtoms`; likely a small fix).
- MAJOR (V) bridge crash-safety: an ambiguous in-process failure (timeout after broadcast)
  deletes the done-sentinel and the next scan repeats the credit/release: double-mint SBTC
  or double-release reserve BTC. And a crash between sentinel and final-txid persist wedges
  the outpoint forever (done with a placeholder). Fix: never delete the sentinel on
  ambiguous errors (verify on-chain before retry); reconcile placeholder-done entries from
  chain state on boot.
- MAJOR (V) ops: the bridge is currently non-operational (its Sequentia wallet unloaded by
  the 2026-07-22 hard-fork cutover restart; process down).
- Minor (A): the SBTC-aware bot maker for BTC books is unbuilt (liquidity depth, not a
  functional hole; BTC books are served by the xmaker HTLC fleet meanwhile).

### T7. Ambra parity (spec section 9.6)

- BLOCKER (V): cross takes whole-offer (forward courier only; reverse HTLC courier not
  built; sells route through pegged covenants only, also whole-offer).
- BLOCKER (V): no book-walking; market takes at most one offer; limit never fills first.
- BLOCKER (V): composer still two code paths (`_crossComposerChildren`): controls vanish on
  BTC pairs, no fee controls there, no price field, and NO reference-currency input at all.
- BLOCKER (A): BTC pairs off the live terminal: no poll (static prices), trades/stats never
  render there (client-side omission; the relay serves them), interactive-HTLC bid rows
  untappable, non-pegged BTC limit unplaceable; empty-book vs relay-unreachable conflated
  on both pair classes.
- MAJOR (V): same-chain rail toggles required then IGNORED (LN/LN settles on-chain);
  quote_asset never threaded into `/swap` (web's asset<->asset pure-LN is absent).
- MAJOR (V): LN readiness reads only own channels; the LSP-frontable verdict
  (web 6737661c/f81f0c48) is not ported: fresh wallets are steered off Lightning, timing
  copy wrong in both directions.
- MAJOR (V): full-screen settlement takeovers persist + a global one-cross-at-a-time lock.
- MAJOR: one-sided same-chain book (no bids/spread/mid), open-orders is a link-out covering
  covenants only (no fill %, no cross/LN entries), and the ENTIRE terminal-rebuild branch
  (incl. the SBTC maker flow that broadcasts real BTC) has never run on-device.

### T8. Live box findings (probe of 2026-07-22 evening)

- :9981 LSP process started 19:16, BEFORE commits 0407fe30/70a7514e existed: leg-bridge
  Findings 3/4/5 are staged on disk, NOT live. Restart `lsp-b5b1` to load them.
- :9955 relay runs WITHOUT `-node-rpc` and WITHOUT `-trade-log`: the covenant watcher is
  DISABLED in production (no chain reconciliation, no partial re-rest, no covenant trade
  recording; consistent with last_price=0 on every market) and trade history dies on
  restart. Also `-offers-per-min 100000` (rate limits effectively off).
- ln-asset AND ln-asset-b are down (no lightningd process, stale pid files); consequently
  the :9966 and :9971 relays serve EMPTY market lists (sub-asset rails dead) and the
  bridge's front-ln failed with "getroute: Could not find a route".
- E2E `final8` FAILED no-loss at 23:34: handshake + both fundings + relay all succeeded,
  the LSP front wedged on getroute (~3s retry loop, then silence), the maker never claimed;
  the taker's 13,290,703,044-atom asset HTLC `00ecb6e3...:0` refunds at T_seq 42506 (chain
  was 42470 at probe time; the window is open NOW). Confirm which agent refunds the TAKER
  side (xresume covers maker sessions) and recover it.
- TWO concurrent `supervise-xresume.sh` instances (duplicate settler supervisor); ~146
  churning cross makers.
- A second, non-systemd lsp-server.mjs on :9982 from an old ssh scope (kill-or-adopt; check
  the Caddyfile route table to be sure which processes serve `/lsp` and `/seqob`).
- sbtc-bridge process down; its Sequentia wallet not loaded.
- Chain at ~42470 and producing; pos_exprace hard fork at 44300 is ~1830 blocks (~40h)
  away and its restart will recreate this degradation without a runbook.

### T9. Unbuilt/undecided product surface (critic findings)

- A maker/taker DEX fee subsystem does not exist anywhere (spec sections 4/8 mention the
  distinction). Decision: either declare "no DEX trading fee, chain fees only" and amend
  the spec, or design a fee schedule (offer field + review display + enforcement).
- Confidential book cannot have durable resting orders by construction (validator forbids
  covenant/cross/LN terms for confidential offers; interactive offers evict on disconnect).
  Decide: documented online-only, or a blinded-covenant design later.
- Trade history is local-only on web, absent per-user on Ambra; no export, lost on
  reinstall.
- No markets-overview surface for pair discovery with liquidity/volume.
- No notifications for needs-action states (refund windows opening, fills while away,
  resting-order expiry).
- Reference-rate staleness is invisible (localStorage cache with no badge).
- Shared constants (MARKET_SLIP 15%, min_lot 0.1%, front cap, 0-conf cap) are independent
  literals per surface; one source of truth needed (LSP /status).
- Multi-device same-seed operation is unconsidered (cancel nonces, per-device stores).

---

## 4. The plan

Ordering rationale: (P0) make what exists run and survive the fork, and recover funds;
(P1) close every fund-safety and false-success hole before more E2E; (P2) make BTC pairs
first-class, which is the largest visible UX gap; (P3) make rail-blind settlement real;
(P4) bring Ambra to the same terminal and ship once; (P5) product polish + spec decisions.
Each item names its repo(s). Deploy pipeline as always: laptop commit -> push -> box pull
-> build on box; full-fleet cutovers only.

### P0. Stabilize + recover (box ops; ~hours, do first)

1. Recover the final8 taker asset HTLC (T_seq 42506 has passed): drive the taker refund
   from the harness's persisted state; then sweep for any other stranded HTLCs from
   final2..final7. Add a stranded-HTLC detector to the health probe (item 6).
2. `systemctl restart lsp-b5b1` to load staged Findings 3/4/5; fix its logging (journal to
   a file, not a dead socket); re-run one bridge E2E only AFTER P1.1-1.2 land.
3. Bring ln-asset + ln-asset-b back with a CONSISTENT seqln install (the asset-bin
   isolation pattern; per the binary-upgrade memory, no DB surgery), verify holdinvoice
   loads from config, and confirm the :9966/:9971 books repopulate. This also removes the
   front-ln getroute wedge cause.
4. Dedupe supervise-xresume (kill one instance; make it a systemd unit with a single
   owner). Reconcile the ~146-maker churn (requote-exit is expected; crash-looping is not).
5. Restart the sbtc-bridge with its wallets loaded; add wallet-load to its unit's
   ExecStartPre so the next node cutover cannot silently kill the peg.
6. Redeploy :9955 seqobd WITH `-node-rpc` (covenant watcher ON in production) and
   `-trade-log` (durable trade history); sane rate limits. Verify covenant partial re-rest
   and trade recording live.
7. Kill-or-adopt the rogue :9982 LSP after checking the Caddyfile route table; document
   the intended service topology (which relay port is canonical for what; :9945 seqdexd is
   legacy RFQ pending retirement) and diff reality against it.
8. Health monitor: a cron probe (ports up, wallets loaded, per-relay book non-empty, LSP
   /status ok, LN nodes up, bridge up, stranded-HTLC scan) writing a status endpoint the
   box already serves; alert = a red line on /status the user can see.
9. WRITE THE HARD-FORK BRING-UP RUNBOOK before height 44300: ordered restart checklist
   per service class (committee -> dexnode wallets incl. sbtc-bridge + bridge-taker ->
   LN fleet consistent binaries -> relays with correct flags -> LSPs -> maker fleets ->
   seeding), each step with its verify command. Execute it at the fork.

### P1. Fund-safety + false-success closure (before any further E2E or exposure)

1. leg-bridge locktime-ordering gate (wallet repo, tooling/lsp): at handshake refuse maker
   terms unless T_btc wall-time exceeds T_seq wall-time + the taker hold's remaining life +
   a claim margin (propose 6 BTC blocks; document the policy in-code). Unit tests.
2. leg-bridge job-liveness: `/bridge/asset` rejects when job.status is terminal; maxTicks
   exhaustion marks `interrupted` (resumable, driven by resume-on-boot), never `failed`.
3. Kill the seqlnSwap whitelist misroute (wallet): forward `bridge`, `btc_node_key`, maker
   rails, `btc_sats`/`asset_atoms`, `taker_seq_refund_pub`, `taker_*_inbound`; and make the
   LSP `/swap` REFUSE (422) a rail-crossed request that lacks `bridge:true` instead of
   falling through to the custodial submarine path. This closes the false-success hole even
   before the full bridged path lands.
4. SBTC bridge crash-safety (sbtc-bridge repo): keep the done-sentinel on ambiguous
   failures and verify on-chain before any retry; boot-time reconciliation of
   placeholder-done entries from chain state; never delete a sentinel in a catch block.
5. SBTC mis-sell binding (both wallets + relay validator): a taker path may only treat a
   BTC-advertised covenant as pegged BTC if `covenant.asset_a == SBTC id`; otherwise refuse
   the row. Update sbtc-peg-design 5.1 to the `advertise_sell_as` mechanism with this
   binding as a MUST.
6. Land the uncommitted seqdex sub-asset SELL state-file work (tests already pass).
7. Relay WS namespace filter (seqdex): subscribers receive only their chosen namespace's
   snapshot+deltas.

### P2. BTC pairs first-class (the big UX win)

1. Fix the web reverse-quote orientation (wallet): `fetchRQuote`/`findReverseOffer` must
   merge BOTH orientations like the render does, restoring the covenant branch and the
   silent-peg taker loop. Then run the full SBTC loop E2E on the box (maker rest -> taker
   cross -> settle -> both peg-outs) and verify orientation/units live.
2. Cross MARKET semantics (wallet): remainder is CANCELED, never auto-rested (align with
   same-chain `takeMarketWalk`); walk multiple cross offers best-first when size spans
   offers; slippage bound surfaced.
3. Partial cross fills (seqdex protocol, the T8 pattern): maker quotes proportional terms
   for a requested slice and re-rests the remainder (`ProportionalBtc`/mulDiv64 already
   exist); `xlift`/`xsell` gain `-amount`; drivers accept proportional terms bound to the
   signed offer's ratio. This kills "10 -> 43" on today's BTC liquidity. Covenant/SBTC
   resting remains the destination for offline durability; this makes the interactive
   fleet spec-compliant meanwhile.
4. Sub-asset BUY composer slice (wallet): stop overwriting the typed BTC with the whole
   offer; the end-to-end partial machinery already exists.
5. Limit semantics on every route (wallet): Limit on LN/mixed routes must rest (or be
   honestly gated), never silently market-execute; BTC-pair native limit (toggle OFF)
   rests the exact-amount HTLC offer; fills-then-rests where a crossing offer exists.
6. Price field (wallet, both): a real always-present price input (Limit: user's price,
   default = inside; Market: sweep estimate + slippage bound), driving the amount ratio.
7. Ladder aggregation (wallet, both): aggregate same-price rows into levels with cumulative
   depth (client-side; the relay serves the flat book).
8. Trade-feed coverage (seqdex): cross/LN maker loops send SettleAck at settlement (the
   relay already accepts it for their sessions) so BTC/LN pairs get last price + candles;
   `anchorConfs` advancement wired so `min_anchor_depth>0` fills can reach FILLED.
9. Order options (relay + wallets): additive offer fields for IOC/FOK/post-only; matcher
   honors FOK-cancel and post-only-reject; composer exposes them as Limit options.

### P3. Rail-blind settlement made real

1. Wallet bridged take done right: sized to the user's amount (uses P2.3 partials),
   formatted review with mismatch warning, full field forwarding (P1.3), phase 2 ported
   from the harness (self-custody asset fund, hold registration, `/bridge/asset`,
   anchor-ordering wait: wait for the maker's BTC HTLC confirmation BEFORE funding the
   asset), resume across reloads, `taker_seq_refund_pub` always sent.
2. LSP shape coverage: wire the remaining crossed shapes through the leg-bridge; until a
   shape is wired the composer must know (an LSP capabilities read) and fall back to the
   native path at the same price instead of promising a bridge that will fail post-confirm.
3. Retire the custodial submarine shapes: replace `xsubbuy`/`xsublift` dispatch with
   self-custody equivalents on the sub-asset pattern (device keys/invoices), or route those
   shapes through the leg-bridge; delete the custodial path when both shapes are covered.
4. One book completed: fold the pure-LN relay's offers into `/book/unified` and let the
   rail-blind take reach them; per-rail requotes stop gating Review when the unified book
   has crossable liquidity on another rail.
5. 0-conf cap single source of truth: LSP `/status` advertises the cap in sats; wallet
   reads it; LSP env documented in sats; CLI `-max-0conf` unit aligned and tested.
6. JIT pay-leg truly 0-conf (`mindepth=0` if the seqln zeroconf path allows; else honest
   copy).
7. Matching ownership decision executed (see section 5 decision 3): guard ownerless
   interactive crosses now (extend the sub-asset never-auto-cross guard to pure-LN and
   submarine pairs); build the covenant-vs-LN settlement owner (either `seqob-bridge run`
   subscribing to CrossRail matches, or the LSP leg-bridge consuming them) so the silent
   bridge exists.
8. Relay session re-attach by session_id (authenticated by the session pubkeys) + server
   keepalive + WS delta sequence numbers with a re-snapshot signal.
9. Interactive reorg-undo (seqdex): real reorg watcher wired (re-open offers, restore
   active_amount, retract trades from ring+log with a reversal record).
10. Oversell soft accounting per maker (relay) + serialize lift sessions per offer.

### P4. Ambra to the same terminal (one-shot ship at the end)

1. Truly one composer: kill `_crossComposerChildren`; all controls on all pairs (fee
   controls, price field, reference-currency input, blinded toggle where applicable).
2. Book-walking taker + partial pegged takes (`takeAtoms` FFI) + partial cross once P2.3
   lands; two-sided same-chain book with spread/mid; BTC pairs on the live poll with
   trades/stats; tappable bid side; empty-state split (unreachable vs empty).
3. LN verdict = LSP-frontable (port web 6737661c + f81f0c48 + 68dc371d wording); same-chain
   pure-LN with quote_asset threading through LspClient and `/swap`.
4. Compact in-flight strip without navigator takeover; keyed multi-record XchainStore
   (relax the global one-cross lock to same-UTXO/channel gating only).
5. Open-orders in the terminal (fill %, cross/LN entries, reclaim reachable).
6. Reverse (sell) courier or covenant-only sells made first-class; whichever P2 lands as
   the sell path, Ambra matches it.
7. FULL on-device E2E of everything in the branch (SBTC maker flow especially), then merge
   `terminal-rebuild` -> main, build + sign + deploy the APK ONCE, per the
   never-claim-done-without-runtime-test rule.

### P5. Product completion (after or alongside P4)

1. Trade history: persisted per-fill receipts (price/size/fee/rail/txids) on both
   surfaces; export; survives reinstall where possible (server-side only if user opts in).
2. Markets overview surface (pairs with liquidity/volume/last/spread).
3. Notifications: refund-window-open nags (T_seq/T_btc), fill alerts, resting-order expiry
   + renewal UX.
4. Reference-rate staleness badge; shared constants served by LSP /status and read by both
   wallets.
5. Spec amendments: fee decision (section 5 below), advertise_sell_as (P1.5), confidential
   book durability wording, multi-device statement, the section-7 mobile exception if the
   single-cross lock is deliberately kept anywhere.
6. Retire legacy: the seqdexd RFQ surface (:9945) once Ambra's last dependency is gone
   (consolidation doc phase 4); retire the web `terminal-rebuild` branch now (merged).

## 5. Decisions needed (Andreas)

1. **DEX fee schedule.** Nothing exists (no maker/taker fee anywhere). Options: (a) declare
   "no DEX trading fee; chain/LN fees only" and amend the spec (fastest, honest); (b)
   design a fee schedule now (offer field, review display, enforcement, who collects).
   Recommendation: (a) for testnet now; revisit before mainnet.
2. **Confidential resting orders.** By construction the blinded book is online-only today.
   Options: document online-only for v1 vs design a blinded-covenant tier later.
   Recommendation: document online-only; revisit with the Simplicity tier.
3. **Ownerless matches.** Guard pure-LN/submarine interactive pairs from auto-cross (like
   sub-asset) until settlement owners exist, vs building the owners first.
   Recommendation: guard now (small), build the covenant-vs-LN owner in P3.7, revisit
   interactive-vs-interactive auto-cross only when a settler exists.
4. **Locktime margin policy** for the leg-bridge (P1.1): confirm the proposed rule
   (T_btc wall >= T_seq wall + hold life + 6 BTC blocks) or set different margins.
5. **Multi-device same-seed**: declare unsupported for now (recommendation) or scope the
   work (nonce coordination, shared stores).

## 6. Execution notes

- Verification bar: every P1+ item lands with unit tests where the layer has them, and
  every settlement-path change gets a live box E2E before its rail is called done. No
  "done" without runtime proof on the surface it ships to (Ambra = on-device).
- The web wallet keeps deploying continuously from `main` as it has been; the ONE-SHOT
  discipline applies to the Ambra terminal ship (P4.7) and to announcing the SBTC feature
  (after P2.1's full-loop E2E).
- Order within phases is written in dependency order; P0 and P1 are strictly sequential
  before any new bridge E2E; P2 items 1-5 are independently shippable behind the existing
  UI.
