# OpenDAMP: network-enforced restricted assets on Sequentia

Status: M0 (protocol specification). Nothing in this document is deployed.
Companion to `openamp-design.md`, which specifies the co-signed enforcement
model this protocol coexists with. OpenDAMP is the Sequentia adaptation of
DAMP, the Decentralized Asset Management Protocol proposed for Liquid by
Riabov and Chystiakov (Blockstream, August 2026). The protocol moves policy
enforcement from an online co-signer into a Simplicity covenant: ordinary
transfers require no issuer signature, holders supply policy proofs, and the
chain verifies them.

OpenDAMP does not replace OpenAMP. Enforcement is an issuance-time election
committed into the asset contract (and therefore into the asset id), chosen
per asset:

- `cosign` (OpenAMP): richest rule set, opt-in confidentiality from outside
  observers, transfer availability depends on the policy service.
- `damp` (OpenDAMP): transfers never depend on the policy service, the
  operator never signs a transfer, asset tags are explicit on every output.

This is the two-of-three trade recorded in `openamp-design.md` section 9 made
into a product choice instead of a design argument.

## 1. Roles and assets

- The **issuer** creates the regulated asset A and controls an issuer key I
  (one x-only point; a FROST group key in production). The reissuance token
  for A is governed outside the protocol, but any later issuance of A must
  enter user covenants, or the issuer has created unregulated units.
- The issuer also creates a distinct **verifier asset** V. A valid verifier
  output carries a fixed amount q of V. Retaining V's reissuance authority
  lets the issuer create parallel verifier outputs as demand grows, which
  reduces contention.
- A **holder** controls an x-only key X and receives A in a user covenant
  C_U(X).

## 2. Covenant constructions

Both covenants are taproot outputs with the BIP341 NUMS internal key (no key
path) and Simplicity leaves at tapleaf version 0xbe. All tagged hashes use
the Elements taproot domains (`TapLeaf/elements`, `TapBranch/elements`,
`TapTweak/elements`). A Simplicity spend on Sequentia must present the
witness stack exactly as `[simplicity_witness, simplicity_program, CMR(32),
control_block]`; all dynamic data (keys, signatures, Merkle proofs) is
carried inside the Simplicity witness blob.

### 2.1 User covenant C_U(X)

Following the DAMP paper's Taproot state construction: let U be the Elements
TapLeaf hash of the fixed Simplicity user program, K the NUMS key, and

    D(X)   = H_TapData(X)
    R_U(X) = TapBranch(U, D(X))
    C_U(X) = P2TR(K, R_U(X))

One fixed program serves every holder; changing X changes only the address.
The program recomputes C_U(X) from the witness-supplied X (via the
`tapdata_init` / `tappath` jets and the taproot-construction jets) and
compares it with its own locking script, so a witness cannot substitute
another key. It then checks:

1. BIP340Verify(X, sig_all_hash(T), sigma_X) over the spending transaction.
2. The current input carries explicit asset A and is not input zero.
3. Input zero carries explicit verifier asset V in amount q.

These checks establish custody only. Policy is enforced by the verifier
covenant spent in the same transaction.

### 2.2 Verifier covenant C_V(pi)

Let P(pi) be the primary spending path instantiated with policy commitment
pi, and G(I) the issuer path requiring a BIP340 signature under I over
sig_all_hash. R_V(pi) commits to both; C_V(pi) = P2TR(K, R_V(pi)). The NUMS
key provides no key-path spend. On the primary path, P(pi) checks:

1. Its input is transaction input zero and carries q units of V.
2. Output zero returns q units of V to the same C_V(pi).
3. At least one input is a C_U(X) input (which independently requires its
   owner's signature, so only holders can advance the verifier output).
4. Every output exposes an explicit asset identifier, EXCEPT outputs for
   which `output_is_fee` holds (see 2.3). Every output carrying A pays
   C_U(Y) for its witness-supplied recipient key Y, computed with the
   taproot-construction jets.
5. All enabled policy predicates hold (section 3).

The output scan is a bounded unroll: the covenant commits to a maximum
output count N_max, checked against `num_outputs`. N_max is part of the
covenant's semantics and is sized against the Simplicity budget: execution
cost is bought with witness bytes (budget = witness size + 50, capped at
4,000,050) against a block weight of 200,000 on Sequentia mainnet and
400,000 on the testnet.

### 2.3 Sequentia delta: any-asset fees

Sequentia has no privileged fee asset. Fee outputs (empty scriptPubKey) may
carry any accepted asset, so the confinement scan must tolerate them, with
one exception that the covenant enforces absolutely: **an output that is a
fee output must not carry asset A.** This makes Rule 1 of the OpenAMP design
(a restricted asset never appears in a fee output) consensus-enforced for
OpenDAMP assets. Holders pay fees in an ordinary asset directly, or through
the fee-conversion pattern with any registered fee broker; the conversion
output to the broker is an ordinary C_U output of A and passes confinement.

### 2.4 Confidentiality position

Asset identifiers must be explicit on every output the scan reaches; there
is no unblind jet, so the covenant can only police what it can read. Value
commitments may remain confidential on outputs and inputs that no enabled
policy predicate needs to read; the transfer-limit predicate requires
explicit values on payments to other owners while sender change may stay
blinded. Issuers who need holdings hidden from outside observers should
elect `cosign` instead; this is the point of offering both.

## 3. Policy commitment and predicates

### 3.1 Commitment

    pi = H_"OpenDAMP/policy/v1"( version_u8 || asset_A || seq_u64 || rules_root )

`seq` increments on every policy update. `rules_root` is a Merkle root over
the enabled predicate commitments in fixed order (absent predicates commit
to the empty hash). The verifier program takes pi as its instantiation
parameter, so C_V(pi) commits to exactly one policy version.

### 3.2 Predicate: blacklist by outpoint (the freeze mechanism)

For an input outpoint (t, v), the policy key is k_out = SHA256(t || BE32(v)).
The issuer commits to a Merkle tree (sparse Merkle tree or Cartesian Merkle
tree; the concrete tree is fixed by the snapshot `tree` field) containing
blacklisted outpoints. Each regulated input supplies a non-membership proof
against the committed root. A listed UTXO cannot satisfy the verifier; an
unlisted UTXO spends without contacting the issuer.

This is how a court-order freeze works for an OpenDAMP asset: the issuer
publishes a policy update adding the outpoint, effective for all spends
after the verifier output moves to C_V(pi'). Consensus-level supervision
(supervised assets) is not used for OpenDAMP assets: a C_U output is not a
single-owner script, so a supervision freeze would not bind it, and the
blacklist provides the same power with per-outpoint granularity and a
public, signed, versioned history.

### 3.3 Predicate: whitelist by owner key

Membership proofs keyed by stable owner x-only keys, which permits repeated
transfers among approved holders. The verifier checks the owners of
regulated inputs and the recipients of regulated outputs. The list is
compiled from SeqPal ID eligibility stamps by the registrar: enforcement is
on-chain, vetting stays exactly where it is in the cosign model. Newly
verified investors become spendable-to when the issuer publishes the next
list version; publication cadence is a registrar service parameter.

### 3.4 Predicate: transfer limit

All regulated inputs must belong to the authenticated sender X; payments to
other owners must use explicit values and their sum must not exceed the
committed limit. Outputs returning to X are change and may stay blinded.

### 3.5 Predicate: height windows

Lockups and receive windows (the Reg S pattern) are enforced by requiring
the transaction's locktime to prove a height bound (`lock_time` /
`parse_lock` jets) combined with list membership keyed by owner class. A
covenant cannot read the chain height directly; the locktime proof is the
standard reduction.

### 3.6 What stays off the chain

Velocity, holder caps, and any rule requiring global chain state stay
registrar-maintained: they are approximated by policy updates at issuer
cadence, not enforced per transfer. Offering documents for an OpenDAMP asset
must state which rules are covenant-enforced and which are registrar
commitments. Issuers needing the full stateful rule set per transfer elect
`cosign`.

## 4. Snapshot publication and data availability

A Merkle root authenticates proofs but cannot generate them. Holders (or
mirrors) must retain the current snapshot to spend without the issuer's
service; loss of the data halts the primary path until it is recovered or
the issuer publishes an update. The registrar therefore treats snapshot
publication as a first-class obligation:

    snapshot/v1 = {
      "v": 1, "asset": A, "verifier_asset": V, "q": q,
      "pi": <hex>, "seq": n, "prev_pi": <hex or null>,
      "tree": "cmt-v1" | "smt-v1",
      "predicates": {
        "blacklist": { "root": <hex>, "entries": [...] | "url": ... },
        "whitelist": { "root": <hex>, "entries": [...] | "url": ... },
        "limit": <u64 or null>,
        "windows": [...]
      },
      "issuer_sig": <BIP340 over H_"OpenDAMP/snapshot/v1"(canonical-json-without-sig)>
    }

Every version is content-addressed and published through the asset registry
and the operator transparency log (hash-chained, anchored on-chain), and
served over a subscription endpoint wallets poll. The genesis snapshot's
hash is committed in the asset contract (section 5), so pi_0 is verifiable
from the asset id alone.

## 5. Contract binding

The registry contract's `openamp` block gains:

    "enforcement": "damp",
    "verifier_asset": <asset id of V>,
    "verifier_amount": q,
    "issuer_update_key": <x-only I>,
    "genesis_policy": <pi_0 hex>,
    "genesis_snapshot_hash": <sha256 of snapshot seq 0>

As with every contract field, this is committed into the issuance entropy:
the asset id proves its own enforcement model, its verifier asset, and its
genesis policy. Enforcement can never be retrofitted in either direction.

## 6. Issuer operations

- **Update**: spend C_V(pi) through G(I), create C_V(pi') with q of V, and
  publish snapshot seq+1 before or at broadcast. Wallets treat a verifier
  output whose pi has no published snapshot as unspendable-by-them and alert.
- **Halt**: move V anywhere else through G(I). This is the pause analog and
  is reversible by recreating a verifier output.
- **Parallel verifier outputs**: with retained V reissuance authority the
  issuer may run N verifier outputs to reduce the spending race. Keeping
  their policy versions consistent during an update is an operational
  runbook item: update all outputs in one transaction where possible, or
  publish the union semantics (a transfer is valid under the pi its chosen
  verifier input commits to).
- **Redemption**: the primary path has no redemption exit. The issuer path
  still permits an informal exit: a collaborating holder and the issuer can
  move A outside confinement (both signatures required; a holder who does
  not consent is unaffected). A dedicated redemption branch with fixed rules
  (holder signature plus issuer signature, outputs only to a fixed
  redemption covenant or a provably unspendable burn) is planned for v2.

## 7. Security considerations

Inherited from the DAMP paper with Sequentia specifics:

- **Custody**: the issuer alone cannot spend a holder's balance on any path;
  every user input requires its owner's signature. Compromise of I permits
  policy revocation or denial of service, not theft. I should be a FROST
  group key.
- **Contention**: a single verifier output is a spending race; see parallel
  outputs above. An optional coordinator can order transactions without
  becoming an authorization service, at the cost of potential MEV.
- **Data availability**: section 4. The registrar's snapshot service going
  down does not halt transfers for holders who retained the snapshot; that
  is the tier's headline property.
- **Anyone-can-spend hazard**: an unenforced 0xbe leaf is anyone-can-spend.
  Never fund an OpenDAMP covenant on a chain where `getdeploymentinfo` does
  not report simplicity active. On Sequentia: mainnet always-active; the
  live testnet is active (BIP9 bit 21, since height 89,856); regtest
  requires `-evbparams=simplicity:0:::`.
- **Budget exhaustion**: N_max and proof sizes bound the witness; the
  covenant must be sized so a maximal legitimate transfer fits the
  Simplicity budget and the chain's block weight with margin.
- **Fixed input-zero position**: functionally limiting (the paper notes it);
  accepted for v1.

## 8. Toolchain and milestones

Simplicity programs are written in SimplicityHL and compiled outside this
repository; the node vendors only the Simplicity interpreter and jets. A
template registry pins `program_id -> CMR` for the fixed user program and
each verifier program version; wallets and the registrar verify CMRs against
this registry, never against locally compiled artifacts.

- **M0 (this document)**: protocol, commitment and snapshot formats, contract
  binding.
- **M1**: U and P(pi) in SimplicityHL; golden CMR vectors; functional test on
  elementsregtest proving transfer, confinement refusal, blacklist refusal
  and non-membership spend, whitelist, limit, issuer update, halt.
- **M2**: policy-commitment library and snapshot service in the operator
  daemon; `enforcement` election plumbed through issuance (refused with a
  capability error until M1 ships); registry validation of the new contract
  fields.
- **M3**: wallet support (sig_all_hash signing, proof assembly from
  snapshots, SPK-based UTXO discovery as a descriptor variant); venue
  integration spec update.
- **M4**: testnet pilot: one live asset through issue, transfers with the
  policy service deliberately stopped, freeze by policy update, unfreeze,
  and an issuer halt drill.
