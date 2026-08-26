# Holding an OpenAMP restricted asset from Sequentia Core

OpenAMP is how an issuer governs a regulated asset on Sequentia: shares, bonds,
fund units, anything whose transfers must be restricted to people the issuer has
approved. The design is in [`openamp-design.md`](openamp-design.md), and the
policy server that enforces it lives in the
[`openamp`](https://github.com/ConcatenaLabs/openamp) repository.

This document is the other side of that relationship — what a **holder** running
Sequentia Core does. It covers the desktop GUI and the two RPCs underneath it.

## What a restricted asset actually is

Units of a restricted asset live in an **enclave**: a taproot output whose script
path is

```
<K_holder> CHECKSIGVERIFY <K_policy> CHECKSIG
```

with a nothing-up-my-sleeve internal key, so there is no key-path spend. Both
signatures are needed for every transfer, which is what lets the issuer enforce a
restriction with no consensus rule behind it: the policy server simply declines to
sign a transfer it disapproves of. An asset issued with clawback has a second,
disclosed leaf carrying the issuer's key in place of the holder's.

The policy key is named in the asset's issuance contract, and the asset id is a
hash commitment to that contract. So the binding between an asset and the server
that governs it is verifiable by anyone, offline, with no registry to trust.

Being a holder therefore takes exactly three things:

| | What it is | Where it comes from |
|---|---|---|
| Enclave key | An ordinary key of your wallet | `getnewaddress`, or the GUI button |
| Account id (AID) | How the issuer refers to you | Derived from the key — `getopenampaccount` |
| A signature | BIP340, over each sighash the server hands back | `signopenamptransfer` |

The account id is a hash of your registered key set, not something granted to you,
so Core derives it rather than fetching it. That is what lets you show an issuer —
or a platform built on one, such as SeqPal — who you are before you hold anything
at all.

## In the GUI

The **OpenAMP** tab (Alt+8) does all three.

1. **Use a new key from this wallet.** This takes an ordinary address key and makes
   it your enclave key. The account id appears beneath it immediately, because it
   follows from the key.
2. **Give the issuer the enclave key.** They register it; the account they create
   for it is the same id shown here. For a platform that asks for an account, the
   account id is the value to paste.
3. **Derive the enclave address.** Paste the asset's policy key — and its issuer
   key if the asset has clawback — and the page shows the address your units are
   held at, along with the leaf and control block needed to spend them. Both keys
   are public and both are in the issuance contract.
4. **Sign a transfer.** When the issuer's server builds one it answers with an
   object carrying `tx` and `to_sign`. Paste that in; the result is the `sigs`
   object its completion endpoint expects.

Changing the enclave key changes the account. Anything already held under the old
key stays where it is, so change it only before you hold something.

## From the command line

Deriving an account, with the enclave for one asset:

```sh
sequentia-cli getopenampaccount '["<your x-only key>"]' <policy key> [<issuer key>]
```

```json
{
  "aid": "7a117ddc0d98c9756ac1586e80970924691a2117",
  "address": "ert1psdyadcwv57sk828k3kyke65wv77tfrxsr50rl4p6sjen869s2vfs2qtq5u",
  "script_pubkey": "5120...",
  "transfer_leaf": "20...ac",
  "transfer_control": "c450929b...",
  "claw_leaf": "20...ac",
  "claw_control": "c450929b..."
}
```

The x-only key is the 33-byte compressed public key from `getaddressinfo` with its
first byte removed. Give the policy key and the enclave is derived too; leave it
out and only the account id comes back.

Signing a transfer the issuer's server built:

```sh
sequentia-cli -rpcwallet=<wallet> signopenamptransfer "<tx hex>" \
  '[{"vin":0,"sighash":"<hex>","xonlykey":"<your key>","leaf":"<hex>","control":"<hex>"}]'
```

The wallet must be unlocked. The node recomputes each sighash from the transaction
and **refuses to sign unless it matches** what the server asked for, so a signature
made here can authorise nothing but that exact transaction. It also checks, through
the control block, that the input really is an output committing to a leaf your key
appears in — a server cannot get you to sign for an input that is not yours.

## Why Core does not talk to the policy server itself

It cannot. The Qt libraries in `depends/` are built with no TLS backend at all
(`-no-openssl`, and the same for the platform ones), so a released `sequentia-qt`
has no way to open an https connection, and any policy server worth using is https.
Re-adding OpenSSL to the Qt build is a decision about the whole GUI's supply chain,
not a detail of this feature.

So Core does the half that is genuinely local — deriving, checking and signing —
and whatever already speaks https carries the request: the issuer's own web flow, a
platform front-end, or `curl`. That split is the shape an offline signer already
has, and it keeps the property that matters: every request is public data, and the
private key never leaves the wallet.

To register directly against a server that is reachable over plain http, the whole
of it is one call:

```sh
curl -s -X POST http://127.0.0.1:8722/v1/users -d '{"pubkeys":["<your x-only key>"]}'
```

## With SeqPal

SeqPal identifies a person by a **SeqPal ID**, and a wallet is linked to one by its
public key: SeqPal's own backend does the registering with OpenAMP, so nothing here
needs to reach the policy server. Give SeqPal the enclave key from the OpenAMP tab,
and the OpenAMP account it creates is the account id the tab already shows.

A SeqPal ID linked only by an ordinary descriptor, with no OpenAMP account attached,
still works for supervised assets, network-enforced (OpenDAMP) assets and the
distributions attached to them — but it cannot hold an enclave-based restricted
asset until an OpenAMP account is attached to it. Attaching one later keeps the same
SeqPal ID.

## What this does not do

Balances, ownership reports, the transparency log and the list of assets a server
governs are all read from the server's REST API, which is public and unauthenticated
(`GET /v1/assets`, `GET /v1/users/{aid}/balance`). Core does not mirror them.

Network-enforced (OpenDAMP) assets are a different tier with no co-signature and no
enclave: transfers there are built by the holder from the published policy snapshot
and policed on chain by a Simplicity covenant. `getopenampaccount` derives the
co-signed enclave only.
