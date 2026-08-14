# Supervised assets

For issuers and node operators. What a supervised asset is, what it can and cannot do,
and how to operate one. The design reasoning is in
[supervised-assets-implementation.md](supervised-assets-implementation.md) and the
decisions behind it in
[alberto-supervised-assets-decisions-2026-08-13.md](alberto-supervised-assets-decisions-2026-08-13.md);
this document assumes neither.

Shipped in 24.1.0. Active from height 1 on mainnet and on custom/regtest chains; on the
live testnet from height 94,600.

## What it is

A supervised asset is one whose issuer can freeze holdings by consensus rule. It exists
for assets that represent an off-chain claim the issuer is legally obliged to be able to
arrest — a fiat-backed stablecoin under the GENIUS Act being the case that motivated it.

The capability is declared when the asset is issued and is part of what makes the asset
that asset: the asset id is derived over the supervision declaration. Three consequences
follow, and they are not adjustable.

**An asset cannot gain or lose supervision.** An unsupervised asset can never become
supervised, and a supervised one can never be released. Migration means issuing a
different asset and persuading every holder to move.

**Nobody can lie about it.** A node never reads the claim that an asset is supervised; it
recomputes the asset id from the issuance and compares. Forging supervision would mean
forging the id.

**Everything committed at issuance is permanent.** Both keys and the feature bits are in
the id. Choose them before issuing, because there is no later.

## What it is not

Supervision is the power to stop a transfer. It is not a power over funds:

- The issuer **cannot spend** a holder's output. Freezing prevents a spend; it never
  redirects one. There is no issuer key that can move somebody else's coins.
- The issuer **cannot mint** with the freeze keys. Minting is the reissuance token, a
  separate authority held as an ordinary UTXO. A stolen freeze key cannot print.
- Freezing **cannot take** value. *Seize* and *burn*, which regulators do ask for, are
  reached economically instead: a permanent freeze is a burn, and a permanent freeze plus
  reissuing the same amount to the address a court names is a seizure. This is why
  consensus requires a supervised asset to be reissuable (below), and it is the same
  freeze-burn-reissue cycle large issuers already run elsewhere.

Two limits on reach are deliberate, and both exist so that consensus never destroys value
belonging to someone other than the frozen party:

- Only **spending** is blocked. Paying *to* a frozen script stays legal — which is what
  makes pre-emptive freezing of a known destination useful, since funds arrive already
  trapped.
- Only **single-owner** spends are blocked: P2WPKH and P2TR key-path. A shared script — a
  2-of-2 channel funding, an HTLC, a covenant branch — is never frozen, because a freeze
  there would trap an innocent counterparty's whole balance, and because scripts that race
  against absolute deadlines would hand the frozen party the timeout while the honest
  party is barred from broadcasting. Use `isassetfrozen` to see whether a freeze could
  reach a given script at all (`freezable`).

## The two keys

Every supervised asset commits **two** x-only BIP340 keys, and they must differ.

The **operational key** signs freezes and unfreezes. It is used routinely, so it is
exposed by nature.

The **recovery key** signs one thing only: a rotation record replacing the operational key
or itself. It cannot freeze anything. Keep it deep cold and touch it a handful of times in
the asset's life.

The asymmetry is the point. A thief who takes the operational key can grief — freezing
holders, visibly, on-chain — but can never take the authority, because rotation is beyond
that key. The issuer rotates with the recovery key and the incident is over. Had rotation
been signed by the operational key itself, compromise would be a race the attacker wins by
rotating first, silently, which is worse than having no rotation at all.

Neither key is ever given to a node. The node says what to sign; the key signs wherever it
lives; the node assembles the result.

**Splitting the authority across several people needs nothing from the protocol.** Because
the keys are Schnorr, an issuer can hold either one as a FROST or MuSig2 threshold key: a
quorum signs, the full key never exists anywhere, and the chain sees one ordinary key and
one ordinary signature. Signer turnover is handled off-chain by resharing under the same
public key, so it needs no rotation record at all.

## Issuing

Precompute the asset id, so the terms can be checked before anything is broadcast:

```
sequentia-cli getsupervisedassetid <txid> <vout> <operationalkey> <recoverykey>
```

Then issue with `rawissueasset`, adding a `supervision` object:

```json
{
  "asset_amount": 1000, "asset_address": "<addr>",
  "token_amount": 1,    "token_address": "<addr>",
  "blind": false,
  "supervision": { "operationalkey": "<32-byte hex>", "recoverykey": "<32-byte hex>",
                   "pause": false }
}
```

Consensus refuses the issuance otherwise, because none of these can be repaired
afterwards:

- **Exactly one issuance** in the transaction. With two there is no honest answer to which
  one the declaration describes.
- **Reissuance tokens, non-zero and explicit.** Seize and burn are answered by
  freeze-plus-reissue rather than by giving an issuer a spending power, so an asset that
  cannot be reissued must not be able to call itself supervised. Note that any non-zero
  amount grants unlimited reissuance — the tokens are not consumed when used.
- **Nothing blinded**: every output explicit in both asset and value. Consensus cannot read
  a blinded output's asset, so a supervised asset inside one would be unfreezable. It has
  to be every output, because which asset a blinded output holds is exactly what cannot be
  determined.
- **Two distinct, valid x-only keys.** Equal keys would collapse the separation between
  using the authority and rotating it.

`getsupervisedassets` then shows the asset, the terms it was issued under, and its current
keys.

## Freezing, and lifting a freeze

Every record follows the same three steps: ask the node what to sign, sign offline, hand
the signature back.

```
# 1. the message to sign — also tells you WHICH key must sign it
sequentia-cli getsupervisionrecordhash freeze <asset> <targethash> null <txid> <vout>

# 2. sign the sighash with the operational key, wherever it lives

# 3. assemble, attach to a transaction, broadcast
sequentia-cli buildsupervisionrecord freeze <asset> <targethash> null <signature>
sequentia-cli addsupervisionrecordoutput <rawtx> <script> <asset>
```

The record is an output that stays in the UTXO set: creating it freezes, spending it
unfreezes. Its signature binds the first outpoint the transaction spends, so it is
single-use and cannot be replayed to re-freeze something the issuer deliberately released.

A freeze takes effect **from the block after** the one containing the record — never
within the same block — which removes any ambiguity about ordering and gives mempools a
full block to shed transactions the freeze has just invalidated.

Lifting a freeze is spending its record, authorised by `getsupervisionunfreezehash` plus
`setsupervisionunfreezesig`. The record's own script is deliberately not the spend
condition: a script is fixed when it is created, so it could only ever name the key that
was current then, and honouring it would leave every old freeze liftable by a rotated-away
key. Consensus checks the **current** operational key instead, which is the only thing
that can follow a rotation.

Two records may name the same target; the freeze lifts only when the last of them is
spent.

## Pause

If — and only if — the asset was issued with `"pause": true`, the issuer may freeze every
single-owner holding at once with a single record naming the wildcard target instead of a
script. Lifting it is spending that record, exactly like any other freeze.

It is a committed bit rather than a power every supervised asset carries, so that an issuer
who never wants it cannot be given it, and a holder can see before accepting the asset
whether it can be stopped wholesale. Shared scripts stay exempt from a pause exactly as
they are from a targeted freeze, so a long pause cannot swing the outcome of contracts
racing an absolute deadline.

## Rotating a key

```
sequentia-cli getsupervisionrecordhash rotateoperational <asset> <newkey> <oldkey> <txid> <vout>
```

The reply's `signwith` field says `recovery`, and only the recovery key's signature will be
admitted — the operational key cannot rotate anything, not even itself. `rotaterecovery`
replaces the recovery key, and is likewise signed by the recovery key.

Each rotation names the key it replaces, so the chain of rotations can be replayed from the
UTXO set in any order. `getsupervisedassets` reports both the keys the asset was issued
with, which never change, and the current ones.

## Publishing a record without being front-run

A freeze record sitting in the public mempool is a warning: a holder watching for it can
move the funds to a fresh script before it confirms, and since a freeze names a script
rather than a person, the issuer is then chasing. Submit records straight to a producer
instead:

```
sequentia-cli submitsupervisionrecord <hex>
```

The producer holds the record privately for inclusion; `getsupervisionsubmissions` shows
what it is holding. Producers accept these only from operators they have configured.

## For node operators

The freeze registry is a pure function of the UTXO set, rebuilt by scanning it at startup.
Each node logs what it found:

```
Supervision: registry loaded: 0 supervised asset(s)
```

`getsupervisedassets` is the quickest check that a node agrees with its peers — every node
derives the same registry from the same blocks, so a disagreement is a real one.

Because consensus *derives* an asset id rather than reading it, a node without this code
derives a different id for the same issuance. That is why the rule needs an activation
height and why the testnet cutover at 94,600 is a hard fork: a node still on 24.0.0 at that
height forks off. Below the activation height the rule is inert, so upgrading early is
strictly better than upgrading late.

Wallets must not blind change on a supervised asset. The node wallet and the DEX wallet
daemon were both fixed for this in 24.1.0; a third-party wallet that blinds change will
produce transactions consensus rejects.

## RPC reference

All of these live under the `supervision` help category.

| RPC | What it does |
| --- | --- |
| `getsupervisedassetid` | Derive the asset id an issuance would create, and the declaration it must carry |
| `getsupervisedassets` | Every supervised asset, its issued terms, and its current keys |
| `getassetfreezes` | The scripts currently frozen for an asset |
| `isassetfrozen` | Whether an address or script is frozen, and whether a freeze could reach it |
| `getsupervisionrecordhash` | The message to sign to have a record admitted, and which key must sign it |
| `buildsupervisionrecord` | Assemble a record's script from a signature produced offline |
| `addsupervisionrecordoutput` | Append a record output to a raw transaction |
| `getsupervisionunfreezehash` | The message authorising a freeze record to be spent |
| `setsupervisionunfreezesig` | Put that signature into the spending input |
| `decodesupervisionscript` | Read a declaration or record out of an output script |
| `submitsupervisionrecord` | Submit a record straight to a producer, bypassing the public mempool |
| `getsupervisionsubmissions` | The records a producer is holding privately |
