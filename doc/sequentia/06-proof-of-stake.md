# Challenge 3 — Proof-of-Stake consensus (PoC)

> **Branch:** `claude/sequentia-proof-of-stake-w6xady`. This is the first
> proof-of-concept of the Proof-of-Stake consensus from the theoretical paper
> (section iv, principles 3/6, and the consensus algorithm). It builds on the
> Bitcoin-anchoring branch, because the paper derives the consensus randomness
> seed **and** the liveness clock from the Bitcoin anchor.

## 1. Scope of this PoC

The paper's full design is a BFT protocol with a 100-member committee, private
VRF cryptographic sortition, 51/100 countersignature certification, and
Bitcoin checkpoints against long-range attacks. That is a multi-stage build.
This branch delivers the **first, runnable layer**: a stake-weighted,
anchor-seeded, deterministic **single-leader** election that replaces Elements'
fixed federation, with on-chain verification reusing the existing signed-block
signature machinery.

What this PoC **does**:

- Maintains a **stake registry** `{staker pubkey → stake weight}`.
- Elects, for every block height, a **deterministic ranked leader schedule**
  seeded from the previous block hash **and its Bitcoin anchor** (tying PoS to
  challenge 2), weighted by stake.
- Enforces in consensus that a block is signed by the **rank-`r` leader** for
  its slot, where `r` is gated by the time elapsed since the parent (liveness:
  if the primary leader is absent, the next-ranked staker may step in after a
  slot interval).
- Produces blocks from the miner when the node holds the eligible leader's key.
- Exposes `getstakerinfo` / `getposschedule` RPCs.

What this PoC **defers** (documented in §6, future work):

- Private VRF sortition (this PoC uses a *public* deterministic election, so the
  schedule is predictable — see §5 security notes).
- 100-member committee + 51-vote quorum certification (single leader for now).
- On-chain stake registration / unbonding (stake set is chain configuration
  here, not yet chainstate-tracked).
- Bitcoin checkpoints + stake locktimes against long-range attacks.

## 2. How it maps onto Elements' signed blocks

Elements "signed blocks" already carry a challenge/solution in the header:
`CheckChallenge` requires `block.proof.challenge == prev.challenge` (a *fixed*
federation script), and `CheckProof` verifies the witness/solution satisfies
that challenge script (`src/block_proof.cpp`).

PoS changes exactly one thing: the challenge is **computed per block** instead
of inherited. For a block at height `h` with parent `P`:

```
seed_h      = SHA256( P.GetBlockHash() || P.m_anchor_hash || LE32(h) )
schedule_h  = rank stakers ascending by  H(seed_h || pubkey) / weight
leader_r    = schedule_h[r]
challenge_h = <leader_r.pubkey> OP_CHECKSIG
```

Consensus (`CheckChallenge`, PoS mode):

1. The block's `proof.challenge` must equal `<some registered staker> CHECKSIG`.
2. Let `r` be that staker's rank in `schedule_h`. Require
   `block.nTime >= P.nTime + r * pos_slot_interval` (the staker's slot has
   opened — a higher-ranked staker cannot pre-empt a lower-ranked one).
3. `CheckProof` (unchanged) verifies the block signature satisfies
   `challenge_h` — i.e. the block really is signed by `leader_r`.

The signature itself rides the existing `proof.solution` (legacy) or
`m_signblock_witness` (dynafed) plumbing untouched. The Bitcoin anchor is
already committed in the header (challenge 2), so the seed is covered by the
block hash the leader signs.

## 3. Election in detail

- **Stake registry** (`src/pos.{h,cpp}`, `StakeRegistry`): a singleton
  `{CPubKey → uint64_t weight}`. Populated for the PoC from chain configuration
  (`-staker=<pubkeyhex>:<weight>`, repeatable) or the chain's built-in genesis
  staker set. Future: tracked in chainstate via staking transactions.
- **Weighted ticket**: for staker `k`, ticket
  `t_k = H(seed || pubkey_k)` interpreted as a 256-bit big-endian integer.
  The election compares `t_k / weight_k` (done with 512-bit cross-multiplication
  to avoid division: `k` ranks before `j` iff
  `t_k * weight_j < t_j * weight_k`). Lower ⇒ better ⇒ lower rank. Ties broken
  by pubkey. More stake ⇒ statistically more rank-0 wins, proportional to weight.
- **Schedule**: the full ascending ordering; `rank(pubkey)` is its index.
- **Determinism**: every node computes the identical schedule from the (agreed)
  stake registry and the parent block + anchor. No secret is needed to *verify*
  the leader, only to *sign* as the leader.

## 4. Liveness

`pos_slot_interval` (seconds, chain param / `-posslotinterval`) gates ranks:

- rank 0 may produce as soon as `nTime > parent.MTP`.
- rank `r` may produce once `nTime >= parent.nTime + r * interval`.

So if the primary leader is offline, the chain is delayed by at most
`interval` before rank 1 may step in, etc. This is the PoC analogue of the
paper's *escaping-stall* clause (principle 8) — there, the Bitcoin anchor
falling a bounded number of blocks behind triggers the exception; here, wall
clock relative to the parent does. A future iteration replaces this with the
anchor-driven rule and committee voting.

Fork choice between same-height blocks uses Elements' existing rule (signed
blocks have equal "work" = height, so first-seen wins). Time-gating ensures the
rank-0 leader produces and propagates earliest in the common case, so honest
nodes converge on it — exactly as the federation's round-robin does today.

## 5. Security notes (why this is a PoC, not production)

- **Public schedule.** Because the election uses no private VRF, the leader
  schedule is publicly predictable from the stake set + anchor. This enables
  targeted DoS of upcoming leaders and some grinding on the (anchor-derived)
  seed by whoever produces the parent. The paper's *private* VRF fixes this:
  only the winner can prove they won, after the fact. The election function is
  isolated in `src/pos.cpp` (`PosElection`) precisely so a real EC-VRF
  (RFC 9381) can replace it without touching the consensus wiring.
- **No slashing / no finality gadget.** A single leader per slot gives
  probabilistic finality (like the paper's item-6 comparison to Cardano), not
  the immediate finality of the 51/100 committee. Long-range attacks are not yet
  mitigated (needs the Bitcoin checkpoints + stake locktimes of principle 11).
- **Stake set is configuration.** Until staking transactions land, the registry
  is operator-configured and identical across honest nodes by assumption.

## 6. Roadmap within PoS

1. [x] Stake registry + deterministic stake-weighted, anchor-seeded ranked
       election (`src/pos.{h,cpp}`).
2. [x] Consensus enforcement via per-block challenge in `CheckChallenge`;
       signature check reuses `CheckProof`.
3. [x] Miner elects self and produces when its slot opens.
4. [x] `getstakerinfo` / `getposschedule` RPCs; `-staker` / `-posslotinterval`
       / `-con_pos` options; a `pos` regtest-style chain.
5. [x] Functional test: stake-weighted schedule, leader-signed block accepted,
       wrong-leader / early block rejected, multi-staker liveness handoff.
6. [ ] Private VRF sortition (RFC 9381 EC-VRF over secp256k1).
7. [ ] 100-member committee + 51/100 countersignature certification
       (immediate finality, principle 6) carried in the dynafed witness.
8. [ ] On-chain stake registration / unbonding in chainstate.
9. [ ] Bitcoin checkpoints + stake locktimes vs. long-range attacks (principle 11).
</content>
