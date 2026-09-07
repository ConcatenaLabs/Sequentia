# Sequentia Core 24.7.9

A security release. It fixes the Elements rangeproof cache bug that was used to
drain the Liquid federation of roughly 4000 BTC on 6 September 2026. Sequentia
inherited the defective code verbatim from Elements, so every release up to and
including 24.7.8 carries it.

Update your node.

## Sequentia had no federation reserve at risk

The consequence on Liquid was as large as it was because L-BTC is a peg: real
bitcoin sits with a federation, and coins on the sidechain are claims against
it. Inflate the sidechain asset and you can redeem the difference in bitcoin.

Sequentia is not built that way, and the difference is in consensus, not in
policy. On every Sequentia network — `CMainParams`, `CSequentiaParams`,
`CTestNetParams` — `consensus.has_parent_chain` is `false` and
`consensus.pegged_asset` is set to `consensus.subsidy_asset`. The peg-in
machinery inherited from Elements is inert, and the slot Liquid fills with a
bitcoin peg holds this chain's own policy asset instead. Nothing issued on
Sequentia redeems for bitcoin, and no federation is custodying coins against
anything here.

Nor is there a single asset the whole chain runs through. Fees are payable in
any asset, so there is no equivalent of the position L-BTC occupies on Liquid —
no one asset that everything else is denominated in and settled against.

What the bug does buy an attacker on Sequentia is real and is the reason for
this release: inflation of an issued asset, and a chain split between nodes
whose caches are warm and nodes whose caches are cold. It is not a bitcoin
reserve walking out the door, because there is none to walk.

## The bug

A node caches rangeproofs it has already verified so it does not verify them
twice — once when a transaction enters the mempool, again when it arrives in a
block. The cache entry was keyed on the proof and the value commitment alone.

But verifying a rangeproof takes two more inputs: the **asset commitment**,
which is the generator the proof is stated over, and the **scriptPubKey**, which
is the proof's extra commitment and which also decides whether a zero minimum
value is allowed. Neither was part of the key. So a proof verified in one
context was handed back as valid in any other — a different asset, a different
script — without ever being verified there.

That is enough to break conservation. An issuance rangeproof commits to an
empty, unspendable script and is therefore allowed a minimum value of zero;
replayed onto a spendable output it conjures a reissuance token out of nothing.
And a proof accepted under a second asset generator means an output's amount was
never proven to be in range for the asset it claims to be — two such outputs can
be made to balance the transaction while one of them carries an arbitrary
amount.

The cache is filled during mempool acceptance, so a transaction broadcast by any
peer primes it. A node that never saw the priming transaction, or that has since
restarted, verifies for real and rejects. That is why the failure mode is a
chain split and not a quiet inflation.

## The fix

The cache key now includes the asset commitment and the scriptPubKey.

This is upstream `ElementsProject/elements@c26d719c`, *"fix: range proof cache
bind to asset and scriptpubkey"*, ported byte-identically — our base and theirs
are both Elements 23.x, so it is the same two lines in the same file at the same
line numbers. Upstream it sits on `master`, `elements-23.x` and
`elements-23.3.x`, and in no tagged Elements release: 23.3.3 is vulnerable and
23.3.4rc2 was still in preparation when Liquid was drained.

A regression test, `blind_tests/rangeproof_cache_binding_test`, covers the three
replays: a proof reused under a different asset, under a different script, and
an issuance proof reused on a spendable output. All three are accepted by the
old code and rejected by the new.

## This one does need a coordinated cutover

24.7.8 needed none — a node on 24.7.7 and one on 24.7.8 agreed about every
block. This release is different.

The cache key itself is node-local and salted at every startup, so it appears in
no serialised format and needs no activation height. But a patched node rejects
a chain built on the exploit, which is precisely the point of patching. Nodes
should be updated together rather than trickled out.

## Still to come

The upstream hardening of `blindpsbt.cpp` from the same round
(`ElementsProject/elements#1592` and `#1593`) is not in this release. It is
wallet-side rather than consensus, but every part of it is reachable from a PSET
handed to you by a counterparty: a blinded value proof that checks only the
lower bound of the proven range and so accepts a proof whose committed value
exceeds the amount shown, an out-of-bounds read on an explicit value, three
asserts an attacker can trip to abort the process, and a null dereference on a
PSET v0 output with no amount. It will follow separately.
