# Sequentia UX Audit and Design-Change Spec (2026-07-02)

Handover document for an implementing session. Six user-facing surfaces were audited adversarially at
the code level ("what does an average user expect, and is it there?"). This spec collects every design
change, ranked, with file:line evidence and a concrete fix, plus the systemic themes that let one fix
resolve many findings.

> **Lightning pass added 2026-07-04.** Pure-LN and submarine asset<->BTC-LN swaps are now proven live end
> to end (seqdex `phase3-pure-ln`, M0-M5, including a real Bitcoin testnet4 leg), but no wallet can reach
> them yet. Section 8 (new) specifies bringing Lightning to the Web wallet, Ambra, and the DEX under the
> LSP / hosted-SeqLN model (we run SeqLN; users do not run a node, Phoenix-style), with cross-cutting theme
> T16, per-surface additions in 4.1-4.3, and punch-list Tier G. Start with 8.9's custody/trust decision.

> **Implementation status (2026-07-04, in progress).** Being implemented on feature branches (NOT merged;
> pending browser/device verification). Custody/trust decision (8.9): RESOLVED — Tier-1 dropped, Tier-2 via a
> **CLN native signer split** (device holds keys, we host the node). Progress:
> - **Lightning backend — COMPLETE + proven** (`seqln` `sequentia-stable`, committed): the non-custodial signer
>   split M0-M5 (Rust device signer byte-exact vs libhsmd), the capstone (a device-keyed hosted-channel node did
>   a real pure-LN GOLD<->BTC seqob trade), the secure BOLT-8 Noise_XK transport, the WASM browser signer, and a
>   WebSocket transport + wallet SDK (`contrib/seqln-signer/wasm`). See `seqln-tier2-hosted-channels-design.md`.
> - **Web wallet non-LN** — DONE (branch `claude/lightning-ux-overhaul`, pushed): T2, T13, T5, T6, T9, T7, T14,
>   T11, T12 + 4.1/4.2 cleanups. Flagged browser-verify: T4 (PSET fee preview), T5 Max-with-same-asset-fee, T3
>   (needs the qr.rs wasm rebuild).
> - **Web wallet Lightning UI** — DONE (same branch): the "Instant (Lightning)" Swap rail + on-device wasm
>   signer connect on unlock + `seqln.js` LSP client, and a runnable hosted-LSP service (`tooling/lsp/`). PROVEN
>   in Node 11/11: the wallet's SDK connects the device signer, a keyless hosted node boots, and a pure-LN
>   GOLD<->BTC buy settles ~2.1s with the device co-signing and REAL per-asset movement. Needs for LIVE: a
>   deployed hosted-LSP endpoint (`SEQ_LSP_URL`) + a real browser for the DOM/wss/IndexedDB. Requirement found:
>   hosted asset channels must be ANNOUNCED (not private) for the taker's asset-getroute.
> - **Ambra non-LN** — DONE (branch `claude/lightning-ux-overhaul`): the T10 seed/app-lock P0s (FLAG_SECURE,
>   lock pops sensitive sheets, onboarding lock default-on), T1 (fee_asset via the Rust FFI), T6 safe subset
>   (cached BTC balance + offline marker + Lock-BTC auth + faucet guidance), T9 (registry service), T12 copy.
>   Deferred: BTC tx history (needs a new FFI + esplora scanning), fee-amount in issue/burn/stake dialogs.
> - **Explorer** — DONE (branch `claude/ux-audit`, pushed): T2, T1, T9.
> - **Bridge** — DONE (branch `claude/ux-audit`, pushed): T8 (0-conf delivered honesty), T4, T11, T12.
> - **Remaining:** Ambra Lightning (needs the signer as a Rust FFI, mirroring the WASM build), the Qt surface
>   (4.4), the hosted-LSP box deploy (for a live demo), then browser/device verification + merge.

## 0. How to use this document

- Work section 3 (cross-cutting themes) FIRST. Those are root-cause fixes that clear findings across
  multiple surfaces at once (for example the any-asset-fee display gap and the BTC price-key bug).
- Section 4 is the full per-surface finding list (the reference detail; nothing is dropped).
- Section 5 is a single global punch list in execution order with acceptance criteria.
- Section 6 lists what is already correct; do not regress these while fixing.
- Severity: P0 = user blocked or actively misled at an irreversible action; P1 = major expectation
  violation or a project-rule violation; P2 = minor; P3 = polish.
- These are testnet surfaces. Several fixes are data/deploy fixes on the box, not code (flagged).

## 1. Surfaces, repos, deploy

All repos are `github.com/GracedEternalKingCabbageMan/<name>`; develop on the laptop, push, pull on the
box. The box now serves everything over HTTPS at `https://sequentiatestnet.com` (Caddy -> serve-public.js
on :8080); see [[sequentia-server-deploy]].

| Surface | Repo (branch) | Source of truth | Served at |
| --- | --- | --- | --- |
| Web wallet | `sequentia-web-wallet` (main) | `index.html` (all app logic + copy, ~2021 lines), `swap.js`, `seqob.js`, `btc.js`, `xswap.js`, `xrswap.js`, `xmaker.js`, `xcourier.js` | `/wallet/` |
| Wallet kit (LWK) | `SWK` (sequentia) | `lwk_common/src/qr.rs`, `lwk_wasm/src/*` (the wasm the wallet calls) | rebuilt into `/wallet/pkg` |
| Explorer + landing | `sequentia-explorer` (main) | `esplora/client/src/**` (built by `build-public.sh`), `downloads/index.html`, `serve-public.js` (routes/faucet) | `/explorer/`, `/testnet4/`, `/download/`, `/` |
| Bridge UI | `compages` (main) | `web/index.html`, `web/app.js`, `daemon/lib/*.js` (user-visible API strings), `contracts/src/CompagesVault.sol` | `/bridge/` |
| Desktop GUI | `SequentiaByClaude` (claude/...) | `src/qt/**`, `src/qt/forms/*.ui` | Core download |
| Ambra mobile | `ambra` (main) | `app/lib/**` (Dart UI), `ambra_core/src/api/mod.rs` (Rust API), `app/android/**` | APK on `/download/` |
| Registry data | `sequentia-registry` (main) | `seed/legacy-assets.json` (correct) + the DB row on the box (some wrong) | `/registry/` |
| Hosted SeqLN LSP (NEW) | `seqln` + `seqdex` (`phase3-pure-ln`) | SeqLN nodes on both networks + `seqob-maker -mode pureln\|lightning` + a Boltz-shaped swap gateway | behind `/lsp` for web + Ambra |

## 2. Global severity tally

- P0 (11): DEX 4, Ambra 2, Qt 2, Bridge 2, Explorer 1.
- P1 (approx 45), P2 (approx 55), P3 (approx 40).
- The heaviest surfaces are the DEX Swap tab (thin snapshot book bolted onto a pay/receive composer)
  and the Qt/Ambra fee-display layer (any-asset-fee engine correct, display still assumes tSEQ).
- Section 8 (NEW): Lightning integration across all surfaces. Net-new capability (proven at the daemon,
  absent from every wallet), so it grades mostly P1; the single P0 is LN finality + custody honesty.

## 3. Cross-cutting themes (fix once, resolve many)

**T1. Any-asset fee/amount is displayed as tSEQ (the display layer still assumes one asset).**
The fee engine is correct everywhere; the presentation is not. Hits: Qt send-confirm labels a non-tSEQ
fee as tSEQ and folds it into a bogus "Total Amount" (`SequentiaByClaude/src/qt/sendcoinsdialog.cpp:403,419`);
Qt tx-details Debit/Credit/Net rows all tSEQ (`transactiondesc.cpp:233-283`); Ambra history hardcodes the
fee label "tSEQ" (`ambra/app/lib/history_screen.dart:271`, needs `fee_asset` added to `TxRow` in
`ambra_core/src/api/mod.rs:569`); explorer mempool total shown as tSEQ + "sat/vB"
(`sequentia-explorer/esplora/client/src/views/mempool.js:22,42,50`); Qt PSET fee row hardcoded tSEQ
(`psbtoperationsdialog.cpp:196`). Root fix: a per-surface helper that formats any amount with its own
asset ticker/precision and a reference-value suffix, used at every fee/total/debit/credit sink.

**T2. BTC price-key mismatch (`WBTC` vs `BTC`).** The live `/prices` feed keys Bitcoin as `BTC`, but code
looks up `WBTC`, so the BTC row is unvalued and choosing "BTC" as the reference currency silently blanks
every approximate value app-wide. Hits: web wallet (`sequentia-web-wallet/index.html:677-688`), explorer
(`esplora/client/src/views/util.js:132`), Qt (`src/qt/guiutil.cpp:780,808`). Root fix everywhere:
`prices['BTC'] || prices['WBTC']`, and when the chosen reference has no price fall back to USD with a
visible notice instead of blanking.

**T3. Sequentia addresses carry the Liquid `liquidnetwork:` URI scheme.** Receive QR codes brand Sequentia
as Liquid and hand external scanners a wrong-network URI. Shared root: `SWK/lwk_common/src/qr.rs:19`
(web wallet, `index.html:1392`). Qt has its own copy plus scheme soup: `formatBitcoinURI` emits
`liquidnetwork:` (`src/qt/guiutil.cpp:213`), parser accepts only liquid (`guiutil.cpp:147`), paymentserver
registers `bitcoin:` (`paymentserver.cpp:40`), Open URI placeholder says `bitcoin:` (`forms/openuridialog.ui:29`).
Root fix: one `sequentia:` scheme constant used by format + parse + paymentserver + placeholder in each
codebase; emit the bare address (or `sequentia:`) from `qr.rs` for Sequentia params.

**T4. Fee/cost is shown only AFTER commit (no pre-submit preview).** Hits: web wallet default Send confirm
shows no fee at all because `psetDetails` throws on the transparent-by-default explicit outputs
(`sequentia-web-wallet/index.html:1609-1635`); Ambra rescue broadcasts with no preview
(`ambra/app/lib/rescue_screen.dart:172-230,416-465`); Qt CPFP broadcasts with no cost dialog
(`src/qt/walletmodel.cpp:749-784`); bridge shows no fee/receive/ETA before signing
(`compages/web/index.html`, `web/app.js`); DEX cross composer shows "Maker fee 0 BTC" until after terms
(`sequentia-web-wallet/xswap.js:306`). Root fix: every money-moving flow computes and shows the fee (in
its asset) and the total before the irreversible action.

**T5. No visible balance and no Max in order entry / send.** Hits: DEX composer never validates pay+fee
against balances and Max ignores the fee (`sequentia-web-wallet/swap.js:358-495`); web wallet Send has no
Max and no available-balance line (`index.html:1535-1539`); Qt max-send with a same-asset fee is a dead end
(subtract-fee disabled for non-policy assets, `src/qt/sendcoinsentry.cpp:129-134`); Ambra has no Max
(`ambra/app/lib/send_screen.dart`). Root fix: show "Available: X TICKER" by the amount field and a Max that
subtracts the fee when the fee asset equals the sent asset.

**T6. BTC is second-class, violating the dual-chain principle.** Hits: web wallet has no BTC transaction
history (`index.html:1783-1823` renders only Sequentia txs) and no BTC explorer link; Ambra has no BTC
history/explorer link, silently hides the BTC balance when the testnet4 scan fails while the sync chip
stays green (`ambra/app/lib/shell.dart:106-112`), gives no BTC faucet guidance, and its cross-chain "Lock
BTC" skips payment auth entirely (`xchain_swap_service.dart:226-242`). Root fix: BTC history via esplora
testnet4 `/address/{a}/txs`, a testnet4 explorer URL (`/testnet4/tx/...`), cached BTC balance with an
offline marker, BTC faucet guidance, and `requirePaymentAuth` on the BTC lock.

**T7. Empty market vs relay outage vs unfillable offer (the "never say no price" directive).** Hits: DEX
swallows book-fetch failures to an empty book and then invites posting into the void
(`swap.js:413-415,556`); DEX same-chain "start this market" posts offers no wallet can fill yet (no
in-wallet co-sign responder, `seqob.js:17-21`) while toasting "your market is live" (`swap.js:1113`);
Ambra Swap still uses the retired RFQ daemon and dead-ends on "no price"
(`ambra/app/lib/swap_screen.dart`). Root fix: distinguish fetch-failure ("order book unreachable, retry")
from genuinely empty, only invite first-maker on a real empty book, and either ship the in-wallet
responder (with a keep-tab-open warning) or stop posting unfillable same-chain offers and tag them
"maker offline" so takers fail fast.

**T8. "Delivered"/"final" claimed before it is true (0-conf and anchor honesty).** Hits: bridge marks a
deposit "delivered" at 0-conf with no mempool check on the final send
(`compages/daemon/lib/bridge.js:462-498` vs the guarded issue/burn steps) and its redeem panel copy
contradicts the real ~100-anchor-confirmation release wait, disclosed only after funds are committed
(`compages/web/index.html:332-333` vs `daemon/lib/bridge.js:670-675`); DEX under-warns the taker about the
keep-tab-open requirement. Root fix: never present unconfirmed as final; state the real release gate and
an ETA before the user commits; add `waitWalletTxVisible` to the bridge final send.

**T9. Unknown-asset handling (precision, hex-as-ticker, registry never fetched).** Hits: explorer prints
raw 10^8-scaled atoms because electrs returns no `precision` and the page uses it anyway
(`esplora/client/src/views/asset.js:184-222`, fix: use the registry `disp_precision`); web wallet
reissue/burn skip the unknown-precision guard the Send path has (`index.html:1763-1777`) and there is no UI
to label a received asset even though sends require it ("label it before sending" dead end,
`index.html:612-615`); Ambra fabricates a ticker from hex and assumes precision 8, never fetching the
registry it already has wired (`ambra/app/lib/config.dart:23,83-90`); Qt shows 64-char hex ids in combos
with an 8-decimal hardcode (`src/qt/guiutil.cpp:750-758`, `assetspage.cpp:236`). Root fix: fetch and cache
the registry client-side on each surface, use its precision for every amount, elide hex ids, and add a
"label this asset" affordance in the wallets.

**T10. Seed and app-lock security UX.** Hits (Ambra P0s): the app lock only swaps the navigator base route,
so pushed sheets survive it and the recovery phrase can sit ON TOP of the lock screen after backgrounding
(`ambra/app/lib/main.dart:50-87`, `shell.dart:567-583`); no `FLAG_SECURE`, so seed screens leak into
screenshots and the recents thumbnail. Web wallet P1: seed backup has no verification step and the phrase
lives in plaintext localStorage with no copy button (`index.html:201-209,1231,1681`). Ambra P1: onboarding
never offers to enable the lock and it defaults off (`wallet_repository.dart:26`). Root fixes: pop-to-first
(or overlay the lock above the Navigator) on lock + FLAG_SECURE on seed screens; a spot-check backup step;
an "enable app lock?" onboarding step defaulting on.

**T11. Reload/resume safety mid-flow (fund-loss risk).** Hits (DEX P0s): wallet-maker settlement is
persisted "for resume" but nothing replays it on boot, so closing the tab mid-settlement can lose funds
with no UI (`sequentia-web-wallet/xmaker.js:120-122` persisted, never re-launched); the forward taker is
never told to keep the tab open and an interrupted swap strands BTC ~16h with no guided recovery
(`xswap.js:424-441,511-529`); bridge refresh loses all tracking though the API supports lookup by tx hash
and redemption address (`compages/web/app.js:253,279`). Root fix: on load, rehydrate and re-launch any
non-terminal settlement/refund watcher, add manual lookup fields, and make every interrupted state explain
itself and the recovery path.

**T12. Copy rules.** No "SEQ" for the network in user-visible text (Ambra:
`xchain_swap_screen.dart:312,317,382`, `xchain_swap_service.dart` error strings say "SEQ leg/claim/anchor";
fix to "Sequentia ..."). No em dashes in user-visible strings (DEX: 61 in swap.js, 49 in xswap.js, 36 in
xrswap.js, 14 in xmaker.js; Ambra: `swap_screen.dart:711`, `xchain_swap_screen.dart:392`; bridge README).
Qt strings in `tr()` are clean but ship "LOSE ALL OF YOUR BITCOINS" (`askpassphrasedialog.cpp:111`), a
"bitcoin"-labeled native asset row (`assetspage.cpp:182`), "satoshi(s)" (`coincontroldialog.cpp:554`), and
Elements attribution in About (`configure.ac:9`, `clientversion.cpp:94`). No headline privileging of SEQ:
web wallet pins tSEQ to the top of balances and gives it a unique sub-line (`index.html:1368-1371`); Qt
Overview lists native first (`guiutil.cpp:859`). Root fix: a copy sweep per surface plus the equal-standing
layout changes below.

**T13. No headline portfolio total in the reference currency (equal-standing rule).** Hits: web wallet
Balance opens straight into the asset list with no total (`index.html:242-249,1353-1374`); Qt Overview shows
a stack of per-asset lines with the total as a trailing line styled identically and native pinned first
(`overviewpage.cpp:249-266`). Root fix: a headline card = sum across BTC + all assets in the reference
currency, per-asset rows beneath in a uniform order, nothing pinned; staking note only in the Stake tab.

**T14. The order book is not an order book.** DEX-specific but foundational: one-sided snapshot with no
spread/mid, no depth, no last price, no live updates, no limit orders when the book is non-empty, silent
size capping, and zero-receive quotes (`swap.js:403-495,1128-1146`). Root fix: render a two-sided book with
spread/mid/depth from data already fetched, subscribe to the relay `public_book` WS frames, add a persistent
"post a limit order at my price" action, and add dust/zero-receive/oversize guards.

**T16. Lightning swaps are proven at the daemon but no wallet can reach them (the taker still runs a SeqLN node).**
The pure-LN and submarine lanes settle live through the seqob relay + encrypted courier (seqdex `phase3-pure-ln`,
M0-M5, ~2.1s both directions, no anchor wait on the happy path), but every wallet is LN-blind: the web Swap cannot
even parse an LN offer (`seqob.js:245`, field 22 unencoded), Ambra Swap is still on the retired RFQ model, and the
taker itself needs `-asset-ln-socket` + `-ln-socket` (`seqdex/cmd/seqob-cli/xpln.go`) that a browser wasm wallet and
a Flutter phone do not have. Root fix: host SeqLN for web + Ambra users (LSP / Phoenix model), so the taker needs no
local node. Full architecture, honest custody tiers, finality wording, and the per-surface build: section 8.

## 4. Per-surface findings (full detail)

### 4.1 DEX / Swap tab (`sequentia-web-wallet`: index.html + swap.js + seqob.js + xswap.js + xrswap.js + xmaker.js + xcourier.js)

P0

- Zombie same-chain market: "start this market" posts offers no wallet can fill (no in-wallet co-sign
  responder, `seqob.js:17-21`) while toasting "your market is live" (`swap.js:1113`); other users' takers
  hang for minutes then fail (`seqob.js:428-460`). Fix: ship the responder with a keep-tab-open warning, or
  stop posting unfillable offers, tag them "maker offline", and make takers fail fast.
- Forward taker "Anchor verified" gate trusts the maker's claimed anchor height: `verifyAnchor` uses
  `SWAP.seq_leg.anchor_height` straight from the maker's message (`xswap.js:446,492-503,826-835`) then prints
  "Anchor verified" (`xswap.js:917`). Self-derivation already exists in `xmaker.js:65-82`. Fix: fetch
  `/anchor/<block_hash>` + `/anchorstatus` yourself, require self-derived height >= btc-leg height and
  status ok, fail closed if unreachable, and label "verified against your own node".
- Wallet-maker settlement persisted but never resumed on boot (`xmaker.js:120-122`); XMAKE is a module var
  cleared on reload, and Cancel kills the watcher mid-lift (`swap.js:42,971-975`). Fund-loss. Fix: rehydrate
  and re-launch `settleMakerForward`/reverse-refund watchers on load; disable Cancel once a lift passes
  terms.
- Forward taker never told to keep the tab open; interrupted swap strands BTC ~16h with a silent "Pending"
  and no recovery (`xswap.js:424-441,511-529,1131-1137`). Fix: keep-tab-open warning in the lock modal,
  courier session re-open on resume, explicit refundable-after-block-N state.

P1 (order-entry and book table stakes)

- No balance validation in the composer; failures surface as raw errors after Confirm; Max ignores the fee
  (`swap.js:358-495,1002-1004`).
- A quote can round the receive leg to zero and still be executable (`swap.js:477-480`).
- Oversized amounts silently capped to the best offer; typed field and quote disagree; a `capped` flag is set
  and never read (`swap.js:478,494`).
- Take-only: no way to place a resting limit order once liquidity exists (`swap.js:434-449`).
- Relay outage indistinguishable from an empty market; invites posting into the void
  (`swap.js:413-415,556`).
- Book is a one-shot snapshot: no WS updates, no refresh, no staleness indicator (`swap.js:403-431`).
- One in-flight cross swap hijacks the whole tab for hours (`swap.js:147-156,174-178`).
- Maker-listener disconnect leaves a phantom "Resting" panel (`xcourier.js:272`, `swap.js:953-968`).
- Empty-book flows ask for "an amount" but require two, then scold (`swap.js:501,438,1080`).
- Can post same-chain offers for assets you do not hold (`swap.js:1072-1092`; cross path checks at
  `swap.js:896-901`).
- Unknown-precision assets tradable at precision 0 (`index.html:601`; Send guards, swap does not).
- Book fails core expectations: one side only, no spread/mid/depth/last-price, price quoted receive-per-pay
  (reverse of convention), pickers show no activity/liquidity signal (`swap.js:769-793,1128-1146`).
- Cancel during an in-flight lift hides without aborting; double-click Review stacks modals
  (`index.html:1605`, `swap.js:862-869,993-1005`).
- Taker signs the maker's PSET with no visible JS re-verification of amounts (`swap.js:1055-1063`); confirm
  the wasm enforces it or add a check that outputs match the quote before signing.

P2: cross composer shows "Maker fee 0 BTC" until after terms (`xswap.js:306`); `min_anchor_depth` neither
settable nor shown (`swap.js:926`); timeouts are raw block heights with no ETA/countdown and the Refund
button renders pre-locktime (`xswap.js:518,1058-1060`); `min_fill` ignored on lift (`swap.js:470-495`); no
fills/history and success is a transient toast (`swap.js:998`); requote race can paint a stale quote
(`swap.js:449-452`); cross book rows inert while same-chain rows are clickable (`swap.js:643-651` vs
`1136-1144`); `fillFromOffer` ignores the reference-input mode (`swap.js:1148-1158`); offer lifetime a fixed
half-disclosed 1h (`swap.js:1106`, `renderMyOrders` shows no expiry/status `1160-1180`); em dashes pervasive;
no thousand separators (`index.html:559-562`); reverse-swap reload leaves a stuck stepper unexplained
(`xrswap.js:286,835-838`); the fee "(estimate)" is the exact charged amount and no pay+fee total row
(`swap.js:57,486-491,1046-1052`).

P3: "Order book: ..." error prefix noise; spinner flicker on every keystroke; BTC "Balance 0" during scan;
`renderMyOrders` blanks on fetch error; stepper amounts lack ref hints; BTC Max hidden without reason; Send
tab static "sat/vB" label until JS rewrites it (`index.html:276`); dead code `randId` and legacy
`xQuoteForm`.

**Lightning (net-new capability; see section 8)**

Lightning (net-new capability, not a defect in existing code; see section 8)

P1

- No Lightning swap route: findRoute returns only `same`/`cross` (`swap.js:247-262`) and the wallet cannot even parse an LN offer (`seqob.js:245`, LightningTerms field 22 "reserved and not yet encoded"), so verifyOffer (`seqob.js:289-298`) drops every LN offer as forged. The proven instant lanes (pure-LN + submarine, seqdex `phase3-pure-ln` M0-M5) are unreachable from the wallet. Fix: encode + verify field 22 in canonicalOfferBytes (`seqob.js:221-249`), add a `kind:'ln'` route selected when a resting offer carries LightningTerms, and drive it through the LSP-hosted SeqLN node so no local taker node is needed. See 8.2 + 8.5.
- No instant-vs-anchored control for a BTC<->asset pair: the user cannot choose the Lightning route (instant, off-chain) over the on-chain cross-chain HTLC route (~1 block + anchor wait). Fix: a segmented control on the route row; extend setFinality (`swap.js:753-758`) with pure-LN / submarine-0-1-conf / capped-0-conf branches; never label a 0-conf leg final. See 8.4.

### 4.2 Web wallet core (`sequentia-web-wallet/index.html`, `btc.js`; shared: `SWK/lwk_common/src/qr.rs`, `SWK/lwk_wasm`)

P1

- Default Send confirm shows no fee: `psetDetails` throws on transparent-by-default explicit outputs and the
  fee row is in a swallow-all try/catch (`index.html:1609-1635`); the BTC path shows fee+total
  (`index.html:940`). Fix: compute the fee from the PSET (sum inputs minus outputs per asset) and make the
  fee + "Total (amount + fee)" rows mandatory.
- Receive QR encodes `liquidnetwork:` (`index.html:1392` via `SWK/lwk_common/src/qr.rs:16-20`). See T3.
- BTC price mapping dead: `priceTicker('BTC')` returns `'WBTC'` (`index.html:677-679`); BTC row unvalued and
  selecting "BTC" reference blanks all values. See T2.
- No headline total; tSEQ pinned to top with a unique sub-line (`index.html:242-249,1353-1374,1368-1371`).
  See T13.
- BTC transactions never appear in History; no pending/confirmed indicator for BTC (`index.html:1783-1823`,
  `869-888`). See T6.
- "Label it before sending" is a dead end: no labeling UI exists (`index.html:612-615,1758`).
- Reissue/Burn mis-scale for unlabeled assets (1e8 trap the Send path guards) (`index.html:1763-1777`).
- Wallet-scoped state survives "Remove wallet": stakes/labels/ref-currency persist and a new wallet inherits
  the previous wallet's stakes (`index.html:1780,1958`). Fix: namespace localStorage by wallet fingerprint,
  clear all `swk.sequentia.*` on remove.
- No Max and no visible available balance in Send (`index.html:1535-1539`).
- "New address" can outrun the 20-address gap limit; funds received there never show (`index.html:1751`,
  `SWK/lwk_wasm/src/esplora.rs:94-100`). Fix: cap advancement or track max index and `fullScanToIndex`.
- Seed backup has no verification and the only copy is plaintext localStorage; no copy button
  (`index.html:201-209,1231,1681`).

P2: reveal-phrase has zero friction and stays revealed (`index.html:1779`); Remove wallet guarded only by
`confirm()` (`1780`); raw library/RPC errors leak in most paths (`prettyErr` maps only two patterns,
`1423-1430`; boot error uses innerHTML `2018`); address validation only at Review, BTC worst
(`1567,927`); scanned payment QRs ignore the `assetid` param (wrong-asset send, `1489-1497`); unknown assets
render raw atoms (`601,1372`); Review can stack modals (`1597,1714`); corrupted stored phrase bricks the app
with no reset (`1219-1221,2018`); multi-asset txs show only the largest delta (`1817-1820`); pending vs
confirmed conflated (`881`); stale WBTC faucet button + DEFAULT_ASSETS (`259,575`); em dash in a string
(`768`); stake list is device-local fiction (`2003-2005`); "No assets yet" shown during first sync
(`249,1345`).

P3: static "sat/vB" fee label (`276`); default fee-rate never stated (`540`); success is a 9s toast only
(`1661`); `refValueStr` loses precision above 2^53 atoms (`684`); comma-decimal rejected (`563`); confirm
modals mis-titled "payment" for burn (`1768`); issuance precision defaults 0 (`452`); History caps at 60
silently (`1788`); History rows show no ref value (`1820`); tSEQ faucet button uniquely styled (`255`);
receive address silently rotates on payment (`1344`); no session lock; seed grid uses innerHTML (`1682`); no
BIP39 passphrase option.

XSS sweep: clean. All attacker-influenced strings go through `el()`/`textContent`; registry strings strip
angle brackets (`591`). Only innerHTML sinks are the boot error and the seed grid (both safe content).

**Lightning (net-new capability; see section 8)**

P1

- No bolt11 BTC-LN receive or send: the wallet has no Lightning path at all, so the dual-chain principle (T6) is incomplete on its Lightning axis. The compiled `lwk_wasm` pkg already exports an `Invoice` BOLT11/BOLT12 parser (`SWK/lwk_wasm/src/boltz.rs:215-274`) that `index.html` does not import (`index.html:529`), and the Send scanner already strips a `lightning:` URI (`index.html:1489-1497`). Fix: a Lightning Receive mode (reverse submarine swap into an on-chain asset, any asset, equal standing) on renderReceive (`index.html:294-306`), and an LN Send path (submarine swap paying an external bolt11) branched off btnReview (`index.html:1714-1734`); the wallet's own keys hold P and the on-chain HTLC while the LSP owns the BTC-LN leg (non-custodial). See 8.5.
- No asset-LN balance surface (Tier-2 hosted channels, later): hosted asset-LN balances must count as equal-standing rows (T13), labeled "Lightning / instant-spendable", never shown more final than on-chain, with the LSP-liveness dependency disclosed. See 8.3.

### 4.3 Ambra mobile (`ambra/app/lib/**`, `ambra/ambra_core/src/api/mod.rs`, `ambra/app/android/**`)

P0

- App lock bypassed by open overlay routes: lock swaps only the navigator base (`main.dart:50-87`); pushed
  sheets survive, so the recovery-phrase sheet stays visible over the lock screen (`shell.dart:567-583`).
  Fix: `popUntil(isFirst)` on lock, or render LockScreen as a top-level Overlay above the Navigator.
- No `FLAG_SECURE`: seed create/reveal screens are screenshot-able and captured in the recents thumbnail
  (`onboarding.dart:138-146`, `shell.dart:572-581`; `android/app/src/main/AndroidManifest.xml`). Fix:
  FLAG_SECURE while a seed is on screen (or app-wide).

P1

- Rescue fee-rate: label says fee-asset units/vB, code interprets native sat/vB (off by up to 1e8, and "sat"
  on a Sequentia surface) (`rescue_screen.dart:270,508` labels vs `178,451` conversion; send does it right at
  `send_screen.dart:299-307`).
- Rescue broadcasts with no fee preview or review step (`rescue_screen.dart:172-230,416-465`).
- History fee hardcoded "tSEQ" (`history_screen.dart:271`); add `fee_asset` to `TxRow`
  (`ambra_core/src/api/mod.rs:569`). See T1.
- BTC absent from History; no testnet4 explorer link (`history_screen.dart:50`; `config.dart:38`). See T6.
- BTC balance vanishes silently on a testnet4 scan failure while the sync chip stays green
  (`shell.dart:106-112,305-333`); not cached (`wallet_cache.dart:15-31`). See T6.
- Cross-chain "Lock BTC" spends real BTC with no payment auth and no review (`xchain_swap_service.dart:226-242`,
  `xchain_swap_screen.dart:295`). See T6.
- Onboarding never offers security setup; app lock defaults off and is buried in More
  (`wallet_repository.dart:26`, `shell.dart:684-691`).
- Send/swap review sheets dismissible mid-broadcast (tx sends, success UI never shows, re-send risk)
  (`send_screen.dart:309-324,360-371`, `swap_screen.dart:454-459`).
- "SEQ" used as a chain abbreviation (`xchain_swap_screen.dart:312,317,382`, `xchain_swap_service.dart` error
  strings). See T12.
- Em dashes in strings (`swap_screen.dart:711`, `xchain_swap_screen.dart:392`, `xchain_swap_service.dart`).

P2: no recipient-address validation before Review (`validateAddress` never called, `send_screen.dart:273`;
BTC accepts `tsqb1` until prepare throws; hint shows tsqb1 even for BTC at `429`); no Max and no fee headroom
(`285-287`); amount fields open the full text keyboard (`widgets.dart:188-242`); keyboard covers rescue-sheet
fields (`rescue_screen.dart:233-280,467-519`); faucet has no BTC path and empty state overpromises
(`config.dart:81`, `shell.dart:211-216`); unknown assets show truncated hex with assumed 8 decimals and the
registry is wired but never fetched (`config.dart:23,83-90`); Swap tab still on the retired RFQ model
(`swap_screen.dart:122,287`); default backend is plain HTTP to a hardcoded IP (`config.dart:11`,
`AndroidManifest.xml` cleartext) -> point at `https://sequentiatestnet.com`; reverse cross-chain (asset for
BTC) has no UI (`xchain_swap_screen.dart`).

P3: 4s snackbar success (`send_screen.dart:328-336`); scanner copy says "Sequentia address" though it also
scans BTC (`scan_screen.dart:207`); no torch/gallery import; crude QR URI parsing (`send_screen.dart:255`);
no per-word import feedback (`onboarding.dart:295`); too-many-decimals rejected vaguely (`format.dart:25`);
sync chip says "offline" with no timestamp (`shell.dart:318`); Android back exits from any tab; issue/burn/
stake dialogs name the fee asset but not the amount; faucet funds index 0 while Receive cycles; welcome copy
overstates lock protection (`onboarding.dart:47`).

**Lightning (net-new capability; see section 8)**

P1

- Swap is still on the retired RFQ model (already P2 at `swap_screen.dart:122,287`, T7) and cross-chain is buy-only + anchor-gated (~20-30 min) with no reverse (`xchain_swap_screen.dart`; `xchain_swap_service.dart` XchainStore holds the secret P). Fix: add the LSP-hosted instant Lightning swap for BOTH buy AND sell, reusing the existing on-device secret-P persistence, and keep the on-chain HTLC path as the refund rail. Lands the T7 "no price" fix and the instant swap together. See 8.5.
- No bolt11 BTC-LN receive/send and no push: the send scanner reads BIP21 only (`send_screen.dart:251`), the manifest declares INTERNET + CAMERA only (no POST_NOTIFICATIONS / messaging service, `ambra/app/android/app/src/main/AndroidManifest.xml`), and the only lifecycle handling is re-lock on pause (`main.dart:50-57`). Fix: an LN receive (hosted/JIT-channel invoice) + send (pay bolt11) path via a new `ambra/app/lib/lsp_client.dart` + `ln_*` FFI in `ambra/ambra_core/src/api/mod.rs`, plus push (FCM/APNs or a self-hosted/UnifiedPush channel to match the no-Play-Services stance) so a backgrounded incoming payment or settled swap can complete. Mirrors 4.2. See 8.5.

### 4.4 Desktop GUI / Qt (`SequentiaByClaude/src/qt/**`, `src/qt/forms/*.ui`)

P0

- Send-confirm labels a non-tSEQ fee as tSEQ and corrupts "Total Amount": `nFeeRequired` is in the fee
  asset's atoms (`src/wallet/spend.cpp:1495`) but formatted with the policy unit and added to the tSEQ bucket
  (`sendcoinsdialog.cpp:403,419-429`; insufficient-funds path `328,799`). See T1.
- Tx-details Debit/Credit/Net rows label every asset as tSEQ (`transactiondesc.cpp:233-283`; only the fee row
  is asset-aware); contradicts the asset-aware history list (`transactiontablemodel.cpp:463`).

P1

- Fee asset defaults to tSEQ, not the transacted asset (`sendcoinsdialog.cpp:222-231,908-915`). Rule
  violation.
- Fee rates shown in tSEQ/kvB regardless of fee asset; the fee-asset combo reads as the rate unit while the
  number is tSEQ-value (`sendcoinsdialog.cpp:936,891`; `bitcoinamountfield.cpp:427`;
  `forms/sendcoinsdialog.ui:841-883`).
- "Use available balance" + same-asset fee is a dead end; subtract-fee disabled for all non-tSEQ sends
  (`sendcoinsdialog.cpp:861-863`, `sendcoinsentry.cpp:129-134`; wallet supports it, `walletmodel.cpp:759-767`).
- Assets page lists the Sequence token as "bitcoin" (`assetspage.cpp:182-183`, node default label
  `init.cpp:1202`).
- "Mask values" does not mask balances (`overviewpage.cpp:229,251`; `formatWithPrivacy` unused).
- CPFP "Speed up" broadcasts with no cost preview and a silent 5x multiplier (`walletmodel.cpp:749-784`).
- Payment URIs/QRs use `liquidnetwork:`; Open URI advertises `bitcoin:` (see T3;
  `guiutil.cpp:213,147`, `paymentserver.cpp:40`, `forms/openuridialog.ui:29`).
- Fee-policy panel: operator tool mixed into end-user Settings, refuses to work without a wallet though its
  RPCs are node-scoped, invents jargon "atoms per rfa", silently clobbers manual edits, lists only held
  assets (`feepolicydialog.cpp:55-125`, `bitcoingui.cpp:504-516`).
- Fee-asset dropdown offers assets the node will reject; failure only at send (`sendcoinsdialog.cpp:224-228`;
  RBF/replace same blind spot `walletmodel.cpp:544-548,825`).
- Coin control mixes assets into unlabeled tSEQ-formatted numbers and sums across assets
  (`coincontroldialog.cpp:531-536,623,660,694`).

P2: "sat"/mtSEQ/utSEQ sub-units in the confirm alternates line (`sendcoinsdialog.cpp:421-429`); Send-tab
"Balance:" is policy-only (`757`); Replace dialog raw with ambiguous rate label (`walletmodel.cpp:806-846`);
fee-bump confirm not value-comparable across assets (`589-603`); Overview headline is not the ref total,
native pinned first (`overviewpage.cpp:249-266`, `guiutil.cpp:859`); hardcoded "Bitcoin (testnet4)" strings +
untranslated (`overviewpage.cpp:198,451-481`); fee-policy "Launch price server" orphans the sidecar
(`feepolicydialog.cpp:295` vs `bitcoingui.cpp:1017-1032`); "LOSE ALL OF YOUR BITCOINS"
(`askpassphrasedialog.cpp:111`); ref-currency "SEQ" head entry + BTC->WBTC coupling
(`bitcoingui.cpp:1684`, `guiutil.cpp:780,808`); issued assets 8-decimal hardcode + hex-forever
(`guiutil.cpp:750-758`, `assetspage.cpp:236`); PSET fee row hardcoded tSEQ (`psbtoperationsdialog.cpp:196`);
About attributes Elements (`configure.ac:9`, `clientversion.cpp:94`); stale hardcoded asset table incl. WBTC
(`assetsdir.cpp:61-70`).

P3: empty wallet shows a literal em dash (`guiutil.cpp:875`); PSBT/PSET naming soup; window title "[test]"
(`networkstyle.cpp:87`); Receive/Assets/Staking share one icon (`bitcoingui.cpp:281-302`); QSettings
namespace "Bitcoin" collides with Bitcoin-Qt (`guiconstants.h:49`); "Custom asset (hex)" tooltip for
registered assets (`bitcoinamountfield.cpp:468`); "satoshi(s)" in coin control (`coincontroldialog.cpp:554`);
fee-asset choice not persisted.

### 4.5 Explorer + downloads landing (`sequentia-explorer`)

P0

- Ambra APK download link is dead and fails silently: `downloads/index.html:66` -> 302 to the greeting page
  (file missing from `DOWNLOAD_DIR`; SPA catch-all `serve-public.js:299` redirects). Fix: restore the APK (or
  fix the filename), make unknown `/download/*` return 404, add a link-check.

P1

- Asset pages and All Assets list show raw atoms (off by 10^8): `views/asset.js:184-222` uses
  `asset.precision` which electrs does not return; `disp_precision` is computed (`asset.js:42`) but only used
  for the Precision row. Same in `lib/elements.js:9-20` -> asset-list. Fix: use `disp_precision` everywhere.
- Address pages show no balances on a multi-asset chain (`views/addr.js:81-135`; electrs omits
  `chain_stats` sums). Fix: per-asset funded/spent block, or client-side aggregate from `/address/:a/utxo`.
- Live registry names the Sequence token "Sequentia" (network/token collision): box DB row wrong though the
  seed is correct (`sequentia-registry/seed/legacy-assets.json:5`). Surfaces in the assets list and tSEQ page
  (`views/asset-list.js:14`, `views/asset.js:40`). Fix: correct the DB row to "Sequence"; special-case
  `nativeAssetId` to prefer `nativeAssetName` so a bad registry row cannot rename the token.
- Search cannot find assets by ticker/name; placeholder omits assets (`driver/search.js:22-52`,
  `views/search.js:14`). Fix: registry ticker/name branch (assetMap is already loaded).
- "Explorer API" nav tab is broken on the deployed site (absolute `/explorer-api` -> 302 to greeting);
  landing CTAs point at sequentia.io which has no docs (`views/navbar.js:16`, `views/lander.js:9-12`). Fix:
  relative href + server fallback; point CTAs at `esplora/API.md`.
- Reference-currency "BTC" option silently dead (`views/util.js:132`, `WBTC` key). See T2.
- No faucet path outside the browser wallet; downloads never mentions funding (`GET /faucet` 302s;
  `serve-public.js` handles only POST at :173). Fix: a standalone `GET /faucet` page linked from downloads +
  greeting + explorer footer.
- Downloads page has no "what happens next": one run line, no faucet/wallet/docs links, no expectations
  (`downloads/index.html:77-90`).

P2: "sat/vB" and tSEQ-labeled totals on the Sequentia mempool page and fee tooltips
(`views/mempool.js:22,42,50`, `views/tx.js:173`); WBTC inconsistently alive (on-chain + registry + faucet,
no price); downloads footer "Block explorer" -> `/` not `/explorer/` (`downloads/index.html:90`); Linux run
path wrong (`:78`, actual `sequentia-core-23.3.3/bin/elements-qt`); no checksums/sizes; All Assets default
sort on server-absent fields (`app.js:110-113,428`); block committee row lacks quorum context and no chain
anchor-status strip (`views/block.js:89-94`; `/anchorstatus` consumed by nothing); faucet sets no
amount/cooldown expectations (`sequentia-web-wallet/index.html:255-261`, `serve-public.js:185`); greeting
Downloads card omits Android (`serve-public.js:276`).

P3: two golds `#f5b301` vs official `#ffc629` (`flavors/sequentia-testnet/extras.css:19`, logo svg); navbar
brand is Concatena, not a Sequentia mark (`views/navbar.js:8`); testnet4 GitHub icon -> sequentia.io
(`build-public.sh:27`); stock "Esplora" meta description; broadcast page unreachable + latent Blockstream
nav-toggle (`views/pushtx.js`, `views/nav-toggle.js`); "Powered by esplora" footer; "Esplora is currently
unavailable" leaks stack name (`views/error.js:6`); no release date/macOS note.

Correct already: tx fee shown in its actual fee asset with `<ticker>/vB` (`views/tx.js:126-132`); blinded
outputs honest; block pages show the Bitcoin anchor + finality + committee with a working cross-link; no em
dashes; no mixed content; viewports present; garbage search handled.

### 4.6 Bridge (`compages`)

P0

- Every failure path says "contact the operator" but no contact exists anywhere (`web/app.js:412-420,330`;
  no mailto/link in `web/`). Fix: add an operator contact + a reference (deposit nonce or `txid:vout`) to the
  footer and every such message.
- Redeem copy misstates the release gate: says "once confirmed on Sequentia" (`web/index.html:332-333`) but
  the real gate is ~100 Bitcoin-anchor confirmations, disclosed only after the intent is created
  (`daemon/lib/bridge.js:670-675`, `daemon/lib/api.js:160`). Fix: state the real gate + an ETA before commit.

P1: Sequentia destination validated only by length >= 14 -> silent lock-then-refund on bad input
(`web/app.js:198`, `CompagesVault.sol:172-177`, `bridge.js:161-167`); no balance/Max/sufficiency check
(`web/index.html:299`); zero fee/receive/ETA disclosure before submit (no bridge fee is charged and the
operator pays Sequentia network fees from USDX, but the user cannot know it); refresh mid-flow loses tracking
though `GET /api/deposit/tx/:hash` and `GET /api/redeem/:addr` exist (`web/app.js:253`, `api.js:174-186`);
low-decimal redemption silently burns the sub-unit remainder (`eth.js:75-79`, `bridge.js:690-697`);
non-bridged asset to the redeem address is a one-way trap warned only after (`bridge.js:680-685`); "delivered"
claimed at 0-conf with no mempool check on the final send (`bridge.js:462-498`); Sequentia txid/asset id are
dead text while only the ETH leg gets an explorer link (`web/app.js:321-322`).

P2: custodial disclosure is footer small print (`web/index.html:367-373`); "testnet" implied never asserted;
vault address shown with no "do not send directly" warning (`CompagesVault.sol:101-116`); raw wallet/RPC
errors dumped (`web/app.js:255`); refund/retry statuses lose info (`web/app.js:324-336`); per-asset cap not
pre-checked (`api.js:121`); unauthenticated unthrottled POST /redeem mints a fresh address per call
(`bridge.js:558-568`).

P3: delivered message uses the ETH symbol not the `.e` ticker (`web/app.js:318`); amount parser rejects ".5"/
"1."/comma (`web/app.js:56`); README em dashes + a stale "fee asset is tSEQ" claim (`README.md:68-125,110`);
no favicon; a11y gaps; token paste only on change; mobile no-wallet dead end; checksum error unhelpful;
"assets you can return" lists global circulation; tb1 hint should say "your Sequentia address".

## 5. Prioritized punch list (execution order) with acceptance criteria

Tier A. Security and fund-safety (do first).
1. Ambra P0 app-lock overlay + FLAG_SECURE. Accept: after backgrounding with the reveal-phrase sheet open,
   resume shows only the lock screen; screenshot on any seed screen is blocked.
2. DEX P0 anchor self-verification. Accept: the "Anchor verified" line is computed from
   `/anchor/<block_hash>` + `/anchorstatus`, fails closed when unreachable, and a maker-supplied height that
   the node cannot confirm blocks the reveal.
3. DEX P0 maker-settlement resume + taker keep-tab-open/recovery. Accept: reloading mid-settlement re-launches
   the watcher and shows the swap; the lock modal warns to stay open; an interrupted taker sees an explicit
   refundable-after-block-N state.
4. Ambra P1 BTC-lock payment auth + review. Accept: Lock BTC requires biometric auth and shows amount, HTLC
   address, fee, timeout before broadcast.
5. Bridge P0 contact path + real release-gate copy; P1 "delivered" only after mempool visibility. Accept:
   every failure message carries a working contact + reference; the redeem gate and ETA are shown before
   commit; "delivered" is never shown for an unconfirmed or unrelayed send.

Tier B. Correctness at the point of money movement (any-asset-fee display, T1/T4).
6. Qt P0 send-confirm fee/total and tx-details per-asset formatting; Ambra rescue fee-unit fix + review; Qt
   CPFP cost preview; web wallet default-send fee row. Accept: for a non-tSEQ send/rescue/bump, the fee and
   total display in the correct asset and match what is actually broadcast, verified against the node.

Tier C. Project-rule violations (T2, T3, T12, T13).
7. BTC price-key fix (web/explorer/Qt). 8. `liquidnetwork:` -> `sequentia:`/bare (SWK qr.rs + Qt). 9. Headline
   ref-currency total + de-pin tSEQ (web/Qt). 10. Copy sweep: no "SEQ" for network, no em dashes, fix
   "bitcoin"/"BITCOINS"/Elements-attribution (all surfaces). Accept: grep shows no `liquidnetwork:` emitted on
   Sequentia, no em dashes in user-visible strings, no "SEQ" as the network, a headline total exists, and
   choosing BTC reference shows values.

Tier D. Table-stakes flows.
11. Balance validation + Max across DEX/wallet/Qt/Ambra send (T5). 12. BTC first-class: history + explorer
   links + cached balance + faucet guidance (web/Ambra, T6). 13. Two-sided live order book + limit orders +
   dust/zero/oversize guards (DEX, T14). 14. Empty vs outage vs unfillable handling (DEX/Ambra, T7).
15. Unknown-asset registry fetch + labeling + precision (all, T9).

Tier E. Explorer/landing data legibility and onboarding.
16. Explorer P0 APK link + P1 precision/address-balances/asset-search/registry-name/BTC-ref. 17. Landing
   after-install block + faucet page + checksums + fixed links.

Tier F. Reload/resume, honesty polish, and the long P2/P3 tails per surface.

Tier G (NEW). Lightning on all surfaces (LSP / hosted-SeqLN model; see section 8).

18. LSP backend + gateway: deploy the hosted SeqLN cluster (SeqLN-on-Sequentia asset channels + SeqLN-on-Bitcoin testnet4 + holdinvoice-seq + `seqob-maker -mode pureln|lightning`) and a Boltz-shaped submarine swap-as-a-service in front of it. Accept: an authenticated thin client with NO local SeqLN node completes buy-asset-with-BTC-LN and sell-asset-for-BTC-LN end to end through the hosted LSP on testnet4, and the wallet's funds are refundable at its own CLTV if the LSP stalls.

19. seqob.js LightningTerms (offer field 22) encode + verify. Accept: an LN offer served by the relay passes local verifyOffer and appears in the book; its canonical bytes match a Go deterministic-marshal ground-truth vector byte for byte.

20. Web + Ambra "Instant (Lightning)" swap lane over the LSP-hosted SeqLN node. Accept: the Swap composer offers an instant-LN route for a BTC<->asset pair, distinct from the on-chain cross route, and a web/Ambra user with no local SeqLN node completes both directions (buy asset with BTC-LN, sell asset for BTC-LN) end to end.

21. bolt11 BTC-LN receive + send in web + Ambra (dual-chain LN axis, T6). Accept: a bolt11 invoice can be generated for the BTC leg (receive) and an external bolt11 paid (send) from both wallets, non-custodially, from ANY asset at the open-fee-market rate (no privileged coin).

22. LN finality + custody honesty (the only P0 in the LN set). Accept: the pure-LN happy path may be shown final; a submarine 0-conf receipt is shown provisional with the anchor gate + ETA; the hosted-channel custody + LSP-liveness statement appears BEFORE commit; grep shows no LN surface labels a 0-conf leg "final".

23. Two net-new LSP submarine modes: issue-a-hold-invoice-for-a-third-party-payer (LN receive) and pay-an-arbitrary-external-bolt11 (LN send). Accept: a non-LN wallet receives from a third-party BTC-LN payer into an on-chain asset, and pays an arbitrary external bolt11 from an on-chain asset; both are refundable at the wallet's CLTV.

24. Backend fast-path prerequisites: wire the hold-invoice reverse for fast BUY and honor `LightningTerms.max_0conf_amount` for fast small SELL. Accept: buying an asset with BTC-LN delivers at 0-1 conf while the LSP absorbs the anchor wait; a sell under the offer cap fronts BTC-LN at 0-conf; both surface finality honestly and never say "final" at 0-conf.

25. Ambra push + background-safe resumable LN swap. Accept: an incoming LN payment or a settled swap wakes the app via push and completes even if the app was backgrounded; hold-invoice timeouts are sized so a backgrounded swap does not silently expire, and an interrupted swap resumes from a persisted record.

26. Hosted-channel inventory + LN balances surface (Tier 2). Accept: asset-LN + BTC-LN hosted-channel balances render as equal-standing rows (no privileged coin, T13) labeled "Lightning / instant-spendable", never shown more final than on-chain, with inbound/outbound liquidity and a top-up flow, and the LSP-liveness dependency disclosed.

27. Tier-2 on-device signer + hosted channels (pure-LN endgame). Accept: a phone/browser holds the channel + commitment keys and co-signs every state over a per-wallet method+param-restricted rune; force-close reclaims funds on-chain (option_data_loss_protect + our watchtower); the LSP cannot move channel funds unilaterally.

## 6. Do not regress (already correct)

Web wallet: any-asset fee UX end to end, XSS-clean rendering, opt-in confidentiality copy, tethered
reference-input mode. Explorer: tx fee in its real asset with `<ticker>/vB`, honest blinded-output rendering,
anchor/finality/committee rows with cross-links. Qt: RBF on by default, opt-in confidential receive, correct
"Stake Sequence (SEQ)" token naming, anchor-supremacy panel copy, no em dashes in `tr()`. Ambra: create/
backup/verify, reference-currency total on home, any-asset send with a true pre-sign fee preview, honest
anchor-gated cross-chain swap, opt-in confidential receive, shared-tb1 messaging on Receive. Bridge: honest
about being centralized, and the release is correctly gated on Bitcoin-anchor finality (never a Sequentia
block count).

## 7. Alberto's UI/UX requests (2026-07-02), mapped

Relayed by Andreas. Each item is marked COVERED (already a finding above; cross-referenced) or NEW, with a
concrete fix. Several motivate one new cross-cutting theme.

**T15 (new theme). Now that HTTPS + the domain are live, sweep every explorer/API/backend link base to
`https://sequentiatestnet.com`, and make same-origin explorer links include the `/explorer/` prefix.** The
wallet's tx links already use `/explorer/tx/...` but the asset-id link omits the prefix (W1 below); Ambra
still defaults to the plain-HTTP raw IP (`ambra/app/lib/config.dart:11`) and has a Sequentia-only explorer URL
(`config.dart:38`). One sweep across surfaces.

### Web wallet
- **W1 (NEW, P1) Asset-id link lands on the greeting page.** Expect: clicking an asset id opens its explorer
  page. Reality: `index.html:1371` links `/asset/<hex>` (root-relative), but the explorer is served under
  `/explorer/`, so `/asset/...` hits the SPA catch-all and redirects to the greeting page (what Alberto saw as
  "159.195.15.140"); the tx links correctly use `/explorer/tx/...` (e.g. `index.html:1661`). Fix:
  `/explorer/asset/<hex>`, and audit every explorer link for the `/explorer/` prefix.
- **W2 (extends the "label it before sending" P1 + T9) Store and show wallet-local asset labels, marked as
  local.** Expect: a name you gave an asset persists and shows even when the price server/registry does not
  know it, visibly distinguished from registry/price-server labels. Reality: no labeling UI; unknown assets
  show raw hex. Fix: persist user labels and render them with a "local label (not in the registry)" marker
  (small badge or italics) so the user knows it is wallet-only.
- **W3 (NEW, P2) Info/help on the asset-creation page.** Expect: an info button explaining, with examples: how
  the name you give relates to the price server/registry and how the wallet determines price; if the
  functionality is active, how to connect to another price server; what "precision" means; what "reissue"
  means; and what "burn" means (does it burn my balance, other people's, all wallets?). Reality: no help
  affordance; these are opaque. Fix: a per-field info popover with plain-language explanations and one worked
  example each for precision, reissue, and burn.
- **W4 (extends "no thousand separators", DEX P2 / wallet P3) Thousand separators everywhere + a locale
  decimal/grouping toggle.** Expect: grouped amounts (1,250,000.5) in all displays across the web wallet AND
  Ambra, plus a setting to swap comma/dot for locales that reverse them. Reality: plain digit strings; only
  `refValueStr` groups. Fix: group thousands in all display contexts; add a Settings toggle for decimal and
  grouping style; keep inputs tolerant of both.
- **W5 (NEW, P2) Reissue shows the destination address.** Expect: the reissue flow shows where the newly
  minted tokens go. Reality: not shown. Fix: display and confirm the reissuance destination address in the
  reissue modal.

### Staking and swap (Alberto wants documentation first)
- **S1 (NEW, docs + UX) Staking is a black box.** Alberto cannot yet specify improvements because the mechanics
  are undocumented: when you stake in the web wallet, which node manages it, how you benefit, who co-signs
  blocks, how you monitor it. Requirement: (a) user-facing documentation of the staking model, and (b) a
  staking UX that does more than freeze coins behind one button (show what the stake does, its status and
  rewards, and how to monitor it). Relates to "stake list is device-local fiction" (wallet P2). Blocked on the
  docs.
- **S2 (reframes T14) Market-buy is the primary DEX use case, not trading.** Expect: the headline Swap action
  is "spend X of asset A, receive Y of asset B at the market price", where entering the spend amount instantly
  fills the receive amount at the current best book price (and the reverse), and the entry can be denominated
  in the reference currency (e.g. "buy $1,000 of tSEQ"). Limit orders are a secondary, less-used tab. Reality:
  the composer auto-quotes against the best offer but has no market-buy framing, no reference-currency-
  denominated entry, and it is take-only when the book is non-empty (T14). Fix: a market-buy/sell mode as the
  default (one-sided entry, auto-fill from the book, reference-currency amount supported); limit orders behind
  a secondary control. Open design question Alberto raised: is a "market order" a dynamic order-book entry, and
  must a maker close and reopen an order when the price moves? Resolve this in the order-book design before
  implementing (likely: takers hit resting offers at market; makers reprice by cancel + repost, or a future
  auto-reprice; document the answer).
- **S3 (NEW, docs) Atomic-swap documentation.** Document how the same-chain atomic swap and the cross-chain
  HTLC actually work, for both users and the implementing team.

### Ambra transaction history
- **B1 (NEW, P2) Show date and time** on each history row (`ambra/app/lib/history_screen.dart`); currently
  absent.
- **B2 (NEW, P2) Show the USD value at the time of the tx** (Alberto: value at tx time, not current). Requires
  capturing the price at broadcast/confirmation and storing it (or backfilling from a price history).
- **B3 (extends Ambra "History fee hardcoded tSEQ" P1) Show the fee and its unit-of-account value for outgoing
  txs**, in the correct fee asset (fix the tSEQ hardcode at `history_screen.dart:271` via the `fee_asset` add
  to `TxRow`) plus a USD equivalent.
- **B4 (NEW, P2) Filters/ordering** at the top of history: by date, asset type, asset amount, fee asset type.
- **B5 (NEW, P2) Drop the txid from the list view** (still reachable in the row detail) and, for outgoing txs,
  use the freed space for an optional **description/memo**: a wallet-DB-stored note, character-limited (pick a
  limit that fits the row), set at create/broadcast time and, if UX-clean, editable in the tx detail view.
  Good cross-wallet feature; consider the web wallet too.

---

## 8. Lightning integration (SeqLN) across all surfaces

Lightning is proven at the daemon + relay layer and absent from every wallet, so this is a net-new build, not an existing surface with defects to audit. Asset-LN <-> BTC-LN pure-LN swaps AND asset-on-chain <-> BTC-LN submarine swaps settle end to end through the seqob order-book relay + encrypted courier (seqdex `phase3-pure-ln`, milestones M0-M5 all done 2026-07-04): ~2.1s, both directions (buy asset with BTC / sell asset for BTC), atomic refund proven, a real testnet4 Bitcoin-LN leg, and no anchor-depth wait on the happy path. The one thing keeping it out of the wallets is that the TAKER runs two SeqLN nodes: `seqdex/cmd/seqob-cli/xpln.go` requires `-asset-ln-socket` + `-ln-socket`, and its two irreducible LN acts (mint an invoice on its incoming leg, pay the maker's hold by bare hash on its outgoing leg) each need a channel + liquidity on both networks. A browser wasm wallet and a Flutter phone have neither. Removing that requirement is this whole section.

The removal is an LSP: WE host SeqLN Phoenix-style (liquidity, hosted channels, watchtower), the wallet stays a thin client, non-custodial for the user's keys and funds where possible. This adds one new backend surface absent from the section-1 table (the hosted SeqLN LSP: repos `seqln` + `seqdex` `phase3-pure-ln`, served behind web + Ambra). Scope for this section is the two thin clients, web + Ambra; Qt (full node-GUI, 4.4) and Fulmen (an LSP-operator GUI, not a thin taker) are outside the Phoenix model and deferred (8.7, open decision).

### 8.1 What already works, and the one gap

Proven and do-not-regress at the daemon (8.7): the seqob relay (`seqdex/internal/seqob/api/server.go` + `ws.go`) is a pure non-custodial matchmaker carrying opaque E2E-encrypted `SwapMsg`; the pure-LN engine (`seqdex/pkg/xchain/pureln.go`, `internal/seqob/client/xdriver_pureln.go` + `xcourier_pureln.go`) and the submarine engine (`submarine.go`, `xdriver_submarine*.go`) settle on one shared preimage; the CLN client `seqdex/pkg/xchain/leg_lightning.go` (clnLNLeg: Pay, PayHash-by-bare-hash, CreateInvoice, and the hold methods) drives both; the hold primitive is `seqln/contrib/holdinvoice-seq/holdinvoice.py`; the LP is `seqob-maker -mode pureln|lightning`. The validator already accepts LN offers (`seqdex/internal/seqob/validator.go:235-285`, `ln_direction` 0-3, requires `maker_ln_node_pubkey` for reverse/pure-LN), so the relay serves them today. No wallet can see them.

The gap, restated so the fix is unambiguous: a channel-less thin wallet CANNOT literally be a pure-LN taker (minting an invoice needs an incoming channel, paying a hold needs an outgoing one). So the fix is not "make the wallet a smaller node" but "move the LN legs onto a node we host and hand the wallet a shape it can already sign". That splits by where the user's funds live, giving two tiers.

### 8.2 The hosted-SeqLN LSP (two tiers)

Tier 1 (ship first; non-custodial; ZERO LN on the device) is a submarine swap-as-a-service. The user's leg stays ON-CHAIN, which both wallets already do end to end: web has the full HTLC toolkit (generateSwapSecret / buildSeqHtlcRedeemScript / ClaimTx / RefundTx imported at `sequentia-web-wallet/index.html:529`, the btcLeg fund/claim/refund bridge `index.html:1028-1078`, the asset seqLeg bridge `index.html:1088+`, and a generic sealed courier `xcourier.js`); Ambra persists the swap secret P on device (`ambra/app/lib/xchain_swap_service.dart` XchainStore); the SWK core has the on-chain HTLC + anchor-reveal gate (`SWK/lwk_wollet/src/btc/xchain.rs`). Only the LSP's leg is Lightning. A new stateless authenticated REST+WS gateway fronts the live `seqob-maker` LP and runs the taker-side LN acts the wallet cannot; it touches the wallet only via {a receive address, the wallet's chosen payment-hash H or a bolt11 to pay, the wallet's signature on its own on-chain leg} and never holds the wallet's keys or P. This maps 1:1 onto the Boltz-v2 submarine client SWK already ships (`SWK/lwk_boltz/`, `boltz_client::BoltzApiClientV2`), so the gateway exposes a Boltz-shaped createswap/quote/status(WS) surface extended with a Sequentia asset id, and the wallet reuses that client pointed at our host instead of boltz.exchange. Two net-new LSP MODES are required that the shipped xsub* flows do not have (those assume the taker runs SeqLN): (a) issue a BTC-LN hold invoice bound to a user-supplied H that a THIRD PARTY on the public Bitcoin LN pays, then fund an on-chain asset HTLC to the user (LN receive); (b) pay an ARBITRARY external bolt11 on the user's behalf, reimbursed by the user's on-chain asset HTLC (LN send). Both are Boltz-shaped and reuse serveSubmarine, but are a different role assignment. Tier 1 needs the wallet to run nothing new except point its swap client at us.

Tier 2 (endgame; truly-instant pure-LN from a phone) is Greenlight/Phoenix-style hosted channels with an on-device signer. The pure-LN "no anchor wait" leg needs the user's value to live in a channel; for a thin client that means WE host the lightningd pair (SeqLN-on-Sequentia asset channels + SeqLN-on-Bitcoin) while the DEVICE holds the channel + commitment keys via a remote-signer split and co-signs each state over clnrest gated by a per-wallet, method+param-restricted RUNE, plus option_data_loss_protect for recovery and a watchtower we run. SeqLN has clnrest built (`seqln/target/release/clnrest`) + createrune/checkrune, but NO remote-signer / hsmproxy yet; that split is the core net-new Tier-2 build. Evaluate porting Greenlight gl-client or VLS rather than a bespoke on-device LN stack, which is very large given SWK/LWK has zero Lightning. Anchoring helps here: a Bitcoin-driven reorg is a tail-truncation that RESETS the CSV clock in the defender's favor, no novel penalty-evasion, so trust collapses to liveness + watchtower, not custody.

Rejected as the primary model: a hosted per-user SeqLN node behind clnrest WITHOUT the signer split, and a plain custodial swap endpoint; both hold the hsm_secret or the in-flight funds and are fully custodial, bounded only by reputation. Acceptable only for tiny amounts, or as the transport skeleton that Tier 2's signer split later hardens. Cross-network liquidity on both networks is structural (the LP must be capitalized on Sequentia AND on Bitcoin-LN); that is liquidity provision, not custody, and cannot be conjured. It bounds swap size and is the liveness backbone, not a trust component.

### 8.3 Custody and trust, stated honestly (the decision the user must make)

This is the load-bearing honesty section. Get the wording right before shipping any LN surface, and disclose it BEFORE the irreversible action (reuse the pre-submit-preview discipline of T4 and the bridge honesty precedent of T8 + section 6).

Tier 1 is non-custodial by construction. The user's money always sits in an on-chain HTLC the user can unilaterally refund after a timelock; the LSP can never take it without paying or receiving the matching BTC-LN payment (which requires revealing or learning P). Worst case is the user's CLTV refund; censorship (the LSP refuses to pay or settle) resolves to the same refund. No LN balance is ever custodied. This is exactly the Boltz trust model.

Two honest residuals in Tier 1, both priceable, both surfaced and never hidden. (1) The fast-buy path uses a hold-invoice reverse so the user gets the asset at 0-1 conf while the LSP absorbs the anchor wait (`seqln-dex-instant-swap-latency.md`); a malicious LSP could settle the hold early and pray for a deep Bitcoin reorg, which it cannot cause, which only pays off on a rare deep reorg, and which burns reputation. Same risk grade as 0-conf, capped, NEVER shown as final. (2) Small sells fronted at 0-conf under `LightningTerms.max_0conf_amount` carry that same 0-conf grade, above which the wallet falls back to the anchor-gated path.

Tier 2 is non-custodial for keys IF the signer split is built: the LSP runs the node and routing but cannot move channel funds unilaterally because the device must co-sign every commitment. Without the signer split, a hosted node holds the hsm_secret and is fully custodial. Either way, hosted channels add a LIVENESS dependency on the LSP that must be disclosed: instant-spendable, but the LSP is the sole channel peer; the escape hatch is force-close to reclaim on-chain, backed by option_data_loss_protect + our watchtower. Under the equal-standing rule (T13) any hosted asset-LN balance is one row among equals, labeled "Lightning / instant-spendable", and never rendered more final than an on-chain balance.

The user decision, flagged in the open questions: Tier-1-now / Tier-2-later is recommended, but the Tier-2 custody mechanism (Greenlight gl-client vs VLS vs a CLN hsmproxy on-device signer split, versus an honestly-labelled custodial interim for small amounts) is the single biggest architecture call and gates whether pure-LN from a phone is genuinely non-custodial.

### 8.4 Finality honesty across the Lightning states

Extend the existing anchor-honest surfacing (web `sequentia-web-wallet/swap.js:753-758` setFinality + the swFinality row `index.html:390-394`; Ambra the quote-view string in `ambra/app/lib/swap_screen.dart`) with the LN states, and centralize the wording in one shared helper per wallet so it cannot regress to "instant/final" where it should not (the strings are already duplicated across same-chain, cross, and the anchor gate, and LN adds three more). Honest per the DEX 0-conf policy and Principle 1:

- Pure-LN happy path (Tier 2): genuinely instant and final, nothing on-chain, zero reorg risk. This one MAY say final.
- Submarine BUY (Tier 1, hold-invoice reverse): "asset delivered now (0-1 conf), Bitcoin-anchor-final in ~20-30 min" while the LSP absorbs the anchor wait. Provisional at receipt, never final at 0-conf.
- Capped small SELL (Tier 1): "instant up to N <asset>, larger sells settle in ~1 block". The 0-conf receipt of irreversible BTC-LN is fronted by the LSP under the cap; never call it final.
- Refund path, all tiers: "if the swap stalls, nothing moved, your funds are refunded (atomic)".

### 8.5 Per-surface build

Web wallet (4.1 Swap + 4.2 core). The wallet is already submarine-swap-shaped, so near-term LN is submarine-only and needs no LN node in the browser. Add a front-end LSP client module (e.g. `seqln.js`) + a `window.SEQ_LSP_URL` global mirroring the `SEQ_SEQOB_URL` / `SEQ_DEX_BASE` pattern (`sequentia-web-wallet/index.html:1261-1307`; default `location.origin + '/lsp'` or reuse `/seqob`), talking to our hosted LSP over REST + the existing `/seqob` WS courier. LN RECEIVE = a Lightning mode on renderReceive (`index.html:294-306`): the user picks an amount + which asset to receive (any asset, equal standing), the wallet generates P via generateSwapSecret, POSTs H + amount + asset + its HTLC pubkey to the LSP, and displays the returned BTC-LN hold invoice; a third party pays it, the LSP funds a Sequentia asset HTLC, and the wallet watches esplora and claims with P via the seqLeg + buildSeqHtlcClaimTx path (the xrswap.js on-chain-claim mechanics, with the LSP issuing the invoice for a third-party payer). LN SEND = detect a pasted/scanned bolt11 (the scanner already strips a `lightning:` URI at `index.html:1489-1497`), parse it with the already-compiled `Invoice` class (`SWK/lwk_wasm/src/boltz.rs:215-274`; add to the import at `index.html:529`) to read amount + payment_hash, request a quote from the LSP, fund a Sequentia asset HTLC locked to H via seqLeg.fund + buildSeqHtlcRedeemScript (LSP claims with P, wallet refunds after T_seq), courier the funded outpoint, and the LSP pays the external invoice and claims. Do NOT wire `BoltzSession` / `BoltzSessionBuilder` from the same pkg: that client targets the public Boltz API on Liquid, a different counterparty than our LSP; it is a mechanics reference only. Swap tab: extend findRoute (`swap.js:247-262`, today returns only `kind:'same'`/`kind:'cross'`) with `kind:'ln'` when the pair is a Sequentia asset <-> BTC over Lightning, route it in onReview (`swap.js:862-869`), and wire an `ln` handle in initSwapTab (`index.html:1239-1331`) beside the xswap/xrswap/xmaker handles; the composer, panes, picker, fee market, and reference-currency hints are reused unchanged. Codec: add the LN atom types to xcourier.js's XcType union (`xcourier.js:31-42`; the Go side already defines XcPln/XcSub) so the sealed CourierSession (`xcourier.js:67-99`) can drive the handshake, and add encodeLightningTerms in seqob.js emitting offer field 22 (`seqob.js:221-249`, byte-exact against the Go deterministic marshal) so LN offers pass verifyOffer (`seqob.js:289-298`) instead of being dropped as forged. Send composer: detect a bolt11 in collectRows/buildSendPset/btnReview (`index.html:1549-1594,1714-1734`) so an invoice routes to the LN driver instead of `new Address()` (which throws on an lnbc string), and adjust the fee-asset UI (`index.html:1453-1459`) since an LN send's cost is the LSP's quoted asset amount + the on-chain funding fee, not a per-vByte rate. The pure-LN endgame (instant, no anchor wait) is deferred: it needs the user to hold funds in a hosted channel with wallet-held keys, which a browser JS+WASM wallet cannot run without an embedded LN state-machine (Tier 2, 8.2).

Ambra (4.3). Ambra is a generation behind the web wallet: Swap is on the retired RFQ `SeqdexClient` (`ambra/app/lib/swap_screen.dart:122,287`, already flagged T7), and cross-chain is buy-only + anchor-gated with no reverse (`ambra/app/lib/xchain_swap_screen.dart`). It can reach LN two ways: migrate its Swap to the SeqOB order-book/courier first, or shortcut LN straight through the LSP REST endpoint without migrating the same-chain path (the faster route to instant LN on mobile, since the LN take goes through the LSP anyway). Add `Backend.lsp => $_origin/lsp` in `ambra/app/lib/config.dart` (co-located with dex/feerates, reusing the node auth-header plumbing), a new `ambra/app/lib/lsp_client.dart` mirroring xchain_client.dart's _post pattern, and `ln_*` FFI in `ambra/ambra_core/src/api/mod.rs` (invoice create/pay shims + the courier sealing beside seqdex.rs; plain LSP REST can stay pure Dart). Surface a Lightning mode on the ReceiveTab beside tb1/tsqb1, an LN branch in the send scanner (`ambra/app/lib/send_screen.dart:251`, today BIP21-only) and the recipient field, and upgrade XchainSwapScreen to the instant LN path for BOTH buy AND sell (today buy-only), keeping the on-chain HTLC path as the refund rail and reusing the on-device secret-P persistence (XchainStore) unchanged. Mobile adds two problems the web wallet does not have. (1) A phone cannot hold a socket open in the background, so the LSP must PUSH (FCM/APNs, or a self-hosted/UnifiedPush channel to match the app's deliberate no-Play-Services stance, consistent with the pure-Dart no-ML-Kit QR choice) to wake the app to claim an incoming payment or complete a backgrounded swap; today the manifest declares INTERNET + CAMERA only (`ambra/app/android/app/src/main/AndroidManifest.xml`) and the only lifecycle handling is re-lock on pause (`ambra/app/lib/main.dart:50-57`). (2) LN hold invoices expire in seconds-to-minutes while the OS suspends backgrounded apps, so an LN swap needs a persisted resumable record, a short foreground-service window while active, and push-driven resume; size LSP-side hold timeouts generously and lean on push, not on the app staying foregrounded. iOS parity (APNs + background + the Tier-2 signer) is deferred, it needs a Mac.

### 8.6 Quote, RFQ, and fee-spread display

An LN offer is a fixed-amount resting seqob offer with a mandatory expiry today; dynamic per-lift pricing is a later refinement (design section 5.3, M4 note), which is the SAME open question as Alberto's S2 market-buy ("buy $1,000 of tSEQ at current price") and must be resolved with the LN quote design, not separately. The LSP gateway needs a short-lived SIGNED quote so the two legs stay consistent and the spread is locked per swap; reuse the existing price-server + offer signing and do NOT re-derive the any-asset rate math (Principle 4, T1). Show the quote + LP spread BEFORE commit (T4): the LN "fee" is the maker spread baked into the rate, not a separate taker-funded network fee, so there is no fee-asset selector on the LN leg (same as the cross leg, where paintFee already disables it); render it in the correct rate/fee asset next to the open-fee-market same-chain UX without implying a privileged asset. When the LSP adds short-lived RFQ, reuse the xswap.js startCountdown pattern for the quote-expiry countdown so the user knows the spread is locked only for the session. BTC-denominated LN entry additionally depends on the T2 BTC price-key fix.

### 8.7 Backend prerequisites and do-not-regress

Backend work that must land before the UI can honestly offer the fast paths: wire the hold-invoice reverse for fast BUY (`seqdex/internal/seqob/client/xdriver_submarine_reverse.go`; the hold primitive is proven, only the driver/binary wiring is missing) so the user gets the asset at 0-1 conf while the LSP absorbs the anchor wait; honor `LightningTerms.max_0conf_amount` for fast small SELLS (the field exists in the proto + validator, but the submarine driver still floors min_anchor_depth at 2 and ignores it); harden holdinvoice-seq (in-memory only today, must survive restart + validate amount/cltv before a production LSP relies on it); confirm the LSP always holds a committee-accepted fee asset so its on-chain HTLC claim/refund and any Tier-2 force-close are relayable. Deploy the hosted SeqLN cluster (SeqLN-on-Sequentia asset channels for any issued asset + tSEQ equally, SeqLN-on-Bitcoin real testnet4 channels, holdinvoice-seq, `seqob-maker -mode pureln` + `-mode lightning`) on the box under systemd, funded with two-network liquidity, confirmed with Andreas before any box deploy (per the pipeline; all proven so far only on the laptop regtest + testnet4 harness).

Do-not-regress at the daemon (section 6, extended): the M0-M5 flows, the ~2.1s both-direction settlement, the atomic refund, the wall-clock timelock ladder, and the seqob relay staying a pure non-custodial matchmaker. Wire-format prerequisite: the seqob.js LightningTerms encoding must be byte-exact against the Go deterministic marshal or LN offers will not verify locally; this needs the ground-truth vectors the same-chain + cross-chain encoders were validated against, and it blocks the wallet even SEEING an LN offer.

Housekeeping (fold into the earlier sections when editing this doc): the section-1 surfaces table gets a hosted-SeqLN-LSP backend row (`seqln` + `seqdex` `phase3-pure-ln`, served behind web + Ambra); the section-2 tally gets the LN P-counts once graded (LN is net-new missing capability, so most items grade P1 under the P0=blocked/misled definition, with the finality-honesty item the only P0 candidate); section 7 cross-refs S2 (market-buy) and S3 (atomic-swap docs, which now span same-chain + on-chain HTLC + pure-LN + submarine) to the LN docs; and add cross-cutting theme T16 in section 3 as the one-paragraph pointer to this section, ready to paste:

**T16. Lightning swaps are proven at the daemon but no wallet can reach them (the taker still runs a SeqLN node).** The pure-LN and submarine lanes settle live through the seqob relay + encrypted courier (seqdex `phase3-pure-ln`, M0-M5), ~2.1s both directions, no anchor wait on the happy path, but every wallet is LN-blind. Hits: the web Swap has no LN route and cannot even parse an LN offer (`sequentia-web-wallet/seqob.js:245` offer field 22 "reserved and not yet encoded"; findRoute returns only `same`/`cross` at `swap.js:247-262`, so `verifyOffer` at `seqob.js:289-298` drops every LN offer as forged); Ambra Swap has no LN route and is still on the retired RFQ model (`ambra/app/lib/swap_screen.dart:122,287`, overlaps T7); the taker requirement itself is `seqdex/cmd/seqob-cli/xpln.go` needing `-asset-ln-socket` + `-ln-socket`; Qt and Fulmen are outside the thin-client model. Root fix: host SeqLN for web + Ambra users (LSP/Phoenix model, section 8) so the taker needs no local node, expose an "Instant (Lightning)" swap lane + a bolt11 BTC-LN receive/send path from ANY asset, and surface hosted-channel custody + LN-vs-on-chain finality honestly.

### 8.8 Do not regress (LN-specific)

The seqob relay stays a pure non-custodial matchmaker in both tiers; only the new HTTP/WS swap gateway (Tier 1) and the hosted-channel + signer plane (Tier 2) are added. The wallet does not currently speak the seqob opaque courier for the swap-as-a-service path and should not have to; keep the Tier-1 boundary at HTTP/WS so the wallet reuses its Boltz-style client. Never present a submarine 0-conf leg or a hosted-channel balance as more final than the underlying Bitcoin anchor allows; pure-LN is the only state that may say final.

### 8.9 Open decisions (fold into planning before the LN build starts)

These are the choices this section deliberately leaves open; the custody/trust tier is the load-bearing one.

- CUSTODY/TRUST, the central decision: ship Tier 1 submarine swap-as-a-service now (non-custodial by HTLC atomicity, no LN node on the device) and defer Tier 2 hosted channels? And for the Tier-2 pure-LN endgame, which non-custodial signer path: Greenlight gl-client vs VLS vs a CLN hsmproxy on-device signer split, versus an honestly-labelled LSP-custodial interim for small amounts? This gates whether pure-LN from a phone is genuinely non-custodial and is the single biggest architecture call.
- Is an authenticated LSP-runs-the-taker REST endpoint (more custodial than the hosted-channel signer split) acceptable as the interim, given the directive is non-custodial "where possible"? It is the fastest path to LN on Ambra without first migrating its Swap to the SeqOB courier.
- Confirm the LSP will run the two net-new submarine modes the shipped xsub* flows do not have: (a) issue a BTC-LN hold invoice on a user-supplied H that a third party pays, then fund an on-chain asset HTLC to the user (LN receive); (b) pay an arbitrary external bolt11 on the user's behalf, reimbursed by the user's on-chain asset HTLC (LN send).
- LSP API shape: reuse SWK's Boltz-v2 client (`lwk_boltz`) verbatim against a Boltz-shaped gateway extended with a Sequentia asset id, or build a Sequentia-native REST+WS API? Boltz v2 has no asset-id concept, so asset submarine swaps need an extended create-swap request; fork boltz_client vs a thin new client.
- Ambra sequencing: migrate its Swap to the SeqOB order-book/courier first and then add LN, or shortcut LN straight through the LSP REST endpoint without migrating the same-chain path? Affects a large amount of mobile work.
- Scope: confirm LN is scoped to the two thin clients (web + Ambra) and that Qt (full node-GUI) plus Fulmen (LSP-operator GUI) are deferred, and add the hosted-SeqLN-LSP backend row to the section-1 surfaces table.
- Direction/amount for v1: buy-first (hold on the BTC leg, lower risk) then sell; and submarine-first (on-chain user leg) before pure-LN (which needs the user holding asset-LN inventory in a hosted channel)?
- Push infrastructure for Ambra: FCM/APNs, or a self-hosted/UnifiedPush channel to match the app's deliberate no-Play-Services stance (pure-Dart no-ML-Kit QR)?
- Liquidity economics: who funds the LP's both-network inventory and the inbound liquidity for a brand-new wallet, and what is the pay-to-open fee model? Structural, bounds swap size and liveness, cannot be conjured.
- Deploy: confirm the hosted SeqLN + LSP nodes run on the box (clone -> run-dir -> binary confirmed with Andreas) and the testnet4 BTC-LN liquidity is sized for real end-to-end receive/send, before any box deploy (proven so far only on the laptop regtest + testnet4 harness).
