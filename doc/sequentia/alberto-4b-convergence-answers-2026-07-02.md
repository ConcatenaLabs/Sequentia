# Answers on the 4b convergence rule: the swap double-spend, and the residual balanced split

Alberto, this answers your 7/2 messages: the atomic-swap double-spend question (does the
attacker need a big fraction of both networks), and the residual balanced-split questions
(restart, operator intervention, and a time-based "reconverge after N Bitcoin blocks" rule).
Every number below is checked against the shipped code, not recalled.

## Bottom lines

1. The efficient 4b atomic-swap double-spend is a **Sequentia-stake attack with zero Bitcoin
   hashpower**, not an attack on both networks. It needs a quorum-certified rival, which needs
   roughly majority active stake. So your "attack a big fraction of both" instinct is right in
   spirit about cost, but wrong about which chain: Bitcoin is left untouched on purpose, so the
   Bitcoin leg stands while the Sequentia leg is rolled back.

2. A pure Bitcoin reorg can only **cancel** a swap, never **rob** it: the anchor-ordering gate
   ties the Sequentia leg's fate to a Bitcoin height at or above the Bitcoin leg, so any reorg
   deep enough to drop one drops both. Bitcoin's reorg-resistance is therefore not the
   rate-limiter for a profitable swap double-spend; honest-majority stake is.

3. 4b does not lower Sequentia's numeric safety threshold below the honest-majority-stake bound
   it already assumes. It does deliberately trade away part of immediate finality's
   *stronger-than-majority* resistance in the already-compromised (attacker > 50% stake) regime.
   That is a conscious choice, worth stating plainly in the paper.

4. On the residual balanced split: **do not add a time-based rule that can reorg a finalized
   block.** It converts a rare, detectable, fail-safe stall into an attacker-inducible,
   zero-hashpower reorg of final history, which is exactly the swap double-spend anchoring
   supremacy is built to forbid. The correct fix for the residual is smaller and safer: make the
   fork-choice tiebreak globally deterministic (see §2.3). That converges the residual
   automatically once nodes can see both chains, with no deadline and no weakening of finality.

5. Restart is not a consensus healer, and operator intervention is fine on testnet but must not
   be the mainnet safety mechanism. The deterministic-tiebreak fix is what removes the reliance
   on both.

## 1. The atomic-swap double-spend (your 2:24 message)

**How it is actually mounted.** The profitable version does not touch Bitcoin. It exploits
root cause B: two Sequentia blocks at one height, both anchored to the *same* valid Bitcoin
block, so the anchor watcher sees "ok" on both and anchoring genuinely cannot arbitrate. The
attacker, holding majority stake, does the following:

1. Takes the maker side of a swap. The taker reveals the secret s by claiming the Sequentia
   (asset) leg on chain A; the attacker uses s to claim the Bitcoin leg. Bitcoin leg settled.
2. Produces a rival Sequentia chain B, quorum-certified, that omits the taker's asset-leg claim,
   forking at or below the height where the claim sat but above the checkpoint floor.
3. Everyone converges to B (more or equal certification, and the attacker controls the
   comparator because it controls the committee). The asset leg is gone; the Bitcoin leg,
   claimed with s, stands. The attacker keeps both legs.

The point you were reaching for: the attacker leaves Bitcoin alone precisely so the Bitcoin leg
survives. A Bitcoin reorg would revert *both* legs (see bottom line 2), which cancels the swap
rather than robbing it, so the attacker never wants one.

**What it costs (checked constants).** Committee 100, quorum = committee/2 + 1 = 51. A
quorum-certified rival needs roughly majority of active staked Sequence: about 51 of 100 seats,
which given VRF-sortition variance is near-certain in one round at about 62% of active stake, or
marginally above 50% with retries across successive heights (each height is a fresh committee).
Minimum stake output is 40,000 SEQ, which is 0.01% of the 400,000,000 cap, so seats are a stake
question, not a Sybil-identity question. Bitcoin side of this path: reorg depth 0, hashpower 0%.

Relative to a plain Bitcoin double-spend (which needs majority of roughly 800 EH/s, i.e. capex
on the order of tens of billions of USD), the swap double-spend costs about half the value of
the total *active staked* Sequence. For a young sidechain that is far cheaper, so honest
majority stake is the assumption the whole thing rests on, and it must be stated as such.

**Two honest caveats I will not paper over.**

- *4b weakens finality in the compromised regime.* Without 4b, a quorum-finalized block survives
  on honest nodes against any stake fraction short of a Bitcoin reorg or the 2016-block
  checkpoint. With 4b, an attacker above 50% stake can reverse finalized history with no Bitcoin
  reorg and, since there is no slashing today, no penalty. 4b keeps the same numeric threshold
  but gives up the "better-than-majority until Bitcoin says otherwise" property in the
  already-lost regime. That is the right trade for liveness, but it is a trade.

- *A cheaper sub-majority variant exists, and it needs a stall.* Below 51% the required
  quorum-certified rival is unforgeable and escaping-stall cannot help (it is sub-quorum by
  construction and 4b demands a quorum cert). But 4b's "strictly above the local finalized
  height" only bites while the honest frontier keeps moving. Freeze it with an induced stall (a
  partition, or the CPU starvation that produced the live 96/4, often alongside a Bitcoin
  reorg-of-reorg) and a sub-majority attacker gets a nonzero, compounding-over-heights chance
  plus cheap sub-quorum private-branch padding. So your "attack both / needs a rare coincidence"
  instinct is vindicated for this cheaper variant: it wants the stall, which usually wants the
  Bitcoin event.

**The dial is swap policy, not consensus.** Do not weaken escaping-stall or 4b to plug this. The
taker's anchor-depth and finality policy is the lever, plus the standing honest-majority-stake
requirement. Checkpoint immunity (2016 Bitcoin blocks buried, about 14 days) is real but nobody
waits two weeks, so essentially every real swap lives in the immediate-finality window and
trusts honest majority. See §4 for the concrete policy fix.

## 2. The residual balanced split (your 2:51 to 3:13 messages)

First, the framing that makes the rest fall out. Your 96/4 is the **asymmetric** case, and 4b's
first part already heals it: the 96 keep producing quorum-certified blocks strictly above the
fork, the 4 see them and yield, and the 4 can never produce a competing quorum cert, so they
never make the 96 yield. That is exactly why you praised it.

Your residual is the **symmetric** case: about 49 versus 49 with the rest asleep, so neither
side ever reaches 51, both advance sub-quorum via escaping-stall, and 4b's strictly-above rule
never fires because neither side ever gets a quorum cert above the split. This is a genuinely
different regime, and it is governed by the fork-choice comparator, not by 4b and not by the
finality gate (sub-quorum escaping-stall blocks are never immediately finalized, so the gate
never locks them). That single fact is the key to all five of your questions.

### 2.1 Will a restart fix it? (Q1)

Partially, and not reliably. A restart re-reads the fork-choice keys (the cf7556543 reload fix)
and re-runs the comparator, so it re-selects a tip; but it picks whatever its peers feed it by
the *same* comparator, so restart gives local re-selection, not guaranteed global convergence.
Restarting both sides can even preserve or re-balance the split. And for a true network
partition, restart cannot help until connectivity returns, because a node cannot compare a chain
it cannot see. Restart is a legitimate operator tool, not a consensus healer.

### 2.2 Will operators notice and intervene? (Q2)

On our monitored cluster, yes, quickly: the signature is two live tips with the finalization
height frozen (UpdateTip finds no block at or above quorum) and both anchors "ok", which is
exactly the 96/4 signature we already recognized. invalidateblock on the minority tip is the
current fail-safe band-aid. That is acceptable as a testnet backstop. It is not acceptable as
the mainnet safety mechanism: a Bitcoin-anchored immediate-finality chain whose value
proposition is safe cross-chain and Lightning swaps cannot depend on a human being awake.

### 2.3 The time-based rule, and what to do instead (Q3, Q4)

Recommendation: **do not adopt a time-based rule that can override a finalized block, and you do
not need one.** The real defect in the residual is smaller: in a symmetric split the comparator
ties on work and on countersigs (both sides ~49), the VRF key is documented as unreliable, and
it then falls through to nSequenceId, which is each node's local receive order (validation.cpp
lines 153 to 155). First-seen is node-local, so partitioned nodes each keep their own chain: the
comparator itself perpetuates the split.

Fix that fall-through. Replace nSequenceId with a globally deterministic, content-derived key
(the lowest block hash is the simple choice). Then, the moment nodes can see both sub-quorum
chains, every node picks the same one and the residual converges on its own, with no deadline
and no finality override, because the dropped side was never final. This also hardens the
original finality-split-stall. It is a fork-choice change, so it wants coordinated adoption, but
it is not a hard fork (no block becomes invalid). One caveat to note in the paper: a lowest-hash
key is grindable by a party that already controls about half the committee (they can vary block
content to lower their hash), but that party is by definition already in the compromised regime
and could converge the split to their chain anyway; a committee-signature-derived key is harder
to grind if we want to close even that edge.

Why not the timer, stated as tradeoffs (Q4): a "reconverge after N Bitcoin blocks" rule that can
reorg a finalized block trades a rare, detectable, fail-safe *liveness* fault (a stall) for a
new *safety* hazard (a finalized-block reorg on a Bitcoin-block clock). For an immediate-finality
chain that is a strictly worse trade. Concretely it (a) reintroduces the cross-chain swap
double-spend, because it produces a Sequentia reorg with no Bitcoin reorg, which is precisely the
decoupling the anchor-ordering gate forbids; and (b) reopens the same-height certificate
comparison that 4b deliberately withdrew (the posterior-corruption channel). The deterministic
tiebreak gives you the convergence you want without either.

If you ever did want an automatic healer beyond the tiebreak fix, its only defensible form fires
strictly where no quorum certificate exists in the contested band (never overrides finality) and
re-runs the fixed comparator. But once the tiebreak is deterministic, that healer is redundant
for the only case it could touch. So the honest answer is: the tiebreak fix is the healer.

### 2.4 Attack surface of the time-based rule (Q5)

If the timer can override finalized blocks, its attack surface is the whole problem: an attacker
induces a balanced partition (a network split, selective delivery, or DoS of the committee
boxes, the last of which the live 96/4 shows is enough) and rides the deadline to reorg an honest
finalized block. That collapses the cost from majority-of-both-chains to
maintain-a-balanced-partition-for-N-blocks plus win the tiebreak, with zero Bitcoin hashpower.
Even constrained to never-quorum bands, the timer adds the partition-induction and
tiebreak-timing surface for no benefit the deterministic fix does not already provide. This is
the core reason the recommendation is "tiebreak yes, finality-overriding timer no."

## 3. Suggested convergence rule-set for the theoretical paper

Stated as a strict precedence, which is what the code already enforces plus the one addition:

1. **Anchoring supremacy.** A block is valid only while its anchor is on Bitcoin's best chain to
   any depth; anchor-driven forks resolve automatically and deterministically from Bitcoin
   ground truth. This outranks everything below.
2. **Immediate finality, modulo Bitcoin.** A quorum-certified block is final; the finality gate
   rejects forks at or below it, including a same-height rival with more signatures. The sole
   release valve is anchoring lowering the finalized point when Bitcoin orphans the anchor.
3. **4b strictly-above convergence (your first part).** A node abandons its finalized block only
   for a rival that forks no lower than the checkpoint floor, is anchor-valid throughout, and
   carries a quorum cert strictly above the local finalized height. Never a same-height
   comparison. Heals asymmetric splits (the 96/4).
4. **Deterministic residual tiebreak (the addition).** In the symmetric sub-quorum split where
   no side ever reaches quorum, the comparator's final key is content-derived and global (not
   first-seen), so all nodes that can see both chains converge without a deadline and without
   overriding finality.
5. **No finality-overriding time-based reconvergence.** Explicitly rejected, for the reasons in
   §2.3 and §2.4.
6. **An ongoing partition is a liveness property, not a safety bug.** While it lasts both sides
   advance sub-quorum and cannot converge; on heal, rule 4 converges them. State this honestly.

## 4. The swap-safety coupling (do this regardless of the rest)

The swap gate VerifySeqLegSafe currently requires the Sequentia leg's block to anchor at a
Bitcoin height at or above the Bitcoin leg, with anchor status "ok"; it does **not** require the
block to be quorum-certified, and min_anchor_depth is a maker-only dial that defaults to 0 with
no relay-enforced floor. That means a swap can release on a sub-quorum block, which is exactly
the block a residual split can drop. Two changes close this by construction:

- Require the Sequentia leg to sit in a **quorum-certified (immediately-final)** block before
  releasing the counter-leg, not merely an anchor-deep one. This is the difference between "a
  Bitcoin reorg can cancel me" (fine) and "a stake attacker or a residual split can rob me" (not
  fine).
- Give cross-chain offers a **sane non-zero min_anchor_depth default** and enforce it in the
  swap driver (the VerifySeqLegSafe path), not just as an optional per-offer dial. Whatever the
  maximum reorg depth of any convergence mechanism we keep, swaps must require more than that
  before releasing; the convergence bound defines the safe swap depth.

## 5. Open items and one question back

- Change 4a is acked and staged on the box; it is node-local with no consensus-rule change, so
  old and new nodes interoperate with no fork risk, and it also carries the fork-choice-key
  reload fix (without which every restarted node currently sees its own chain as uncertified). I
  have not deployed it; a production committee restart is Andreas's call to trigger.
- 4b's strictly-above rule and the deterministic-tiebreak addition are both consensus-level and
  still design-stage; nothing beyond 4a plus the reload fix has shipped.
- One question back: do you want the residual tiebreak keyed on the lowest block hash (simple,
  slightly grindable in the already-compromised regime) or on a committee-signature-derived value
  (harder to grind, more to specify)? I lean to the signature-derived key for the paper and the
  block-hash key as the minimal implementation, but it is your call which becomes canonical.
