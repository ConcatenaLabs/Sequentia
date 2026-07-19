# SBTC — a fixed-multisig BTC peg for Sequentia, and the DEX silent-peg

Status: **design for build** (all product decisions made with the user 2026-07-19). This is
the canonical design for the SBTC peg and its DEX integration. It is deliberately narrow:
**Sequentia uses NATIVE BTC**; SBTC exists only for the two use-cases below.

## 0. Why SBTC exists (and why it is narrow)

Sequentia's identity is **native Bitcoin, not a Liquid-style pegged BTC**. Native BTC is
the distinct, privileged asset in every wallet (the only asset shown at 0 in a fresh
wallet, top of the send/receive dropdowns) and stays that way.

There is exactly one thing native BTC cannot do: **rest a DEX limit order while the user is
offline.** Bitcoin has no covenants; Elements does. A resting, partial-fillable, offline
limit order needs a covenant, which needs a Sequentia asset. So a BTC limit order is
handled by wrapping the user's real BTC into **SBTC** for the duration of the rest. SBTC is
also exposed publicly as a normal, unprivileged asset (e.g. confidential-tx wrapping).

**A BTC peg cannot be trustless** — a peg-out must enforce "release real BTC iff SBTC was
burned," which is only enforceable on Bitcoin with covenants (which don't exist). So SBTC
is a **trusted federated peg**. Committee-custody was ruled infeasible (Bitcoin can't verify
the committee's BLS certs or express its 126-of-250 threshold, and custody can't rotate
per block). **Decision: a fixed N-of-M operator multisig** (the Liquid custody model),
simplest and least new consensus code.

## 1. The pegged asset — SBTC is DISTINCT from tSEQ (the one consensus subtlety)

In stock Elements the peg mints `consensus.pegged_asset`, and `pegged_asset ==
subsidy_asset == policyAsset` (all L-BTC). Sequentia's policy asset is **tSEQ** (the Sequence
token, for staking), and `pegged_asset` defaults to `subsidy_asset` (`src/chainparams.cpp:
505,774,1571-1572`). A BTC deposit must **not** mint tSEQ. So the peg must mint a **distinct
SBTC asset**, decoupled from the policy/subsidy asset:

- Set `consensus.pegged_asset` = a dedicated **SBTC** asset id, independent of `policyAsset`
  (tSEQ) and of `subsidy_asset`. (Sequentia has no coinbase subsidy — `genesis_subsidy=0`,
  principle 5 — so `subsidy_asset` is inert and safe to leave; the required change is
  decoupling `pegged_asset` from it so peg-in mints SBTC, and `sendtomainchain` /
  `claimpegin` burn/mint SBTC — `src/pegins.cpp:367`, `src/wallet/rpc/elements.cpp:498,852`.)
- SBTC gets a registry entry (ticker SBTC, subtitle "Pegged Bitcoin") and is treated as a
  normal, **unprivileged** asset everywhere (one row among equals). Native BTC stays
  privileged/distinct.
- `parent_pegged_asset` = the parent chain's BTC (real testnet4 BTC) — the asset a peg-in
  deposit is denominated in (`src/pegins.cpp:66`).

## 2. Custody — a fixed N-of-M operator multisig (the fedpeg)

- `has_parent_chain = 1`, `validatepegin = 1`, reusing the box's existing parent bitcoind
  link (`mainchainrpc*`, already wired for anchoring on testnet4 — the peg must not disturb
  `con_bitcoin_anchor`).
- `fedpegScript` = a fixed `N-of-M` `OP_CHECKMULTISIG` (or Taproot) over M designated
  operator keys. Testnet: we control the M keys (break-and-fix is fine, no real value).
- Peg-in address = P2SH/P2WSH of `calculate_contract(fedpegscript, claim_script)` — a
  per-user HMAC tweak of the fedpeg (`src/pegins.cpp:74-143`); `getpeginaddress`.
- Custody UTXOs live under the fedpeg on Bitcoin. Rotation is a rare manual ceremony
  (out of scope for the initial testnet build).

## 3. The watchman signer (GREENFIELD, off-chain)

The node only **emits the burn** on peg-out; it never signs the Bitcoin release. A new
off-chain **watchman** service (run by the M operators) does the release:

1. Watch the sidechain for peg-out burns: outputs whose script is `IsPegoutScript` =
   `OP_RETURN <parent_genesis_hash(32)> <parent_scriptPubKey>` (`src/script/script.cpp:
   248-278`), emitted by `sendtomainchain` (`src/wallet/rpc/elements.cpp:436-506`).
2. Construct a Bitcoin tx spending the fedpeg UTXO(s) to the burn's declared parent
   `scriptPubKey`, minus a Bitcoin fee.
3. Collect `N-of-M` operator signatures and broadcast on the parent chain.

Testnet implementation: a single service holding the M operator keys (or a small set of
services), polling the sidechain + the parent bitcoind. Idempotent + crash-safe (a
persisted "processed burns" set), never double-spends a fedpeg UTXO, never releases more
than the burn amount. This is the security-critical component; keep it simple and audited.

## 4. Peg-in / peg-out flows

- **Peg-in:** user sends real BTC to their `getpeginaddress` on testnet4 → after
  `peginconfirmationdepth` confirmations, `claimpegin` submits the SPV proof (validated
  against the parent bitcoind, `CheckPeginTx`, `src/pegins.cpp`) → mints SBTC to the user.
- **Peg-out:** `sendtomainchain <btc_address> <sbtc_amount>` burns SBTC + emits the pegout
  script → the watchman releases real BTC to `<btc_address>`.

Both are exposed publicly (SBTC is a usable asset).

## 5. The DEX silent peg — resting on-chain-BTC LIMIT orders

The one place the peg is **silent** (transparent): a maker rests a BTC limit order bringing
**real** parent-chain BTC AND the taker wants **real** parent-chain BTC.

1. Maker places a BTC limit bid. The wallet silently **pegs in** the maker's real BTC →
   SBTC, and rests the SBTC in a **covenant** (`CovenantTerms.asset_b = SBTC id`) on the
   `<asset>/BTC` book pair. The order rests, partial-fillable, offline-liftable.
2. A taker fills (fully or partially). The maker is credited the asset; the taker receives
   the SBTC (the covenant FILL).
3. The taker's SBTC is silently **pegged out** → real BTC to the taker's parent-chain
   address. Neither party need notice SBTC was involved.

A **market** taker paying real BTC settles interactively (online) and needs no peg. A maker
who wants to hold SBTC directly (not real BTC) simply skips the peg-out. The covenant /
matcher / relay plumbing (partial fills, CrossRail, the bridge-less settler) already exists.

## 6. Build order (bundled; ONE build/verify at the very end)

1. Consensus/config: decouple `pegged_asset`→SBTC, enable `has_parent_chain`+`validatepegin`
   +`fedpegScript` on a testnet config; verify anchoring intact. (node)
2. Watchman signer (off-chain). (new service)
3. SBTC registry + wallet as a normal unprivileged asset; public peg-in/out UX. (registry, wallet)
4. Silent DEX integration (peg-in on rest, peg-out on fill) into the covenant flow. (wallet, relay)
5. The DEX terminal settlement rewrite (rail-blind covenant book-walking; BTC via SBTC
   covenants). (wallet — the Tier A control surface is already committed on `terminal-rebuild`)
6. Verify EVERYTHING once (node rebuild, watchman, wallet builds, all combos incl. peg-in/out
   + silent resting BTC limit + partial fill), then ship. Do not rebuild/deploy mid-way.

## 7. Open items to confirm during build
- M and N for the operator multisig (testnet: e.g. 2-of-3 under our control).
- The SBTC asset id derivation (a dedicated genesis/issuance vs a computed pegged_asset).
- Whether `pegged_asset`-decoupling needs a small consensus change or is reachable via
  existing args (`-con_pegged_asset` does not currently exist; verify during step 1).
