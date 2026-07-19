# Sequentia DEX Terminal — Product & UX Spec

Status: **canonical**. This is the source of truth for the Swap tab across every surface
(`sequentia-web-wallet`, `ambra`) and the settlement plumbing behind it (`seqdex`, the
LSP). When code and this document disagree, the code is wrong.

It does not invent a new protocol. It **ties together** work that already exists and
states, in one place, the product it is all supposed to add up to:

- `seqdex-orderbook-design.md` — the non-custodial order-book relay and the
  offer-carries-*intent* model (a resting offer is signed price/size/expiry/pair/keys;
  the PSET or HTLC legs are built only at lift time).
- `cross-chain-orderbook-consolidation.md` — BTC is just another asset in the **one**
  book; the offer schema already carries `CrossChainTerms cross_chain = 21` beside
  `SameChainTerms same_chain = 20`; the privileged RFQ maker is to be retired.
- `dex-rail-agnostic-matching` (memory) + `seqln-phase2-dex-integration.md` — matching
  is blind to rails; the LSP bridges rails at settlement.
- The 11 first principles (memory `MEMORY.md`): no privileged coin, dual-chain, any-asset
  fees, reference-currency display, transparent-by-default, Bitcoin-anchored finality.

---

## 1. Product thesis

The Swap tab is **one trading terminal**. Every pair behaves identically — GOLD/USDX,
EURX/BTC, it does not matter. You see a real two-sided order book, you place **market or
limit** orders that **walk the book** and **partially fill**, you enter size in the
asset's units **or** in your reference currency, and you choose how you pay and receive
(Lightning or on-chain) **without that choice ever touching the book or the matching**.
BTC is a first-class asset, so BTC pairs are full order-book pairs, not a degraded
whole-offer mode. Lightning makes it fast enough to trade rapidly (HFT-friendly), so the
terminal never blocks you between orders.

---

## 2. Non-negotiable invariants

These are the rules the current build repeatedly breaks. A change that violates one is a
regression by definition, no matter what else it fixes.

1. **The composer is a constant control surface.** Choosing an asset, a rail, or an order
   type changes *what is inside* a control, never *whether the control exists*. The
   market/limit toggle, the pay/receive rail toggles, the reference-currency amount
   input, and the price field are always present. (See §6 for the exact list.)
2. **The book and the matching engine are blind to rails.** Rail is not a column, a
   filter, or a separate market. Orders match on **price / asset / size** only. (§5)
3. **Market orders sweep and cancel the remainder; limit orders fill then rest.** A
   market order walks the book across price levels and any quantity with no liquidity
   behind it is **canceled** — a market order never rests. A limit order fills what it can
   up to its price and rests the remainder at its price. (§4)
4. **Never a whole-offer overshoot.** Asking to buy 10 never signs you up for 43. If only
   a 43 rests, a market buy of 10 takes 10 *of* it (partial fill of the resting offer); a
   limit buy of 10 rests a bid for 10. The whole-HTLC courier is a settlement mechanism,
   never the user's unit of trade.
5. **A canceled order that locked no funds never blocks the next order.** Getting a quote,
   opening a review, and backing out must leave zero state. "Another lift in flight" may
   only ever refer to an order that actually holds value on-chain or in a channel.
6. **Pending/open orders are a compact strip, never a full-screen takeover.** You place an
   order and keep trading; status is glanceable. This is a hard constraint because of §7.
7. **BTC is a first-class asset.** Every capability in this spec applies to BTC pairs
   identically. If a control or behavior degrades when a leg is BTC, that is the bug.

---

## 3. The order book

Per `seqdex-orderbook-design.md`, a resting offer is a **signed intent** (price, size,
expiry, pair, maker keys, settlement variant) carried by the non-custodial `seqob` relay.
The relay never holds funds, never holds a fund-moving PSET, never blinds — it holds
signed text and couriers lift/settle messages between two peers. Settlement legs (PSET
co-sign same-chain; HTLC cross-chain; LN over the LSP) are constructed at lift time.

The terminal renders this as a **real two-sided book**:

- **Bids and asks**, aggregated by price level, with size at each level and cumulative
  depth. Best bid/ask at the inside, spread and mid shown, last trade price shown.
- **Live** via snapshot + delta (web can use WS; Ambra polls). Prices update without a
  manual refresh.
- **Never "no price."** Distinguish three empty states explicitly (per `ux-audit-spec`
  T7/T14): *empty book* → invite a limit order; *relay unreachable* → "order book
  unreachable, retrying"; *unfillable at your size/price* → say so. Never a bare "no
  offers" that reads as breakage.
- Cross-chain (BTC) offers appear in the same book as same-chain offers; they are
  `CrossChainTerms` rows, not a separate list or a separate daemon (retire the
  `/v1/xchain/*` RFQ per the consolidation doc).

---

## 4. Order types & matching

Grounded in standard price-time-priority matching (see Sources). The Sequentia twist is
only that a "fill" is an atomic swap settled per §5, not a custodial ledger update.

### Matching

- **Price-time priority.** Best price first; within a price level, oldest offer first
  (FIFO). An aggressive order consumes the inside level, then the next, then the next —
  it **walks the book**.
- **Maker / taker.** A resting limit order is the *maker* (adds liquidity); an order that
  executes immediately against it is the *taker* (removes liquidity). Fees follow this
  (§8), taker ≥ maker.
- **Partial fills.** A large order fills against several smaller resting offers; each
  resting offer may be partially consumed; the incoming order may itself end partially
  filled. Execution price is the size-weighted average across the levels swept.

### Order types

- **Market.** Executes immediately against the best available prices, **walking the book**
  until the requested size is filled or the opposing side is exhausted. Any remainder with
  no liquidity behind it is **canceled** (it is an immediate-or-cancel at any price). It
  never rests. Show the taker a realized VWAP and a **slippage guard** (reject/confirm if
  the sweep would execute worse than a bound).
- **Limit.** Executes against resting orders at the limit price **or better** (walking the
  book up to the limit), then **rests the remainder at the limit price** as a maker order
  that continues to work until filled, canceled, or expired. Durable: a rested order is
  liftable even while the maker is offline (the offline-order model in the design doc; the
  covenant CLOB for same-chain; durable cross offers for BTC).
- **HFT order options** (at least): **IOC** (fill now, cancel remainder — a limit with no
  rest), **FOK** (all-or-nothing immediately), **post-only** (rest only; reject if it
  would take). These matter because Lightning settlement makes rapid resubmission viable.
- **Guards** (per `ux-audit-spec` T14): dust minimum, zero-receive rejection, oversize /
  slippage bound.

---

## 5. Rails & settlement (the corrected model)

This is the section the current build gets fundamentally wrong. Read it carefully.

### The user's two choices

Every order carries **two independent settlement preferences the user sets**: how they
**pay** (Lightning or on-chain) and how they **receive** (Lightning or on-chain). Four
combinations, all valid, all first-class. The toggles are always visible and always
editable (§2.1). There is **no "the system picks the rail" and no "Auto"** — the user
always states their preference, and the backend always honors it.

### Matching does not see the rails

The order book and the matching engine are **completely blind** to these choices. A
"pay-BTC-over-LN" buyer and a "receive-BTC-on-chain" seller **match** if their price and
size cross — full stop. Rails are not part of the offer's matchable fields.

### Settlement honors the rails, per leg

Once the engine pairs two orders, the backend settles each leg (the BTC leg and the asset
leg) by looking at the two sides' choices for that leg:

- **Compatible** (both want that leg on the same rail) → settle **peer-to-peer** directly,
  using the already-proven primitive for that rail:
  - on-chain asset↔asset → same-chain PSET co-sign (`pkg/swap`),
  - on-chain BTC↔asset → cross-chain HTLC (`pkg/xchain`),
  - LN↔LN → pure-LN atomic swap (one preimage, both HTLCs over Lightning).
- **Incompatible** (buyer pays BTC over LN, seller wants BTC on-chain) → the **LSP inserts
  itself as an invisible counterparty on that leg**: it receives the buyer's LN BTC and
  delivers on-chain BTC to the seller (and mirror for the asset leg). Each user gets
  exactly the rail they chose; neither knows the LSP was there.

### The bridge is atomic and non-custodial (decided)

The LSP bridge **must be hash-locked / HTLC-atomic end-to-end** — one preimage binds both
sides of the bridged leg so the LSP can never abscond with a leg and there is no
trusted-custody window. The LSP is a counterparty of *last resort for rail conversion*,
not a custodian. (Submarine and sub-asset swaps are the existing atomic LN↔on-chain
primitives this reuses.)

### Finality

Bitcoin anchoring governs finality identically for every path (Principle 1). Each offer
carries `min_anchor_depth` (default 0), surfaced **honestly** per rail: pure-LN is final
on preimage; on-chain/HTLC legs show anchor-bound confirmation; 0-conf is tolerated only
where the offer allows and is labeled as such. Never a silent "Pending."

### Rail → primitive map (reference)

| Pay leg | Receive leg | Both sides agree | Bridged by LSP |
|---|---|---|---|
| chain | chain | PSET (a↔a) / HTLC (BTC↔a) | — |
| LN | LN | pure-LN atomic swap | — |
| LN | chain | submarine (existing) | LSP LN↔chain on the mismatched leg |
| chain | LN | sub-asset / submarine mirror | LSP chain↔LN on the mismatched leg |

The user never selects a row here. They set pay + receive; the backend selects the row
and inserts the LSP only where the counterparty disagrees.

### The BTC leg: NATIVE BTC, and the silent SBTC peg for *resting* orders

**Sequentia uses NATIVE Bitcoin, not a pegged BTC (unlike Liquid).** Native BTC is the
distinct, privileged asset (the only asset shown at 0 in a fresh wallet, top of the
send/receive dropdowns) and it stays that way — nothing here defaults to or replaces it
with a pegged asset.

There is exactly ONE narrow reason a pegged form is needed: **Bitcoin has no covenants,
Elements does.** So an on-chain-BTC **LIMIT** order cannot rest on the DEX while the user
is offline (there is nothing on Bitcoin to hold it). For that case only, the order is
handled by a **SILENT peg**: the user's real BTC is pegged in to **SBTC** (via the
existing Elements peg-in/out — not a new bridge, no third-party fronted inventory), the
SBTC rests in a covenant (partial-fillable, offline-liftable), and on fill the SBTC is
pegged out so the **counterparty receives real BTC**. It is transparent — the user places
and receives native BTC; SBTC is an implementation detail of a resting order. A MARKET
taker paying real BTC settles interactively (online) and needs no peg. SBTC is otherwise a
normal, unprivileged Sequentia asset, and the same peg-in/out is exposed publicly (e.g.
confidential-tx wrapping). This mechanism must be designed WITH the user and only after
verifying the existing Elements peg operates on Sequentia's anchored-PoS chain.

---

## 6. The composer (control by control)

One composer for all pairs. Every row below is **always rendered**; state changes its
contents, never its presence.

1. **Pay asset / Receive asset** pickers. Either can be BTC or any Sequentia asset.
2. **Amount + denomination toggle.** Enter size in the asset's units *or* in the
   reference currency (USD / BTC / chosen). The reference-currency input is a first-class
   entry mode, not a hint that disappears. Both fields stay linked (edit one, the other
   derives), respecting whichever the user is actively typing.
3. **Market / Limit toggle.** Always present, on every pair, empty book or not. Market =
   take now (§4). Limit = rest at your price (§4). (Optional order-option control for
   IOC/FOK/post-only when Limit is selected.)
4. **Price field.** Enabled for Limit ("your price"), shows the book's inside price as a
   default/placeholder. For Market it shows the current sweep estimate + slippage bound.
5. **Pay rail / Receive rail toggles** (Lightning ↔ on-chain), always present, always
   editable, independent (§5). **They start UNSELECTED — there is no default.** An order
   cannot be placed until the user has chosen both a pay rail and a receive rail; the
   place/Review action stays disabled with a clear reason until they do. They never change
   the book or the quote's *price* — only how settlement is arranged and the *finality*
   line.
6. **Confidential toggle.** Transparent (default) vs blinded namespace (opt-in).
7. **Review + place.** Shows exactly what will execute: for a market order, the sweep
   (fills X now across N levels at VWAP, cancels any remainder); for a limit order, what
   fills now vs what rests. Fees per §8. Then one confirm.

The Review must **match what executes**. No number changes between what the composer
showed and what the modal locks (the "10 → 43" class of bug is a Review/execution
mismatch and is forbidden).

---

## 7. Terminal layout & throughput

The Swap tab is a terminal, not a wizard. Components (responsive; web shows more at once,
Ambra stacks/tabs them but keeps all of them reachable):

- **Order book** (two-sided depth, spread/mid/last, live).
- **Price history** — a chart of the pair's price over time, sourced from the **relay
  trade feed** (the executed-fill stream `seqob` publishes), not a separate price oracle.
- **Trade history** — your executed fills, persisted.
- **Open / pending orders** — a **compact strip**: resting limit orders and in-flight
  settlements with glanceable status (resting / partially filled / settling / needs
  action), each expandable for detail or a cancel/refund action. **Never** a full-screen
  stepper that blocks the composer.
- **The composer** (§6).

**High throughput / HFT.** Lightning is instant, final, and cheap, so the terminal must
support firing many orders in quick succession: fast order entry, **no per-trade modal
takeover**, and **no "finish this one before starting another"** gate (that gate is only
ever legitimate for an order actually holding funds on the *same* UTXO/channel it needs
to reuse — never a blanket lock). On web, order entry should be keyboard-friendly.

---

## 8. Money, fees, confidentiality

- **Reference-currency display and input everywhere** (BTC / USD / chosen). No privileged
  SEQ: SEQ is one asset row among equals; the balance headline is the **total across BTC +
  all assets** in the reference currency. A fresh wallet shows only a `0 BTC` row.
- **Any-asset fee market.** The fee is payable in any accepted asset (default = the asset
  being traded), shown **per-asset** and honestly. Maker vs taker fee is distinguished.
  For a cross leg whose fee is set at lift, show "maker fee set at lift," never a fake "0
  BTC." Never label a non-BTC fee in "sat/vB" (sat is Bitcoin-only).
- **Transparent by default, confidential opt-in.** The default unblinded address is
  Bitcoin-format; the blinded book is a namespace the user chooses per §6.6.

---

## 9. Where the current build stands, and the rebuild

### Gap analysis

The Swap tab is built as **separate flows per rail** — same-chain covenant, cross HTLC
courier, pure-LN, submarine, sub-asset — each with its own screen, its own conditional
controls, and (for cross) a **whole-offer, take-only** model inherited from the RFQ
hybrid the consolidation doc says to retire. That structure is the root cause of every
symptom: controls vanish because they belong to different code paths; BTC degrades because
it is a different path; Review shows a different number because the courier lifts a whole
offer; "another lift in flight" fires because a per-swap screen persists state on a mere
quote. It cannot be patched into §1–§8; it has to be restructured into **one rail-blind
composer + one book, with rails affecting only an invisible atomic settlement step.**

### Rebuild plan — build order, not ship milestones

There are no releases mid-way. The numbered steps below are the **order I build in**; the
product ships once, complete, verified on web and Ambra together. Settlement cryptography
is proven and is **not** rebuilt (PSET co-sign, HTLC, pure-LN, submarine). The work is the
book/matching consolidation, the composer, and the settlement router.

1. **One book.** Finish `cross-chain-orderbook-consolidation.md`: cross-chain offers rest
   in `seqob` as `cross_chain` terms; the tab discovers BTC markets from `seqob`, not
   `seqdexd`; retire the privileged RFQ maker. BTC is now a normal book pair.
2. **Market/limit + book-walking taker.** Replace whole-offer lift with a taker that
   sweeps the book (partial fills across levels, IOC-cancel remainder for market; rest the
   remainder for limit). Same code for same-chain and cross.
3. **Rail-blind matching + atomic LSP bridge.** Rails leave the matchable offer fields;
   settlement routes per §5, inserting the LSP as an atomic HTLC bridge only where the two
   sides' rail choices differ.
4. **The invariant composer.** Rebuild the composer to §6: all controls always present,
   reference-currency input, market/limit, pay/receive rail toggles, Review == execution.
5. **Terminal shell.** Order book + price chart + trade history + **compact open-orders
   strip**; remove the full-screen per-swap takeover; HFT-fast re-entry.
6. **Parity.** Web and Ambra to the same spec; on-device verification for Ambra.

Each step is testable on testnet as it lands, but **none is a release**. The RFQ path
stays as a fallback until the order-book path is proven both directions, then it is
retired — all inside the single final delivery.

---

## 10. Decisions (all closed)

- **Market remainder is canceled** (never auto-rested). **Limit remainder rests** at the
  limit price.
- **LSP bridge is atomic / non-custodial** (hash-locked, no custody window).
- **Pay/receive rails have no default.** Both toggles start unselected; an order cannot be
  placed until both are chosen (§6.5).
- **Price history comes from the relay trade feed** (`seqob`'s executed-fill stream).
- **Order types:** Market and Limit are the core, implemented exactly per §4. The advanced
  options **post-only, IOC, and FOK** are included as order options on a Limit order —
  they fall out of the book-walking matcher for free (post-only = reject if it would take;
  IOC = don't rest the remainder; FOK = all-or-nothing) and complete the terminal for the
  rapid-trading use case. They are secondary controls, not clutter on the core flow.
- **There are no v1/v2 milestones.** The "phases" in §9 are internal **build order**, not
  releases. Nothing is shipped mid-way; the product ships once, complete, verified across
  web and Ambra.

---

## Sources (matching-engine mechanics)

- [Order-driven markets & price-time priority — Longbridge](https://longbridge.com/en/learn/order-driven-market-101864)
- [What is an order book — Cube Exchange](https://www.cube.exchange/what-is/order-book)
- [Matching engines — Jelle Pelgrims](https://jellepelgrims.com/posts/matching_engines)
- [Order types (IOC/FOK/post-only) — Bitfinex Help](https://support.bitfinex.com/hc/en-us/articles/115003451049-Bitfinex-Order-Types-and-Order-Options)
- [Maker vs taker fees — Kraken](https://support.kraken.com/articles/360000526126-what-are-maker-and-taker-fees-)
