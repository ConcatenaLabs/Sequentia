# Sequentia Core 24.1.0

This release adds **supervised assets**: assets whose issuer can freeze and unfreeze
holdings, for the cases where an asset represents an off-chain claim that its issuer
is legally obliged to be able to arrest. It also completes the rename of the binaries
begun in 24.0.0's cycle, so the daemon is `sequentiad` and the client is
`sequentia-cli`.

**This is a hard fork.** Every committee node, the dexnode and the explorer node must
be running this release before testnet height **94,600**, which lands at roughly
**05:00 UTC on 2026-08-15**. A node still on 24.0.0 at that height will derive asset
ids differently from its peers and fork off.

Below 94,600 the new rule is inert: a supervision declaration is ordinary data and an
issuance derives its asset id exactly as a node without this code would. So deploying
early is strictly better than deploying late, and there is no window in which running
this release early does anything different.

## Supervised assets

An asset id is derived, not declared. Consensus computes it from the issuance, which
is what makes an asset id unforgeable — you cannot claim to be an asset you are not.
Supervision extends that derivation with a third leaf committing to the supervision
descriptor, so a supervised asset is a *different asset id* from the same issuance
without supervision. The consequences are worth stating plainly:

- An asset issued unsupervised can never become supervised, and the reverse. The
  status is fixed at issuance because it is part of what makes the asset that asset.
- A node cannot be tricked into treating an unsupervised asset as supervised, because
  it never reads the claim; it recomputes the id and compares.

The freeze registry is built once at startup by scanning the UTXO set. Each node logs
what it found:

```
Supervision: registry loaded: 0 supervised asset(s)
```

On the testnet that will say 0 until the first supervised asset is issued.

New RPCs live under the `supervision` help category; `getsupervisedassets` returns the
registry and is the quickest check that a node agrees with its peers.

### What an issuer has to decide before issuing, not after

Everything below is committed into the asset id, so none of it can be changed, added or
removed once an asset exists. [doc/sequentia/supervised-assets.md](../sequentia/supervised-assets.md)
is the full guide for issuers and operators.

**Two keys, and they do different jobs.** A supervised asset commits an *operational* key,
which signs freezes and unfreezes, and a *recovery* key, which signs nothing but rotation
records — replacing the operational key, or itself. The recovery key cannot freeze; the
operational key cannot rotate, not even itself. That asymmetry is the whole reason there
are two: someone who steals the operational key can freeze holders and be seen doing it,
but can never take the authority, because the issuer rotates it away with a key kept deep
cold. A single self-rotating key would make compromise a race the attacker wins by rotating
first, silently, which is worse than no rotation at all.

Both keys are BIP340 x-only, which means an issuer wanting several officers to hold the
authority needs nothing from the protocol: FROST or MuSig2 produce one ordinary signature
under one ordinary key, and the chain never learns a quorum is behind it. No private key is
ever given to a node — the node says what to sign, the key signs elsewhere, the node
assembles the result.

**Reissuance tokens are mandatory**, non-zero and explicit. Seize and burn are answered
economically — a permanent freeze is a burn, and a freeze plus reissuing the same amount to
the address a court names is a seizure — rather than by giving an issuer a power to spend
other people's outputs. An asset that cannot be reissued therefore cannot call itself
supervised.

**Pause is opt-in and permanent either way.** With `"pause": true` at issuance the issuer
may stop every single-owner holding at once with one record naming a wildcard target;
without it, that power does not exist for that asset and cannot be added. A holder can see
which it is before accepting the asset.

**Nothing blinded.** Every output of a supervised issuance must be explicit in asset and
value, and supervised assets can never enter a blinded output afterwards, because consensus
cannot read a blinded output's asset. The node wallet and the DEX wallet daemon were fixed
in this release so ordinary sends do not blind change on these assets.

### Freezing, unfreezing, and what a freeze does not reach

Creating a freeze record freezes; spending it unfreezes. A freeze takes effect **from the
block after** the one carrying the record, so a record and a spend in the same block are
never ambiguous, and mempools get a full block to shed transactions the freeze invalidated.

The record's own script is not what authorises spending it. A script is fixed when it is
created, so it can only name the key that was current then — honour that and a rotation
leaves every existing freeze liftable by the very key the rotation was meant to retire.
Consensus checks the asset's *current* operational key instead, in both directions.

A freeze blocks spending only, and only of single-owner scripts — P2WPKH and P2TR key-path.
Paying *to* a frozen script stays legal, which is what makes freezing a known destination in
advance useful. Shared scripts — channel funding, HTLCs, covenant branches — are never
frozen, because freezing one would trap an innocent counterparty's whole balance and would
hand the frozen party any race against an absolute deadline. `isassetfrozen` reports whether
a freeze could reach a given script at all.

A freeze record is public once it reaches the mempool, and a holder watching for it can move
first. `submitsupervisionrecord` hands a record straight to a producer instead, which keeps
it out of the public mempool until it confirms.

Mainnet and the custom/regtest chains have supervision active from height 1, so an
asset can be issued supervised at launch rather than migrated into supervision
afterwards — which, per the above, is impossible.

## The binaries are named after the project

`elementsd`, `elements-cli`, `elements-qt` and the rest are now `sequentiad`,
`sequentia-cli`, `sequentia-qt`, `sequentia-tx`, `sequentia-util` and
`sequentia-wallet`. A node also introduces itself to its peers as
`/Sequentia Core:24.1.0/` rather than `/Elements Core:...`.

**This breaks every script and unit file that invokes the old names**, including
anything on an operator's own machine. Two things were deliberately left alone so an
existing node keeps its chain and its wallets without being moved: the configuration
file is still `elements.conf` and the default data directory is still `~/.elements`.

A side effect worth knowing, because it is a free diagnostic: since the user agent
only changed in this line of releases, `getpeerinfo` showing a peer as
`/Elements Core:...` proves that peer predates the rename, and therefore predates
supervised assets.

## Also in this release

- The Linux build installs a desktop entry and application icon, so the GUI no longer
  appears with a generic placeholder icon in the dock and application menu.
- The GUI's price-server launcher no longer picks the bundled Windows interpreter on
  Linux, which made *Settings -> Price server* fail with "Ensure Python is available"
  on machines that had a perfectly good `python3`.
- Releases are now built and published by the server itself when a version tag
  appears, rather than by hand from a laptop.
