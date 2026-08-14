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
