# Note for Andreas & Saba — the demo assets cannot be verified, and the proof files won't change that

**From:** Alberto (with bubu)
**Date:** 2026-07-16
**Re:** `sequentia-registry-proofs-and-box-access.zip`, part A

---

## Short version (sent first, on chat)

> Before I touch the sequentia.io files — don't upload anything yet, they won't do
> what you think. bubu went and read your registry repo on GitHub (it's public, so we
> didn't need to ask you anything in the end) and then checked the chain. GOLD was
> issued with `contract_hash = 0`. So were the other five. Nothing is committed
> on-chain, `sequentia.io` isn't baked into any asset id — it's just text in your seed
> file. The proof files would fail the chain check before the registrar ever fetches
> them.
>
> Your own seed file says this, by the way. Long note below with the evidence, for
> Saba too. Short version: the demo assets can't be verified by any code path in your
> registry, and the root cause is the same bug we found in Core's issuance. We'll fix
> Core on our side — me and bubu, not you and Saba, because I'm about to merge the
> whole new Core UI and we'd collide on the same Qt files.

---

## 1. First: we didn't need to ask you anything

bubu's initial draft of this note asked you two questions — the exact contract JSON
your registrar parses, and whether it re-verifies proofs over time. Then I pointed out
that your registrar is presumably on GitHub. It is:
[`GracedEternalKingCabbageMan/sequentia-registry`](https://github.com/GracedEternalKingCabbageMan/sequentia-registry),
public, 515 lines, zero deps. Both answers were sitting in it. Noted on our side: read
the repo before asking the human.

For the record, the two answers:

**Contract schema** (`server.js:156-186`):

```json
{
  "name": "…",
  "ticker": "…",
  "precision": 8,
  "entity": { "domain": "…" },
  "issuer_pubkey": "…",
  "version": 0
}
```

`ticker` 1..12 chars; `precision` 0..8; `issuer_pubkey` a 33-byte compressed or 32-byte
x-only key, mandatory, must not be all-zeros; `version` must be `0`. Unknown top-level
keys are rejected. `contract_hash = SHA256(canonical JSON)` where canonical means keys
sorted lexicographically and no insignificant whitespace. Unambiguous — we can
implement against it directly.

**Re-verification:** there is none. No scheduler, no revocation. `verified` is written
once at registration (`server.js:421`) and persists. So if an issuer's website goes
down, nothing happens — the entry stays verified. bubu had told me the opposite was
possible; the code settles it.

---

## 2. Plan A cannot work

GOLD's issuance input, on chain:

```bash
curl -sS https://sequentiatestnet.com/api/tx/00d17e2595ab625792353ce31ea703f73647b8d123e9e5814001e9b744c119c0 \
  | jq '.vin[0].issuance'
```

```json
"contract_hash": "0000000000000000000000000000000000000000000000000000000000000000"
```

GOLD was issued with a zero contract hash. So were the other five. There is no contract
committed on chain, which means **`sequentia.io` is not baked into the asset id**. It is
a string typed into `seed/legacy-assets.json`. Nothing more.

Your seed file says so itself:

> "They were issued with contract_hash = 0 (no on-chain contract), so they cannot pass
> cryptographic contract verification; they are seeded as pre-approved, UNVERIFIED
> entries (legacy: true, verified: false)"

So the note you sent and your repo disagree with each other, and the repo is right.

We traced every path in `server.js` that can set `verified`. There is exactly one
(line 421), and it requires `verified_chain`:

- **`POST /`** dies twice before the domain proof is ever fetched. First, the demo
  contracts carry the all-zeros placeholder `issuer_pubkey`, which `validateContract`
  rejects outright (line 169). Second, even with a real pubkey, `SHA256(contract)` =
  `646f250b…` is compared against the on-chain zero and throws *"contract does not match
  on-chain commitment"* (line 396). `verifyDomainProof` at line 412 is unreachable for
  these assets.
- **`POST /admin/seed`** forces `legacy: true`, which leaves `verified_chain = false`,
  so `verified = false` by construction.

**Conclusion:** `refresh-registry.sh` will fail for all six regardless of what is served
from sequentia.io. This isn't a hosting problem. Those assets were born without a
contract. I'm not uploading the files until we agree on what actually fixes this.

**Corollary:** since nothing is committed, *"I would've changed the domain to
sequentiatestnet.com but it's too late, can't do that after the asset was issued"* isn't
right either. The domain in the seed is free text. It was never binding and you can
still change it today. It just won't make anything verified.

---

## 3. The root cause is in Core, and it's still live

The demo assets weren't issued wrong by accident — Core issues every asset this way.

- The Qt Assets page offers three inputs: amount, reissuance tokens, confidential
  checkbox. No ticker, no name, no issuer domain.
- `src/qt/assetspage.cpp:231` calls `issueasset` with those three params only, so
  `contract_hash` stays at its default of 32 zero bytes.
- The RPC does expose `contract_hash` (`src/wallet/rpc/elements.cpp:1400`), but as a raw
  32-byte hex value. The user is expected to already know a contract JSON exists, what
  fields it carries, and to canonicalise and hash it themselves. Nothing in `doc/`
  explains it.

So every asset anyone issues from Sequentia Core today is permanently unverifiable,
exactly like the demo six. This is what my original question was really about: GOLD
showing as raw hex in Core isn't a missing verification step, it's an asset that can
never be verified.

---

## 4. What we're going to build

In the Qt issuance flow:

1. Real fields: ticker, display name, precision, issuer domain.
2. Core builds the contract JSON and the canonical hash itself, matching your registrar
   byte-for-byte. The user never sees a hash.
3. `issuer_pubkey` derived from a wallet key rather than left as a placeholder.
4. A pre-flight HTTPS reachability check on the domain **before** broadcasting. After
   issuance the domain is fixed forever, so a typo is unrecoverable. The check has to
   happen while it's still free.
5. After issuance, Core emits the ready-made
   `.well-known/sequentia-asset-proof-<assetid>` file with the exact one-line body, so
   nobody has to compose it by hand.
6. Documentation of the full chain — domain committed at issuance → proof file →
   registrar verifies → node trusts `verified` — written for humans and for AIs.

We'll also document in `contrib/asset-registry` that a manual `verified` toggle is a
registrar-operator override and explicitly **not** the recommended default for a public
network. Your point on that stands and it's getting written down rather than argued.

---

## 5. What to do about the existing six

Two honest options, both uncomfortable:

1. **Re-issue them with real contracts.** Correct, but it changes the asset ids —
   precisely the orphaning you wanted to avoid. And tSEQ is the genesis token, so per
   your own note it can't be re-issued at all. That one needs a decision either way.

2. **Let the registrar mark legacy seed entries as verified.** Which is the mechanism
   you called "not normal practice" — and, as things stand, the only way your registry
   can ever display its own demo assets short of re-issuing them.

I lean towards: fix Core first so this stops reproducing, then re-issue the five
non-genesis demo assets properly with real contracts and real proofs on sequentia.io
(which at that point will actually do something), and special-case tSEQ. But it's your
registry and your call on the legacy entries — tell us which way you want it and we'll
document whatever we pick so future researchers see the reasoning, not just the outcome.

---

## 6. Why Core-side work stays with us

Purely merge-conflict avoidance, no disagreement anywhere. I'm about to merge the whole
new Sequentia Core UI, which rewrites the Qt layer including the Assets page. If Saba
edits `assetspage.cpp` in parallel we'll spend longer reconciling than building. Core
stays with me and bubu; the box, the registrar and the public registry stay with you and
Saba, as now.

— Alberto
