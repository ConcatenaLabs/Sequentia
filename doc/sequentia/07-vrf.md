# Challenge 3 (cont.) — VRF cryptographic sortition

> This extends the PoS design (doc 06) with the paper's **private** cryptographic
> sortition (section iv, principle 3): a verifiable random function (VRF), its
> node interface, the `-posvrf` consensus mode that replaces the public schedule
> with private sortition, VRF-sortitioned committees, and — §6 — MuSig2
> signature aggregation toward paper-scale committees. All implemented in this
> repository unless a subsection says otherwise.

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

## 4. Consensus integration (`-posvrf`) — implemented

Replaces doc-06's public ranking with **private sortition**, keeping the rest of
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
5. **Committee.** Under private sortition nobody can rank stakers (each beta
   is secret until published), so committee membership is **threshold-based**,
   Algorand-style: staker `i` is a member iff
   `PosVrfSlot(beta_i, w_i, W) < committee_size`. Since
   `P(slot < T) = T·w/W`, the expected committee size is exactly
   `committee_size`, weight-proportionally. The block's challenge lists the
   *claimed* members (`<leader> CHECKSIGVERIFY <q>-of-<k> CHECKMULTISIG` with
   `q` fixed at a majority of the **expected** size — the paper's 51-of-100 —
   independent of the claimed count `k`); each claimed member's eligibility is
   proven by a tagged `SEQCMT` coinbase commitment (`pubkey ‖ proof`) verified
   in `ContextualCheckBlock` (`bad-posvrf-member-missing/-invalid/
   -not-selected`); the signatures themselves are enforced by the script. A
   leader needs a quorum of genuinely-selected members to cooperate — exactly
   the paper's certification model.

**Status: implemented** behind `-posvrf` (requires `-con_pos`), including
committee certification (`-poscommitteesize` 2..16) per §4.5.

## 5. Roadmap position

- [x] VRF primitive (`src/vrf.{h,cpp}`) + unit tests.
- [x] `vrfprove` / `vrfverify` RPCs + functional test.
- [x] `-posvrf` mode: coinbase-committed proof (tagged `SEQVRF` OP_RETURN,
      covered by the merkle root and hence the leader's signature), the
      stake-weighted sortition slot `PosVrfSlot` (capped at `POS_VRF_MAX_SLOT`),
      consensus validation in `ContextualCheckBlock` (proof verifies against
      the leader's challenge key over the slot seed; block time must respect
      the proof-derived slot), `CheckChallenge` reduced to registered-staker +
      leader-only-form in this mode, miner/`generateposblock` integration, and
      `feature_pos_vrf.py` (peer-validated VRF blocks, commitment present,
      slot respected, non-staker and proof-less templates rejected).
- [ ] Validate the primitive against RFC 9381 vectors / adopt a reviewed
      secp256k1 VRF ciphersuite.
- [x] VRF-sortitioned committees: threshold membership (expected size =
      `-poscommitteesize`, weight-proportional), per-member `SEQCMT`
      eligibility commitments validated in consensus, fixed majority quorum,
      producer-side eligibility filtering in `generateposblock`
      (`feature_pos_vrf_committee.py`). Signature *aggregation* (BLS/MuSig)
      for paper-scale 100-member committees remains future work (script
      multisig caps the claimed-member list at 16).
</content>

## 6. Paper-scale committees — aggregation design (future work)

The committee form caps the claimed-member list at 16 because each member is a
separate `OP_CHECKMULTISIG` pubkey and each countersignature a separate
scriptSig push: an `n`-of-`m` script is `O(m)` pubkeys + `O(q)` 72-byte
signatures, which blows past `max_block_signature_size` for the paper's
100-member committee. Reaching 100 needs **signature aggregation** so the block
carries *one* signature regardless of committee size. Two viable routes,
neither requiring a header-format change:

1. **BLS aggregate signatures.** Each sortition-selected member signs the block
   hash with a BLS key; the leader aggregates the `q` signatures into a single
   48/96-byte aggregate. The block challenge becomes
   `<agg-pubkey-or-list-commitment> <q> OP_CHECK_BLS_AGG` (a new opcode or a
   tapscript leaf), and the coinbase keeps the per-member `SEQCMT` VRF
   eligibility proofs (unchanged). *Pros:* smallest blocks, non-interactive
   aggregation. *Cons:* a new pairing-curve dependency and a new verification
   opcode — the largest consensus-surface change.

2. **MuSig2 / Schnorr half-aggregation over secp256k1.** Stay on the existing
   curve: members run MuSig2 to produce one BIP340 Schnorr signature for the
   slot, verified by a single `OP_CHECKSIGADD`/taproot path. *Pros:* no new
   curve; reuses secp256k1 and Taproot already in the tree. *Cons:* MuSig2 is
   interactive (two rounds among the online committee), which fits the paper's
   synchronous-round committee model but complicates the producer flow.

Either way the **consensus rule is unchanged in spirit** — leader VRF proof +
a quorum of sortition-eligibility proofs in the coinbase + one aggregate
signature over the block — so `ContextualCheckBlock`'s membership checks
(`bad-posvrf-member-*`) carry over verbatim; only the signature *encoding* and
its verification path change. The committee-membership threshold math
(`PosVrfIsCommitteeMember`, `PosVrfSlot`) and the `SEQCMT` eligibility
commitments are already aggregation-ready.

**The MuSig2 primitive is implemented** (`src/musig.{h,cpp}`, route (2) — no
new curve): the BIP327 module is enabled in the vendored secp256k1, and the
primitive aggregates a signer set into one 32-byte x-only key
(`MuSigAggregatePubkey`, order-independent — the aggregate depends only on the
*set*), produces a single 64-byte BIP340 signature from all signers
(`MuSigSign`, full two-round protocol run locally since the producer holds the
keys), and verifies it (`MuSigVerify`). A `q`-of-`m` quorum is realized by
aggregating exactly the `q` signing members, so the block would commit to the
`q` member list (the `SEQCMT` eligibility proofs already name them), carry one
64-byte signature regardless of `q`, and the verifier re-aggregates the named
members and Schnorr-verifies. Unit-tested in `src/test/musig_tests.cpp`
(aggregate/sign/verify round-trip, order-independence, tamper/wrong-set
rejection, the quorum-subset property, and the n-of-n boundary).

**Remaining for full integration** (the consensus wiring, deferred): the block
signature today is verified by the script interpreter via the challenge script
(`CheckProof` → `GenericVerifyScript`), which uses ECDSA `OP_CHECKMULTISIG`.
Routing a single aggregate BIP340 signature through block validation needs a
non-script verification path for PoS-VRF-committee blocks (verify the 64-byte
MuSig signature directly against the aggregate of the named members) plus the
producer-side two-round signing in `generateposblock`. This is a contained,
well-specified change — the primitive it depends on is done and tested — and is
the recommended next step to raise the committee cap from 16 to the paper's 100.
