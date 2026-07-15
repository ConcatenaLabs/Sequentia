# Asset contracts: how an asset gets a name

An asset id is 64 characters of hex. Nothing about it says "GOLD", and nothing on
the chain ever will: Sequentia stores no names. Yet wallets show names. This
document explains where that name comes from, why it has to be decided at the
moment of issuance, and what you must do to make it appear.

If you only read one paragraph: **the name, ticker and issuer domain of an asset
are hashed into its asset id when you issue it. They cannot be added, changed or
corrected afterwards.** An asset issued without them is anonymous for as long as
it exists, and the only remedy is to abandon it and issue a different asset. This
is not a policy someone chose to be strict about; it is arithmetic.

## The chain of trust, end to end

```
   you decide         Core hashes          the chain           you publish        a registry           a wallet
  name/ticker/  ->   the contract   ->   stores only    ->     the proof    ->    checks both   ->    shows the
     domain          into the id         the id/hash          on your domain      and vouches           name
```

1. **The contract.** A small JSON document holding the asset's public identity:
   its name, ticker, precision, issuer domain and issuer public key.
2. **The commitment.** `contract_hash = SHA256(canonical JSON of the contract)`.
   The issuance input commits to that hash, and the asset id is derived from
   `(issuance prevout, contract_hash)`. Change one byte of the contract and you
   get a different asset id — a different asset.
3. **The proof.** The contract claims a domain; anyone could claim any domain. So
   the issuer must publish a file on that domain naming the asset id. Only whoever
   controls the domain can do that.
4. **The registry.** It re-derives the asset id from the chain, checks that the
   submitted contract hashes to the committed value, fetches the proof, and only
   then marks the entry `verified: 1`.
5. **The wallet.** Sequentia Core polls a registry (`-assetregistryurl`, every
   `-assetregistrypoll` seconds, 300 by default) and **trusts only `verified: 1`
   entries** — see `src/assetregistry.cpp`. Everything else stays hex. Core does
   not check domains itself; it trusts the registry it was pointed at.

Step 5 is worth dwelling on: the name is not on the chain and is not gossiped
between nodes. A node with no registry configured shows hex for everything, and
two nodes pointed at different registries can legitimately show different names
for the same asset. The registry is the naming authority, and which one you trust
is your choice.

## Issuing an asset properly

### From the GUI

The Assets page asks for the name, ticker, domain and decimal places before it
will issue anything. It checks the fields against the registry's rules while they
can still be retyped, warns you if the domain does not resolve, and makes you
confirm that these choices are permanent. After issuing, it hands you the proof
file to publish.

### From the RPC

```
issueasset 1000 1 false null null null '{"name":"Gold (troy ounce)","ticker":"GOLD","domain":"example.com","precision":8}'
```

`issueasset` builds the contract, hashes it canonically, commits it, and returns
what you need next:

```json
{
  "asset": "<asset id>",
  "token": "<reissuance token>",
  "contract": { "entity": {"domain": "example.com"}, "issuer_pubkey": "...", "name": "...", "precision": 8, "ticker": "GOLD", "version": 0 },
  "contract_hash": "<sha256 of the canonical contract>",
  "proof_url": "https://example.com/.well-known/sequentia-asset-proof-<asset id>",
  "proof_line": "Authorize linking the domain name example.com to the Sequentia asset <asset id>"
}
```

**Keep the `contract` object.** The chain stores only its hash. If you lose the
contract you cannot register the asset, and you cannot reconstruct it by guessing:
one different byte, one different hash.

`issuer_pubkey` defaults to a fresh key from your wallet. You can pass your own,
but it must be a real key — the registry rejects the all-zeros placeholder.

### Publishing the proof

Serve `proof_line`, and nothing else, at `proof_url`:

- over **HTTPS** with a valid certificate,
- with `Content-Type: text/plain`,
- with the body equal to that exact line once trimmed — no HTML wrapper, no extra
  text. The registry compares the whole body, not a substring, so that nobody can
  smuggle the line into a page they do not really control.

The file's name contains the asset id, so it cannot be prepared in advance: the
asset id does not exist until the issuance is broadcast. Issue first, publish
second, register third.

### Registering

Send the asset id and the contract to a registry:

```
curl -X POST https://<registry>/ -H 'Content-Type: application/json' \
  -d '{"asset_id":"<asset id>","contract":{...}}'
```

It will refuse the entry if the contract does not hash to the on-chain
commitment, if the asset id does not re-derive from its issuance, if the proof is
missing or wrong, or if the ticker is already taken — tickers are first-come. Once
accepted, wallets pick the name up within one poll interval.

## The rules a contract must satisfy

These mirror what the registry enforces, and Core checks them before spending
anything, because after issuance nothing can be fixed.

| field | rule |
|---|---|
| `name` | 1–255 characters |
| `ticker` | 1–12 characters of `A-Z a-z 0-9 . -`; case is preserved, uniqueness is case-insensitive and first-come |
| `precision` | whole number 0–8; must equal the issuance's `denomination` |
| `entity.domain` | a domain name you control |
| `issuer_pubkey` | 33-byte compressed or 32-byte x-only key, lower-case hex, not all zeros |
| `version` | `0` |

Unknown top-level fields are rejected so that the hash stays well defined.

## Canonical JSON

The hash is taken over a canonical serialisation, because two JSON documents that
mean the same thing can be written many ways and would hash differently:

- object keys sorted lexicographically,
- no insignificant whitespace,
- strings escaped as JavaScript's `JSON.stringify` escapes them.

```
{"entity":{"domain":"sequentia.io"},"issuer_pubkey":"02...","name":"Gold (troy ounce)","precision":8,"ticker":"GOLD","version":0}
```

`src/assetcontract.cpp` implements this, and `src/test/assetcontract_tests.cpp`
pins it against a contract hashed by the reference registry implementation. If
you touch the serialiser and that test fails, do not update the expected value:
the test is right and the change is wrong. A one-byte divergence produces assets
the registry cannot verify, silently, with no way back.

### A byte-order trap

`contract_hash` as printed by `sha256sum` is the value to commit, in that order.
But `issueasset`'s raw `contract_hash` argument is parsed big-endian, like a txid,
so it **reverses** what you give it. Passing a SHA256 digest there commits the
reversed bytes and yields an asset no registry will ever verify. Use the
`contract` argument and let Core do it; the raw argument exists for callers that
already know exactly what they are doing.

## Why the testnet demo assets are hex

The demo assets on the public testnet (tSEQ, USDX, GOLD, OILX, SILVR, EURX) were
issued before any of this existed, with `contract_hash = 0`:

```
$ curl -s https://sequentiatestnet.com/api/tx/<their issuance txid> | jq '.vin[0].issuance.contract_hash'
"0000000000000000000000000000000000000000000000000000000000000000"
```

They commit to no contract and therefore to no domain. The `sequentia.io` shown
against them in the registry is a label typed into its seed file, not something
the chain attests; it was never binding. They cannot be verified by any code path
in the registry — the contract check runs before the domain proof is even
fetched — so publishing proof files for them changes nothing. They are seeded as
`legacy: true, verified: false` and Core deliberately skips them.

They are the reason this document exists. Anything issued from now on commits its
contract properly and can be verified; those six cannot, and would have to be
re-issued under new asset ids to get names in Core.
