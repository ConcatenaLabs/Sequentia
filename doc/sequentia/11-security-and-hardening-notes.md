# Security & hardening notes (pre-mainnet review)

Items found by adversarial review that are **safe to defer** (none risks funds,
a consensus split, or a stuck chain on a correctly-configured network) but
should be reviewed/closed before a value-bearing mainnet. Each records the
analysis so the decision is auditable.

## 1. Unvalidated fork/sibling blocks can be stored to disk (peer DoS)

**What.** On a signed/PoS chain every valid block has equal "work" (height):
`GetBlockProof` returns 1 for signed blocks (`src/chain.cpp`). PoS leader/
committee validity is checked in `CheckPosStakeRules`, which runs
authoritatively at connect time and, at accept time, only when the block builds
directly on the active tip (`pindex->pprev == m_chain.Tip()`,
`src/validation.cpp`). A block that is a **sibling of the tip** (same height,
`pprev == tip->pprev`) passes the unrequested-block work gate (equal work ⇒
`fHasMoreOrSameWork`), is *not* PoS-checked at accept time (`pprev != tip`),
passes `CheckBlock`/`ContextualCheckBlock` (PoS rules excluded there), and is
written to disk. It is never connected (equal work doesn't displace the tip),
so `CheckPosStakeRules` never runs and the peer is never punished. An attacker
can sign unlimited distinct siblings with **their own** key (the per-block
challenge means anyone's self-signed block passes `CheckProof` structurally) and
push them, filling disk. This is unique to the per-block-challenge PoS model —
in stock Liquid only the fixed federation can sign a header.

**Why it's deferrable.** It is a resource-exhaustion nuisance, not a
consensus/fund risk: the blocks are never connected, never affect the active
chain, and are reclaimed by pruning. It requires an actively-malicious peer
pushing *unrequested* blocks.

**Proper fix (needs review — touches block download / consensus).** Either
(a) PoS-validate a fork block against the **registry at its own parent** before
persisting — which for a sibling means rewinding the stake registry one block
(`PosRevertBlockStake(tip)` → validate → `PosApplyBlockStake(tip)`), or more
generally maintaining short-range registry snapshots; or (b) decline to *store
unrequested* equal-work blocks that cannot be PoS-validated yet (require
strictly-more work for unsolicited storage), relying on headers+getdata to
fetch genuine forks. Option (b) is smaller but changes propagation of honest
equal-work forks (rare under committee certification; covered by headers
fetch). Both are propagation/consensus-adjacent and want human review +
multi-node fork tests before landing.

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
wrong; the comparator already drives equal-work reorgs). See
[doc 10 §6](10-liveness-and-escaping-stall.md). The remaining liveness items
(stages 4–5: anchor clock, dynamic committee floor) are fidelity refinements.

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
**400,000** on the Sequentia chain — a tenth of Bitcoin's 4,000,000, so at the
~1-minute target cadence (10× Bitcoin) a saturated Sequentia chain grows at the
same rate as a saturated Bitcoin chain (~100 KB of base data per block).
Enforced in `CheckBlock`/`ContextualCheckBlock` and respected by the miner;
tested in `feature_max_block_weight.py`. (See doc 12 for SEQ supply / genesis.)

These notes keep the boundary between "the four challenges + PoS" (delivered)
and the longer whitepaper roadmap explicit.
