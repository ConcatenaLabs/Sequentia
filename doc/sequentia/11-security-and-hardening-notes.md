# Security & hardening notes (pre-mainnet review)

The record of the pre-mainnet adversarial review: what was found, what was fixed,
what is accepted by design, and what remains. §5 is the consolidated audit
summary; §§1–4 give the detailed analysis of specific items. Each records the
reasoning so the decision is auditable. (Earlier drafts framed some of these as
"safe to defer"; the consensus/security-relevant ones have since been fixed —
see §5.)

## 1. Unvalidated fork/sibling blocks — tip-siblings FIXED; deeper forks bounded

**What (the original issue).** On a signed/PoS chain every valid block has equal
"work" (height): `GetBlockProof` returns 1 for signed blocks (`src/chain.cpp`).
PoS leader/committee validity is checked in `CheckPosStakeRules`. A **sibling of
the tip** (same height, `pprev == tip->pprev`) passes the unrequested-block work
gate, was not PoS-checked at accept time, and was written to disk; it is never
connected, so the peer was never punished. Worse, the fork-choice key
`m_pos_countersigs` was set from the *unverified* coinbase committee count, so a
forged sibling could be ranked above the honest tip and force reorg churn. (The
per-block challenge means anyone's self-signed block passes `CheckProof`
structurally — unique to this PoS model; in stock Liquid only the fixed
federation can sign a header.)

**Fixed (the churn lever).** `SetPosForkChoiceKeys` now counts only **distinct**
committee members and **clamps** the fork-choice count to
`MAX_POS_AGG_COMMITTEE_SIZE`, so no block — forged or not — can advertise more
certification weight than a real full committee. This removes the gross
fork-choice-inflation lever (a forged sibling can no longer claim 65535
countersigs to outrank the honest tip).

**Reverted (the accept-time sibling check) — needs a determinism-safe redesign.**
An attempt to also reject invalid tip-siblings at accept time by temporarily
reverting the tip's stake to recreate the sibling's parent registry
(`CheckPosStakeRulesAtAccept`) produced a verdict that could **differ** from the
direct tip-child path — a node seeing a block as a sibling rejected it while a
node seeing it as a tip-child accepted it, causing a consensus split and wrongful
peer bans (caught by `feature_pos_stake`). That was worse than the bounded DoS it
targeted, so it was reverted to the original tip-child-only accept-time check.
The forged-sibling storage flood therefore remains (bounded: resource-only,
pruning-reclaimed, requires an actively-malicious peer, and the clamped
fork-choice keys prevent it from churning the tip). A correct fix is a
determinism-safe parent-registry validation (e.g. against persisted short-range
registry snapshots rather than a live revert) + multi-node fork tests.

**Why deeper forks are not a storage vector.** `AcceptBlock` only stores a block
that has **more-or-equal work** than the tip
(`if (!fHasMoreOrSameWork) return true; // Don't process less-work chains`,
`src/validation.cpp`). On a height-is-work chain that means only blocks at the
tip's height (or higher) are ever written to disk — a *lower-height* (deeper)
fork is rejected before storage. So the only unconnected blocks that can be
stored are equal-work ones at the tip height, i.e. siblings of the tip, which the
accept-time check above now validates and rejects if forged.

**Narrow residual.** An equal-work block at the tip height whose parent is *not*
the tip's parent (a "cousin": built on some other, previously-stored tip-1-height
block) still skips the accept-time PoS check (its parent registry isn't the one
we can recreate) and is stored. This requires the attacker to already have gotten
that alternate parent stored, is bounded (equal work, can't churn the tip; clamped
fork-choice key), and is pruning-reclaimed. Closing it fully means generalising
the accept-time validation to any in-range active-chain ancestor (bounded revert)
or an oldest-evicting cap on stored unconnected blocks — low priority. (Note the
tempting "don't store unsolicited equal-work blocks" is **wrong**: a
full-threshold block that should displace a sub-threshold tip arrives unsolicited
at the same height; the §3.8 fork choice needs it kept.)

## 2. PoS consensus rejects and peer banning — analyzed, NOT a bug

A review flagged that registry-relative PoS rejects (`bad-pos-leader-not-staker`,
`bad-posvrf-member-not-selected`, `bad-pos-early`, …) use
`BlockValidationResult::BLOCK_CONSENSUS` (a 100-point ban) and worried an honest
relayer could be banned for a verdict that is parent-state-dependent.

**Conclusion: this is not a real issue.** `CheckPosStakeRules(block,
pindex->pprev)` always runs with the stake registry synced to the block's
**actual parent**: at accept time only when `pprev == tip` (registry == tip ==
pprev), and at connect time `ConnectBlock` (which calls it) runs *before*
`PosApplyBlockStake`, and `DisconnectTip` reverts stake in lockstep during a
reorg — so when block `B` with parent `P` is validated, the registry is exactly
`P`'s state (verified: `src/validation.cpp` ConnectBlock@3139 →
CheckPosStakeRules@2325, PosApplyBlockStake@3180; DisconnectTip revert@3037).
A failure is therefore an **objective** invalidity of `B` for its real parent,
identical in character to any other `BLOCK_CONSENSUS` reject, and banning the
relayer of a genuinely-invalid block is correct. No change made.

## 3. Anchor-driven liveness fork-choice (cross-reference)

The deterministic full-vs-sub-threshold fork-choice preference (whitepaper §3.8)
is **implemented** — via the `CBlockIndexWorkComparator` secondary keys, not a
header-format change (the earlier "needs header-time work" assessment was
wrong; the comparator already drives equal-work reorgs). The comparator keys are
**certification (countersig count) then VRF score** only. An anchor-freshness key
was tried here and **removed** (conceptual-creator review): in an
immediate-finality system a fork-choice rule keyed on the Bitcoin anchor could
let a new Bitcoin block reorder — even overwrite — already-certified blocks. The
VRF/committee result is the truth; cross-chain-swap freshness is delivered by
block *production* (every new block anchors to the freshest Bitcoin block) and,
as a refinement, a committee signing preference — never by fork choice (doc 10
§7). A **hard immediate-finality gate** (`UpdateTip` + `ContextualCheckBlockHeader`)
now also locks a quorum-certified block against any SEQ-internal competitor — even
one carrying more signatures (equivocation) — using a soft, retryable rejection so
a Bitcoin reorg of the anchor (handled by the watcher, lowering the finalized
point) is the only thing that can displace it. Tested in `feature_pos_finality.py`.
The two formerly-open liveness items are now decided: block timing is already aligned
with the whitepaper's wall-clock round model (no change), and the dynamic
committee floor is **not** implemented (its trigger/curve is underspecified in
the paper and its liveness purpose is already met by escaping-stall). No
specified consensus mechanism remains open.

## 4. Whitepaper features beyond the implemented consensus scope

A coverage analysis of the whitepaper against this node confirmed the node/
consensus core (open fee market, Bitcoin anchoring, PoS with VRF sortition +
committees + checkpoints + min-stake + unbonding + escaping-stall, opt-in CT /
Bitcoin-identical addresses, and RAS issuance via inherited Confidential Assets)
is implemented. The following whitepaper items are **node-level but out of the
declared four-challenge scope** — large future consensus subsystems, not
regressions, and not claimed by the design docs:

- **Asset ACLs (§4.5)** — whitelist/blacklist/freeze/amount/timelock filters on
  assets. Per the project direction these are to be built with **Simplicity**
  (the new Elements scripting language), not bespoke opcodes — a future
  scripting-layer addition, not a base-consensus change here.
- **Programmable accounts (§4.6)** — an account VM / `OP_DEPLOY` etc.
  **Deferred long-term, and possibly dropped** if Simplicity proves to make
  them unnecessary. Not pursued.
- **Utreexo / accumulator statelessness (§3.6, §3.10)** — a future upgrade to
  be explored only once it matures in Bitcoin upstream; only generic Bitcoin
  `-prune` is inherited today.

**Block-size parameter (§3.10) — DONE.** The chain now caps block weight at a
per-chain value (`consensus.nMaxBlockWeight`, `-con_maxblockweight`), set to
**200,000** on the Sequentia chain — a twentieth of Bitcoin's 4,000,000, so at
the ~30-second target cadence (20× Bitcoin, `-posslotinterval=30`) a saturated
Sequentia chain grows at exactly the same *total* rate as a saturated Bitcoin
chain (200,000 / 30 s == 4,000,000 / 600 s; the cap counts full serialized
weight, so total disk — not just user data — is what is held equal).
Enforced in `CheckBlock`/`ContextualCheckBlock` and respected by the miner;
tested in `feature_max_block_weight.py`. (See doc 12 for SEQ supply / genesis.)

These notes keep the boundary between "the four challenges + PoS" (delivered)
and the longer whitepaper roadmap explicit.

## 5. Pre-mainnet adversarial audit — findings & remediation

A structured adversarial review covered six subsystems (PoS block validation;
anchoring/reorg-following/fork-choice; VRF + MuSig2 crypto; genesis/money/
tokenomics; stake registry/unbonding; fee market/RPC/DoS). No reviewer found a
way to steal funds, mint SEQ beyond the 400M cap, or force a permanent consensus
split. Findings and their disposition:

**Fixed (consensus/security):**
- **CSV inactive on the real chain (CRITICAL).** The chain's soft-fork
  activation heights were inherited from Bitcoin (`CSVHeight=419328`, …), so
  BIP68/BIP112 — and thus the staking-output unbonding lock — were unenforced.
  Now all set to 0/1 (buried-active from genesis). Without this the unbonding
  lock was a no-op on spend (nothing-at-stake). (`src/chainparams.cpp`.)
- **Forged-sibling fork-choice / reorg churn (HIGH) — partially fixed.** The
  gross lever is closed: fork-choice keys are clamped/deduped so no block can
  claim more than a full committee's worth of countersigs. The accept-time
  sibling *rejection* was reverted (its live-revert approach caused a consensus
  split — see §1); the residual forged-sibling storage flood is bounded.
- **Sortition-seed grindability (MEDIUM).** The seed mixed the producer-grindable
  SEQ block hash; now it is derived from the parent's Bitcoin **anchor hash** +
  height (header fields — deterministic on every node, and unbiasable: it is
  Bitcoin's PoW). (`ComputePosSeed`, doc 06/07. An earlier VRF-score-chained seed
  was reverted — that value is set after block-index creation and was not a
  node-consistent seed input, causing a split.)
- **Placeholder-genesis safety (HIGH, operational).** A node refuses to start on
  the real chain (`-chain=sequentia`) with the published placeholder genesis
  unless `-allowplaceholdergenesis` is set. (`src/init.cpp`, doc 13.)
- **Single fee asset per tx (MEDIUM).** Mempool now rejects multi-fee-asset txs
  (attacker-controllable mis-valuation; also fixed an empty-`fee_map` UB).
- **`generateposblock` committee-array DoS cap (MEDIUM).**
- **`MAX_MONEY` per-chain.** Bitcoin chains keep 2.1e15; Sequentia uses 4e16
  (400M SEQ). (Raising it globally had broken inherited Bitcoin tx tests.)

**Accepted by design (documented, not bugs):**
- **Escaping-stall down to a single signer**, and **genesis→block-1 via
  escaping-stall**, are the intended bootstrap mechanism (a lone genesis founder
  must be able to certify until others stake). A "race" between competing
  sub-threshold blocks is resolved deterministically by the §3.8 fork choice; the
  economic backstop against short-range equivocation is the **checkpoint depth**
  (`-poscheckpointdepth`) — there is no slashing (nothing-at-stake is bounded,
  not eliminated). State this in operator guidance.
- **Anchor-freshness ordered above the VRF tiebreak** (doc 10 §7): a stale-
  anchored leader can be orphaned by a fresher-anchored competitor. This is the
  *intended* real-time-swap behaviour (it incentivises fresh anchoring); churn is
  bounded to ~1-block reorgs at anchor-advance boundaries, with no oscillation
  (anchor heights are monotonic).
- **Anchor R3 (Bitcoin best-chain membership) is a soft, eventually-consistent
  gate**, not hard consensus: view-dependent results are classified
  `BLOCK_RECENT_CONSENSUS_CHANGE` (non-banning, retryable), so honest nodes with
  lagging bitcoind defer rather than split. Corollary: `-validateanchor=0` nodes
  accept blocks validating nodes defer — a deliberate configuration choice. The
  hard anchor rules are R1/R2 + the reorg-following watcher.

**Open hardening (lower priority, not launch-blocking):**
- Narrow equal-work "cousin" fork disk-fill (§1 residual; deeper/lower-work forks
  are already rejected before storage by the more-or-equal-work gate).
- **Anchor watcher cs_main RPC — FIXED.** The reorg-following watcher
  (`src/anchor.cpp`) now snapshots each candidate block's (immutable) anchor
  under `cs_main`, then queries bitcoind *outside* the lock, so a slow/hung
  parent daemon no longer stalls block/RPC/net processing. Residual: header
  validation (`ContextualCheckBlockHeader`, `src/validation.cpp`) still does the
  R3 anchor RPC under `cs_main` — but only once per *novel-anchor header* (not a
  loop), and header acceptance is inherently synchronous under `cs_main`, so the
  stall surface is bounded; deferred.
- The stake registry stores sub-min-stake "dust" staking outputs (memory only;
  deterministic, no split). A per-output floor is **not** the fix — it would
  break legitimate *split stake* (a staker whose total clears the floor across
  several smaller outputs). Bounded by on-chain cost (each dust output costs a
  fee + block weight); left as accepted low-risk rather than regress split-stake.

**Requires external sign-off (cannot be done in-repo):**
- Independent crypto review of the vendored secp256k1 **MuSig2** module (confirm
  it matches a known-good upstream commit, no local patches) and the **ECVRF**
  byte layout / test vectors (no official SECP256K1 vectors exist).
- A **BDB** CI build so the `feature_any_asset_fee*` cross-asset RBF/CPFP tests
  (which need legacy-wallet issuance) actually execute; this build is SQLite-only.
