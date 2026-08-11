# To Alberto & bubu — the empty blocks, the denomination sweep, and one heads-up about the price server

Thanks for both write-ups — the empty-blocks note and the denomination emails. I worked through all of
it and, at Andreas's request, took the follow-ups the whole way rather than handing them back: PR #15
is merged, Core is rebuilt from the merged tree, and the `seqpald` trap is fixed. Here's what I found,
what I changed, and why (§5 covers the three things you'd otherwise have picked up).

---

## 1. The empty blocks — diagnosed, and fixed the production-correct way

Your instinct in point 2 of the empty-blocks note was right: it's wallet-side fee estimation, not
the committee. But the mechanism is worth spelling out because it's a genuine any-asset-fee
dynamic, not a testnet quirk.

**What's happening.** `minrelaytxfee` and `blockmintxfee` are both at the default 0.1 atoms/vB. On
a chain with no fee pressure, wallets estimate the *minimum* and attach a **raw** 0.1 atoms/vB. But
the node values every fee through the price server: `valued = raw × rate / 1e8`. So a floor fee is
only mineable while the fee asset's rate ≥ 1e8. The moment the asset ticks below 1e8, the identical
fee is valued *below* the floor and the tx stalls — until the rate recovers. I confirmed it on the
box: your issuance (`7ab291f5…`) sat unconfirmed while the committee produced coinbase-only blocks,
and it confirmed (165 confs now) right when the policy asset floated back above 1e8. The mempool
histogram you saw (`[[0.0, …]]`) is exactly this: everything sitting on the floor, tipping across
it as the rate moves.

**Why not just lower `-blockmintxfee`.** That was my first instinct and it's wrong — it hides the
defect. In production every fee asset floats (no privileged coin), so a floor-fee tx can always go
sub-floor as its asset moves; and early mainnet may look exactly like today (low volume, homogeneous
fee policy), so we should solve it the way we'd solve it in production. The production answer is that
**wallets fee with margin above the floor**, not that the miner drops the floor. The node is already
production-correct: it mines a tx when its valued fee clears the floor and skips it otherwise — that's
what a rational producer should do, and it's local policy, not consensus, so hundreds of producers
valuing assets slightly differently never splits anything.

**The fix.** The web wallet already does this right — `DEFAULT_FEERATE = 2000` (2 atoms/vB, ~20× the
floor) *and* `policyFeeRate()` scales the fee by the published rate, so when the rate floats below 1e8
it pays proportionally more atoms. **Core didn't**: `DEFAULT_FALLBACK_FEE = 0` (disabled), so on an
empty chain — no smart-fee data — it fell back to exactly the floor. That's why *your* Core-issued tx
stalled and web-wallet txs didn't.

- **`Sequentia` master, commit `003b21401`:** `DEFAULT_FALLBACK_FEE` → `2000` atoms/kvB, matching the
  web wallet's headroom. It only applies when the estimator has no data; a busy chain is unchanged.
  It's a one-constant change, but it needs a **Core rebuild** to take effect (your desktop build).
- I did **not** touch the committee's `-blockmintxfee`/`-minrelaytxfee`.

The fuller version (Core querying `getfeeexchangerates` and scaling the fallback by the fee asset's
rate, like the web wallet) is a reasonable follow-up, but the 20× flat margin absorbs any realistic
float, so it isn't needed to unblock the demo.

---

## 2. Heads-up: I have an open change on `price_server.py`, and it composes with your PR #15

Separately from the denomination work, I added an **abstract-numeraire reference** to the price
server (`Sequentia` master, commit `4e066eda8`): a new `reference_price_usd` option in `_denominate`
that re-expresses every rate against a fixed abstract value that matches no token, so the policy
asset floats against it like any other asset (Andreas's no-privileged-coin directive) instead of
being pinned to 1e8 by `reference_asset_label`. I set it to the policy asset's price at deploy, so
every rate was preserved at the moment of the switch; tSEQ now drifts around 1e8 rather than sitting
on it.

Two things you should know:

- **It composes cleanly with your PR #15.** Your precision fix is in `scaled_rate`/`fetch_registry`/
  `_admit`; mine is entirely in `_denominate`. Non-overlapping, so the 3-way merge auto-resolves
  (I checked — #15 doesn't touch `_denominate` at all). Semantically they stack correctly:
  `rate = (price / ref_price) × 1e8 × 10^(8−d)` — your `10^(8−d)` precision factor is carried once and
  survives my division by the abstract reference; no double-count, and the reference itself is
  precision-free (it's a price, not an asset). I'm flagging it only because I committed to `master`
  over a file you have an open PR against — no conflict, but merge #15 whenever you like and it'll be
  clean.

- **It's what surfaced the empty-blocks bug.** Pinning tSEQ to 1e8 would have "fixed" the stall by
  making the fee asset never float — which is unrealistic. The float made the testnet finally behave
  like production and exposed that Core doesn't fee with margin. So I'd keep the abstract numeraire.

PR #15 is now **merged** (`eaaa267cb`) and the two changes coexist on `master`. Deploy note: the box's
`/root/price-demo` standalone still runs only my abstract numeraire, not #15's precision scaling — I
left it that way on purpose (it's a drifted standalone and precision scaling is a no-op for the box's
all-8dp priced set); details in §5.3.

---

## 3. The denomination checklist — the items that were ours

I used your `asset-denomination.md` as the contract and closed the open rows. All are no-ops for the
current all-8dp asset set; they matter the moment a non-8 asset (your `tADLT`) ships.

**OpenAMP (§7.9) — fixed.** `openamp` main, commit `98c965c8`. The `if req.Precision == 0 {
req.Precision = 8 }` trap made integer-only (0dp) restricted assets unissuable. I seeded the request
struct's `Precision` to a `-1` sentinel before JSON decode, so an omitted field defaults to 8 while an
explicit `"precision": 0` is honoured as denomination 0; validated 0..18; added tests. I chose the
sentinel over a `*int` pointer to avoid churning the three downstream call sites and the existing
tests. **`seqpald` had the same defect and is now fixed too** — the tracked source (`~/SeqPal/seqpald`,
not the stale untracked openamp copy) *rejected* precision 0 rather than silently rewriting it; same
integer-only-unissuable result. Fixed and pushed (`SeqPal` `2d6f9cc`); details in §5.2.

**SeqDEX (§7.8) — fixed.** `seqdex` branch `phase3-pure-ln`, commit `ebe370a2`. Market precision was
set only from the per-market CLI flag (default effectively 0/8) with no chain/registry link, so a
non-8 market opened with the default mispriced silently. `NewMarket` now resolves each asset's
precision **explicit operator value › registry precision › 8**. There was no registry client, so I
added a minimal read-only one (`daemon/pkg/registry`) that fetches the minimal index (`precision` is
field 3) and is fail-soft (timeout/parse/unknown → fall back, never crash market creation). Config
`SEQDEX_ASSET_REGISTRY_URL` (default `http://localhost:3005/index.minimal.json`, which I verified
serves 200 on the box). Amounts stay atoms-based — this only fixes where the precision *number* comes
from. One honest limitation: proto3/CLI can't tell "operator chose 0" from "unset" (both default 0),
so a provided 0 is treated as unset and resolved from the registry; a genuine 0dp asset still comes
out right because the registry reports 0.

**Web wallet (§7.5) — MED-4 fixed.** `sequentia-web-wallet` main, commit `7d85b396`. The same-chain
composer was already precision-correct, but the cross/mixed rail write-back (`deriveXOpposite`, the
sub-asset SELL requote) used `trim()`, which rounds to a fixed 8 decimals. For a sub-8 asset that
writes more decimals than the asset supports into the amount field, and `parseAtoms` then throws on
submit, making the trade un-postable via the swap tab. I added `fmtUnits()` and formatted each leg at
its own precision (asset at `am.precision`, BTC at 8). MED-5 was already closed (it blocks sending an
asset of unknown precision). Its fee handling is already correct (scales by rate, 20× margin) — no
change.

**Compages (§7.10) — confirmed, left unchanged.** No live bug. Its ETH side is genuinely
decimals-aware (`unitsToSats(units, decimals)` scales by `10^(e−8)` from the ERC-20's own
`decimals()`), and it issues its own Sequentia asset at 8dp, so the intermediate atoms are true 1e8
and USDX displays correctly. The only exposure is a future non-8 Sequentia asset. A single
`SEQ_PRECISION` constant + storing the issued precision on the mapping would make that
correct-by-construction, but it's a deliberate design change (schema + signatures, and a second
"precision" easily confused with ETH `decimals`), so I didn't slip it in during an audit. Flagging it
for you to decide.

**Core (§7.1), Price server (§7.2), Registry (§7.3)** — your work, and correct. I didn't touch them
beyond the price-server reconciliation above.

---

## 4. What I deliberately did **not** do

- **`accept_unlimited_issuances` / uncapping atoms** — untouched. Per your §10 it's a consensus
  decision needing an overflow review, and the denomination hatch doesn't need it. Agreed, not
  silently.
- **Lowering the committee fee floor** — untouched, for the reasons in §1.

---

## 5. The three follow-ups from the first draft — now done

Andreas asked me to just handle these rather than hand them back, so:

1. **PR #15 is merged** into `master` (merge commit `eaaa267cb`). It auto-merged clean against the
   abstract-numeraire change (§2), and I **built `elementsd` from the merged tree on the box** to prove
   your Core changes and my wallet fallback fee compile and link together — they do. `elements-qt` (the
   desktop GUI, carrying your denomination display changes) built clean from the merged tree too. I did
   **not** cut a committee cutover: nothing in #15 or the
   fallback fee is consensus-affecting, and the committee doesn't use the wallet fee path, so there was
   no reason to disturb block production. To get the fallback fee into a running Core you just rebuild
   your desktop off `master` (or we cut a fresh /download release — say the word).

2. **`seqpald` is fixed** — `SeqPal` `2d6f9cc`. It didn't have the silent `→2` rewrite the openamp copy
   had; instead it *rejected* precision 0 ("must be between 1 and 8"), which is the same defect in a
   different shape (integer-only assets unissuable). The deploy path used a plain `int` (can't tell
   unset from 0), so I made `deployReq.Precision` a `*int` (nil → "precision is required"; non-nil
   honoured, including 0), and both the deploy and patch paths now validate `0..8`. Builds clean. (The
   `~/openamp/.../seqpald` copy is an untracked stale build; the tracked source is `~/SeqPal/seqpald`.)

3. **The box price server** — the code is on `master` via the merge, but I deliberately **did not touch
   the box's `/root/price-demo` standalone**. It has drifted ~1000 lines from the repo (it predates the
   price-server redesign), so wholesale-overwriting it would deploy an untested redesign into the path
   that feeds the node's fee valuation — high blast radius. And it would buy nothing today: the box only
   publishes rates for the 7 mock-feed assets, all 8dp, so your precision scaling is a standing no-op
   there (the non-8 registry assets — the SeqPal 2dp mints, `RTHREE` 0dp — aren't in the price feed, so
   they're never priced). Syncing that standalone to the repo is a real ops task I'd rather do carefully
   and separately; flag me when you want it and I'll do it with a proper before/after rate check.

Still genuinely optional / yours:
- **Compages `SEQ_PRECISION`** — only if you'll ever bridge a non-8 asset (no live bug).
- **SeqDEX deploy** — set `SEQDEX_ASSET_REGISTRY_URL` if the registry isn't at the default, else it just
  works.

Commits, for the record: `Sequentia` `eaaa267cb` (merge #15) · `003b21401` (Core fallback) · `4e066eda8`
(abstract numeraire); `seqdex` `ebe370a2` (market precision); `openamp` `98c965c8` (0-trap); `SeqPal`
`2d6f9cc` (seqpald 0-trap); `sequentia-web-wallet` `7d85b396` (MED-4). All pushed.

— Saba
