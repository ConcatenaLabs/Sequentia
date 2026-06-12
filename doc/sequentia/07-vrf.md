# Challenge 3 (cont.) — VRF cryptographic sortition

> **Branch:** `claude/sequentia-proof-of-stake-w6xady`. This extends the PoS PoC
> (doc 06) toward the paper's **private** cryptographic sortition (section iv,
> principle 3). It adds a verifiable random function (VRF) primitive and its
> node interface; the consensus integration that replaces the public schedule
> with private sortition is scoped here as the next step.

## 1. Why a VRF

The doc-06 election is a *public* deterministic schedule: anyone can compute
`H(seed ‖ pubkey)/weight` for every staker, so the entire leader order is known
in advance. That enables targeted DoS of upcoming leaders and lets whoever
produces a block grind the (anchor-derived) seed it passes on.

A **VRF** fixes this. For a secret key `sk` (public key `Y = sk·G`) and input
`alpha`, the holder of `sk` can compute:

- a 32-byte pseudorandom output `beta`, and
- a proof `pi`,

such that anyone with `(Y, alpha, pi)` can verify `beta` is the **unique**
correct output — but **nobody without `sk` can compute or predict `beta`**. So a
staker learns privately whether it won a slot, publishes the proof only when it
produces a block, and no one can predict future leaders or grind their identity
(VRF outputs are unique per key).

## 2. The primitive (`src/vrf.{h,cpp}`) — implemented

An ECVRF-style construction over secp256k1, built on the curve operations the
vendored library exposes (`parse`/`create`/`tweak_mul`/`combine`/`negate`):

```
prove(sk, alpha):
  Y     = sk·G
  H     = hash_to_curve(alpha, Y)          # try-and-increment onto secp256k1
  Gamma = sk·H
  k     = HashToScalar(sk, H)              # deterministic nonce
  U     = k·G ;  V = k·H
  c     = HashToScalar(H, Gamma, U, V)     # Fiat-Shamir challenge
  s     = k + c·sk   (mod n)
  pi    = Gamma(33) ‖ c(32) ‖ s(32)        # 97 bytes
  beta  = SHA256(suite ‖ 0x04 ‖ Gamma)

verify(Y, alpha, pi):
  recompute H; U = s·G − c·Y ; V = s·H − c·Gamma
  accept iff HashToScalar(H, Gamma, U, V) == c ; then beta = SHA256(…‖Gamma)
```

The verify identity holds because `s·G = (k + c·sk)·G = U + c·Y` and likewise
for `V`. Output uniqueness follows from `Gamma = sk·H` being determined by
`(sk, alpha)`; the proof's `(c, s)` are a Schnorr-style proof of correct
exponentiation.

**Properties (unit-tested in `src/test/vrf_tests.cpp`):** prove/verify
round-trip; rejection of wrong key / wrong input / tampered or mis-sized proof;
output uniqueness & determinism across re-proofs; distinct outputs across keys
and inputs; all-distinct, balanced outputs over many inputs (pseudorandomness
smoke test). **Node-level (`feature_vrf.py`):** node A proves, node B verifies
with no shared secret, plus the negative cases.

**Caveats (why it's a PoC primitive, not a standards drop-in):** it is *not*
validated against RFC 9381 test vectors and uses a non-standard secp256k1 suite
byte; the hash-to-curve is try-and-increment (not constant-time, which is
acceptable since `alpha` is public but should be reviewed); the challenge uses a
full 32-byte scalar rather than the RFC's truncated `c`. These are isolated in
`src/vrf.cpp` and do not affect callers.

## 3. Node interface — implemented

- `vrfprove "privkey" "input_hex"` → `{pubkey, proof, output}`.
- `vrfverify "pubkey" "input_hex" "proof_hex"` → `{valid, output?}`.

## 4. Consensus integration — design (next step)

Replace doc-06's public ranking with **private sortition**, keeping the rest of
the PoS machinery (signed-block challenge, time-gated production, first-seen
convergence):

1. **Per-slot output.** A staker computes `beta = VRF(sk, seed)` for the slot
   seed (the same `H(prevhash ‖ prev anchor ‖ height)` as doc 06).
2. **Verifiable priority/slot.** Define a locally-checkable, stake-weighted slot
   number from `beta` and the staker's weight `w` (total weight `W`):

   ```
   q    = beta / w                       # 256-bit
   slot = ⌊ top64(q) · W / 2^64 ⌋        # in [0, W); lower beta / higher w ⇒ lower slot
   ```

   This is the continuous, VRF-driven analogue of doc-06's integer rank, and —
   crucially — a validator can check it from the *single* published proof
   (it does not need every staker's `beta`).
3. **Carry the proof.** The block commits to the leader's VRF proof in a
   coinbase `OP_RETURN` (tagged `SEQVRF`), so it is covered by the merkle root
   and therefore by the leader's block signature — no header-format change, no
   new genesis.
4. **Validate.** In `ContextualCheckBlock` (which has the full block and the
   parent): extract the proof, `VrfVerify` it against the leader's challenge
   pubkey and the slot seed, recompute `slot`, and require
   `block.nTime ≥ parent.nTime + slot · interval` (the doc-06 liveness gate,
   now VRF-driven). `CheckChallenge` in VRF mode only checks the challenge is a
   registered staker; the rank/time check moves to `ContextualCheckBlock` where
   the proof is available. The leader signature (`CheckProof`) is unchanged.
5. **Committee.** Committee certification (doc 06) needs every member's
   eligibility proof; in VRF mode each member would publish its own sortition
   proof and the block would aggregate them. For the first VRF integration,
   committee certification is disabled (single leader); unifying the two is the
   step after.

This is deliberately staged: the primitive (the hard, security-critical crypto)
is finished and tested now; the consensus wiring above is a contained, well-
specified change to land next, behind a `-posvrf` flag.

## 5. Roadmap position

- [x] VRF primitive (`src/vrf.{h,cpp}`) + unit tests.
- [x] `vrfprove` / `vrfverify` RPCs + functional test.
- [ ] `-posvrf` mode: coinbase-committed proof, sortition slot, validation in
      `ContextualCheckBlock` (§4).
- [ ] Validate the primitive against RFC 9381 vectors / adopt a reviewed
      secp256k1 VRF ciphersuite.
- [ ] VRF-sortitioned committees with aggregated eligibility proofs.
</content>
