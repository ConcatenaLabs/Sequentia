# Sequentia UX Audit and Design-Change Spec (2026-07-02)

Handover document for an implementing session. Six user-facing surfaces were audited adversarially at
the code level ("what does an average user expect, and is it there?"). This spec collects every design
change, ranked, with file:line evidence and a concrete fix, plus the systemic themes that let one fix
resolve many findings.

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

## 2. Global severity tally

- P0 (11): DEX 4, Ambra 2, Qt 2, Bridge 2, Explorer 1.
- P1 (approx 45), P2 (approx 55), P3 (approx 40).
- The heaviest surfaces are the DEX Swap tab (thin snapshot book bolted onto a pay/receive composer)
  and the Qt/Ambra fee-display layer (any-asset-fee engine correct, display still assumes tSEQ).

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
