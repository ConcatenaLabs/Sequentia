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

**ECVRF-SECP256K1-SHA256-TAI**, structured per RFC 9381 over the curve
operations the vendored library exposes
(`parse`/`create`/`combine`/`negate`, plus the ECDH module for the
constant-time secret-scalar multiplications `sk·H` and `k·H`; the
variable-time `tweak_mul` is used only on the verify side, where every
scalar is public). Only compressed public keys are valid VRF identities
(prove and verify both reject uncompressed, symmetrically). The suite octet is the RFC's
experimental value `0xFF` (§5.5), and each hash is domain-separated as
`suite ‖ front ‖ … ‖ 0x00` with the RFC front octets (`0x01` encode-to-curve,
`0x02` challenge, `0x03` proof-to-hash):

```
prove(sk, alpha):
  Y     = sk·G
  H     = encode_to_curve(Y, alpha)        # §5.4.1.1 try-and-increment, 0x02 prefix
  Gamma = sk·H
  k     = HashToScalar(sk, H)              # deterministic nonce (see §2 deviations)
  U     = k·G ;  V = k·H
  c     = challenge(Y, H, Gamma, U, V)     # §5.4.3: SHA256(…), truncated to 16 bytes
  s     = k + c·sk   (mod n)
  pi    = Gamma(33) ‖ c(16) ‖ s(32)        # 81 bytes (§5.5)
  beta  = SHA256(suite ‖ 0x03 ‖ Gamma ‖ 0x00)   # §5.2 proof_to_hash, cofactor 1

verify(Y, alpha, pi):
  recompute H; U = s·G − c·Y ; V = s·H − c·Gamma
  accept if and only if challenge(Y, H, Gamma, U, V) == c ; then beta = proof_to_hash(Gamma)
```

The verify identity holds because `s·G = (k + c·sk)·G = U + c·Y` and likewise
for `V`. Output uniqueness follows from `Gamma = sk·H` being determined by
`(sk, alpha)`; the proof's `(c, s)` are a Schnorr-style proof of correct
exponentiation. The challenge binds the public key `Y` (RFC ordering), and `c`
is the leading 16 bytes of the hash interpreted as a big-endian integer
(always `< n`).

**Properties (unit-tested in `src/test/vrf_tests.cpp`):** prove/verify
round-trip; rejection of wrong key / wrong input / tampered or mis-sized proof;
output uniqueness & determinism across re-proofs; distinct outputs across keys
and inputs; all-distinct, balanced outputs over many inputs (pseudorandomness
smoke test). **Node-level (`feature_vrf.py`):** node A proves, node B verifies
with no shared secret, plus the negative cases.

**Residual deviations from RFC 9381 (why it's still not a certified drop-in):**
secp256k1 is *not* one of the RFC's registered ciphersuites (those are P-256 and
edwards25519), so there are **no official test vectors** to validate against and
the suite octet uses the RFC's experimental `0xFF`; the deterministic nonce uses
SHA-256 over `(sk, H)` rather than the RFC 6979 HMAC_DRBG of the P-256 suite
(an interop-irrelevant, unverifiable step — only nonce secrecy/determinism
matters for soundness); and the try-and-increment hash-to-curve is not
constant-time (acceptable because `alpha` is public, but worth a review). The
verifiable framing — encode-to-curve, the `Y`-bound truncated-16-byte challenge,
the proof encoding, and proof-to-hash — follows the RFC. All of this is isolated
in `src/vrf.cpp` and does not affect callers; the construction is pinned by
golden known-answer vectors in `vrf_tests.cpp`.

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
4. **Validate.** In `CheckPosStakeRules`, called from `ConnectBlock` (which
   has the full block, the parent, and — crucially — the stake registry at
   exactly the parent's state): extract the proof, `VrfVerify` it against the
   leader's challenge pubkey and the slot seed, recompute `slot`, and require
   `block.nTime ≥ parent.nTime + slot · interval` (the doc-06 liveness gate,
   now VRF-driven). `CheckChallenge` at header time is structural only —
   anything registry-dependent waits for connect time, because headers and
   block data are accepted ahead of (or on a different branch than) the
   active chain the registry mirrors. The leader signature (`CheckProof`) is
   unchanged.
5. **Committee.** Under private sortition nobody can rank stakers (each beta
   is secret until published), so committee membership is **threshold-based**,
   Algorand-style: staker `i` is a member if and only if
   `PosVrfSlot(beta_i, w_i, W) < committee_size`. Since
   `P(slot < T) = T·w/W`, the expected committee size is exactly
   `committee_size`, weight-proportionally. The block's challenge lists the
   *claimed* members (`<leader> CHECKSIGVERIFY <q>-of-<k> CHECKMULTISIG` with
   `q` fixed at a majority of the **expected** size — the paper's 51-of-100 —
   independent of the claimed count `k`); each claimed member's eligibility is
   proven by a tagged `SEQCMT` coinbase commitment (`pubkey ‖ proof`) verified
   at connect time (`bad-posvrf-member-missing/-invalid/
   -not-selected`); the signatures themselves are enforced by the script. A
   leader needs a quorum of genuinely-selected members to cooperate — exactly
   the paper's certification model.

**Status: implemented** behind `-posvrf` (requires `-con_pos`), including
committee certification (`-poscommitteesize` up to 16 in the script form, up to 100 with `-posaggcommittee`) per §4.5.

## 5. Roadmap position

- [x] VRF primitive (`src/vrf.{h,cpp}`) + unit tests.
- [x] `vrfprove` / `vrfverify` RPCs + functional test.
- [x] `-posvrf` mode: coinbase-committed proof (tagged `SEQVRF` OP_RETURN,
      covered by the merkle root and hence the leader's signature), the
      stake-weighted sortition slot `PosVrfSlot` (capped at `POS_VRF_MAX_SLOT`),
      consensus validation in `CheckPosStakeRules`/`ConnectBlock` (proof verifies against
      the leader's challenge key over the slot seed; block time must respect
      the proof-derived slot), `CheckChallenge` reduced to registered-staker +
      leader-only-form in this mode, miner/`generateposblock` integration, and
      `feature_pos_vrf.py` (peer-validated VRF blocks, commitment present,
      slot respected, non-staker and proof-less templates rejected).
- [x] Bring the primitive to RFC 9381 ECVRF structural conformance
      (ECVRF-SECP256K1-SHA256-TAI: experimental suite octet, RFC domain
      separators, `Y`-bound challenge truncated to 16 bytes, RFC proof
      encoding and proof-to-hash). Residual, documented deviations: no
      official secp256k1 vectors exist, and the nonce uses SHA-256 rather
      than RFC 6979 (§2).
- [x] VRF-sortitioned committees: threshold membership (expected size =
      `-poscommitteesize`, weight-proportional), per-member `SEQCMT`
      eligibility commitments validated in consensus, fixed majority quorum,
      producer-side eligibility filtering in `generateposblock`
      (`feature_pos_vrf_committee.py`).
- [x] Paper-scale committees: MuSig2 signature aggregation
      (`-posaggcommittee`, committee cap 100) — §6.

## 6. Paper-scale committees — MuSig2 aggregation (implemented)

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
signature over the block — so the connect-time membership checks
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

**The consensus wiring is implemented** behind `-posaggcommittee` (requires
`-posvrf`; raises the `-poscommitteesize` cap from 16 to 100):

- **Challenge.** `OP_1 <leader(33)> <agg_key(32)>` (`BuildPosAggChallenge`):
  the leading `OP_1` is a version marker no other challenge form can start
  with. The challenge no longer lists members — it commits to the single
  MuSig2 aggregate of the member set.
- **Member set = the `SEQCMT` commitments.** Under aggregation the coinbase
  eligibility commitments don't just *prove* a claimed list, they *are* the
  list: `CheckPosStakeRules` (`ConnectBlock`) requires every named member to be distinct, at most the 100-member cap (`bad-posvrf-member-count`), and
  sortition-selected (same `bad-posvrf-member-*` checks), at least
  `PosQuorum(committee_size)` members to be named (`bad-posvrf-agg-quorum`),
  and `MuSigAggregatePubkey(named set) == agg_key` (`bad-posvrf-agg-key`) —
  so the one signature is by precisely the proven-eligible members.
- **Proof.** The solution is two pushes: the leader's DER signature and the
  64-byte BIP340 aggregate signature, both over the block hash. `CheckProof`
  verifies them directly (ECDSA + Schnorr) instead of through the script
  interpreter — `OP_CHECKMULTISIG` cannot express one signature over an
  aggregate of up to 100 keys, which is exactly why the cap existed.
- **Producer.** `generateposblock` filters the provided keys to the
  sortition-selected members as before, then runs the local two-round MuSig2
  signing over all of them (n-of-n over the named set realizes the q-of-m
  quorum). Block size is constant in the committee size: ~105 bytes of
  challenge + ~137 bytes of solution whether 3 members signed or 100.

Tested in `feature_pos_agg_committee.py` (quorum enforcement, peer validation
of the aggregate form, constant-size solution, and that `-poscommitteesize=40`
starts only with the aggregation flag) and `pos_tests.cpp`
(`pos_agg_challenge_roundtrip`). The `generateposblock` path above runs the two
MuSig2 rounds *locally* (the producer holds every committee key) — convenient
for a single operator, but not how a decentralized committee works.

### Distributed signing — implemented

For a real committee, each member runs on its own node and never shares its
key. BIP327's secret nonce is deliberately non-serialisable (so it can't be
persisted and accidentally reused — reuse leaks the key), so each member's node
keeps the live secret nonce in an in-memory **session store** (`src/musig.cpp`)
between the two rounds and consumes it exactly once. Five RPCs expose the flow:

- `musigaggregatepubkey [pubkeys]` → the committee's 32-byte aggregate key
  (the challenge's `agg_key`).
- `musignonce sessionid privkey [pubkeys] msg` → **round 1** on a member's
  node: its 66-byte public nonce, stashing the secret nonce under a *fresh*
  session id (a duplicate id is refused — single-use).
- `musigpartialsign sessionid privkey [pubkeys] [pubnonces] msg` → **round 2**:
  the member's 32-byte partial signature; consumes the session and refuses any
  message, member set, or signing key that differs from round 1 (binding
  against nonce-reuse misuse), as well as a nonce count ≠ the member count.
- `musigaggregate [pubkeys] [pubnonces] [partials] msg` → the final 64-byte
  signature (public; coordinator-side), self-checked against the aggregate key.
- `musigverify [pubkeys] msg sig` → BIP340 verification.

Two RPCs wire this into block production without any node holding all the keys:

- `getposblocktemplate leaderkey [{pubkey, vrfproof}…]` — the leader assembles
  the unsigned block (its own `SEQVRF` proof, each member's `SEQCMT`
  eligibility commitment from the supplied VRF proofs, and the aggregate
  challenge over the member set) and returns the block hex plus the 32-byte
  `signhash` (in internal byte order — the exact bytes consensus Schnorr-checks)
  and the member set.
- `submitposblock blockhex leaderkey aggregatesig` — the leader attaches its own
  signature and the committee's aggregate (from `musigaggregate`) and submits;
  the block is validated and accepted by every node like any other.

`feature_pos_distributed_committee.py` runs the whole loop across **three
separate nodes**, each holding one member key: every member VRF-proves its
eligibility, the leader templates the block, the members nonce and partial-sign
on their own nodes, the partials aggregate into one signature, and the
committee-certified block is accepted network-wide — for several blocks in a
row. `musig_tests.cpp` adds the distributed-rounds round-trip and the
session-safety properties (single-use, duplicate-id refusal, message/set
binding). What remains purely operational (a transport to ferry nonces and
partials between hosts, and member liveness/timeout handling) is deployment
tooling, not consensus — the consensus and signing layers are complete.
