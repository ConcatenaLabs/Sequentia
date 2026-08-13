# Supervised assets: implementation notes

Companion to `supervised-assets-proposal.pdf`. That document argues the case and states
the boundaries; this one is for whoever writes the code. It assumes you accept the
design and want the details, the insertion points, and the places it will bite.

Status: nothing here is implemented. A sketch written during analysis was reverted
unbuilt. File references are against `master` at the time of writing.

---

## 1. The rule in one paragraph

An asset may be issued **supervised**, which commits a **supervision descriptor** into its
asset id at issuance and can never be added, removed or changed afterwards. The descriptor
carries a version, feature bits, an **operational freeze key** and a **recovery key**. The
issuer publishes **freeze records**, each signed by the operational key. Consensus rejects
a transaction that *spends* a supervised-asset output when the output's script is frozen
**and** the spend reveals a single-owner script; a freeze takes effect from the block
*after* the one carrying its record. Creating outputs that pay a frozen script stays legal.
A supervised issuance must carry reissuance tokens, and supervised assets may never appear
in a blinded output. The rule is height-gated.

Everything below is a consequence of one of those clauses, and the design is settled:
Alberto's decisions of 2026-08-13 (`alberto-supervised-assets-decisions-2026-08-13.md`)
are folded in here, so this file is the design of record for the code.

---

## 2. Why the two qualifiers, in implementation terms

Both exist to stop consensus destroying value belonging to someone other than the frozen
party. They are not tunable.

**Spend-only.** `Consensus::CheckTxInputs` (`src/consensus/tx_verify.cpp:195`) rejects the
*whole transaction*, not one input. If output creation were also restricted, a frozen
party appearing anywhere in a multi-party transaction would void it for everyone. With
spend-only, the innocent parties' inputs stay unspent and their funds are untouched; they
simply fail to transact with a frozen counterparty.

**Single-owner-only.** A freeze names a script *hash*; at freeze time nobody, including
consensus, can tell what is behind it. At spend time the witness reveals the script. So
the classification is made where the information exists. Without it:

- Freezing a Lightning funding output (a 2-of-2, `seqln/common/initial_channel.c:159`)
  traps the innocent peer's entire balance with no unilateral escape, since every exit
  from a channel spends that one output.
- Freezing an HTLC or a covenant refund branch converts a *temporary* freeze into a
  permanent transfer of value to the frozen party, because those scripts encode races
  against absolute deadlines (`CLTV`/`CSV`). The frozen party wins the timeout while the
  honest party is barred from broadcasting.
- An issuer cannot distinguish a `to_local` output that is safe to freeze from one whose
  freezing destroys a peer's penalty claim: the scripts are byte-identical
  (`seqln/onchaind/onchaind.c:2718-2722` vs `:3056-3058`) and the difference is private
  off-chain state.

The accepted cost: a holder who parks supervised funds in a shared script before being
frozen is out of reach. This is deliberate and is parity with Ethereum, where value
inside a pooled contract is equally out of reach of an address blacklist.

---

## 3. Data structures

### 3.1 The supervision descriptor, committed into the asset id

**Decided (Alberto, option b with a descriptor).** A supervised asset derives its id over a
small **descriptor**, not over a bare key and not via a separate declaration output:

```
descriptor = { version:u8, feature_bits:u16, operational_key:x-only-32, recovery_key:x-only-32 }
```

The asset id is derived with this descriptor mixed into the entropy, the way
`CalculateReissuanceToken` (`src/issuance.cpp:50-64`) already distinguishes blinded from
explicit issuance by a constant. So the type, the authority and the reserved capability
bits are all cryptographically bound: forging a descriptor for an existing asset id is a
second-preimage on the fast-merkle node, and no field can be retrofitted, because the id
is a hash of the thing that declares it.

Two consequences to hold onto:

- **Keys are x-only BIP340** (Alberto §1). Schnorr gives issuer multisig for free: FROST
  (t-of-n) and MuSig2 (n-of-n) both produce one ordinary signature under one ordinary
  x-only key, so an issuer splitting freeze authority across compliance, legal and cold
  storage needs nothing from us. The codebase already has taproot/Schnorr. Do not use
  ECDSA.
- **The version and feature_bits cost no enforcement code today and must exist in the
  first release**, because the id is forever and a field not committed now can never be
  added to already-issued assets without a new, wallet-invisible asset class. Reserved
  meanings, none implemented now: a total-supervision bit (all scripts freezable, with
  trapped-innocent-funds handled a level up by issuer reissuance or delayed shared-script
  effectiveness), a pause-allowed bit (§3.4), and a whitelist bit (securities allowlists,
  out of scope). Reserving them keeps standard-vs-total a flag rather than a future
  migration. Building any of them now is explicitly out of scope.

**Sequencing consequence, stated because it is the one real cost of option (b).** The
descriptor is in the derivation, and consensus *derives* the asset id rather than reading
it (`CalculateAsset`, `src/confidential_validation.cpp:219`), so a node without the rule
derives a different id for the same issuance. A supervised asset therefore cannot exist
until every node runs the new binary, i.e. not before the activation height. For USDC that
means bridge deposits stay closed until the fork activates, then `USDC.e` is re-issued
supervised. (An earlier draft recommended a declaration-output encoding to allow issuing
before the fork; Alberto chose derivation for the cryptographic-authority and
forward-compatibility properties, accepting that USDC waits for the fork. Recorded here so
the tradeoff is explicit, not to reopen it.)

### 3.1a Two keys, and rotation records

**Decided (Alberto §2).** One immutable key is wrong for a live stablecoin: any compromise
forces migration of the whole asset. So the descriptor commits **two** keys.

- **Operational key.** Day-to-day freezes and unfreezes. Signs freeze records.
- **Recovery key.** Deep cold, itself FROST-shared and geographically split. Its *only*
  power is signing **rotation records** that replace the operational key, or replace
  itself. It cannot freeze.

Separating use from rotation removes the compromise race by construction. A stolen
operational key can grief, visibly and on-chain, but cannot rotate itself away; the issuer
rotates via the recovery key and the incident closes. Simple self-signed rotation would be
worse than nothing: under compromise, attacker and issuer both hold the key, whoever
rotates first wins permanently, and the attacker's first move is to silently seize the
authority.

A **rotation record** is an output like a freeze record (§3.2) but signed by the *recovery*
key, carrying the new operational key (or new recovery key). The registry tracks the
current operational key per asset, starting from the descriptor and advanced by each
rotation record in chain order, reorg-inverted like everything else.

**The unfreeze trap (build this deliberately).** After a rotation, old freeze records must
not remain liftable by the old key, or a stolen old key unfreezes everything. So **unfreeze
validity is checked by consensus against the registry's *current* operational key, not
against the record's own script.** The `OP_CHECKSIG` in the record's own scriptPubKey is
not sufficient authority to unfreeze; the spend must also satisfy the current key. This is
the one place the freeze registry feeds back into spend validation of the records
themselves.

Ordinary signer turnover (an officer leaving) needs none of this: FROST proactive
resharing re-randomizes shares off-chain under the same pubkey. Rotation records exist only
for a key-level incident.

### 3.1b What supervised does NOT grant, and the reissuance-token rule

**Decided (Alberto §3).** Consensus never lets an issuer *spend* a holder's frozen output.
Seize and burn are not consensus machinery: an issuer-spend power would break the script
model and be the chain's largest honeypot. They fall out economically instead, which is
exactly how Tether operates at scale (~$1.1B reissued to law enforcement and victims):

- A permanent freeze is a de-facto **burn**.
- A permanent freeze plus reissuing the same amount to a court-designated address is a
  de-facto **seize**.

The single enabling condition is that the asset is reissuable. So: **a supervised issuance
must create reissuance tokens (`tokenamount > 0`, explicit).** This is a one-line check at
issuance that prevents a "supervised but unable to comply" asset from existing, and it
closes the seize/burn half of the GENIUS / FinCEN-OFAC capability requirement with zero
enforcement code. Whether the check lives in consensus or in registry/wallet policy is
negotiable; that freeze+reissue is *the* answer to seize/burn, and issuer-spend is never
built, is not.

### 3.2 Freeze record

A record is an output. Suggested shape, mirroring `BuildDelegationScript`
(`src/pos.cpp:696`):

```
<FREEZE_MARKER> OP_DROP <asset_id:32> OP_DROP <target_spk_hash:32> OP_DROP
<issuer_freeze_pubkey:33> OP_CHECKSIG
```

Creating the record freezes; spending it unfreezes. Two authority checks, both by
consensus, both BIP340 Schnorr:

- **Admission (the freeze).** A freeze record is only admitted to the registry if the
  issuance transaction also carries a signature over the record contents by the asset's
  **current operational key** (from the registry, per §3.1a). The `OP_CHECKSIG` in the
  script alone is not admission authority; do not repeat the delegation precedent, where
  creating a record needs no signature and would let anyone freeze anyone.
- **Unfreeze.** Spending the record lifts the freeze, but consensus additionally requires
  the spend to satisfy the *current* operational key (the unfreeze trap, §3.1a), not just
  the key baked into the record's own script.

**Pause is a freeze record with a wildcard target** (Alberto §7), not a new record type. A
sentinel target value (e.g. all-zero `target_spk_hash`) means "every script for this
asset". It reuses signed admission, the registry, unfreeze-as-spend, reorg handling and
eviction unchanged. The single-owner classifier still applies to a pause: a paused asset's
shared-script spends are *not* blocked, for the same reason freezing shared scripts is not,
so pause immobilises single-owner holdings while leaving contract races alone. Gate pause
admission on the descriptor's pause-allowed feature bit.

**Rotation records** (§3.1a) share this shape but are signed for admission by the
**recovery** key and carry a new key rather than a target.

### 3.3 Registry

```cpp
// asset -> target hash -> count of unspent records naming it
std::map<CAsset, std::map<uint256, uint32_t>> m_frozen GUARDED_BY(m_mutex);
```

A count, not a set: two records may name the same target, and the freeze must lift only
when the last is spent, or the registry stops being an exact function of the UTXO set
under reorg.

Derived from unspent records, so it inherits the `StakeRegistry` discipline
(`src/pos.h:163`): applied in `ConnectTip`, exactly inverted in `DisconnectTip`, rebuilt
by UTXO scan at startup. Failure to read undo data must be fatal (`AbortNode`), as it is
for the stake registry, or the registry silently desyncs from consensus.

**Do not** put this behind `g_con_pos`. The freeze rule and PoS are independent; a chain
could schedule one without the other, and a skipped revert leaves the registry claiming
freezes the chain no longer contains.

### 3.4 Freeze effectiveness: the block after the record

**Decided (Alberto §5).** A freeze record confirmed in block N takes effect for spends in
block N+1 onward, never in block N itself. This removes the same-block record-plus-spend
ambiguity, whose outcome would otherwise hinge on registry-update ordering inside
`ConnectBlock`, precisely the kind of ordering where mempool acceptance and block
validation can diverge, and a divergence there is a chain split. It also hands the mempool
eviction pass (§5) a full block to run before the freeze bites. Implement by having
`CheckTxInputs` consult the freeze set as of the *parent* block: the registry state applied
through N-1, or equivalently apply a block's freeze records only after validating that
block's spends. Pick one and make mempool and block validation use it identically.

The cost is that the frozen party gains one block, about sixty seconds. Point §6 (the
issuer-to-producer submission channel) is what actually addresses front-running; the
one-block delay is not the exposure that matters.

---

## 4. Enforcement

Insertion point: the non-pegin branch of the input loop in `Consensus::CheckTxInputs`
(`src/consensus/tx_verify.cpp:~231-243`), immediately after `spent_inputs.push_back`.
That function is shared by mempool acceptance (`validation.cpp:918`) and `ConnectBlock`
(`validation.cpp:2902`), so one insertion covers both, which is what keeps the two from
disagreeing.

Sketch:

```cpp
if (g_asset_freeze_height > 0 && nSpendHeight >= g_asset_freeze_height) {
    const FreezeRegistry& freezes = FreezeRegistry::GetInstance();
    if (!freezes.Empty() && coin.out.nAsset.IsExplicit()) {
        if (IsSingleOwnerSpend(tx, i, coin.out.scriptPubKey) &&
            freezes.IsFrozen(coin.out.nAsset.GetAsset(),
                             FreezeTargetHash(coin.out.scriptPubKey))) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                 "bad-txns-asset-frozen", ...);
        }
    }
}
```

Notes that matter:

- `CheckTxInputs` has no `Consensus::Params`. Follow the `g_con_elementsmode` pattern
  (`src/primitives/transaction.h:29`, set per-chain in `chainparams.cpp`): a global int
  mirroring `Consensus::Params::asset_freeze_height`, 0 meaning off.
- The empty-registry short-circuit keeps the cost at one map lookup for chains and assets
  that never use this.
- **`IsSingleOwnerSpend` must be exactly identical in mempool and block validation.**
  Put it in one function with no policy inputs. A divergence here is a chain split.
- Guard on `IsExplicit()` before `GetAsset()`. `CConfidentialAsset::GetAsset()` asserts
  explicitness and would abort the node on a blinded input; a naive check here is a free
  remote crash.

### 4.1 Classifying a single-owner spend

Decidable from the revealed script and witness:

- P2WPKH: witness is `<sig> <pubkey>`.
- P2TR key-path: witness is a single signature, no control block.
- Everything else (P2WSH, P2TR script-path, bare multisig, P2SH): **not** single-owner,
  freeze does not apply.

Be conservative: anything unrecognised is not single-owner. A false negative means a
freeze does not bite; a false positive means someone's contract funds are destroyed.

---

## 5. Mempool: the part most likely to break the chain

This is not optional and it is not small.

A spend that was valid on entry becomes invalid the moment a freeze record confirms.
Nothing evicts it: `removeForBlock` drops only included and directly-conflicting
transactions, and a freeze record conflicts with nothing. Consequences, in order:

1. The stale transaction stays resident (default expiry 336 h).
2. `addPackageTxs` re-selects it into every template.
3. `CreateNewBlock`'s final `TestBlockValidity` throws, so the producer skips its slot.
4. Every producer does the same. **The chain stops making blocks.**

And `CTxMemPool::check` re-runs `Consensus::CheckTxInputs` over every entry inside a bare
`assert`, with the comment "CheckTxInputs() should always pass"
(`src/txmempool.cpp:920`). A freeze is a new way for an accepted transaction to become
invalid with none of its inputs spent and no ancestor changed, so under `-checkmempool`
the node aborts.

Required:

- On block connect: for each newly frozen `(asset, target)`, remove mempool spends of
  matching outputs, with descendants.
- On block disconnect: the inverse, so unfrozen transactions can be re-accepted.
- Relax or re-derive the `check()` assert, with a comment naming this rule.
- Make the producer distinguish "template failed, retry" from "template failed
  permanently" so one bad transaction cannot silently cost every slot.

---

## 6. Explicit-only, and the wallet problem

Consensus cannot read a blinded output's asset, so supervised assets must never be
blinded. Enforce at output creation, inductively from issuance, so a supervised asset can
never enter a blinded output in the first place. Enforcing only at spend time is not
enough: a holder who already owns a blinded supervised output would be stranded
permanently.

**The wallet is the trap.** The node wallet blinds change automatically once a transaction
has two or more change outputs, which under any-asset fees is the *ordinary* case for
sending an issued asset (asset out, fee paid in another asset). The DEX's own wallet
daemon does the same. Shipping the consensus rule without fixing both would break ordinary
sends of supervised assets and look like an unrelated wallet bug. Fix the wallets first,
or ship them together.

---

## 7. Activation

`Consensus::Params::asset_freeze_height`, same convention as `pos_exprace_height` and
`pos_escape_stall_mtp_height`: 0 means off, positive H means enforced from H.

Mandatory, not stylistic. The rule rejects spends, so applying it to history produced
before it existed makes that history unvalidatable: a node syncing from scratch, or any
node after `-reindex`, stops dead at the first spend a later freeze covers. CONTRIBUTING.md
records this being learned twice.

Choose H above the tip at release, and cut over every node at once, as with the Simplicity
activation and the 60-second-block fork.

---

## 8. Decisions taken, and what is left open

All five questions this file previously asked are decided
(`alberto-supervised-assets-decisions-2026-08-13.md`). Recorded here so nobody reopens
them by accident:

1. **Single-owner classification.** Unchanged from §4.1: P2WPKH and P2TR key-path only,
   conservative on anything unrecognised, one shared function so mempool and block
   validation cannot diverge.
2. **Where the key is committed.** A descriptor in the asset-id derivation (§3.1), not a
   declaration output. USDC waits for the fork.
3. **Mempool eviction.** Still the highest-risk piece (§5); Alberto agrees the machinery is
   required and wants it reviewed rather than redesigned.
4. **Fees.** Supervised assets **may** pay fees (Alberto §4). A frozen output cannot pay
   fees at all, since the whole transaction is invalid, so no value leaks from frozen
   coins. The launder-through-producer route is weak because fee outputs are explicit and
   coinbase attribution is public, and it requires being or buying a producer. The residual
   is granularity: a coinbase aggregates many payers' fees and UTXO cannot partially freeze
   an output, so the coinbase is freezable whole like any other output, and a producer
   accepting supervised-asset fees knowingly takes that exposure, which the negotiated fee
   market already prices. Banning it would strand holders who own only the stablecoin,
   which is the core use case, and a ban is a pure tightening that can be added later
   behind the usual height gate.
5. **Pause.** A freeze record with a wildcard target (§3.2), gated on a descriptor feature
   bit. This is the natural first-release cut if scope must shrink, provided the bit is
   reserved.

Genuinely still open, and both are scheduling rather than design:

- **Whether the reissuance-token requirement (§3.1b) is a consensus check or registry and
  wallet policy.** Alberto prefers consensus and calls it negotiable. Consensus is one
  comparison at issuance and cannot be bypassed; policy is looser but keeps the rule out of
  the validation path. I lean consensus, since a supervised asset that cannot comply should
  not be expressible.
- **First-release scope.** Freeze, rotation and the descriptor are mandatory in release
  one, because all three are committed in the asset id and cannot be retrofitted. Pause can
  slip. The issuer-to-producer submission channel (§10) is infrastructure and can land
  after, but not much after.

---

## 8a. Front-running, and the submission channel

**Raised by Alberto (§6), and it is a real limit on the compliance promise.** A freeze
record sits in the public mempool before it confirms, so a mempool watcher can move funds
to a fresh script before it bites, reducing every freeze to a chase. The one-block
effectiveness delay (§3.4) is not the cause and removing it would not fix it.

The same race exists on Ethereum, where issuers use private relays. The equivalent here is
authenticated submission of freeze records directly to block producers, outside the public
mempool. That is infrastructure rather than consensus and does not belong in this rule, but
it must be **scheduled alongside** the consensus work: without it the compliance guarantee
is materially weaker in practice than it looks on paper, and that gap should not be
discovered by an issuer.

## 9. Test plan

Minimum before this goes near a live chain.

Functional, per rule:

- A supervised asset can be issued; an ordinary asset cannot be frozen.
- A freeze record without a valid issuer signature is not admitted.
- A frozen single-owner output cannot be spent; the same output before the activation
  height can.
- A frozen output can still *receive*.
- A frozen script in a 2-of-2 / HTLC / covenant spend is **not** blocked.
- Spending the freeze record unfreezes, and the coin becomes spendable again.

Reorg:

- Freeze in block N, disconnect N, coin spendable again; reconnect, frozen again.
- Record created and spent in the same block.
- A reorg that reorders a freeze relative to a spend.
- After `-reindex`, the registry matches the pre-reindex state exactly.

Mempool:

- Transaction accepted, then a freeze confirms: it is evicted, and block production
  continues.
- The same with descendants.
- `-checkmempool` does not abort.
- Unfreeze re-admits.

Adversarial:

- Blinded supervised output cannot be created.
- Blinded input does not crash validation (`GetAsset()` assert).
- Freeze record naming an asset that is not supervised is inert.
- Registry growth is bounded by what the UTXO set can hold.

---

## 10. Scope estimate

A release, not a patch. Ordered roughly by dependency:

1. **Descriptor and derivation** (§3.1): descriptor type and serialization, entropy
   derivation, issuance validation, plus the reissuance-token requirement (§3.1b).
2. **Registry** (§3.3): freeze set, current-operational-key tracking, apply/revert/rebuild
   with the `StakeRegistry` discipline.
3. **Records** (§3.2): freeze, unfreeze-with-current-key (the trap), rotation, wildcard
   pause.
4. **Enforcement** (§4) with next-block effectiveness (§3.4).
5. **Mempool eviction** (§5), the highest-risk piece.
6. **RPCs**: `freezeasset`, `unfreezeasset`, `rotatesupervisionkey`, `listassetfreezes`,
   `getassetsupervision`, plus the machine-readable rejection reason.
7. **Wallets**: explicit change for supervised assets in the node wallet and in the DEX's
   wallet daemon (§6), before or with consensus.
8. **Display**: the property and a coin's frozen status must be visible before anyone can
   acquire or try to spend, across the web wallet, explorer, registry and DEX.
9. **sequentia-qt** (Alberto's standing reminder, and it belongs on this list rather than
   after it): issuance page with the supervised flag and reissuance-token handling, coin
   display showing frozen status before a spend is attempted, send flows keeping change
   unblinded for supervised assets, and the new RPCs surfaced in the GUI. Plan the Qt delta
   as part of each change above, not as a follow-up.
10. **Issuer-to-producer submission channel** (§8a): infrastructure, scheduled alongside.

Mandatory in release one, because all three are committed in the asset id and can never be
retrofitted: the descriptor with its version and reserved feature bits, both keys, and
rotation. Pause may slip if its feature bit is reserved.

It also permanently enlarges the consensus surface, which is the scarcest thing the project
has. That remains the strongest argument for keeping the reserved bits unimplemented until
someone actually needs them.
