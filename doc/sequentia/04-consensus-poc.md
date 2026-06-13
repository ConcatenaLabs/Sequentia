# Challenge 3 — Consensus (PoC: strong federation; since implemented: PoS)

> **Status:** this document captures the original PoC decision to run on a
> strong federation and the *planned* path to PoS. The full Proof-of-Stake
> consensus — stake-weighted election, private VRF sortition, committee
> certification with MuSig2 aggregation, on-chain stake, and Bitcoin
> checkpoints — has **since been implemented**; see
> [`06-proof-of-stake.md`](06-proof-of-stake.md) and [`07-vrf.md`](07-vrf.md).
> §3 below is kept as the design rationale.

Challenge 3 (Proof-of-Stake with Bitcoin checkpoints) was deprioritised for
the proof of concept, which runs on a **strong federation** that Elements
provides natively — no new consensus code was required for the PoC.

## 1. The PoC: Elements "signed blocks" (strong federation)

Elements replaces Bitcoin's proof-of-work with **block signing** by a federation:

- Enabled by `g_signed_blocks = true` (set for the Sequentia chain in
  `src/chainparams.cpp`).
- The blocksigning rule is a script: `consensus.signblockscript`. For a real
  federation this is an `OP_CHECKMULTISIG` over the functionaries' keys requiring
  a threshold (e.g. `m`-of-`n`); the dev chain uses `OP_TRUE` (`0x51`).
- The signature(s) live in the header: `CProof` (legacy) or
  `m_signblock_witness` + `DynaFedParams` (dynamic federation), in
  `src/primitives/block.h`. They are outside `SER_GETHASH`, so signing is over the
  committed header (which, with challenge 2, **includes the Bitcoin anchor**).
- **Dynamic federations** (`DynaFedParams`, BIP-style "dynafed") allow rotating
  the signer set / rules via in-band proposals without a hard fork — useful for a
  federation that evolves, and already wired in the imported tree.

### Block production

Federation members run with block-signing enabled and use the existing signing
tooling (`signblock` / `getnewblockhex` / `combineblocksigs` RPCs, and the
`elements`-style block-signer flow). For the PoC a single signer (or a small
`m`-of-`n`) is sufficient to demonstrate liveness and finality.

### Why this satisfies the PoC

- **Immediate finality** (paper principle 6): a signed block cannot be reorged
  except by the challenge-2 anchoring rule (Bitcoin reorg). This is exactly the
  property the federation gives.
- **No native fee coin required** (challenge 1) is orthogonal to who signs blocks,
  so the federation composes cleanly with the open fee market.

## 2. Interaction with the two priority features

- **Fee market:** the federation signer is the "block producer" of the paper. Its
  `exchangerates.json` + dynamic price server determine which fee assets it
  accepts and how it ranks them. Different signers may run different price
  policies; consensus only checks fees were paid, not how they were valued.
- **Anchoring:** signers set the block's Bitcoin anchor (doc 03 §3 R4) and sign
  over it. The anchor is committed, so a block's anchor cannot be altered without
  invalidating the signature.

## 3. Path to the full PoS design (since implemented — docs 06/07)

Recorded so the PoC doesn't paint us into a corner; see the theoretical paper for
the full treatment.

- **Sortition/committee:** replace the fixed `signblockscript` with a per-round
  committee selected by cryptographic sortition (VRF), weighted by SEQ at stake,
  seeded from the previous Sequentia block **and** a Bitcoin-derived seed.
- **Quorum certification:** a block is final when ≥ 51/100 committee
  countersignatures are collected (paper principle 6). The dynafed witness model
  is a natural carrier for an aggregated/threshold signature.
- **Checkpoints (anti-long-range):** voluntary commitments of Sequentia state into
  Bitcoin (paper principle 11), combined with stake lock-times exceeding the
  checkpoint depth, to defeat posterior-corruption / long-range attacks.
- **Liveness / escaping-stall:** the Bitcoin anchor (challenge 2) already gives the
  time source the paper's "escaping stall" rule needs (principle 8): when the
  Sequentia tip falls a bounded number of Bitcoin blocks behind, an exception rule
  restores liveness.

The PoC's federation is a strict simplification of the above (committee of size 1
or small `n`, no sortition, no stake weighting), so migrating later is additive
rather than a rewrite. **Crucially, challenge 2 (anchoring) is already the
substrate the PoS design depends on for both its randomness seed and its liveness
clause — building it now is on the critical path regardless of when PoS lands.**
