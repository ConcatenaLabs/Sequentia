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

## First step

M0: add `channel_asset` to `struct channel` (both defs), default = `fee_asset_tag`, plumb it through the
channeld/onchaind init messages, add `amount_asset_is(amount, asset)` — all no-behavior-change, Phase 1 green.
