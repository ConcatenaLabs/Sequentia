# SeqLN asset-aware channels — code-grounded build plan

The DESIGN is settled in `seqln-core-lightning-fork-spec.md` §4–5 and `seqdex-lightning-feasibility.md`
(Analysis A / Phase 2). This doc is the executable BUILD ORDER grounded in the SeqLN (CLN fork) code, with a
regression-safe milestone sequence. Goal: a Lightning channel that holds an **issued Sequentia asset** (GOLD),
not just the policy asset (tSEQ), so the DEX can do **pure-LN asset↔BTC-LN swaps** (the instant, trustless
endgame — see `seqln-dex-instant-swap-latency.md`).

## What's already done vs the gap

- **Done (Phase 1, live):** POLICY-asset channels (funded with tSEQ). CLN-on-Elements funds channels in the
  policy asset; that is the 2019 L-BTC path and it works on SeqLN today.
- **The gap:** ISSUED-asset channels (GOLD, USDX, …). CLN's deepest assumption is *one policy asset per
  network*: `common/amount.c:amount_asset_is_main()` memcmps every output's asset against the hardcoded
  `chainparams->fee_asset_tag`, and `bitcoin/tx.c:bitcoin_tx_output_get_amount()` *asserts* every output is
  that asset. So any non-policy output is currently invisible/unspendable. There is no `channel_asset` concept.

## The key simplification (Sequentia-specific)

Any-asset fees dissolve Liquid's two-asset problem: pay the on-chain LN fee output **in the channel's own
asset** (GOLD). Then the funding/commitment/close tx is **single-asset GOLD**, and BOLT-3's implicit-fee
subtraction (funder's output − outputs = fee) stays self-consistent with GOLD as the unit. No L-BTC-for-fees,
no two-asset commitment accounting. `bitcoin/tx.c:elements_tx_add_fee_output()` must emit the fee in
`channel_asset`, priced via the node's Sequentia fee/exchange-rate logic.

## Core design decision: how to make `amount_asset_is_main` asset-aware

It reads the global `chainparams->fee_asset_tag` and takes no context. 94 call sites. Do NOT introduce a
global "current asset" (a node runs many channels of different assets concurrently — a global is a
funds-safety bug). Instead:
- Keep `amount_asset_is_main(amount)` meaning "is this the policy asset" (its 94 existing callers — wallet,
  watch, fee, dust on policy paths — keep working unchanged → Phase 1 stays green).
- Add `amount_asset_is(amount, asset_id)` and thread an explicit `channel_asset` (33-byte asset id) through
  the channel/commitment/onchain contexts. Commitment/HTLC/close builders compare against `channel_asset`,
  not the policy asset.
- `channel_asset` lives in `struct channel` (`lightningd/channel.h:141`) and `common/initial_channel.h:34`,
  and is carried in the peer→subdaemon init messages (channeld, onchaind, closingd) and the commitment builders.
- **Absence = the policy asset** everywhere (Bitcoin channels + existing Sequentia policy-asset channels are
  the `channel_asset == fee_asset_tag` case), so the default path is byte-for-byte unchanged.

## Milestones (each independently testable; Phase 1 test stays green throughout)

**M0 — plumb `channel_asset` (no behavior change).** Add `channel_asset` to the channel structs, default it to
`chainparams->fee_asset_tag`, and thread it into the subdaemon init/wire messages without yet using it in any
math. Add `amount_asset_is(amount, asset)`. *Test:* the Phase 1 policy-asset channel still opens/routes/closes
on regtest (pure regression; channel_asset == policy asset everywhere). This is the safe foundation commit.

**M1 — open + cooperative-close a GOLD channel (no HTLCs).** `open_channel2`/`accept_channel2` `asset_id` TLV
(absent = policy asset; accepter fails an unsupported asset). Funding output, first commitment, to_local/
to_remote, and the fee output all in `channel_asset`. Generalise `elements_tx_add_fee_output()` to the channel
asset. *Test:* `fundchannel` a GOLD channel on the 2-node regtest → CHANNELD_NORMAL → cooperative close → GOLD
returns to both wallets. (Wallet must see GOLD outputs — audit the wallet's `amount_asset_is_main` skips.)

**M2 — HTLCs denominated in the asset.** Audit every amount sum/compare/dust-trim in `channeld` + the
commitment builders to use `channel_asset` (the bulk of the diff). Add/settle/fail HTLCs in GOLD atoms
(integer atoms, no msat sub-unit for assets). *Test:* a single-hop GOLD payment across the channel settles;
dust-trim honored.

**M3 — force-close + onchaind claims in the asset.** `onchaind` re-derives to_local/HTLC-timeout/HTLC-success/
penalty in `channel_asset`; penalty/claim txs pay their fee in a committee-accepted asset (the channel asset,
per §4). CSV/`to_self_delay` sized in wall-clock (~270 Sequentia blocks for Bitcoin's ~40); funding final at
anchor depth (§3 policy). *Test:* unilateral close of a GOLD channel; HTLC-timeout and HTLC-success both
sweep GOLD; a revoked-state penalty sweeps GOLD.

**M4 — asset invoices + same-asset routing (v1).** BOLT11 carries the asset id in a TLV, amount in the asset's
atoms (BOLT11 core encoding untouched; wallet applies registry display precision). Routing requires same-asset
end-to-end; run asset channels unannounced in v1 (or extend `channel_announcement` with the asset id). *Test:*
pay a GOLD invoice across one asset channel.

**M5 — pure-LN cross-network swap (asset-LN ↔ BTC-LN).** The DEX endgame: a translating node with an LN node
on each network binds two independently-routed LN payments by one shared secret (HTLC now, PTLC later), with
a hold invoice on the incoming (asset) leg. Unblocks instant, trustless asset↔BTC-LN DEX swaps. Cross-asset
hops only via signed RFQ quotes (short expiry) — never open-ended cross-asset forwarding (free-option / the
reason multi-asset LN routing was declared dead).

## Constraints / risks

- **Funds-critical + live.** Keep the Phase 1 policy-asset channel test green at every commit; add a
  GOLD-channel regression per milestone. Byte-identical commitment reconstruction on both peers is the core
  correctness constraint (both must denominate in `channel_asset` identically) — `common/initial_channel.h`
  is shared by both, which helps.
- **Confidential Transactions deferred.** Unblinded/explicit-amount channels only (CT-in-commitment balloons
  proofs, clashes with explicit-fee, buys ~no privacy — every prior impl deferred it). Sequentia is
  transparent-by-default, so this is natural.
- **No prior art** for issued-asset channels in any LN codebase (Blockstream specced, never shipped) — the
  M2/M3 amount audit is where the real engineering is.
- **Amounts:** integer atoms per the asset; follow Taproot Assets — keep BOLT11/onion fields structurally
  intact, carry asset id + integer amount, apply display precision at the wallet layer.

## Progress (SeqLN `sequentia-stable`; all commits byte-for-byte identical by default, tests green)

DONE — the **asset-denomination tx foundation** (the subtle, byte-identical-critical layer):
- `amount_asset_is(amount, asset_id)` primitive; `amount_asset_is_main` delegates to it (commit `ed4fc18`).
- `channel_asset[33]` in the shared commitment `struct channel`, defaulted to the policy asset in
  `new_initial_channel()` (commit `7d3da5f`; `run-full_channel` green).
- Per-tx `output_asset` on `struct bitcoin_tx` (default = policy asset) so setting it once denominates a whole
  tx (to_local/to_remote/HTLC + the fee output) in the channel asset; fee-computation + get_amount paths use
  it too. `wally_tx_output_asset()` variant + `bitcoin_tx_set_output_asset()` (commit `067832a`; tx-encode,
  2of2-weight, full_channel tests green).
- `initial_commit_tx()` threads `channel_asset` and calls `bitcoin_tx_set_output_asset()` — the OPEN
  commitment now denominates in the channel asset (commit `70211e2`; `run-full_channel` green).
- Input witness_utxo asset: `psbt_input_set_wit_utxo_asset()` + `bitcoin_tx_add_input` routes through
  `tx->output_asset`, so a tx's INPUTS (the funding UTXO → the elements sighash) carry the channel asset too,
  or an asset channel's signatures would be invalid (commit `35028dc`; tx + full_channel tests green).
- **Whole-daemon build verified:** lightningd + channeld + openingd + dualopend + closingd + onchaind + hsmd
  all build + link cleanly with the six commits above. The tx-denomination CORE (inputs + outputs) is done and
  integrates across every channel subdaemon; default (policy asset) is byte-for-byte identical.
- **lightningd + channeld channel_asset path** (commit `f92bcce`): channel_asset in lightningd's struct channel
  (defaulted to policy; not yet DB-persisted → defaults on reload), threaded through the `channeld_init` wire
  (byte,33) — lightningd sends it, channeld overrides its policy-defaulted initial_channel with it. Wire
  codegen regenerated; both build+link. So channeld denominates its initial commitment in the channel asset.

REMAINING — the wallet-coupled OPEN linchpin (traced; e2e-only-verifiable, needs a live regtest debug loop):
- In dual-funding (dualopend) the funding output is already IN the PSBT — dualopend just `find_txout`s it. So
  making it GOLD lives in **lightningd's dual-funding PSBT construction + wallet coin-selection** (select GOLD
  inputs, add the funding output in GOLD). This is the funds-critical wallet piece.
- Plus: an `asset` param on the `fundchannel`/`openchannel_init` RPC → set lightningd `channel->channel_asset`;
  thread it into the `dualopend`/`openingd` init wire (so they build the initial commitment in GOLD); the
  `open_channel2` `asset_id` TLV so the accepter learns it; the `closingd_init` wire + `create_close_tx`
  channel_asset for the coop close; and hsmd signing (should follow automatically since the PSBT witness_utxos
  now carry the asset).
- These are tightly coupled through lightningd's wallet/funding and produce no live signal until a full
  `fundchannel(GOLD)` → CHANNELD_NORMAL runs, so they MUST be built in an iterative build-run-debug regtest
  loop (rebuild + 2-node Sequentia regtest + attempt open + read logs + fix), not blind. That is the dedicated
  next effort; the tx-core + channeld path above is the reusable foundation it plugs into.

## Regtest loop — LIVE, policy path RUNTIME-verified, GOLD blocker pinned (2026-07-03)

Stood up a 2-node Sequentia lightning regtest (elementsd `-chain=liquid-regtest`, whose policy asset matches
CLN's `liquid_regtest_fee_asset`; two lightningd built from `sequentia-stable` with these changes; port 17300 /
lightning-dirs `$JOB/tmp/ln{1,2}`; `--force-feerates=5000` since regtest has no fee estimate).
- **Runtime bug caught + fixed** (commit `3e50f69`): a chain-PARSED / CLONED bitcoin_tx skipped `bitcoin_tx()`'s
  `output_asset` init → the asset-aware `get_amount_sat` assert read garbage and aborted lightningd on a wallet
  output. Fixed via `set_default_output_asset()` in `pull_bitcoin_tx_only`/`clone_bitcoin_tx`. Unit tests
  missed it; the live loop caught it immediately.
- **Policy-asset channel fully verified with all changes:** `fundchannel` → CHANNELD_NORMAL on both nodes → a
  50k-sat payment (HTLC + commit_tx) completed → cooperative close → CLOSINGD_COMPLETE. So open + operate +
  close are byte-behaviour-intact; the tx-core + lightningd/channeld work is proven not to regress.
- **GOLD blocker pinned (the wallet):** issued a GOLD asset, sent 100 to ln1 — `listfunds` shows ONLY the
  policy-asset outputs; the GOLD UTXO is invisible. CLN's wallet records/coin-selects only the policy asset
  (`amount_asset_is_main` filters in `wallet/wallet.c`).

## Wallet now RECORDS non-policy assets + CRITICAL libwally issuance bug fixed (2026-07-03)

DONE + verified (commit `ff1f492b` main repo; libwally submodule branch `sequentia-issuance-denomination`
commit `5bc915e3`):

- **Wallet records any issued asset.** `struct utxo` gained `asset[33]`; `got_utxo()` records each output's
  own asset+value with no policy assert; `wallet_extract_owned_outputs()` no longer skips non-policy outputs;
  `outputs` table gained an `asset` BLOB column (migration + INSERT + every SELECT + `wallet_stmt2output`
  read). `migrate_setup_coinmoves()` was crashing the DB migration (it runs the now-asset-aware utxo SELECT
  *before* the asset column exists) — gave it a minimal direct query. `chaintopology` skips the policy-only
  on-chain-invoice check for non-policy owned outputs (the wallet still records them).
- **CRITICAL: libwally could not parse Sequentia issuance txs.** Root cause: Sequentia's `CAssetIssuance`
  (`SequentiaByClaude src/primitives/confidential.h:200,208`) adds a 1-byte `nDenomination` after the inflation
  keys; SeqLN vendors stock libwally 1.4.0, which under-read every issuance input by 1 byte → `wally_tx_from_
  bytes` EINVAL → `bitcoin/block.c:231` NULL-deref → **lightningd SIGSEGV on any block with an issuance.** This
  blocked not just asset channels but SeqLN syncing the real Sequentia chain at all (the live node only
  survives because it hasn't re-parsed an issuance block). Patched `transaction.c` (analyze_tx count, field
  parser + new `wally_tx_input.issuance_denomination`, `get_txin_issuance_size` +1, `tx_to_bytes` re-emit) so
  issuance txs round-trip byte-exact; non-issuance inputs untouched. See memory
  `seqln-issuance-denomination-parse-bug`.
- **Verified end-to-end on liquid-regtest:** rebuilt the chain transparent-only (`blindedaddresses=0`, matching
  Sequentia's default), issued GOLD unblinded, a node **synced past the issuance block without crashing** and
  recorded a **100-GOLD UTXO with the correct GOLD asset tag** (DB `outputs.asset` = reversed `83053bb2…499d`).

**DEPLOY — DONE.** The libwally fix is now shippable: forked to `GracedEternalKingCabbageMan/libwally-core`
(public), pushed to branch `sequentia-issuance-denomination` (commit `5bc915e3`); seqln `.gitmodules` now
points at the fork + branch and the submodule pointer records `5bc915e3` (seqln `sequentia-stable` commit
`ee0fcaab`, pushed). A fresh `git clone --recursive` (or `submodule update --init`) delivers the patch —
verified. When pulling on the box, run `git submodule sync && git submodule update --init` after `git pull`
so the changed submodule URL is picked up (a plain `submodule update` would still try the old ElementsProject
URL and fail to find `5bc915e3`).

Two follow-up display/UX gaps (non-blocking): `listfunds` doesn't yet expose the `asset` field; the elements
node needs a fee exchange rate set for an asset before it will send it (`setfeeexchangerates`).

## Asset-aware coin selection + funding output DONE + broadcast-verified (2026-07-03)

Commit `27019a6d` (seqln `sequentia-stable`): the wallet can now select an issued asset and build a
single-asset funding tx.

- `wallet_find_utxo()` takes an `asset` and never mixes assets — only returns UTXOs of that asset, defaulting
  to the policy asset when NULL. This also closes a latent bug: now that non-policy UTXOs are recorded, the
  old (unfiltered) selection could have grabbed a GOLD UTXO and spent its value as policy sats.
- `fundpsbt` / `addpsbtoutput` accept an optional `asset` (display id, parsed to the 33-byte `0x01||reversed`
  tag). `finish_psbt` denominates the change and the elements fee output in that asset; `psbt_using_utxos`
  tags each input with its own `utxo->asset`. New `psbt_append_output_asset` builds outputs in a given asset.
  Result: the whole funding tx is single-asset (Sequentia's open fee market lets the fee be paid in the
  channel asset), which is exactly what keeps the downstream commitment tx single-asset.
- **Broadcast-verified on liquid-regtest:** `fundpsbt asset=GOLD` -> `addpsbtoutput asset=GOLD` -> `signpsbt`
  -> `sendpsbt` produced a **consensus-valid, on-chain-confirmed single-asset GOLD tx** (1 GOLD input of 100;
  outputs: 50 GOLD dest + 49.9999 GOLD change + 0.0000 GOLD fee, all GOLD). The default (no-asset) path still
  selects only the policy asset and never touches GOLD.

The channel 2-of-2 **funding output** is now just `psbt_append_output_asset` with a p2wsh script — the
primitive is proven.

## M1 DONE — GOLD channel opens to CHANNELD_NORMAL + cooperative close (2026-07-03)

Commit `ac829e0a` (seqln `sequentia-stable`): the single-funder open path (openingd) is asset-aware, so a
Lightning channel can be **funded in an issued asset** end to end.

- `fundchannel_start` gains an `asset` param -> `funding_channel.channel_asset` -> `openingd_funder_start`
  (new `channel_asset` wire field). `open_channel` carries the asset in a new `asset_id` TLV
  (`open_channel_tlvs` type 3); the fundee adopts it (absent TLV == policy asset, so ordinary opens are
  wire-identical). Both sides `memcpy` the negotiated asset onto `channel->channel_asset` right after
  `new_initial_channel()`, so the already-asset-aware `initial_commit_tx()` builds **byte-identical GOLD
  commitment txs** on funder + fundee. openingd returns the asset to lightningd in `openingd_funder_reply`/
  `openingd_fundee`; `wallet_commit_channel` stamps it on the new channel.
- Fixed the asset-blind asserts that crashed/blocked GOLD channels: `bitcoin_tx_compute_fee` /
  `psbt_input_get_amount` / `bitcoin_tx_output_get_amount_sat` now read the raw explicit value instead of
  asserting the policy asset; the `watch.c` funding-output watch matches the funding amount by raw value (it
  is pinned to the exact scriptpubkey+outpoint) so a GOLD funding output fires the lockin callback.
- **Verified on liquid-regtest** (raw `fundchannel_start`/`fundchannel_complete` + the asset-aware
  `fundpsbt`/`addpsbtoutput`, since the `multifundchannel` plugin is not yet asset-aware): a 10-GOLD channel
  opened -> **CHANNELD_NORMAL on both nodes** (single-asset GOLD funding tx: 10 GOLD funding + GOLD change +
  GOLD fee) -> cooperative close -> **CLOSINGD_COMPLETE**. The open succeeding at all proves both peers built
  the same GOLD commitment tx (mismatched sigs would have failed `funding_signed`). Default (no-asset) opens
  are unchanged.

The commit-tx + closing-tx builders were already asset-driven via `channel->channel_asset`, so no channeld/
closingd changes were needed for the no-HTLC path.

**Remaining for a production M1 / next milestones:**
- ~~DB persistence of `channel_asset`~~ **DONE** (commit `3aa834d1`): `channels.channel_asset` column +
  save in `wallet_channel_save` + load in `wallet_stmt2channel` (NULL -> policy). Verified: a GOLD channel's
  asset is stored as the GOLD tag, and after restarting lightningd the channel reloads to CHANNELD_NORMAL and
  cooperatively closes (CLOSINGD_COMPLETE), which only validates if the reloaded asset is GOLD. NB: upgrading an
  existing node's DB on a non-release (`-modded`) build needs `--database-upgrade=true` once.
- ~~`listfunds`/`listpeerchannels` asset display + `multifundchannel` plugin~~ **DONE** (commit `816fd758`):
  `fundchannel id amount asset=GOLD` now opens a GOLD channel to CHANNELD_NORMAL in ONE call (the plugin
  threads `asset` to fundchannel_start + fundpsbt and builds the 2-of-2 funding output via
  `psbt_insert_output_asset`); `psbt_elements_normalize_fees` was fixed to balance per-asset (it emitted a
  policy fee = whole input for a GOLD tx). `listfunds` outputs + `listpeerchannels`/`listfunds` channels
  surface a non-policy asset's display id. Still TODO: `openchannel_init`/`dualopend` (v2 dual-funding) asset
  path; `psbt_compute_fee` has the same policy-only bug (only matters for onchaind/anchor/force-close = M3).
- ~~**M2**: HTLCs denominated in the asset~~ **DONE + regtest-verified** (commit `9a2a6ec2`): a `pay` of a
  0.1-GOLD invoice settles `complete` over a 10-GOLD channel, moving exactly 0.1 GOLD ln1->ln2. The asset is a
  per-tx property, so it was one `bitcoin_tx_set_output_asset()` in `commit_tx()` (covers to_local/to_remote/
  HTLC/anchor/fee outputs) + a `channel_asset` param on `htlc_tx()`/`htlc_success_tx()`/`htlc_timeout_tx()`,
  threaded from `channel->channel_asset` in `channeld/full_channel.c`. Dust/trim + implicit-fee + msat->atom
  math needed no change (atoms == sat base unit). NULL asset == policy, so the BOLT-3 vector tests still pass
  byte-exact.
- **M3** (onchaind/force-close) — **core DONE + regtest-verified** (commit `286952e1`): a unilateral close of
  a 10-GOLD channel resolves on-chain — the commitment tx is single-asset GOLD and after the to_self CSV
  onchaind sweeps DELAYED_OUTPUT_TO_US, landing 9.8999 GOLD in ln1's wallet. Threaded `channel_asset` through
  `onchaind_init` (new byte,33 field) into an onchaind global; `onchaind_tx_unsigned()` (the real sweep
  builder) sets the tx asset; the onchaind grind/verify htlc builders + onchain_control htlc_tx calls pass it;
  relaxed the ~9 policy asserts in onchaind.c; fixed two generic asset-blind asserts that abort while PARSING a
  GOLD force-close tx (`bitcoin/tx_parts.c` cached-value assert + `psbt_output_get_amount`); `psbt_compute_fee`
  now balances per-asset. BOLT-3 vector tests still pass. **Adversarial audit (7-agent workflow) done:** the HTLC-sweep, their-unilateral,
  penalty, and unknown-commitment resolution paths audited CLEAN (my onchaind assert-relaxation +
  onchaind_tx_unsigned set_output_asset + htlc_tx channel_asset cover them). It found TWO verified hazards, both
  on the **anchor-CPFP** path (`lightningd/anchorspend.c`, only reachable for anchor channels — my verified
  channels use static_remotekey): the anchor input's witness_utxo was policy-tagged (fixed, commit `54a8c3c3`:
  `psbt_input_set_wit_utxo_asset`), and the CPFP tx is inherently MIXED-asset (GOLD anchor + policy wallet fee)
  so it needs a separate channel-asset change output + per-fee-asset `psbt_compute_fee` to balance (FIXME'd).
  **Until then a GOLD ANCHOR channel cannot CPFP-fee-bump its force-close (funds-at-risk); WORKAROUND: use
  non-anchor channel types for asset channels.** Guarded-skip follow-ups (no crash): gossip utxoset scans
  (`bitcoind.c`/`chaintopology.c`) + coop-close fee (`closing_control.c`) skip non-policy outputs — matters for
  PUBLIC asset channels + display, not resolution.
- Then **M4** (invoices+routing carry asset id + display precision), **M5** (pure-LN cross-network swap).

REMAINING for M1 (a large, funds-critical, REGTEST-GATED integration — verifiable only end-to-end, so it must
be done carefully, not rushed):
1. `create_close_tx()` + closingd: thread channel_asset (cooperative close in the asset).
2. **Wire codegen**: add channel_asset to `openingd`/`channeld`/`closingd` init messages (`.csv` +
   `tools/generate-wire.py`), so subdaemons learn the asset.
3. `lightningd/channel.h` struct channel + **DB persistence** (schema migration) of channel_asset.
4. Wallet **RECORDING** + **coin-selection** + **funding output** of non-policy assets — DONE + verified
   (below).
5. Open negotiation: `open_channel2`/`accept_channel2` `asset_id` TLV; `fundchannel` asset param.
6. Regtest e2e: `fundchannel` a GOLD channel → CHANNELD_NORMAL → cooperative close → GOLD returns.

Then M2 (commit_tx + HTLCs in the asset; the 11 commit_tx call sites), M3 (onchaind/force-close), M4
(invoices+routing), M5 (pure-LN cross-network swap). Each is further multi-day funds-critical work with no
prior art. The tx foundation above is the reusable core the rest builds on.
