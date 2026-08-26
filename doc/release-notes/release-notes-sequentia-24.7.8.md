# Sequentia Core 24.7.8

Core can now hold an OpenAMP restricted asset. Two RPCs and a GUI tab, no
consensus change.

## What was missing

A restricted asset lives in a 2-of-2 taproot enclave — the holder's key and the
issuer's policy key — so every transfer needs the issuer to co-sign. That is how
an issuer enforces who may hold a regulated asset without any consensus rule
behind it.

Being a holder takes three things: an account id the issuer knows you by, the
enclave address units arrive at, and a BIP340 signature over each sighash the
issuer's policy server hands back. Core could produce none of them. Restricted
assets were reachable only from the web and mobile wallets, and a Core user could
not attach an OpenAMP account to an identity on a platform built on one.

## `getopenampaccount`

Derives the account id (AID) and the enclave from public keys alone — no server,
no chain, no wallet, no network.

The account id is a hash of the registered key set, so it is derived rather than
granted: a wallet can show it before the holder has been told anything about a
particular asset. The enclave follows from the holder's key and the asset's
policy key, and returns the address, the leaf script and the control block.

Deriving the enclave locally is not only convenience. The policy key is committed
in the asset id through the issuance contract, so a holder can confirm the address
they are about to be paid at is the one that asset's own id implies — rather than
trusting the party that would gain by lying about it.

## `signopenamptransfer`

Signs the enclave inputs of a transaction the issuer's policy server built.

Unlike `signsupervisionhash` it does not take the server's word for what it is
signing. A supervision sighash is a bare tagged hash with no transaction behind
it, so that RPC can only sign the bytes it is handed. Here there *is* a
transaction, so the node recomputes each sighash from it and refuses unless the
two agree: a signature made through this RPC can authorise nothing but the exact
transaction that was passed in, whatever the server claimed about it.

It also checks, through the control block, that the input really is an output
committing to a leaf the named key appears in — so a server cannot get a holder to
sign for an input that is not theirs — and it signs only with keys the wallet
already holds.

## The OpenAMP tab

Alt+8 in the GUI: take a key from the wallet, read off the account id, derive the
enclave address for an asset, and sign a transfer the server built.

The tab deliberately does not connect to a policy server. The Qt libraries this
release is built against carry no TLS backend, so the GUI cannot open an https
connection, and every policy server worth using is https. Core therefore does the
half that is genuinely local — deriving, checking and signing — and the https half
is left to whatever already has it: the issuer's own web flow, a platform
front-end, or `curl`. That is the shape an offline signer already has, and it
keeps the property that matters: every request is public data, and the private key
never leaves the wallet.

For a platform such as SeqPal this covers the whole flow, because the platform's
own backend does the registering with OpenAMP; the wallet only ever hands over a
public key and signs.

`doc/sequentia/openamp-holder.md` is the holder's guide.
