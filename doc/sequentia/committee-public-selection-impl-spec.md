# Implementation spec: public fixed-size committee (Option A)

Alberto confirmed Option A on 2026-07-03: committee membership is PUBLIC (the deterministic
schedule), VRF is retained for LEADER election only, the committee for height H+1 is knowable
once block H exists (about a minute of lookahead), and the payoff is the certificate shrinking
from ~32 KB to ~300 bytes plus a large drop in stall probability. This spec turns that decision
into a buildable change. It is consensus-level: it does NOT deploy to the live chain (existing
blocks would re-derive membership and fail) and folds into the planned re-genesis.

Two design points that Option A newly forces are called out as DECISIONS below; both are
consensus rules and want a ruling before the consensus code is written. Everything else is
mechanical.

## 1. Committee selection

Replace threshold VRF membership with the public schedule prefix.

- Today: a staker is a member for a slot iff `PosVrfSlot(beta, weight, total) < g_pos_committee_size`
  (pos.cpp:364), an independent per-staker threshold, so the eligible count is a random
  variable and can exceed the cap.
- Option A: the committee for height H is the first `K = min(pool, CAP)` entries of
  `PosSchedule(registry, seed)` (pos.cpp:102-118), the existing public ranking by
  `H(seed || pubkey) / weight`. This function already exists and is already used for the
  non-VRF committee (`PosCommittee`, pos.cpp:130). The change is to make the VRF/BLS
  certification path use it instead of `PosVrfIsCommitteeMember`. `K` and the members are then
  identical on every node, and the eligible set never exceeds `CAP`, so two disjoint quorums
  cannot form.

Quorum derives from the ACTUAL K, not the nominal cap. Today the quorum is pinned to
`PosQuorum(g_pos_committee_size)` at validation.cpp:2254/2310, block_proof.cpp:65, and
pos_producer.cpp:263/645/1075. All of these become `PosQuorum(K)` where `K` is the committee
size computed from the registry for that slot. Consequence: a 60-staker launch runs 31-of-60,
not the impossible 51-of-60 the nominal rule would demand today.

Leader and committee are separate roles and stay separate. The leader is still VRF-elected
(its slot secret until it publishes), and need not be in the committee; the block challenge is
already "leader OP_CHECKSIGVERIFY, then committee quorum" (`BuildPosBlockChallenge`,
pos.cpp:149). Keeping the leader private preserves the leader-targeted-DoS blindness that was
the point of VRF; only committee membership becomes public.

### DECISION 1: the committee seed source (grinding)

Making the committee public also makes it grindable in a way the private committee was not.
Under threshold VRF an attacker choosing its block's Bitcoin anchor could shift the NEXT slot's
seed, but could not EVALUATE which choice helped, because each member's VRF output is secret. A
public committee is computable for every candidate seed, so a leader who can pick among `k`
valid anchors gets best-of-`k` tries at the next committee. Measured inflation (population
50,000, per height):

| committee | coalition | 1 try | best-of-6 | best-of-16 |
| --- | --- | --- | --- | --- |
| 100 | 33% | 2.0e-4 | 1.2e-3 | 3.1e-3 |
| 200 | 33% | 3.9e-7 | 2.3e-6 | 6.2e-6 |
| 250 | 33% | 1.8e-8 | 1.1e-7 | 2.9e-7 |

The multiplier is about `k`. It only applies on blocks the attacker actually leads (so times
its stake share) and `k` is bounded by how many recent Bitcoin blocks pass the anchor-freshness
gate (a handful, not 16). Even so, at CAP 250 a 16-way grind moves a one-third coalition's
capture from once per ~53 years toward once per ~10 years once the lead-probability factor is
folded in. Real, not fatal, worth removing.

Two clean options:

- **1a. Keep `seed(H) = H(parent.m_anchor_hash, H)` (PosSeedForChild today) and accept the
  bounded grind.** Simplest, zero new code. Defensible at CAP 250 (the grind stays in the
  single-digit-years range for a one-third coalition and is negligible for a quarter-stake
  one), weaker at CAP 100.
- **1b. Seed the committee from a BURIED anchor: `seed(H) = H(anchor_hash_of(H - D), H)` for a
  small constant `D` (say 2 to 6).** The anchor that seeds H's committee is then fixed `D`
  blocks before H is produced, so the leader of H-1 cannot grind it; an attacker would have to
  lead H-D and capture H, D apart, which is far harder. Cost: the committee becomes knowable
  `D` blocks earlier, i.e. slightly more DoS lookahead (the exact trade Alberto already
  accepted at one block; `D` makes it `D+1`). One extra field read in the seed function,
  otherwise mechanical.

Recommendation: **1b with D = 2.** It removes the new grind surface almost entirely for one
added block of lookahead, and it composes with any CAP. If you prefer to minimise lookahead and
lean on CAP for the margin, 1a with CAP 250 is acceptable.

## 2. Certificate format and the registry BLS keys

This is where the 32 KB to 300 bytes comes from, and it requires one registry change.

Today each BLS certificate entry carries, per member, `{staking pubkey, VRF proof, BLS pubkey,
BLS proof-of-possession}`, about 257 bytes each (chainparams.cpp:444 sizes the block proof at
`300 * 100 + 2000`). All of it exists to prove PRIVATE-sortition membership. The BLS pubkey is
derived from the member's staking PRIVATE key (`PosBlsSeedFromKey(const CKey&)`,
pos_producer.cpp:49/243), so a validator, holding only the staking pubkey, cannot recompute it,
which is why it is carried in every block.

Under Option A the member SET is public (section 1), so the only per-member data a validator
still cannot derive is the BLS pubkey. Put it in the registry once:

- **Registry gains a BLS pubkey per staker.** `StakeRegistry` (pos.h:104) currently maps
  pubkey to weight in two layers (config and UTXO). Add the member's BLS pubkey to each entry.
- **Proof-of-possession is verified ONCE, at registration**, not per block. The staking output
  (or the config entry, for genesis stakers) commits to the BLS pubkey and a PoP; the
  consensus rule that admits a staking output verifies the PoP then. This binds the BLS key to
  the staker permanently and stops anyone registering someone else's BLS key.
- **The certificate becomes**: the leader signature, one 96-byte BLS aggregate signature, and a
  `K`-bit signer bitfield over the deterministic committee order (32 bytes at CAP 250). No
  per-member pubkeys, VRF proofs, or PoPs. Validation: recompute committee(H) and `K`, read the
  bitfield's set bits as the signers, look up their BLS pubkeys in the registry, aggregate,
  verify the single signature, and check `popcount(bitfield) >= PosQuorum(K)`. This is the
  ~300-byte, member-independent certificate.

Block-proof size cap (`consensus.max_block_signature_size`) drops accordingly; it can stay
generous (a few KB) without cost since the real certificate is tiny.

The coinbase SEQCMT / SEQVRF member commitments (`BuildPosVrfMemberCommitment`,
`ExtractPosVrfMembers`) are removed for the committee (the leader's own VRF proof for its slot
time-gate stays). The validation blocks at validation.cpp:2240-2287 (MuSig2) and 2296-2330
(BLS) that re-verify each member's VRF proof and threshold membership are replaced by the
recompute-and-lookup flow above.

## 3. Code seams (where the change lands)

- `src/pos.cpp`: `PosVrfIsCommitteeMember` retires for committee membership; add
  `PosPublicCommittee(registry, seed) -> {members, K}` (thin wrapper over `PosCommittee` that
  also returns `K` and each member's registry BLS pubkey). `PosQuorum` unchanged; callers pass
  actual `K`. `PosSeedForChild` / `ComputePosSeed` change only if DECISION 1b is chosen.
- `src/pos.h`: `StakeRegistry` entry gains a BLS pubkey; `PosBlsMember` / certificate structs
  lose the per-member proof fields; add the bitfield encode/decode.
- `src/pos_producer.cpp`: the member-collection loop (211-279) selects from the public
  committee instead of VRF-threshold; shares still flood, but the assembled certificate is the
  bitfield + aggregate; quorum checks (263, 645, 1075) use actual `K`.
- `src/validation.cpp`: the two certification blocks (2240-2330) recompute the committee and
  verify against registry BLS keys; the finality/quorum key (`m_pos_countersigs`, 2444, 3168)
  reads the popcount; escaping-stall (2257/2313) unchanged in spirit, `min_members` against
  actual `K`.
- `src/block_proof.cpp`: quorum check (65) uses actual `K`.
- Registration path (wherever staking outputs enter the registry, plus genesis config stakers
  in chainparams.cpp): parse and PoP-verify the BLS pubkey; store it.
- `src/chainparams.cpp`: `g_pos_committee_size` becomes the CAP (default per DECISION 2);
  genesis staking registry entries carry BLS pubkeys.

## 4. DECISION 2: CAP value

My recommendation is CAP = 250 (quorum 126): clears the classical one-third Byzantine bound
(capture once per ~53 years at exactly one-third stake, and section 1's grind keeps it in the
years range under 1b), one 35%-sleepy half-hour outage per ~1.6 years, representativeness
+/-5.8pp, and after the certificate shrink there is no efficiency reason to stay small. CAP 200
(quorum 101) and CAP 100 (quorum 51) are documented fallbacks (Table 5 of the memo). The value
is a single constant and does not affect any of the code above, so it can be finalised at
re-genesis; I will build against 250 unless told otherwise.

Related minor choice: `PosQuorum(K) = K/2 + 1` gives signer-overlap 2 at even `K` and 1 at odd
`K`. If you want the one-equivocator margin at every size, use `Q = floor(K/2) + 1` with an
extra `+1` when `K` is odd. It only matters at odd pool sizes below the cap; default is the
current formula.

## 5. Migration

Re-genesis only. On the current 100-staker fleet with any CAP >= 100, `K = min(100, CAP) = 100`
and `Q = 51`, so a re-genesis chain behaves identically to today in normal operation; the fix
changes nothing observable until the staker pool grows past the cap. The genesis registry must
carry BLS pubkeys for the bootstrap stakers (section 2), which is a genesis-format change to
fold into the re-genesis work already planned.

## 6. Test matrix (tests-first)

1. **Disjoint-quorum regression** (the section-1 safety fix): regtest cluster, pool > committee,
   induced netsplit. Red under the current threshold rule (two same-height certificates form);
   green under the public cap (cannot). This is the acceptance test.
2. **Certificate size**: assert a real block's certificate is ~300 bytes and independent of `K`.
3. **Quorum-from-K**: pool < CAP (e.g. 60 stakers, CAP 100) certifies at 31-of-60, not 51-of-60.
4. **BLS-key registry**: a staking output with a bad PoP is rejected at registration; a member
   whose BLS key is not in the registry cannot contribute to a certificate.
5. **Leader outside committee**: a VRF leader not in the public top-K still produces a valid
   block certified by the committee.
6. **Seed source** (if 1b): the committee for H is stable under the current leader's anchor
   choice; changing `anchor(H-1)` does not change committee(H).
7. **Sandbox** (Alberto's empirical directive, existing box): ~150 stakers, cap 30, induced
   partitions to watch the split happen (threshold) and not happen (public); EU/US/Asia tags to
   check representation against Table 1; 20/35/45% stopped to check stall frequency against
   Tables 2 to 4.

## 7. Status of the memo's six decisions

1. Section-2 fix / Option A: **CONFIRMED by Alberto**. This spec.
2. CAP value: recommended 250, building against it, final value locks at re-genesis (DECISION 2).
3. Quorum from actual `K`: in scope here; odd-`K` overlap-2 variant optional (section 4).
4. 3A Tier 1 (certificate gossip + valve suppression): SEPARABLE, node-local, not blocked by
   this change; awaiting go/no-go.
5. Unified share-lock (3A Tier 2 + 3B): SEPARABLE, node-local; awaiting go/no-go.
6. Test plan: section 6.

Not started in code. This change is a separate tests-first branch/PR from the analysis reply.
The only ruling that must precede the consensus code is DECISION 1 (seed source); DECISION 2
(CAP) and the 3A/3B fixes do not block it.
