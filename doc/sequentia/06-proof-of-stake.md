# Challenge 3 — Proof-of-Stake consensus (PoC)

> This is the first proof-of-concept of the Proof-of-Stake consensus from the
> theoretical paper (section iv, principles 3/6, and the consensus algorithm).
> It builds on the Bitcoin anchoring of doc 03, because the paper derives the
> consensus randomness seed **and** the liveness clock from the Bitcoin anchor.
> Docs 07 (VRF sortition, committees, aggregation) and the later roadmap items
> below extend this layer; all of it now lives on the main development branch.

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
- **Committee certification** (`-poscommitteesize`, 1–16 in this script form,
  default 1 = disabled): the slot's committee is the first *n* entries of the
  schedule, and the block challenge becomes
  `<leader> OP_CHECKSIGVERIFY <q> <c_1..c_n> <n> OP_CHECKMULTISIG` with `q` a
  strict majority — the PoC form of the paper's 51-of-100 certification
  (principle 6): a block *cannot exist* without a committee quorum, which is
  what gives immediate finality. The script interpreter enforces it via the
  unchanged `CheckProof`. Sizes beyond 16 (up to the paper's 100) use MuSig2
  signature aggregation — implemented as `-posaggcommittee`, doc 07 §6.
- Produces blocks from the miner when the node holds the eligible leader's key
  (plus a committee quorum of keys via `generateposblock`'s `committeekeys`).
- Exposes `getstakerinfo` / `getposschedule` (incl. committee + quorum) RPCs.

What this base layer originally deferred — **all four since implemented** (§6
roadmap items 6–10, doc 07):

- Private VRF sortition (`-posvrf`; the base layer's *public* deterministic
  election remains available without the flag — see §5 security notes).
- Paper-scale (100-member) committees via MuSig2 signature aggregation
  (`-posaggcommittee`), including distributed signing across hosts.
- On-chain stake registration / unbonding (CSV-locked staking outputs, §5).
- Bitcoin checkpoints (dynamic + operator-configured static) and CSV stake
  locktimes against long-range attacks.

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

Consensus, split across two stages because the stake registry mirrors the
*active tip's* UTXO set and headers/blocks can be accepted far ahead of it:

1. `CheckChallenge` (header time) checks only the challenge's *form* — a
   recognized PoS leader/committee challenge.
2. `CheckPosStakeRules` (in `ConnectBlock`, where the registry equals the
   block's parent state) does the election: the leader must be a registered
   staker; let `r` be its rank in `schedule_h`, require
   `block.nTime >= P.nTime + r * pos_slot_interval` (the staker's slot has
   opened — a higher-ranked staker cannot pre-empt a lower-ranked one); and
   the challenge's committee must be exactly the elected one with a
   strict-majority quorum.
3. `CheckProof` (unchanged) verifies the block signature satisfies
   `challenge_h` — i.e. the block really is signed by `leader_r`.

The signature itself rides the existing `proof.solution` (legacy) or
`m_signblock_witness` (dynafed) plumbing untouched. The Bitcoin anchor is
already committed in the header (challenge 2), so the seed is covered by the
block hash the leader signs.

## 3. Election in detail

- **Stake registry** (`src/pos.{h,cpp}`, `StakeRegistry`): a singleton
  `{CPubKey → uint64_t weight}`. Populated from chain configuration
  (`-staker=<pubkeyhex>:<weight>`, repeatable, the genesis staker set) plus
  the chainstate-tracked UTXO staking layer of §5 (CSV-locked staking
  outputs, mirrored on every connect/disconnect).
- **Weighted ticket**: for staker `k`, ticket
  `t_k = H(seed || pubkey_k)` interpreted as a 256-bit big-endian integer.
  The election ranks stakers ascending by the 256-bit integer quotient
  `t_k / weight_k` (`WeightedTicket` in `src/pos.cpp`). Lower ⇒ better ⇒ lower
  rank. Ties broken by pubkey. More stake ⇒ statistically more rank-0 wins,
  proportional to weight.
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
  only the winner can prove they won, after the fact. The election functions
  are isolated in `src/pos.cpp` (`PosSchedule` / `PosRank`) precisely so the
  EC-VRF (RFC 9381) sortition of doc 07 could replace them without touching
  the consensus wiring — which `-posvrf` now does.
- **No slashing.** With committee certification enabled (majority quorum), a
  block cannot exist without most of the committee signing it — the paper's
  immediate-finality property — but nothing yet punishes a committee that signs
  two blocks at the same height (the paper handles this with the
  enforce-consensus rule, principle 4). Long-range attacks are mitigated for
  online nodes by the principle-11 defenses (CSV stake locktimes + Bitcoin
  checkpoints, roadmap items 8/9); bootstrapping a *fresh* node against a
  long-range fork from checkpoints alone remains future work.
- **Stake is on-chain (with a config bootstrap layer).** The registry sums a
  `-staker` configuration layer (the genesis stake set) and a UTXO layer:
  every unspent staking output — the bare script
  `<csv> OP_CHECKSEQUENCEVERIFY OP_DROP <pubkey> OP_CHECKSIG` holding an
  explicit policy-asset amount with `csv >= -posunbonding` — adds its amount
  to its key's weight. The layer is a pure function of the UTXO set
  (rebuilt from it at startup, mirrored exactly on every tip
  connect/disconnect, hence reorg-safe), and unbonding is the CSV-gated
  spend, enforced by the script itself — the stake locktime of
  principle 11. Confidential outputs cannot carry weight (hidden amounts).
  With on-chain stake, all registry-dependent validation runs at block
  connect time (`CheckPosStakeRules` in `ConnectBlock`) — never at header or
  block-acceptance time, both of which can run far ahead of the active chain
  the registry mirrors — so headers-first sync and parallel block download
  cannot mis-evaluate eligibility, in either election mode.

## 6. Roadmap within PoS

1. [x] Stake registry + deterministic stake-weighted, anchor-seeded ranked
       election (`src/pos.{h,cpp}`).
2. [x] Consensus enforcement via per-block challenge: structural form in
       `CheckChallenge` (header time), the registry-dependent election in
       `CheckPosStakeRules`/`ConnectBlock`; signature check reuses
       `CheckProof`.
3. [x] Miner elects self and produces when its slot opens.
4. [x] `getstakerinfo` / `getposschedule` RPCs; `-staker` / `-posslotinterval`
       / `-con_pos` options; a `pos` regtest-style chain.
5. [x] Functional test: stake-weighted schedule, leader-signed block accepted,
       wrong-leader / early block rejected, multi-staker liveness handoff.
6. [x] Private VRF sortition: the VRF primitive (`src/vrf.{h,cpp}`,
       `vrfprove`/`vrfverify`) **and** the `-posvrf` consensus mode
       (coinbase-committed proof, proof-derived stake-weighted slots,
       validated at connect time) — see doc/sequentia/07-vrf.md.
       Now combined with committee certification: VRF-sortitioned
       committees with per-member eligibility proofs (doc 07 §4.5).
7. [x] Committee + majority countersignature certification (immediate
       finality, principle 6): script multisig up to 16 members
       (`-poscommitteesize`, quorum = strict majority, enforced by consensus;
       `feature_pos_committee.py`), and **paper-scale committees up to 100
       members** via MuSig2 signature aggregation (`-posaggcommittee`: one
       BIP340 signature certifies the whole committee — doc 07 §6,
       `feature_pos_agg_committee.py`). **Distributed signing** lets a
       committee of separately-hosted members certify a block without any node
       holding all the keys: the MuSig2 RPC suite plus
       `getposblocktemplate`/`submitposblock`, exercised across three nodes in
       `feature_pos_distributed_committee.py` (doc 07 §6).
8. [x] On-chain stake registration / unbonding: locked staking outputs
       (`getstakescript`, weight = explicit policy-asset amount, minimum
       `-posunbonding` CSV), UTXO-derived registry layer mirrored at
       ConnectTip/DisconnectTip and rebuilt from the UTXO set at startup;
       registry-dependent VRF checks moved to connect time
       (`feature_pos_stake.py`: bootstrap, registration, reorg round-trip,
       restart rebuild, CSV-gated unbond, eligibility loss).
9. [x] Bitcoin checkpoints vs. long-range attacks (principle 11): anyone may
       commit a block hash into the parent chain ("SEQCKPT" OP_RETURN, see
       `getcheckpointpayload`); once buried `-poscheckpointdepth` deep, nodes
       that have the block on their active chain treat it as finalized,
       rejecting forks below it (`bad-fork-prior-to-pos-checkpoint`) — even
       longer, validly-signed branches. Checkpoints only lock in validated
       history, never replace it, so conflicting/bogus checkpoints are
       harmless. The anchor watcher scans parent blocks for commitments
       (`getcheckpointinfo`); the stale-anchor reorg-follower never walks
       below the finality point. Combined with the CSV unbonding period
       (item 8), which must exceed the checkpoint cadence, this closes the
       posterior-corruption window (`feature_pos_checkpoints.py`: a
       longer competing branch from the same staker keys is rejected by the
       checkpointed node). A node that has passed a checkpointed height
       *without* the checkpointed block raises a `conflicts` alarm in
       `getcheckpointinfo` and the debug log (the fresh-sync / wrong-fork
       case): it cannot finalize a block it never validated, so it never
       silently follows a checkpoint, but it flags that it may be on the
       losing side of a long-range fork.
10. [x] **Operator-configured static checkpoints** (`-poscheckpoint=height:hash`,
       repeatable): the dynamic checkpoints above only lock in history a node
       has *already* validated, which does not help a node syncing from
       genesis against a long-range alternate history. A configured checkpoint
       is known before any block is downloaded, so a block presented at the
       pinned height must carry the pinned hash, otherwise it — and any branch
       built on it — is rejected in `ContextualCheckBlockHeader`
       (`bad-pos-checkpoint`); a node fed a bogus long-range chain refuses it
       and disconnects the lying peer rather than following it. This is
       reject-only and never makes a node seek a particular branch
       (surfaced in `getcheckpointinfo`'s `configured` array;
       `feature_pos_config_checkpoints.py`). Automatic fresh-sync chain
       *selection* from the *dynamic* parent-chain checkpoints (actively
       reorganizing onto checkpointed history a node has not yet downloaded)
       remains future work — by design; it needs block-download changes,
       whereas the static backstop above closes the practical fresh-sync hole
       without them.
