# Supervised assets: implementation notes

Companion to `supervised-assets-proposal.pdf`. That document argues the case and states
the boundaries; this one is for whoever writes the code. It assumes you accept the
design and want the details, the insertion points, and the places it will bite.

Status: nothing here is implemented. A sketch written during analysis was reverted
unbuilt. File references are against `master` at the time of writing.

---

## 1. The rule in one paragraph

An asset may be issued **supervised**, which commits an issuer freeze key into its asset
id and cannot be added or removed afterwards. The issuer publishes **freeze records**
naming a scriptPubKey. Consensus rejects a transaction that *spends* a supervised-asset
output when the output's script is frozen **and** the spend reveals a single-owner
script. Creating outputs that pay a frozen script stays legal. Supervised assets may
never appear in a blinded output. The rule is height-gated.

Everything below is a consequence of one of those clauses.

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

### 3.1 Asset-side commitment

Freezability must be committed at issuance and verifiable later without trusting a claim.
Two candidate encodings; **decision needed** (see §8, Q2).

- **(a) In the contract.** Add a field to the canonical contract JSON that
  `contract_hash` covers. Smallest change; consensus never sees the contract, so the
  freeze key must be carried in the freeze record and validated against the asset another
  way. Weak on its own.
- **(b) Distinct derivation.** Derive a supervised asset's id under a distinct constant,
  the way `CalculateReissuanceToken` (`src/issuance.cpp:50-64`) already distinguishes
  blinded from explicit issuance. Then a freeze record can carry the entropy and the
  freeze pubkey, and consensus re-derives the id and checks it matches. Forging that is a
  second-preimage on the fast-merkle node, so authority is cryptographic rather than
  asserted.

(b) is preferred. Note the failure mode of (b) alone, found in analysis: entropy is
public (it is in the issuance transaction and the node prints it), so the record must
*also* carry a signature by the committed freeze key, or anyone could publish freeze
records for anyone's supervised asset.

### 3.2 Freeze record

A record is an output. Suggested shape, mirroring `BuildDelegationScript`
(`src/pos.cpp:696`):

```
<FREEZE_MARKER> OP_DROP <asset_id:32> OP_DROP <target_spk_hash:32> OP_DROP
<issuer_freeze_pubkey:33> OP_CHECKSIG
```

Creating the record freezes; spending it unfreezes. The `OP_CHECKSIG` gates the *spend*
(the unfreeze), so admission to the registry must be gated separately by verifying a
signature over the record contents by the asset's committed freeze key. Do not repeat the
delegation precedent's shape here: in that design creating a record requires no signature
at all, which for freezes would let anyone freeze anyone.

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

## 8. Open questions

1. **Single-owner classification.** Is P2WPKH + P2TR key-path the right set? Include bare
   P2PK or 1-of-1 multisig? Where does the classifier live so mempool and block validation
   cannot diverge?
2. **Where the freeze key is committed** (§3.1): contract field or distinct derivation
   constant. Affects whether the asset class is visible from the id alone.
3. **Mempool eviction design** (§5). The piece most likely to go wrong and the one I most
   want reviewed.
4. **Fees.** May a supervised asset pay fees? It can technically; a frozen holder's fee
   output interacts with producer fee handling and deserves a deliberate answer.
5. **Pause.** Is an asset-wide pause its own record type, or simply a freeze whose target
   is the asset rather than a script?

---

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

A release, not a patch: issuance derivation and validation, an output-type rule, the
registry with apply/revert/rebuild, enforcement, mempool eviction, RPCs
(`freezeasset`, `listassetfreezes`, `getassetsupervision`), wallet change handling in two
codebases, functional tests as above, and display work in the wallet, explorer, registry
and DEX so the property is visible before anyone acquires the asset.

It also permanently enlarges the consensus surface, which is the scarcest thing the
project has. That is the strongest argument for settling §8 before writing code.
