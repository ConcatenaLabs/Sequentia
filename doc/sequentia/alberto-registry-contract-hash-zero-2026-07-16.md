# Note for Andreas & Saba — same conclusion as your PR #3, plus the bug upstream of it in Core

**From:** Alberto (with bubu)
**Date:** 2026-07-16
**Re:** `sequentia-registry-proofs-and-box-access.zip` part A, and
`sequentia-registry` PR #3 (`operator_verified`)

---

## We got there in parallel

While bubu was reading your registry (public on GitHub, so we did not need to
ask you anything in the end) and checking the chain, it found that GOLD's
issuance commits `contract_hash = 0`:

```bash
curl -sS https://sequentiatestnet.com/api/tx/00d17e2595ab625792353ce31ea703f73647b8d123e9e5814001e9b744c119c0 \
  | jq '.vin[0].issuance.contract_hash'
# "0000000000000000000000000000000000000000000000000000000000000000"
```

So the six demo assets commit to no contract and therefore to no domain, and
`sequentia.io` was never baked into their ids — it is a label in your seed file.
The proof files could not have helped: `POST /` dies on the contract check (and
on the all-zeros placeholder `issuer_pubkey`) long before `verifyDomainProof` is
reached, and `/admin/seed` forces `legacy: true` so `verified` stays false by
construction.

Then we refetched your repo and found PR #3 already merged, saying the same thing
in your own words. So: no argument here, and **don't read the above as news** —
it is just the evidence trail for how we got to the same place. `operator_verified`
with `verified_by: "operator"` and `verified_chain`/`verified_domain` left false
is the right shape, and I'm glad the audit trail stays honest. Alberto's original
complaint (GOLD as hex in Core) is fixed by it: the minimal index now carries
`verified: 1` and Core picks the names up on the next poll.

**So: don't upload anything to sequentia.io.** Nothing needs it any more. If we
ever want those six verified for real rather than vouched for, the only route is
re-issuing them with real contracts under new asset ids, and tSEQ can't be
re-issued at all. Not worth it for demo assets, in my view — leave it.

One thing your README and ours now agree on, which I'd asked bubu to write down
before I knew about your PR: the manual flag is an operator override, not the
model for a public network. That's now documented on both sides.

## The part you don't know about: Core issues every asset this way

The demo assets weren't a one-off mistake. Core still does this today, and the
same bug would produce more of them:

- The Qt Assets page asks for amount, reissuance tokens and a confidential
  checkbox. No name, no ticker, no domain.
- `src/qt/assetspage.cpp` called `issueasset` with those three arguments only, so
  `contract_hash` stayed at its 32-zero default.
- The RPC did expose `contract_hash`, but as a raw 32-byte value with no
  explanation anywhere in `doc/` — and it is parsed big-endian, so anyone passing
  a `sha256sum` digest straight in would have committed the reversed bytes and
  got an unverifiable asset while believing they had done it right.

Every asset anyone issued from Sequentia Core, ever, is anonymous for good. That
is the actual root cause of Alberto's question, and it was still live.

## What we built (Core side, our repo)

- `src/assetcontract.{h,cpp}`: canonical JSON (keys sorted, no insignificant
  whitespace, `JSON.stringify` escaping) and its SHA256, plus your field rules and
  the proof line. **Pinned against your implementation**: the unit test hashes the
  GOLD contract from your registry and asserts it equals the `contract_hash` your
  Node code produced (`646f250b…`). If our serialisers ever drift by a byte, that
  test fails rather than shipping assets you would reject.
- `issueasset` takes a `contract` argument (name, ticker, domain, precision),
  builds and commits the hash, fills `issuer_pubkey` from a fresh wallet key
  rather than the placeholder you reject, keeps the contract's `precision` and the
  chain `denomination` in agreement, and returns the contract and the proof line —
  neither of which the chain keeps and neither of which an issuer can reconstruct.
- The Assets page asks for name/ticker/domain, validates them against your rules
  before spending anything, warns when the domain does not resolve, says plainly
  that none of it can be changed afterwards, and then hands over the ready-made
  `.well-known` file.
- `doc/sequentia/asset-contracts-and-verification.md`: the whole chain written
  down for humans and AIs, including the byte-order trap.

Two questions we answered from your code rather than asking, in case they are
worth confirming: the contract schema (`server.js:156-186`) and that there is no
re-verification — `verified` is written once and persists, so an issuer's site
going down later costs nothing. If either is meant to change, tell us, because
the GUI's wording depends on the second one.

## Why Core-side work stays with us

Merge-conflict avoidance only, no disagreement. Alberto is merging the new
Sequentia Core UI, which rewrites the Qt layer including the Assets page; if Saba
edits `assetspage.cpp` in parallel we spend longer reconciling than building.
Core stays with us; the box, the registrar and the public registry stay with you
two, as now.

— Alberto
