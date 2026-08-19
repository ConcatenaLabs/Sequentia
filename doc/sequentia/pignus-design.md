# Pignus: non-custodial collateralised lending on Sequentia

Status: the loan-vault covenant (section 2) is implemented in
`test/functional/pignus_covenant.py` and proven against the node by
`feature_pignus_vault.py`, which is in the test runner. The oracle, the loan
book and the watcher are implemented in the `pignus` repository. The Bitcoin
collateral construction (section 7) is specified here and is the next build.
Nothing is deployed on the live testnet yet.

*Pignus* is the Roman-law term for property pledged as security for a debt: the
creditor holds the pledge, the debt is owed separately, and redeeming the debt
redeems the pledge. That is exactly the shape of the thing, and it is a working
name -- renaming costs one identifier.

Companion documents: `openamp-design.md` and `opendamp-design.md` (the two
restricted-asset models this coexists with, section 8),
`seqdex/docs/simplicity-dex-covenant-offers-design.md` (the covenant-offer design this
borrows its output-map and self-replication techniques from), and
`03-bitcoin-anchoring.md` (why section 6.4 exists at all).

## 1. What is being claimed

A borrower locks collateral in a single taproot UTXO and receives principal in
USDX. The vault's spending rules are the loan agreement, compiled. Precisely:

- **No custodian.** The collateral is never held by a platform, a lender, a
  multisig federation or an issuer. It sits in a UTXO with a NUMS internal key,
  so there is no key path, and the only ways out are the four leaves.
- **No term can be restated.** Both asset ids, the total repayment, both payout
  scriptPubKeys, the oracle key, the price feed, the strike, the maturity and
  the liquidation bonus are compile-time constants inside the leaves, and the
  leaves are committed inside the taproot output key. Changing any of them
  changes the address, so the borrower verifies terms by reconstructing the
  address before funding it.
- **No permission to exit.** REPAY is permissionless, needs no signature, no
  oracle and no witness data whatsoever. A solvent borrower can always leave.
- **No discretionary seizure.** A liquidator cannot choose how much to take.
  The seizure is computed on chain from the attested price and the surplus is
  forced back to the borrower by the same script that lets the seizure happen.

What is *not* claimed: the price is an external fact and something has to
assert it. Section 6.1 states exactly how much power that gives the oracle, and
it is much less than "can take the money".

## 2. The vault covenant

A taproot output, internal key = the BIP341 NUMS point, tapleaf version 0xc4
(Elements tapscript), four leaves. Reference implementation:
`test/functional/pignus_covenant.py`.

Notation: `C` is the collateral asset, `D` the debt asset, `L` the collateral
amount in the vault, `debt` the total repayment (principal plus the whole
term's interest, fixed at origination -- these are term loans, so no interest
accrues on chain).

### 2.1 The output map

The covenant input at consensus index `k` credits the lender at output `2k` and
returns collateral to the borrower at output `2k+1`, with the index recomputed
per input from `OP_PUSHCURRENTINPUTINDEX`. This is the anti-aliasing device from
the SeqOB FILL leaf: because each input derives its own output pair, two vault
inputs can never both point at one shared credit output, so a single payment
cannot settle two loans. The functional test spends two vaults in one
transaction against one credit to prove it.

Every introspected asset and value prefix must be `0x01`. A blinded output is
rejected rather than guessed at -- a covenant cannot police a value it cannot
read. On transparent-by-default Sequentia this is the ordinary case, not a
privacy regression; a borrower who wants a confidential loan needs an
interactive tier instead, which is out of scope here.

### 2.2 REPAY -- permissionless, oracle-free, witness-free

Spendable iff output `2k` pays the lender at least `debt` of explicit asset `D`
at the pinned scriptPubKey, and output `2k+1` returns the whole of `L` in asset
`C` to the borrower's pinned scriptPubKey.

No signature and no witness data: the leaf reads everything it needs from the
transaction, so the witness is just `[leaf, control_block]`. Anyone may repay --
the borrower, a friend, a refinancing bot competing to buy the position. Because
both destinations are pinned, a third-party repayer can only make the borrower
better off, which is why letting anyone do it is safe.

This leaf is the reason an oracle outage is survivable. Every other exit needs
either an attestation or a long timeout; this one needs nothing.

### 2.3 LIQUIDATE -- permissionless, oracle-attested

The witness supplies `[sig, price, ts]`. The leaf reassembles the 48-byte
message `feed_id || ts || price` with `OP_CAT` and checks it against the pinned
oracle key with `OP_CHECKSIGFROMSTACKVERIFY`, then requires `ts >= not_before`
and `price < strike`.

It then computes, on chain,

    gross = ceil(debt * bonus_num / bonus_den)      -- folded at build time
    seize = ceil(gross * price_scale / price)       -- OP_ADD64/OP_SUB64/OP_DIV64

and requires output `2k` to pay the lender `debt` and output `2k+1` to return
`L - seize` to the borrower. The liquidator keeps `seize`, whose value at the
attested price is `gross` -- the debt plus the bonus that pays for the work --
overshooting by less than the value of one collateral atom because the ceiling
rounds the last atom the liquidator's way rather than letting a rounding loss
strand the position.

The signature is checked over exactly the numbers the script then computes with.
There is no second, unauthenticated copy of the price anywhere in the spend.

For an underwater vault `L - seize` is negative, the comparison against a zero
return passes, and the liquidator takes everything -- correct, since the
collateral no longer covers the debt.

### 2.4 DEFAULT -- permissionless, oracle-attested, after maturity

`<maturity> CLTV DROP` followed by the same attestation check and the same
seizure tail, minus the strike test. Once the term is up the debt is due at any
price, so anyone may call the loan; the covenant still forces the surplus home,
which is what makes it safe to let anyone do it. The functional test calls a
loan at a price *above* the strike, so the spend provably could not have been a
liquidation.

### 2.5 RECOVER -- the oracle-liveness backstop

`<recover_after> CLTV DROP <P_lender> CHECKSIG`. If the oracle is dead through
the whole grace window the lender sweeps the vault, surplus and all.

This is the one blunt leaf and it is deliberately last. It exists because a
dead oracle must not freeze collateral for ever, and it is acceptable because
the borrower has the entire term to take the oracle-free REPAY exit and only
reaches this leaf by ignoring it long after maturity. `recover_after` should sit
far enough past `maturity` that a transient oracle outage cannot reach it;
`maturity + 30 days` is the suggested default, and the borrower must check the
gap before funding, because it is the borrower who pays for a short one.

### 2.6 Sizes and limits

Measured leaf sizes: REPAY 192 bytes, LIQUIDATE 352, DEFAULT 345, RECOVER 39.
A REPAY spend is in the same class as a covenant CLOB fill (a few hundred
vbytes, see `sequentia-measured-tx-sizes-and-capacity`), which matters: doing
this in Simplicity instead would cost 7,459 vB and cap the platform at 12
operations per block. Tapscript introspection is the right tool and needs no
consensus change at all -- 0xc4 is gated only by always-active
`SCRIPT_VERIFY_TAPROOT`.

**64-bit bound.** `OP_ADD64` aborts on signed-64-bit overflow. The only large
value formed on chain is `gross * price_scale + price - 1`, so the builders
assert `gross * price_scale + bound < 2^63`, where `bound` is the strike for
LIQUIDATE and a caller-declared `max_price` for DEFAULT. A vault that could
abort mid-spend cannot be constructed -- the builder refuses at origination
rather than leaving a loan that cannot be liquidated.

The ceiling this puts on a single loan, at 8 decimal places and the default 5%
bonus:

| `price_scale` | max debt (units) | price precision |
|---|---|---|
| `1e5` (default) | ~878,000 | 1e-5 relative |
| `1e4` | ~8,784,000 | 1e-4 relative |
| `1e3` | ~87,841,000 | 1e-3 relative |

So the default caps one loan at roughly 878,000 USDX, and a larger loan trades
price precision for size by lowering `price_scale`. Neither knob is a protocol
limit: two loans are two vaults, and nothing stops a borrower opening several.
The numbers are worth stating because the first instinct -- "64 bits is plenty"
-- is wrong here: the seizure forms `gross * price_scale`, and at 8 decimals
that product eats 60 of the 63 available bits by itself.

## 3. Origination

One atomic transaction, signed by both parties, no escrow:

    inputs   borrower's collateral utxo(s), lender's principal utxo(s)
    outputs  0: the vault (L of C at the covenant address)
             1: principal (the loan amount of D) to the borrower
             +  changes, and the network fee output

Either party can walk away before signing and nobody is ever exposed to the
other. This is the same PSET co-signing flow SeqDEX already uses for same-chain
atomic swaps, so it is proven machinery rather than new machinery.

The borrower MUST, before signing, reconstruct the vault address from the terms
and check it equals output 0's scriptPubKey, and check the internal key is NUMS.
That single check is what makes everything in section 1 true; a wallet that
skips it has silently reintroduced a trusted party. `pignus-cli verify` and the
wallet integration both do it, and the daemon never asks a user to sign a vault
it did not reconstruct locally.

**Offers.** Lenders publish signed offers (asset pair, size, rate, term, strike,
oracle) to the book; borrowers take one. The book is pure discovery -- it holds
no funds and cannot alter terms, because the terms are inside the address the
borrower reconstructs. A *funded* resting offer, where the lender's principal
sits in its own covenant that anyone may take by locking a correctly-shaped
vault in the same transaction, is the natural next step and is sketched in
section 9; it needs the covenant to recompute a taproot address from a
witness-supplied borrower key (`OP_TWEAKVERIFY` plus the tagged hashes, the
technique OpenAMP's containment covenant proved), so it is deliberately not in
v1.

## 4. The oracle

The oracle signs

    msg = feed_id (32) || timestamp (8, LE) || price (8, LE)

with BIP340, and that is the entire protocol. `feed_id` is the hash of the
canonical market name, so an attestation for one market cannot be replayed
against another -- the functional test proves a genuine attestation for a
different feed is refused as an invalid signature, because the feed is inside
the signed message.

`price` is quoted as debt-asset atoms per collateral-asset atom, scaled by
`price_scale` (default `1e5`). Quoting per *atom* rather than per unit means the
covenant never has to know either asset's decimal precision.

Prices come from the existing price infrastructure (`contrib/price-server`,
which already feeds the any-asset fee market from a real quote source). Pignus
adds only a signer and a publication endpoint; it deliberately does not add a
second price pipeline.

The oracle is a public, replayable log: attestations are published for everyone,
not handed to a liquidator, so any watcher can verify a liquidation was
justified after the fact.

## 5. Freshness, and the one honest gap

The covenant can test that an attestation is NEWER than `not_before`. Nothing in
tapscript can test that it is RECENT: there is no way to read the current time
inside a script and compare it to a witness-supplied value. `OP_CHECKLOCKTIMEVERIFY`
sets a *lower* bound on the transaction's locktime, which is the wrong
direction.

So a liquidator who saved a signed attestation from a genuine dip may present it
later, after the price has recovered. What this is and is not:

- It is **not theft.** The position genuinely was liquidatable at that moment.
  The liquidator pays the full debt and the surplus is still forced back to the
  borrower at the *attested* price -- and since that price was lower than the
  current one, the borrower's surplus is computed less favourably than it would
  be today. The loss to the borrower is bounded by the difference between the
  dip price and the current price, on the seized portion only.
- It is a **timing advantage**, and the borrower's cure is the same either way:
  repay, or top up before the dip.

Two mitigations, in increasing cost:

1. **Epoch commitment (recommended, oracle-side).** The oracle signs
   `feed_id || epoch || price` where `epoch` advances every N minutes, and the
   vault bakes a `min_epoch`. This does not remove the window, it bounds how far
   back a saved attestation can reach only if `min_epoch` advances -- which
   requires re-covenanting. Useful mainly for short-term loans, where
   `not_before` can be set close to origination.
2. **Re-covenanting on top-up.** A borrower adding collateral moves to a fresh
   vault with a later `not_before`, which retires every attestation older than
   the top-up. This is the practical answer and it falls out of the design for
   free: a top-up is a REPAY-and-reopen, or an explicit new origination.

This gap is inherent to putting an external fact into a script, and every
oracle-driven on-chain lending design has some version of it. It is written down
here rather than left for someone to find.

## 6. Trust surface

### 6.1 What the oracle can and cannot do

Can: assert a price low enough to open LIQUIDATE. That is the whole of its
power.

Cannot: move funds; choose who receives anything (both payouts are pinned);
change how much is seized (it follows from the price it attested, and attesting
a lower price *shrinks* the liquidator's seizure per atom while enlarging it in
count -- the value seized is always `gross`); trigger a default before maturity
(CLTV); stop or delay a repayment (REPAY does not consult it); or keep a
borrower's surplus (the covenant forces it home).

The worst a fully malicious oracle achieves is liquidating solvent positions at
a fabricated low price, which costs the borrower the liquidation bonus and the
difference between the fabricated and the true price on the seized portion --
bad, publicly evident from the signed log, and bounded. It cannot steal the
collateral.

Hardening, in order: publish every attestation so fabrication is detectable;
run the signer behind a threshold key (the `PolicySigner`/FROST seam OpenAMP
already built solves exactly this shape of problem); and let the *vault* name a
2-of-3 oracle set, which needs three `CHECKSIGFROMSTACK` calls and a counter in
the leaf -- a straightforward extension of the current leaf, and the right
answer before real value is involved.

### 6.2 What the platform can do

Nothing. The book is discovery, the watcher is read-only, and the liquidator bot
is just the first participant to notice -- anyone can run one, and the covenant
does not care which one wins.

### 6.3 Liquidation races

Liquidation is a permissionless race, and the winner is whoever gets a valid
spend mined. This is an unpriced race, not a theft: every racer must pay the
lender in full and return the surplus, so the borrower and lender are
indifferent to who wins. The bonus is what prices the race.

### 6.4 Bitcoin anchoring

Sequentia reorgs when Bitcoin reorgs, in real time, and that outranks
everything. So a vault funding transaction can be undone by an anchor-driven
reorg, exactly as a covenant CLOB order can. The watcher therefore classifies a
vault whose funding has been reorged away as GHOST and drops it, the same way
`seqob-watcher` does, and a lender must not treat a loan as originated until its
funding is buried by the depth their risk appetite justifies. This is not a
Pignus-specific caveat; it is the chain's first principle, and any design that
pretended otherwise would be wrong.

## 7. Native Bitcoin as collateral

Sequentia uses **native** Bitcoin on the parent chain, not a pegged
representation, so BTC collateral means a real Bitcoin UTXO -- which has no
introspection, no `OP_CAT`, and no `OP_CHECKSIGFROMSTACK`. None of section 2
runs there. The construction is therefore cross-chain: collateral on Bitcoin,
debt on Sequentia, linked so that the two settle together.

### 7.1 The construction

Bitcoin side: a P2TR funding output with

- internal key = MuSig2(borrower, lender) -- the cooperative path, and the
  cheapest;
- leaf SEIZE: `<P_lender> CHECKSIGVERIFY <P_oracle> CHECKSIG` -- lender and
  oracle jointly, the liquidation path;
- leaf TIMEOUT: `<recover_after> CLTV DROP <P_lender> CHECKSIG` -- the backstop.

Repayment is linked to the BTC release by an adaptor signature, which makes the
solvent path trustless:

1. The lender picks a secret `t` and publishes `T = t·G` and `h = SHA256(t)`.
2. At origination the parties pre-sign, under the MuSig2 key path, the
   transaction that returns the BTC to the borrower. The lender contributes an
   **adaptor** signature under `T`, so the borrower holds a signature they
   cannot yet complete.
3. The borrower repays on Sequentia into a hashlocked output --
   `OP_SHA256 <h> OP_EQUALVERIFY <P_lender> CHECKSIG`, with a CLTV refund to the
   borrower if the lender stalls.
4. The lender claims the repayment, which publishes `t` on the Sequentia chain.
5. The borrower reads `t`, completes the adaptor signature, and takes the BTC
   back.

If the lender never claims, the borrower recovers the principal repayment after
the CLTV and the lender takes the BTC via TIMEOUT: the loan unwinds, nobody is
robbed, and the lender is strictly worse off for stalling, so they do not.

The claimant on the Bitcoin side must re-run the anchor-safety check on the
Sequentia reveal before acting on `t`, for the reason in section 6.4 -- a
covenant cannot introspect anchoring, so this stays a watcher discipline. This
is the same discipline the SeqDEX cross-chain leg already documents.

### 7.2 Why an oracle leaf and not a DLC

The obvious question, and the honest answer is that a DLC is the right tool for
a *settlement* and the wrong one for a *liquidation*.

A DLC pre-signs one contract execution transaction per discretised outcome, each
encrypted to an oracle attestation point, and settles at a **fixed maturity**.
That is a clean fit for "at date X, split the collateral according to the
price". A margin loan does not have that shape: it must be liquidatable the
moment the price crosses the strike, at any time during the term. Making a DLC
do that needs an oracle announcement per time step and a CET set per (time,
price) pair, which multiplies out instead of adding up -- and every one of those
CETs has to be pre-signed at origination, by both parties, before either knows
when the dip will come.

So Pignus uses the shape that fits each job:

- **Liquidation during the term** -- the 2-of-2 SEIZE leaf. The oracle must
  actively co-sign, which is a real and stated trust assumption, and the same
  one section 6.1 already bounds on the Sequentia side.
- **Settlement at maturity** -- a genuine DLC is a good fit and is the planned
  upgrade for the maturity path specifically, because there the outcome set is
  one-dimensional and the date is known. It would remove the oracle's per-loan
  involvement at maturity entirely.

Stating it plainly: BTC collateral is the one tier where the oracle is trusted
*interactively* rather than only for a number. That is the price of collateral
on a chain with no covenants, and it is why the unrestricted-asset tier is the
one to use where a choice exists.

## 8. Collateral tiers

| Tier | Assets | Enforcement | Trust |
|---|---|---|---|
| A | tSEQ, GOLD, SILVR, OILX, EURX, SBTC, and any unrestricted issued asset | The section-2 covenant | Oracle, for one number |
| B | Native BTC | Section 7, cross-chain | Oracle, interactively, for liquidation only |
| C | OpenAMP (`cosign`) assets | The issuer's policy server co-signs | Oracle **and** the issuer |
| D | OpenDAMP (`damp`) assets | Not yet | -- |

**Tier A** is the design. USDX is the debt asset throughout; every unrestricted
asset can be collateral, and the fee for any of it is payable in any accepted
asset, because no asset is privileged here any more than anywhere else on
Sequentia.

**Tier C** deserves a blunt statement. A restricted asset can only live in the
shapes its issuer permits -- for OpenAMP, a 2-of-2 enclave output with the
policy server. A loan vault holding one is therefore an enclave output, and
every exit needs the policy server to co-sign. That is not non-custodial in the
sense section 1 claims: the issuer can refuse a repayment. It is inherent to a
transfer-restricted asset, not a flaw in this design, and the integration is
worth having anyway because pledging a regulated asset is a real use case. The
shape is a *pledge policy* in `openampd`: the issuer registers a loan and
pre-authorises exactly the repay, liquidate and default transitions, so the
server's discretion is spent once at registration rather than at each exit.
The platform must label a Tier C loan as issuer-permissioned wherever a user
can see it; quietly presenting it beside a Tier A loan would be a lie.

**Tier D** is open. OpenDAMP's user covenant requires the holder's signature,
so a vault needs a key that is not simply the borrower's, and the natural route
is expressing the pledge as a DAMP policy predicate rather than wrapping the
asset. It is a genuine design problem and it is not solved here.

## 9. What is next

1. Oracle daemon and the published attestation log (built).
2. Go covenant port, golden-vectored against the proven Python (built).
3. Loan book, vault watcher, liquidator bot (built).
4. Wallet integration: reconstruct-and-verify before signing, in SWK and the
   web wallet.
5. Bitcoin collateral (section 7).
6. 2-of-3 oracle sets in the leaf (section 6.1).
7. Funded resting loan offers (section 3), which needs address reconstruction
   inside the covenant.
8. The maturity-path DLC (section 7.2), and Tier D.
