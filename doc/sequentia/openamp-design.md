# OpenAMP: issuer-governed assets for Sequentia (design)

STATUS: 2026-07-08. Design approved by Andreas; decided: name stays OpenAMP, clawback default ON. M0 in progress. Supersedes the externally drafted "OpenAMP Architecture Specification" memo (Gemini, 2026-07): that memo's problem statement is adopted, its consensus-level solution is rejected (see §10).

Grounding docs: `02-open-fee-market.md` (any-asset fees, producer whitelists), `01-architecture.md`, `simplicity-dex-covenant-offers-design.md` (introspection and Simplicity background), `seqdex-orderbook-design.md` (registered-user trading, later phase).

Name: **OpenAMP** (decided 2026-07-08; the collision with the OpenAMP embedded-systems standard is accepted). Daemon: `openampd`. Repo: `GracedEternalKingCabbageMan/openamp`.

---

## 0. One paragraph

OpenAMP is an open-source, self-hostable equivalent of Blockstream's AMP2 for Sequentia: issuers manage regulated assets (securities, funds, bonds) whose every transfer requires co-signature from an issuer-controlled policy server, with KYC whitelisting, categories, velocity limits, vesting, freezing, distributions, and auditor reports. Restricted assets live only in 2-of-2 taproot outputs (user key + policy key: the "enclave"). The asset ID cryptographically commits to the policy key through the issuance contract hash, so the asset-to-policy binding is verifiable by anyone with no consensus change and no node state. The covenant escape loophole (restricted assets leaking through the multi-asset fee pool into an unrestricted coinbase output) is closed structurally: a restricted asset never appears in a fee output, enforced at four independent layers, the strongest being a per-UTXO Tapscript introspection covenant that makes any out-of-enclave output, including a fee output, invalid by consensus. The fee output of a restricted-asset transfer is always in an ordinary fee asset per Sequentia's open fee market; users can nonetheless pay in the restricted asset itself through atomic in-transaction fee conversion with the issuer (or any registered fee broker), who takes a fee-equivalent slice of the asset into their own enclave account and attaches the real fee.

---

## 1. Parity target: what AMP2 does

Blockstream AMP manages two asset classes on Liquid, enforced by a co-signing server and a registry of **registered users** (identified by GAID, the Green Account ID):

- **Issuer-tracked assets**: no transfer restriction; the platform tracks ownership for reporting.
- **Transfer-restricted assets**: held in 2-of-2 multisig accounts; the AMP cosigner refuses to sign any transfer to an unregistered or unauthorized recipient. Supports **categories** (group-based restrictions), an **issuer authorization endpoint** (issuer's own API approves each transfer), **assignments** and **distributions** (allocating and paying out to registered users), **vesting**, **freezing/blacklisting** (refusal to co-sign), **reissuance and burn**, and **reports**: balances, ownership at a given block height, proof of transfer and proof of balance for auditors and regulators.

AMP2 (public release targeted Q3 2026) keeps this model and re-architects delivery: issuers no longer run node infrastructure; transaction construction, policy enforcement, investor onboarding, and HSM-backed co-signing are consolidated into one hosted flow. Public technical documentation on AMP2 internals is thin; this design targets feature parity with the documented AMP feature set plus AMP2's integration model (PSET in, PSET out; no issuer node), delivered as open source.

Where OpenAMP deliberately goes beyond AMP2:

1. **Self-hostable and open-source**: any issuer runs their own policy server; no platform dependency.
2. **Cryptographic asset-to-policy binding**: the asset ID itself commits to the policy key (§4). In AMP the binding between an asset and its cosigner is a platform database row.
3. **Covenant hardening tier**: with AMP, a compromised or coerced cosigner can sign assets out of the compliance enclave. OpenAMP Tier B makes that impossible below the enclave boundary by consensus (§6).
4. **Transparency log**: every policy decision is written to an append-only hash chain whose root is periodically committed on-chain, so issuers and regulators can audit the server after the fact (§8).
5. **Fee conversion and sponsorship**: because Sequentia fees can be paid in any accepted asset by any party, users can pay transaction costs in the restricted asset itself, with the issuer atomically bridging to the fee market inside the same transaction, or the issuer can simply sponsor the fee (§7). End users never need a separate gas asset. AMP2 on Liquid cannot offer either: L-BTC is mandatory there.

---

## 2. Chain facts this design stands on

All verified in the current tree (branch `claude/sequentia-bitcoin-sidechain-w6xady`):

- **Fee outputs** are Elements explicit fee outputs: empty `scriptPubKey`, explicit value, explicit asset (`CTxOut::IsFee()`, `src/primitives/transaction.h:328`). A fee output can carry any explicit asset. Mempool acceptance requires exactly one fee asset per transaction (`src/validation.cpp:951-960`, `bad-txns-multiple-fee-assets`).
- **The per-node fee-asset whitelist is default-deny**: the `ExchangeRateMap` (`src/exchangerates.h:25`) converts fee value; any asset not explicitly listed converts to 0, meaning the transaction is non-paying and never mined by that producer (`src/exchangerates.cpp:24-37`). Producers add assets via `setfeeexchangerates`. A restricted asset is simply never listed.
- **Coinbase outputs are consensus-pinned to the leader**: from `pos_coinbase_leader_height`, every value-bearing coinbase output must equal `PosLeaderFeeScript(leader)` or the block is rejected (`bad-coinbase-not-leader`, `src/validation.cpp:2548-2559`). A "compliant 2-of-2 coinbase output" is therefore consensus-invalid on Sequentia. Coinbase may not contain fee outputs (`bad-cb-fee`, `src/consensus/tx_check.cpp:60-64`). There is no subsidy and no coinbase issuance (`genesis_subsidy = 0`, `src/chainparams.cpp:397/594`).
- **Tapscript introspection is live**: `OP_INSPECTOUTPUTASSET` (pushes 32-byte asset plus a 1-byte prefix distinguishing explicit `0x01` from confidential), `OP_INSPECTOUTPUTSCRIPTPUBKEY` (pushes witness program plus version for witness outputs, else sha256 of the script and version -1; `src/script/interpreter.cpp:124-135`), `OP_INSPECTNUMOUTPUTS`, `OP_PUSHCURRENTINPUTINDEX`, `OP_INSPECTINPUTSCRIPTPUBKEY`, `OP_TWEAKVERIFY` (verifies taproot pay-to-contract: 33-byte tweaked key, 32-byte tweak, 32-byte internal key; `src/script/interpreter.cpp:2220`), `OP_ECMULSCALARVERIFY`, 64-bit arithmetic, `OP_CAT` (`src/script/interpreter.cpp:1048`), and streaming SHA256 (`OP_SHA256INITIALIZE/UPDATE/FINALIZE`, `:1761-1804`). All available in taproot leaves, active now, no deployment needed.
- **Asset IDs commit to a contract hash**: entropy `E = H(H(prevout) || contract_hash)`, `asset = H(E || 0)`, reissuance token `= H(E || 1)` (`src/issuance.cpp:23-64`). `issueasset` accepts `contract_hash` and `denomination` directly (`src/wallet/rpc/elements.cpp:1391-1417`).
- **Transparent by default**: `m_default_blinded_addresses = false` (`src/chainparams.cpp:504/760`); confidential outputs are opt-in and detectable in script via the asset/value prefix byte.
- **Simplicity is vendored but NEVER_ACTIVE on all Sequentia chains** (`src/chainparams.cpp:382-386/582-585`). Nothing in this design depends on it; §6 notes it as an optional future tier.
- **PSET (Elements PSBT v2) RPCs exist end to end**: `createpsbt`, `walletcreatefundedpsbt`, `walletprocesspsbt`, `combinepsbt`, `finalizepsbt`, `analyzepsbt`, `decodepsbt`.

---

## 3. Threat model, and the covenant escape loophole re-examined

The externally drafted memo frames the core threat: on a chain where fees can be paid in any asset, a restricted asset used as a fee is swept by the block producer into an unrestricted coinbase output and "escapes" its compliance enclave. Its proposed fix is consensus-level "asset-bound covenants": a genesis-registered script template that every output of the asset, including coinbase outputs, must match, with KYC'd block producers using compliant 2-of-2 coinbase addresses.

The loophole is real only if a restricted asset can reach a fee output at all. The correct fix is to make that structurally impossible, not to teach the coinbase about compliance:

**Rule 1 (the load-bearing rule): a restricted asset never appears in a fee output.** The fee output of any transaction moving a restricted asset is in some ordinary accepted asset, so the asset never enters the floating fee pool and never reaches a coinbase. This is a protocol-layer statement, not an economic one: the sender can still denominate the cost in the restricted asset by atomically converting with the issuer or a registered fee broker inside the same transaction (§7), and from the user's perspective the fee is then paid in the asset. What can never happen is the asset itself being the thing a block producer sweeps. That framing is Sequentia's open fee market working as designed: block producers choose which assets they accept for fees, no producer can lawfully accept an asset it is not KYC'd to receive, so producers never whitelist restricted assets, and registered intermediaries fill the gap by accepting them for fee payment instead.

Rule 1 is enforced at four independent layers, ordered weakest to strongest:

| Layer | Mechanism | Survives... |
|---|---|---|
| L1 wallet | wallet never selects a restricted asset as the fee asset | honest software |
| L2 policy server | server refuses to co-sign any transaction containing a fee output (or any non-enclave output) in the restricted asset | malicious user |
| L3 producer whitelist | fee-asset whitelist is default-deny (`ExchangeRateMap`); a fee in an unlisted asset is non-paying, so the transaction is never mined; and `bad-coinbase-not-leader` means the sweep output could not be covenant-shaped anyway | malicious user and lazy server |
| L4 covenant (Tier B) | the UTXO's own Tapscript covenant makes any transaction with an out-of-enclave output in the asset, including a fee output (empty scriptPubKey can never match the enclave template), invalid by consensus on every node | malicious user, compromised or coerced policy server, and colluding block producer |

With L2 alone (Tier A) the design already has AMP2-equivalent security: AMP has exactly one layer, the cosigner. L3 comes free from Sequentia's existing consensus and policy. L4 is the beyond-AMP2 hardening tier.

What the on-chain layers deliberately do NOT enforce: KYC identity, velocity limits, holder caps, vesting schedules, category rules. Those need off-chain data and stay in the policy server, exactly as in AMP2. The on-chain guarantee is precisely **containment**: units of the asset exist only inside enclave outputs (or provable burns), forever, no matter who misbehaves.

Residual trust (identical to AMP2, stated honestly):

- The **issuer** is trusted at issuance and reissuance time to mint into enclave outputs (the reissuance token is an issuer-held bearer instrument; consensus cannot stop an issuer reissuing to a bare address). Issuer tooling enforces it; the registry and any verifier can detect a violation instantly on the transparent chain, which voids the asset's compliance claims.
- The **policy server** is trusted for policy correctness (whom it lets transact). Under Tier B it is NOT trusted for containment.
- Server availability: if the policy key is lost or the server is gone, enclave funds are stuck. Mitigations in §5 (threshold policy key, optional continuity leaf).

---

## 4. Genesis anchor without consensus: the contract commitment

Elements already gives us a genesis-anchored, stateless registry: the asset ID is a hash commitment to the issuance contract. OpenAMP defines a canonical contract JSON (an extension of the `sequentia-registry` entry format):

```json
{
  "name": "Example Bond 2027",
  "ticker": "BONDX",
  "precision": 8,
  "issuer_pubkey": "<33-byte hex>",
  "version": 0,
  "openamp": {
    "version": 1,
    "type": "restricted",            // "restricted" | "tracked"
    "policy_pubkey": "<32-byte x-only hex>",
    "tier": "B",                     // "A" | "B": enclave script family
    "clawback": true,                // clawback leaf present in enclave tree
    "burn_allowed": true,            // covenant permits OP_RETURN burns
    "policy_endpoints": ["https://amp.example-issuer.com"],
    "terms_hash": "<sha256 of legal terms document>"
  }
}
```

`contract_hash = sha256(canonical-json)` is passed to `issueasset`/`rawissueasset`, so `asset_id` commits to `policy_pubkey`, the tier, and the clawback terms. Anyone holding the contract JSON (served by the registry and by the policy server) verifies the binding offline by recomputing the entropy chain. Wallets refuse to treat an asset as an OpenAMP asset unless this verification passes. This is the memo's "issuance commitment" done statelessly: no consensus lookup table, no state bloat, no protocol change; full nodes remain oblivious.

Holder terms are therefore fixed and disclosed at issuance: whether clawback exists, whether burns are allowed, which tier applies. An issuer cannot quietly change them later (a new policy key means a new asset ID).

---

## 5. The enclave: addresses and key structure

Every restricted-asset UTXO pays to a taproot output with **internal key = NUMS** (the BIP341 nothing-up-my-sleeve point, so there is no key-path spend) and a script tree of:

- **L_user** (per holder): `<K_user> OP_CHECKSIGVERIFY <K_policy> OP_CHECKSIG`, the co-signed transfer leaf. `K_user` is derived from the holder's registered account xpub at a per-address BIP32 path; `K_policy` is the asset-wide policy key from the contract.
- **L_cov** (Tier B only, asset-wide constant): the containment covenant, §6. It also requires both signatures, so it is the same 2-of-2 with structural checks prepended; L_user then exists as the cheap path for small transactions and as the upgrade seam (Tier A trees are just `{L_user}`).
- **L_claw** (default ON, opt-out via `"clawback": false`): `<K_issuer> OP_CHECKSIGVERIFY <K_policy> OP_CHECKSIG`. Issuer plus policy server, no user key: court-ordered seizure, lost-key recovery, estate execution. Its presence is committed in the contract hash, so holders accept it knowingly at purchase time. Without it, lost keys mean provable-burn-then-reissue (AMP0's recovery model).

Design decisions and their reasons:

- **Script-path 2-of-2, not MuSig2 key-path, for the MVP.** Plain `CHECKSIGVERIFY` chains are simple for every wallet (SWK, Ambra, hardware later) and make the co-signing protocol a one-round PSET exchange instead of a two-round nonce ceremony. A MuSig2 key-path variant is a later fee optimization and would anyway forfeit Tier B (a key-path spend bypasses all leaves), so Tier B assets must keep NUMS internal keys permanently.
- **The policy key is one x-only point on-chain, threshold behind the scenes.** Recommended deployment: FROST 2-of-3 among issuer-held signers (or issuer + two jurisdictionally separated operators), so no single machine compromise leaks `K_policy` and no single loss bricks the asset. MVP runs a single encrypted key with the HSM interface abstracted; FROST lands in M5.
- **Account IDs.** The GAID analog is the **AID**: `base58check(ripemd160(sha256(account-xpub)))` for the holder's registered restricted-asset account. Registration = KYC dossier + AID + xpub. Because the server knows every xpub, it derives every enclave address: full watch capability on a transparent chain, which is what makes ownership reports (§8) exact.
- **Confidentiality.** Tier A assets may opt into confidential outputs with mandatory blinding-key disclosure to the policy server (the AMP model). Tier B assets are transparent-only: the covenant must read asset tags, and Sequentia is transparent by default anyway. Stated in the contract.

Server-death continuity (open design dial, default OFF): an optional **L_cont** leaf, `<Δ> OP_CHECKSEQUENCEVERIFY OP_DROP <K_issuer> OP_CHECKSIG` (or user-key variant), lets funds move after a long inactivity window Δ if the policy server is permanently gone. Issuer-key variant preserves compliance but adds custody risk; user-key variant becomes a compliance time bomb. Default is no continuity leaf and reliance on the threshold policy key; per-asset choice, disclosed in the contract.

---

## 6. Tier B: the containment covenant

Goal: a consensus-enforced guarantee that every output carrying asset `A` in any transaction spending an enclave UTXO is itself an enclave output (or a permitted burn). This closes the fee loophole absolutely: a fee output has an empty scriptPubKey and can never match the enclave template.

The classic obstacle to recursive covenants is self-reference: the leaf script would need to contain its own hash. We avoid the quine with a **fixed covenant leaf + templated sibling + self-check against the current input**:

- `L_cov` is byte-identical for all holders of asset `A` (it embeds `K_policy` and constants, never `K_user`).
- The per-holder key lives only in the sibling `L_user`, whose script is a fixed template `T(K) = 0x20 || K || OP_CHECKSIGVERIFY || 0x20 || K_policy || OP_CHECKSIG` differing only in the 32-byte key.
- The tree root is `TapBranch(H(L_cov), H(T(K_user)))` (plus `L_claw` folded in at a fixed position when present).

Execution of `L_cov` (witness supplies: `h_cov` claimed hash of L_cov, own `K_user`, and per-output recipient keys `K_i` with parity and ordering bits):

1. **Self-check**: compute `root_self = TapBranch(h_cov, TapLeafHash(T(K_user)))`, then verify `TapTweak(NUMS, root_self)` equals this input's own scriptPubKey (`OP_PUSHCURRENTINPUTINDEX` + `OP_INSPECTINPUTSCRIPTPUBKEY`, compare via `OP_TWEAKVERIFY` after `OP_CAT`-ing the witness parity byte onto the 32-byte program). Because taproot spend verification already proved the executing leaf is committed by that scriptPubKey, and `T(K)` can never equal `L_cov` for any `K` (different template bytes), collision resistance forces `h_cov = H(L_cov)`. The script now holds its own hash without containing it.
2. **Per-output checks** (unrolled to a fixed maximum, with `OP_INSPECTNUMOUTPUTS` bounding the count): for each output, `OP_INSPECTOUTPUTASSET`; require the explicit prefix `0x01` (a confidential output could smuggle asset `A`, so transactions spending Tier B enclave inputs are all-explicit). If the asset is not `A`: no constraint. If it is `A`: the output scriptPubKey must be witness v1 with program `Q_i` such that `TapTweak(NUMS, TapBranch(h_cov, TapLeafHash(T(K_i)))) = Q_i` (`OP_TWEAKVERIFY` again, `K_i` from witness), or, if `burn_allowed`, scriptPubKey exactly `OP_RETURN` (compare the sha256 pushed by `OP_INSPECTOUTPUTSCRIPTPUBKEY` for non-witness scripts against the constant `sha256(0x6a)`).
3. **Authorization**: `<K_policy> OP_CHECKSIGVERIFY <K_user> OP_CHECKSIG` (with `K_user` bound by step 1, taken from witness). Both signatures still required; the covenant adds structure, it does not replace the co-sign.

Tagged hashes (`TapLeaf`, `TapBranch`, `TapTweak`) are computed in-script with `OP_SHA256INITIALIZE/UPDATE/FINALIZE` seeded with the 64-byte precomputed tag-midstate prefixes as push constants, and `OP_CAT` for concatenation. `TapBranch` requires lexicographic ordering of the two child hashes; the witness supplies a swap bit and honest wallets sort. (A dishonest sender could mis-sort and produce a check-passing but unspendable destination; that is equivalent to burning funds they were sending, not an escape, and receiving wallets independently validate their own derived addresses before crediting a payment. Verified in M2 tests.)

Consequences worth stating explicitly:

- A fee output in asset `A` fails step 2 (empty scriptPubKey is neither the enclave template nor `OP_RETURN`), so **no transaction with a fee output in a Tier B asset can exist**, regardless of what any server, wallet, or producer does. Users still pay costs denominated in `A` via fee conversion (§7); what cannot exist is `A` in the fee pool.
- The block producer's sweep never sees asset `A`, so `bad-coinbase-not-leader` and the fee pool are simply never involved. The memo's "pre-cleared miner payouts" machinery is unnecessary, and on Sequentia impossible (§2).
- Inputs need no inspection: Elements consensus already enforces per-asset amount conservation for explicit transactions, so asset `A` appearing in outputs beyond inputs is impossible without issuance, and the issuance path is issuer-controlled (§3 residual trust).
- Multiple enclave inputs in one transaction each run the same output checks redundantly; harmless.
- Estimated leaf size is 2-4 kB with checks unrolled for up to 8 outputs (exact figure and the `OP_TWEAKVERIFY` validation-weight budget, 50 WU each against the witness-size budget, measured in M2). This is witness data on a chain with cheap blockspace, paid only on restricted-asset spends. If it proves tight, the per-asset constant `MAX_OUTPUTS` is a contract field.

**Tier C (optional future)**: the same containment predicate in Simplicity would be smaller and cleaner (real recursion via `disconnect`, no unrolling). Simplicity is vendored but NEVER_ACTIVE on Sequentia; if it is ever activated for SeqDEX covenants, OpenAMP gains a leaf variant, nothing else changes. Not on any critical path.

---

## 7. Fees for restricted-asset transfers

The fee output is always in an ordinary asset (Rule 1), and the sender has three ways to fund it:

- **Self-paid**: the sender's wallet adds fee-asset inputs (tSEQ, USDX, whatever the wallet's existing any-asset fee logic selects from what the user holds and producers accept) plus the fee output and fee-asset change. The issuer is not involved in the fee leg at all; the policy server's only role is the ordinary transfer co-sign. The restricted enclave inputs/outputs and the fee legs coexist in one transaction; Tier B requires the fee asset outputs to be explicit, which they are by default.
- **Fee conversion** (for senders who hold only the restricted asset): the sender pays in the restricted asset; the issuer bridges to the fee market atomically. The co-sign transaction contains: the sender's enclave inputs (asset `A`) and an issuer fee-asset input (say USDX); the recipient's enclave output and sender's enclave change in `A`; a **conversion output** paying `ceil(fee_value x rate)` atoms of `A` to the issuer's own enclave address; the fee output in USDX; and the issuer's USDX change. The conversion output is an ordinary enclave-to-enclave transfer to a registered holder, so the Tier B covenant permits it with no special case. Atomicity is inherent: the sender's signatures cover the conversion output and the issuer's signature covers the fee input, so neither leg can exist without the other, and the sender approves the quoted rate by signing. From the user's perspective the fee is paid in `A`; economically the issuer collected fee-equivalent value in `A` and is made whole by construction, so the flow is self-funding, not a subsidy. The conversion rate is published at the policy endpoint (price-server-fed or issuer-set, per contract), quoted to the wallet before signing, and displayed in `A`'s own units. The same value-preserving rounding as the fee market applies: a high-value asset pays few atoms, and that is correct.
- **Sponsored**: as fee conversion but with no conversion output; the issuer eats the (tiny) fee as a service cost, at their discretion.

Fee brokers: nothing in the conversion flow requires the converter to be the issuer. Any registered holder of `A` willing to receive the conversion output and attach the fee input can serve, and the policy server co-signs it like any transfer between registered users. A standing broker market keeps conversion rates honest; the issuer is simply the default, always-registered, always-willing counterparty.

Policy-engine treatment of conversion outputs: the receiving broker/issuer is a registered holder, so holder caps are unaffected (issuer accounts are cap-exempt anyway); whether conversion is permitted during a lock-in period, and whether it counts against velocity, are per-asset policy flags (default: permitted, counted).

One fee asset per transaction is a mempool rule (`bad-txns-multiple-fee-assets`), so converter and sender never both attach fees; the PSET protocol makes the fee leg explicit and single.

Producer guidance (docs + default config): never add an OpenAMP restricted asset to `setfeeexchangerates`. Defense in depth only; L3/L4 make violations unmineable/invalid respectively.

---

## 8. The policy server: `openampd`

A Go daemon, deployed like our other services (systemd on the box for the testnet instance; issuers self-host in production). Modules:

- **Chain follower**: tracks the Sequentia chain via `elementsd` RPC, anchor-aware: all stateful accounting keys on (block hash, Bitcoin-anchor depth) and rolls back on reorgs, per anchoring supremacy. Velocity windows and snapshot heights are measured in anchor-confirmed blocks; co-sign decisions on unconfirmed state are idempotent (double-requesting a co-sign for the same outpoints returns the same signature; two co-signs spending the same UTXO to different destinations is a plain double-spend race the chain resolves, and confirmed-state counters make it economically pointless).
- **Registry of record**: registered users (AID, xpub, KYC dossier reference, jurisdiction tags, categories), assets (contract JSON, tier, policy rules), assignments, vesting schedules, freeze lists (per user, per UTXO, per asset). SQLite first, Postgres interface-compatible.
- **Policy engine**: a declarative per-asset policy document evaluated on every co-sign request: recipient registered and in an allowed category; sender not frozen; UTXOs not frozen; velocity limits (amount per window per account); holder cap (count of accounts with nonzero balance after the transfer, exact thanks to full address derivation on a transparent chain); vesting (assigned-but-unvested amounts unspendable); lock-in periods; jurisdiction rules; optional forwarding to an **issuer authorization endpoint** (AMP parity: the issuer's own API gets the final yes/no).
- **Co-signing engine**: input is a PSET plus transfer metadata; it independently re-derives every claim from the PSET (never trusts metadata): all restricted inputs are enclave UTXOs of the claimed sender, all restricted outputs pay enclave scripts of registered recipients or permitted burns, no restricted fee output, fee asset not restricted, all outputs explicit for Tier B. Then policy engine, then sign `K_policy` for each restricted input (HSM interface; software key MVP, FROST 2-of-3 in M5), return the PSET.
- **Transparency log**: every decision (co-sign granted/refused, freeze, clawback, registration change) appends to a hash-chained log; the head is committed on-chain in an `OP_RETURN` output periodically (fee in any asset, trivially cheap). Regulators and issuers can replay and verify the server never rewrote history. This is the open-source answer to "why trust the policy server's records".
- **APIs** (REST, mTLS/token-authenticated): issuer surface: users, categories, assets, assignments, distributions, freezes, clawback ceremonies, reports (balances, ownership at height, proof-of-balance, proof-of-transfer, transfer history). Wallet surface: register account, get enclave receive address for AID, request co-sign (PSET), query per-asset policy and own limits/vesting.

Freeze semantics: user-freeze and UTXO-freeze are server-side refusals (Tier A and B); global asset freeze halts all co-signing. Under Tier B even a legally compelled emergency cannot move funds out of the enclave; it can only stop movement or, with `L_claw`, seize into issuer custody with the on-chain trail visible to everyone.

Clawback ceremony: a deliberately heavyweight flow: issuer request, dual authorization (issuer key is offline/cold), transparency-log entry with reason code before the transaction is signed, spend via `L_claw` into a designated issuer enclave address (never a bare address).

Distributions (dividends/coupons): ownership snapshot at an anchor-confirmed height (exact from derived addresses), then batch payments of any asset (typically USDX or tSEQ, not the restricted asset) to holders' ordinary addresses, with the report reconciling entitlement vs paid. Vesting-aware.

---

## 9. Wallet and ecosystem integration

- **SWK web wallet** (primary lane): a "managed assets" account per issuer server: register AID, receive (enclave addresses), send (PSET round-trip through `openampd`), display vesting/limits/freeze state from the wallet API. Dual-chain behavior is untouched: BTC and ordinary Sequentia assets work exactly as today, and the existing fee-asset selector covers the fee leg (restricted assets are excluded from fee selection by contract type). SEQ equal-standing rules apply: a restricted asset is one row among equals, no special hero treatment.
- **Ambra**: same flows over `ambra_core`; QR-carried PSETs stay under the co-sign round-trip anyway.
- **Explorer**: badge OpenAMP assets (from the registry contract), show tier, clawback flag, policy endpoints, and verify-the-binding status; enclave outputs render as "restricted (issuer-governed)".
- **SeqDEX** (later phase): restricted assets trade only between registered users; the maker's resting intent and the taker's fill both carry enclave legs, and settlement PSETs get the policy co-sign like any transfer. The policy engine sees a swap as a transfer with counterparty-delivery conditions, nothing new consensus-side. Cross-chain BTC legs are unaffected (the restricted leg is the Sequentia side).
- **Registry** (`sequentia-registry`): serves contract JSONs, marks `openamp` assets, links policy endpoints.

---

## 10. Rejected alternatives

- **Consensus-level asset-bound covenants (the memo's proposal)**: rejected on five grounds. (1) It breaks stateless UTXO validation: every node must maintain an indexed issuance-commitment database consulted during block validation, the exact state bloat the memo itself concedes. (2) It is consensus-incompatible with Sequentia as built: `bad-coinbase-not-leader` pins every value-bearing coinbase output to the leader's fee script, so "compliant 2-of-2 coinbase outputs" would require weakening a rule we rely on. (3) KYC'd block producers fragment the open fee market and graft an identity requirement onto block production, against the no-privileged-asset, open-producer design. (4) It solves a problem that Rule 1 dissolves: once the asset can never be a fee, the coinbase never touches it. (5) Consensus surface is our scarcest resource; a compliance feature must not add reorg-relevant code paths.
- **Mempool-policy-only enforcement** (reject restricted-asset fee outputs in `AcceptToMemoryPool`): not consensus; a colluding producer defeats it; and it adds a protocol-level notion of "restricted asset" that L3 already provides asset-agnostically.
- **Consensus freeze/blacklist flags**: violates full-node sovereignty and asset neutrality; freezing is an issuer-layer concern and stays there.
- **Pure-covenant compliance (no server)**: KYC, velocity, holder caps, and vesting are inherently off-chain facts; encoding approximations on-chain bloats witnesses and still needs an issuer registry. The server stays; the covenant only guarantees what it can guarantee perfectly: containment.
- **Omnibus enclave (single asset-wide address, server-side ledger)**: makes the covenant trivial but the server custodial; rejected because user keys must be required for every move (non-custodial 2-of-2 is the AMP property worth keeping).
- **MuSig2 key-path enclave**: nicer fees, but a key-path spend bypasses every leaf, so Tier B containment dies. Possible later for Tier A assets only.

---

## 11. What is enforced where (summary table)

| Guarantee | Enforced by | Survives compromised policy server |
|---|---|---|
| containment: asset only in enclave outputs or burns | Tier B covenant (consensus, per UTXO); Tier A: server refusal | Tier B: yes / Tier A: no |
| never in a fee output (fee pool and coinbase unreachable; fee conversion covers the UX) | covenant (B) + server (A) + default-deny producer whitelist + wallet | Tier B: yes |
| transfers only between registered/authorized users | policy server co-sign | no (same as AMP2) |
| velocity, holder caps, vesting, categories, jurisdiction | policy server rules engine | no (same as AMP2) |
| freeze | server refusal (and only that: no consensus freeze) | no |
| clawback only per disclosed terms | L_claw presence committed in asset ID via contract hash | yes (terms cannot be retrofitted) |
| asset-to-policy-key binding | contract hash in issuance entropy | yes (verifiable by anyone) |
| decision-history integrity | transparency log anchored on-chain | detectable after the fact |

---

## 12. Milestones

- **M0, enclave proof (regtest)**: canonical contract JSON + hashing spec; issue a restricted asset with `contract_hash` into Tier A enclave addresses; transfer with a stub co-signer (test harness holding `K_policy`); freeze-by-refusal demo; verify the asset-to-policy binding end to end. Proves the taproot 2-of-2 script path on Sequentia.
- **M1, `openampd` v0**: Go daemon with chain follower (anchor/reorg-aware), user registration (AID/xpub), per-asset policy docs (registration + categories + freeze), PSET validation + co-sign with software HSM, fee-conversion and sponsorship flows, issuer and wallet REST APIs; regtest e2e including refusals, fee conversion, and reorg rollback of counters.
- **M2, Tier B covenant**: implement `L_cov`, byte-exact leaf spec frozen as v1; functional tests: enclave-to-enclave pass; out-of-enclave, fee-output, confidential-output, and mis-sorted-branch cases fail; measure leaf size, witness weight, `OP_TWEAKVERIFY` budget; fix `MAX_OUTPUTS`.
- **M3, issuer operations**: assignments, distributions with snapshot reports, vesting, velocity, holder caps, issuer authorization endpoint, ownership/proof-of-balance/proof-of-transfer reports, transparency log with on-chain anchoring, clawback ceremony.
- **M4, ecosystem**: SWK managed-assets account, explorer badges + binding verification, registry integration, deploy `openampd` to the box, issue a demo restricted asset (BONDX) on the public testnet, public docs.
- **M5, hardening + trading**: FROST 2-of-3 policy key, Ambra integration, SeqDEX registered-user trading, Tier A confidential-outputs option with blinding-key disclosure.

Repo: public repo `openamp` under GracedEternalKingCabbageMan, Go daemon + specs; chain-side changes: none (that is the point). The M0 regtest proof lives in SequentiaByClaude as a functional test (it needs the node test framework's taproot tooling); everything from M1 on lives in `openamp`.

---

## 13. Open questions (for Andreas)

Resolved 2026-07-08: name stays **OpenAMP**; **clawback default ON** (opt-out disclosed in the contract). Still open:

1. **Continuity leaf**: default OFF (rely on FROST policy key), confirm.
2. **Testnet hosting**: one shared `openampd` on the box acting as policy server for demo issuers, with self-hosting documented?
3. **Tier default**: issue new assets as Tier B from the start (M2 lands before any real issuer), or Tier A first with a migration sweep later?
