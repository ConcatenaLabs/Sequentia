# Sequentia Core 24.7.11

Staking from a wallet now works for descriptor wallets, which are what every
wallet created by this software is. No consensus change: a node on 24.7.11 and
a node on 24.7.10 agree about every block. Update whenever convenient; a staker
whose stake shows as not committee-ready on the pool board wants it now.

## What was wrong

Two silent failures, both in the path the Staking tab and `registerstake` take
on the public testnet.

A stake registered from a wallet carried no committee BLS key. On a chain with
the public fixed-size committee that key is what lets the committee certify the
blocks a staker leads; without it the stake has weight, is elected, and produces
nothing, which the pool board reports as "not committee-ready". The RPC had the
arguments to pass one, and nothing passed them.

Turning production on from the Staking tab exported the staker key with
`dumpprivkey`, which descriptor wallets refuse. The tab treated the refusal as
"nothing to enable", so the stake was registered and never produced, with no
message. The only wallets it worked for were legacy ones.

## What changed

- `registerstake` derives the committee BLS registration from the staker key
  and includes it, when the wallet holds that key and the chain runs the public
  committee. The result names the key and carries `committee_ready`; for a key
  the wallet does not hold it says why no registration was made.
- A new wallet RPC, `startstaking`, enables block production for the wallet's
  own staker keys. The wallet hands the keys to the node's producer in-process,
  so nothing is exported, and the node persists the set exactly as
  `startposproducer` does. Descriptor and legacy wallets alike.
- A running producer takes new keys live. `startposproducer` and `startstaking`
  used to persist a new key for the next restart when the node was already
  producing; they now add it at once, and report `added`.
- The Staking tab uses `startstaking`, so staking from the GUI turns production
  on for every wallet type, and says so when a committee key could not be
  registered.

## Repairing a stake registered earlier

A stake that already exists without a committee key needs one more staking
output for the same key that carries it: run `registerstake` for that key
again from the wallet that holds it, for at least the chain minimum. The
registry admits one BLS key per staker, and the derivation is a pure function
of the key, so the outputs never conflict. Then `startstaking`.
