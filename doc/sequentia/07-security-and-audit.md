# Security & audit

This chapter records the security model, the pre-mainnet adversarial audit and
the disposition of every finding, the analyses behind a few non-obvious
decisions, the features deliberately outside the implemented scope, and the
overall implementation status.

The threat review covered six subsystems - PoS block validation; anchoring,
reorg-following and fork choice; the VRF and MuSig2 cryptography; genesis, money
and tokenomics; the stake registry and unbonding; and the fee market, RPC and
DoS surface. No reviewer found a way to steal funds, mint SEQ beyond the 400M
cap, or force a permanent consensus split. Block **validation** is fully
decentralized: every node independently verifies the VRF proofs, committee
eligibility, the aggregate signature, the anchor, and the finality gate.

## 1. Audit findings and their disposition

### Fixed (consensus, policy, and launch-blocking)

- **CSV inactive on the real chain (critical).** Soft-fork activation heights
  inherited from Bitcoin (`CSVHeight=419328`, …) left BIP68/BIP112 - and thus the
  staking-output unbonding lock - unenforced; the lock was a no-op on spend. All
  are buried-active from genesis (`0`/`1`) on the bundled chains
  (`src/chainparams.cpp`).
- **PoS block-signature size too small (critical, blocked production).** The
  per-chain `max_block_signature_size` (74) was smaller than a real certified
  block's solution - the leader's ~73-byte DER signature plus the 64-byte MuSig2
  committee aggregate (~138 bytes) - so `CheckProof` rejected every committee
  block. With the default BLS certificate it is `300 ·
  MAX_POS_AGG_COMMITTEE_SIZE + 2000 ≈ 32,000` bytes on the bundled chains, sized
  for a full 100-member certificate; the `-posbls=0` MuSig2 fallback uses `200`
  bytes (leader DER plus one aggregate). The historical `150` was the
  MuSig2/Liquid figure and is wrong for Sequentia (`src/chainparams.cpp`).
- **Stake-registration outputs were non-standard (critical, blocked staking on
  mainnet).** The bare staking script solved to `TX_NONSTANDARD`, so a
  stake-registration transaction was refused by default relay policy; because
  `-acceptnonstdtxn` is rejected on the real chain, stake registration was
  impossible on mainnet. `IsStandard` (`src/policy/policy.cpp`) accepts the
  staking script as standard when PoS is enabled, so registrations relay and mine
  under default policy.
- **Open fee market silently disabled (critical).** Init re-read
  `-con_any_asset_fees` with a hard-coded `false` default, overwriting the value
  the chain's params set - so the open fee market was off unless every operator
  passed the flag, and would have shipped disabled on mainnet. Init defaults to
  the chain's own setting (`src/init.cpp`); the bundled chains keep their built-in
  open fee market.
- **Absurd-fee ceiling not exchange-rate-aware (fee-market correctness).** The
  `-maxtxfee` / `testmempoolaccept` `maxfeerate` ceiling compared a raw asset fee
  amount against the reference-denominated limit, so a fee paid in a
  low-per-unit-value asset could be spuriously rejected (notably in `bumpfee` and
  `testmempoolaccept`). The fee is valued in the reference unit before the
  comparison (`src/wallet/feebumper.cpp`, `src/rpc/rawtransaction.cpp`), matching
  the relay and RBF checks (see [`02-open-fee-market.md`](02-open-fee-market.md) §4).
- **Sortition-seed grindability (medium).** The election seed mixed the
  producer-grindable Sequentia block hash; it is derived from the parent block's Bitcoin
  anchor hash and height - deterministic on every node and unbiasable, since it is
  Bitcoin's proof-of-work (`ComputePosSeed`, see
  [`04-proof-of-stake.md`](04-proof-of-stake.md)).
- **Forged-sibling fork-choice inflation (high).** The fork-choice key was set
  from an unverified coinbase committee count, so a forged tip-sibling could claim
  excessive certification weight and force reorg churn. `SetPosForkChoiceKeys`
  counts only distinct committee members and clamps the count to
  `MAX_POS_AGG_COMMITTEE_SIZE`, so no block can advertise more certification weight
  than a full committee (residual analysed in [§2](#2-unvalidated-forksibling-blocks)).
- **Placeholder-genesis safety (high, operational).** A node refuses to start on
  `-chain=sequentia` with the published placeholder genesis unless
  `-allowplaceholdergenesis` is set (`src/init.cpp`).
- **Single fee asset per transaction (medium).** The mempool rejects
  multi-fee-asset transactions (attacker-controllable mis-valuation; this also
  fixed an empty-`fee_map` undefined-behaviour edge).
- **`generateposblock` committee-array DoS cap (medium).**
- **Per-chain `MAX_MONEY`.** Bitcoin chains keep `2.1e15`; Sequentia uses `4e16`
  (400M SEQ). A global change had broken inherited Bitcoin transaction tests.
- **Disjoint quorums under threshold sortition (consensus, the "honest
  splits" analysis).** Under private threshold VRF sortition the eligible
  committee is a random variable while the quorum is fixed at a majority of the
  *expected* size, so once the staker pool exceeds the committee target two
  disjoint quorums could certify rival same-height blocks. Closed by
  construction with the **public fixed-size committee**
  (`-pospubliccommittee`): membership is the deterministic public schedule
  prefix and the quorum derives from the actual committee size (strict
  majority, plus one at odd sizes), so any two quorums overlap in at least two
  members (see [`04-proof-of-stake.md`](04-proof-of-stake.md) §4;
  `feature_pos_public_committee.py`). Adopted network-wide at the 2026-07-05
  testnet re-genesis (cap 250, quorum 126).
- **Testnet dynamic-federation misconfiguration (blocked testnet production).**
  The public testnet had `DEPLOYMENT_DYNA_FED` active, which is incompatible with
  the per-block PoS challenge - the challenge was never set, producing
  `bad-pos-challenge`. Dynamic federations are never active on the bundled chains
  (`src/chainparams.cpp`).
- **`-poscommitteesize` ignored on testnet (blocked single-operator bootstrap).**
  The testnet hard-coded the committee size, preventing a single operator from
  standing up a small committee. The testnet honours `-poscommitteesize`; the
  value is not part of the genesis commitment, so changing it does not change the
  genesis. This enables the bootstrap tooling
  ([`05-operating-sequentia.md`](05-operating-sequentia.md) §7).

### Accepted by design (documented, not bugs)

- **Escaping-stall down to a single signer**, including genesis→block-1, is the
  intended bootstrap: a lone genesis founder must be able to certify blocks until
  others stake. At a full majority quorum, immediate finality is fork-free because
  the committee **excludes any equivocating leader** (Liveness theorem 1; see
  `proposals/autonomous-committee.md` §12.4), so an equivocator's blocks never
  reach 51. In the *relaxed* escaping-stall mode the quorum protection is
  intentionally weakened, so competing sub-threshold blocks are instead resolved
  deterministically by the fork choice, with the checkpoint depth
  (`-poscheckpointdepth`) as the long-range backstop - there is no slashing, and
  none is needed for safety.
- **Anchor R3 (Bitcoin best-chain membership) is a soft, eventually-consistent
  gate**, not hard consensus: view-dependent results are classified
  `BLOCK_RECENT_CONSENSUS_CHANGE` (non-banning, retryable), so an honest node with
  a lagging `bitcoind` defers rather than splits. The hard anchor rules are the
  height/hash commitment and the reorg-following watcher.

### Open hardening (lower priority, not launch-blocking)

- A narrow equal-work "cousin" fork (an equal-work block at the tip height whose
  parent is not the tip's parent) skips the accept-time PoS check and is stored;
  it is bounded (equal work, cannot churn the tip via the clamped fork-choice key,
  pruning-reclaimed) and requires an actively malicious peer. Deeper/lower-work
  forks are rejected before storage by the more-or-equal-work gate.
- The reorg-following watcher (`src/anchor.cpp`) snapshots each candidate's anchor
  under `cs_main` and queries `bitcoind` outside the lock. Header validation still
  performs the R3 anchor RPC under `cs_main`, but only once per novel-anchor
  header; the stall surface is bounded.
- The stake registry retains sub-minimum "dust" staking outputs (memory only,
  deterministic). A per-output floor is deliberately avoided because it would
  break legitimate split stake (a staker whose total clears the floor across
  several outputs); the cost of creating dust outputs bounds it.

### Requires external sign-off (cannot be done in-repo)

- Independent cryptographic review of the vendored secp256k1 **MuSig2** module
  (confirm it matches a known-good upstream commit with no local patches), the
  **ECVRF** byte layout and test vectors, and the vendored **blst** (BLS12-381)
  library used for committee certification (pinned in `src/blst/`; confirm it
  matches the named upstream commit with no local patches, per
  `src/blst/SEQUENTIA-VENDOR.md`).
- A **BDB** CI build so the cross-asset RBF/CPFP fee tests (which need
  legacy-wallet issuance) execute; this build is SQLite-only.

## 2. Unvalidated fork/sibling blocks

On a signed/PoS chain every valid block has equal work (height): `GetBlockProof`
returns 1 for signed blocks, and PoS validity is checked in `CheckPosStakeRules`.
A sibling of the tip passes the unrequested-block work gate and is written to
disk before it is connected. The fork-choice inflation lever is closed (distinct,
clamped countersignature counts, [§1](#fixed-consensus-policy-and-launch-blocking)),
so a forged sibling can never outrank the honest tip.

`AcceptBlock` stores only blocks with more-or-equal work than the tip, so on a
height-is-work chain only tip-height blocks are ever stored - a deeper fork is
rejected before storage. The residual forged-sibling storage flood is therefore
bounded: resource-only, pruning-reclaimed, and unable to churn the tip. A
determinism-safe accept-time validation of an arbitrary sibling's parent registry
(against persisted short-range snapshots rather than a live tip revert) is the
correct full closure; a live-revert attempt was not adopted because it could
yield a verdict differing from the direct tip-child path and split consensus.

## 3. PoS consensus rejects and peer banning

Registry-relative PoS rejects (`bad-pos-leader-not-staker`,
`bad-posvrf-member-not-selected`, `bad-pos-early`, …) use `BLOCK_CONSENSUS`
(a banning result). This is correct: `CheckPosStakeRules(block, pprev)` always
runs with the stake registry synced to the block's actual parent - at accept time
only when `pprev == tip`, and at connect time `ConnectBlock` runs it before
`PosApplyBlockStake` while `DisconnectTip` reverts stake in lockstep. A failure is
therefore an objective invalidity of the block for its real parent, and banning
the relayer of a genuinely invalid block is correct.

## 4. Fork choice and immediate finality

The full-versus-sub-threshold fork-choice preference is the
`CBlockIndexWorkComparator` secondary keys - certification (countersignature
count) then VRF score - not a header-format change. Anchor freshness is
deliberately not a fork-choice key: in an immediate-finality system a rule keyed
on the Bitcoin anchor could let a new Bitcoin block reorder or overwrite an
already-certified block. The VRF/committee result is the truth; cross-chain-swap
freshness is delivered by block production and a committee signing preference, not
by fork choice. A hard immediate-finality gate (`UpdateTip` +
`ContextualCheckBlockHeader`) locks a quorum-certified block against any
Sequentia-internal competitor - even one carrying more signatures - using a soft,
retryable rejection, so only a Bitcoin reorg of the anchor (via the watcher,
which lowers the finalized point) can displace it. The mechanics are specified in
[`04-proof-of-stake.md`](04-proof-of-stake.md); the behaviour is exercised by
`feature_pos_finality.py`.

## 5. Features beyond the implemented scope

The node and consensus core - open fee market, Bitcoin anchoring, PoS with VRF
sortition, committees, checkpoints, minimum stake, unbonding and escaping-stall,
opt-in confidential transactions with Bitcoin-identical addresses, and asset
issuance via inherited Confidential Assets - is implemented. The block-weight cap
(`-con_maxblockweight`, 200,000 on the Sequentia chain) holds a saturated chain's
total disk growth equal to Bitcoin's at the 30-second cadence and is enforced in
`CheckBlock`/`ContextualCheckBlock`.

The following whitepaper items are node-level but outside the four-property
scope (future subsystems, not regressions):

- **Asset ACLs** (whitelist/blacklist/freeze/amount/timelock filters) - to be
  built with Simplicity, the Elements scripting language, rather than bespoke
  opcodes.
- **Programmable accounts** (an account VM, `OP_DEPLOY`) - long-term, and possibly
  unnecessary if Simplicity subsumes them.
- **Utreexo / accumulator statelessness** - a future upgrade to track once it
  matures upstream; only generic Bitcoin `-prune` is inherited today.

## 6. Implementation status

All four defining properties and the Proof-of-Stake consensus are implemented and
validated by every node. Block production has three paths
([`04-proof-of-stake.md`](04-proof-of-stake.md) §9): the coordinator/RPC MuSig2
flow, an autonomous single-node producer (`-posproducer`), and an autonomous
peer-to-peer **gossip-and-sign committee** (`-posbls`) that assembles a
BLS-certified block across separate hosts with no coordinator - fully
implemented and tested (`feature_pos_bls_gossip.py`), with anti-DoS
validate-before-relay/misbehaviour scoring and crashed-member round recovery
(`feature_pos_gossip_dos.py`, `feature_pos_gossip_failover.py`), including the
round-robin / anchor-reshuffle recovery paths and large (100-member) committees,
detailed in
[`proposals/autonomous-committee.md`](proposals/autonomous-committee.md)
§12.4 - which also explains why **stake slashing is a deliberate non-goal**
(safety rests on independent validation and Bitcoin checkpoints, not stake-at-
risk, so the deterrent is redundant). BLS certification is the **default**
committee certification on the bundled chains (`-posbls` defaults true on
`-chain=sequentia` and `-chain=test`, false on custom chains); MuSig2 is the
legacy fallback selected by `-posbls=0`. The autonomous gossip-and-sign
committee is how the bundled chains run - the live public testnet runs it with
the public fixed-size committee and bitfield certificates
(`-pospubliccommittee`, cap 250) since the 2026-07-05 re-genesis. The open
hardening items and external sign-offs in
[§1](#1-audit-findings-and-their-disposition) are the
remaining pre-mainnet review tasks.
