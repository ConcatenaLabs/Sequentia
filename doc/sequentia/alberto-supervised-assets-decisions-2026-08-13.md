# Supervised assets — decisions on the open points

*Alberto, August 2026. For Andreas and Saba.*

I went through the proposal and the implementation notes in depth. I agree with the core design and none of it is revisited here: spend-only, single-owner-only enforcement, explicit-only via induction from issuance, height-gated activation, registry derived from unspent records with the StakeRegistry discipline, the whole §5 mempool-eviction machinery, wallets fixed before or together with consensus. Below are only the points where I'm deciding something the documents left open, or adding something to them — with my reasons, kept short. The last point is the structural one and the one I expect us to discuss most.

## 1. Freeze signatures: Schnorr/BIP340 with an x-only key, not ECDSA

Issuers will want the freeze authority split across several officers (compliance, legal, cold storage). With Schnorr this needs nothing from us: FROST (t-of-n) and MuSig2 (n-of-n) produce one ordinary signature under one ordinary pubkey — the full key never exists anywhere, and the protocol doesn't even know a quorum is behind it. Threshold ECDSA exists but is fragile and poorly tooled. So: one word in the spec (BIP340), zero protocol complexity, and issuer multisig comes for free. The codebase already has taproot/Schnorr.

Related, confirming what your notes already flag: record admission must require a signature by the committed key — do not reuse the delegation-script precedent where creating a record needs no signature.

## 2. Key management: two keys, not one — operational + recovery

Commit **two** keys at issuance: an **operational freeze key** for day-to-day freezes/unfreezes, and a **recovery key** (deep cold, itself FROST-shared, geographically split) whose *only* power is signing **rotation records** — rotating the operational key, or itself.

Why not the single immutable key of the current design: any compromise ends in forced migration of a live asset — for a large stablecoin, the nightmare scenario. Why not simple self-signed rotation: under compromise it's worse than nothing — attacker and issuer both hold the key, whoever rotates first wins *permanently*, and the attacker's best first move is to silently rotate to his own key and seize the supervision authority for good. Separating use from rotation removes the race by construction: an operational-key thief can grief (visibly, on-chain) but cannot rotate; the issuer rotates via recovery and the incident is closed. Note that ordinary signer turnover needs none of this — FROST proactive resharing re-randomizes shares off-chain under the same pubkey. The on-chain mechanism exists purely for the key-level incident.

Two things to engineer deliberately:

- **The unfreeze trap.** After a rotation, old freeze records must not remain spendable by the old key: otherwise a stolen old key can lift every existing freeze — exactly what rotation was meant to prevent. Unfreeze validity must be checked by consensus against the *current* operational key in the registry, not only by the record's script.
- **This cannot be added later.** The keys are committed in the asset id. Assets issued before rotation exists stay non-rotatable forever; no future fork can help them. It has to be in the first release.

Bounding the stakes, for calibration: a freeze-key thief can never steal funds (the key spends nothing) and can never mint (reissuance tokens are a separate authority held as UTXOs). Worst case is griefing plus unfreezing, and the ultimate remedy — migration — always exists. What the recovery key buys is precisely *never being forced into that migration*.

## 3. Supervised issuance must carry reissuance tokens (tokenamount > 0, explicit)

The GENIUS Act — and the FinCEN/OFAC proposed rule of April 2026 — requires issuers to hold the technical capability to "seize, freeze, burn, or prevent the transfer". Freeze and prevent-transfer are covered by your design. Seize and burn should **not** become consensus machinery (letting the issuer spend frozen outputs breaks the script model and creates the chain's biggest honeypot); they fall out economically instead: a permanent freeze is a de-facto **burn**; permanent freeze plus reissuing the same amount to the court's address is a de-facto **seize**. Tether already runs exactly this cycle at scale (~$1.1B reissued to law enforcement and victims), so the legal and operational precedent exists.

The single enabling condition is that the asset is reissuable — hence a rule at issuance: supervised ⇒ reissuance tokens issued (and explicit, consistent with the explicit-only rule). It's a one-line check that closes half the GENIUS requirement with zero enforcement code, and it prevents a "supervised but unable to comply" asset class from existing. I'd put it in consensus; if you'd rather keep it registry/wallet policy, that's negotiable — the non-negotiable part is documenting freeze+reissue as *the* answer to seize/burn, so nobody ever tries to implement issuer-spend.

## 4. Fees: supervised assets may pay fees (your §8 Q4)

Deciding your open question in the permissive direction. A frozen output cannot pay fees at all — the whole transaction is invalid — so there is no leak from frozen coins. The launder-through-producer route is weak: fee outputs are explicit, coinbase attribution is public, so chain analysis links the disappeared output to the coinbase trivially — and it requires being, or buying, a producer. The real limit is granularity: a coinbase aggregates many payers' fees and partial freezing of an output is impossible in UTXO. Accepted: the coinbase is freezable whole, like any output, and a producer that accepts supervised-asset fees knowingly accepts that exposure — which the negotiated fee market prices anyway, since producers choose which fee assets they accept.

The alternative (ban supervised assets from fee outputs) would strand holders who own only the stablecoin — our core use case. And it's a pure tightening: if real issuers ever demand it, it can be added later with the usual height-gate. Low regret in deferring.

## 5. A freeze takes effect from the block *after* the one containing the record

Your notes leave the same-block record+spend case undefined; the outcome would hinge on registry-update ordering inside ConnectBlock — exactly the kind of place where mempool acceptance and block validation can diverge, and a divergence there is a chain split. Next-block effectiveness removes all intra-block ambiguity, and gives the eviction machinery a full block to clean the mempools. The cost — the frozen party gains one block, sixty seconds — is neutralized by the next point.

## 6. Plan a direct issuer→producer submission channel alongside the consensus work

As designed, a freeze is systematically front-runnable: the record sits in the public mempool before confirming, and a mempool watcher moves the funds to a fresh script before it bites — reducing every freeze to a chase. The same race exists on Ethereum and issuers deal with it via private relays; we should plan the equivalent (authenticated submission to producers, outside the public mempool). It's infrastructure, not consensus — but if it isn't scheduled together with the consensus work, the compliance promise evaporates in practice.

## 7. Pause (your §8 Q5): a freeze record with a wildcard target, not a new record type

Reuses the entire machinery — signed admission, registry, unfreeze-as-spend, reorg handling, eviction — at the cost of one sentinel target value. The same single-owner classifier must apply to the pause: CLTV deadlines are absolute and keep running while paused, so pausing shared scripts would shift contract races exactly the way freezing them would. If first-release scope has to shrink, this is the natural cut — provided its feature bit is reserved (point 8).

## 8. The structural point: commit a supervision *descriptor*, not a bare pubkey

Your option (b) — distinct derivation of the asset id — is right, for the reason you give: authority becomes cryptographic rather than asserted. My addition is about *what* gets committed. Derive the id over a small descriptor — {version, feature bits, operational key, recovery key} — rather than over a key alone.

The reason is that an asset id is forever. Anything not committed at issuance can never be retrofitted to already-issued assets, and any future supervision variant would otherwise need a new derivation constant — a new, incompatible asset class, invisible to every wallet and freeze record parser written before it. Version-plus-reserved-bits costs zero enforcement code today and keeps three doors open:

- **Total-supervision mode** — the door I most expect us to want. An issuer opt-in at issuance making *all* scripts freezable, with the trapped-innocent-funds problem your single-owner rule rightly protects against handled one level up: by issuer reissuance (the Tether cycle again — coherent, because holders of a supervised asset already trust the issuer completely), or by delayed effectiveness on shared scripts, which forces channel closure and lands everyone on individually-freezable single-owner settlement outputs. Different issuers genuinely want different regimes — Circle freezes only on court order, Tether proactively at 30× the volume — and the bit lets the market see and price the difference at issuance. To be clear about scope: I am *not* proposing to build any of this now. Only to reserve the bit, so that standard-vs-total is a flag and not a future asset-class migration.
- **Pause-allowed** (point 7).
- **Whitelist mode** (securities-style allowlists) — explicitly out of scope, reserved only, so the decision is documented rather than accidental.

I know consensus surface is the scarcest resource we have — this point is designed to *spend none of it now* while keeping us from buying the same discussions back later at the price of a migration.

---

**One standing reminder for everything above**: every consensus or wallet change in this project has a second half in **sequentia-qt**. Issuance page (supervised flag, reissuance-token handling), coin display (frozen status must be visible before anyone tries to spend), send flows (unblinded change for supervised assets), and the new RPCs surfaced in the GUI. Let's plan the GUI delta as part of each change, not as a follow-up — the proposal's own scope estimate already lists display work in wallet, explorer, registry and DEX; the Qt wallet belongs on that list with them.
