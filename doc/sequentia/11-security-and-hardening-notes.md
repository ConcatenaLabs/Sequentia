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
is deferred for the reasons in [doc 10 §6–§7](10-liveness-and-escaping-stall.md):
it needs certification strength at header-acceptance time (a header/proof format
change) or a connect-time chain-work recomputation. Safe to defer — sub-threshold
blocks are validly certified, so same-height forks converge by first-seen then
by height.

## 4. Whitepaper features beyond the implemented consensus scope

A coverage analysis of the whitepaper against this node confirmed the node/
consensus core (open fee market, Bitcoin anchoring, PoS with VRF sortition +
committees + checkpoints + min-stake + unbonding + escaping-stall, opt-in CT /
Bitcoin-identical addresses, and RAS issuance via inherited Confidential Assets)
is implemented. The following whitepaper items are **node-level but out of the
declared four-challenge scope** — large future consensus subsystems, not
regressions, and not claimed by the design docs:

- **Asset ACLs (§4.5)** — whitelist/blacklist/freeze/amount/timelock filters on
  assets. New script/consensus rules beyond Elements Confidential Assets.
- **Programmable accounts (§4.6)** — `OP_DEPLOY`/`OP_ASSIGN`/`OP_SENDER`, gas, an
  account VM. The largest missing subsystem; entirely absent.
- **Utreexo / accumulator statelessness (§3.6, §3.10)** — only generic Bitcoin
  `-prune` is inherited.
- **Block-size parameter (§3.10)** — the chain still uses the inherited 4 MB
  weight limit (`src/consensus/consensus.h`), not the whitepaper's 0.5 MB. This
  is a one-line consensus parameter but a deliberate throughput/economic choice
  (and a launch parameter, like the genesis stake set and SEQ supply), so it is
  left for the launch configuration rather than guessed here.

These are documented so the gap between "the four challenges + PoS" (delivered)
and "the full whitepaper product roadmap" (multi-subsystem) is explicit.
