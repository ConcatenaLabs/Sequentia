# Challenge 3 — Consensus (Proof-of-Stake)

> **Status:** Sequentia's consensus is **Proof-of-Stake** — stake-weighted
> election, private VRF sortition, committee certification with MuSig2
> aggregation, on-chain stake, and Bitcoin checkpoints. The full treatment is in
> [`06-proof-of-stake.md`](06-proof-of-stake.md) and [`07-vrf.md`](07-vrf.md).
> This document records how the consensus maps onto Elements' block-signing
> machinery, which Sequentia inherits and reuses.

Challenge 3 is Proof-of-Stake consensus with Bitcoin checkpoints. Sequentia
implements it on top of Elements' **signed blocks** — Elements replaces
Bitcoin's proof-of-work with a header block signature, and Sequentia drives the
per-block signing rule from a stake-weighted election rather than a fixed
signer set.

## 1. The substrate: Elements "signed blocks"

Elements replaces Bitcoin's proof-of-work with **block signing**:

- Enabled by `g_signed_blocks = true` (set for the Sequentia chain in
  `src/chainparams.cpp`).
- The blocksigning rule is a script: `consensus.signblockscript`. In stock
  Elements this is a *fixed* federation script (e.g. an `OP_CHECKMULTISIG` over
  the functionaries' keys); Sequentia instead **computes the challenge per
  block** from the stake-weighted election (doc 06).
- The signature(s) live in the header: `CProof` (legacy) or
  `m_signblock_witness` + `DynaFedParams` (dynamic federation), in
  `src/primitives/block.h`. They are outside `SER_GETHASH`, so signing is over
  the committed header (which, with challenge 2, **includes the Bitcoin
  anchor**).

This block-signature plumbing is the only piece Sequentia reuses from Elements'
consensus; everything that decides *who* may sign a given block is Sequentia's
PoS layer (docs 06/07).

## 2. How PoS uses it

PoS changes exactly one thing about signed blocks: the challenge is **computed
per block** from a stake-weighted, anchor-seeded election (and, with
`-posvrf`, private VRF sortition), instead of being inherited as a fixed
federation script. The block signature itself rides the existing
`proof.solution` / `m_signblock_witness` plumbing untouched. See doc 06 §2 for
the exact `CheckChallenge` / `CheckPosStakeRules` / `CheckProof` split.

## 3. Interaction with the two other priority features

- **Fee market:** the elected leader is the "block producer" of the paper. Its
  `exchangerates.json` + dynamic price server determine which fee assets it
  accepts and how it ranks them. Different producers may run different price
  policies; consensus only checks fees were paid, not how they were valued. The
  open fee market is orthogonal to who signs blocks, so it composes cleanly with
  PoS.
- **Anchoring:** the producer sets the block's Bitcoin anchor (doc 03 §3 R4) and
  signs over it. The anchor is committed, so a block's anchor cannot be altered
  without invalidating the signature. PoS draws on the anchor for both its
  randomness seed and its escaping-stall liveness clause (doc 10).

## 4. The full PoS design

The consensus delivered is, in layers (all implemented):

- **Sortition/committee:** a per-round committee selected by cryptographic
  sortition (VRF), weighted by SEQ at stake, seeded from the previous Sequentia
  block **and** a Bitcoin-derived seed (docs 06/07).
- **Quorum certification:** a block is final when a strict-majority committee
  quorum is collected (the paper's 51-of-100). The dynafed/signed-block witness
  model carries the aggregated/threshold signature — MuSig2 aggregation reaches
  paper-scale 100-member committees (doc 07 §6).
- **Checkpoints (anti-long-range):** Bitcoin checkpoints (paper principle 11),
  combined with CSV stake lock-times exceeding the checkpoint depth, defeat
  posterior-corruption / long-range attacks (doc 06 §5–6).
- **Liveness / escaping-stall:** the Bitcoin anchor (challenge 2) gives the time
  source the paper's "escaping stall" rule needs (principle 8): when the
  Sequentia tip falls a bounded number of Bitcoin blocks behind, an exception
  rule restores liveness (doc 10).

Challenge 2 (anchoring) is the substrate the PoS design depends on for both its
randomness seed and its liveness clause.
