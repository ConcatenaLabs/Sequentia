# Sequentia Core 24.7.10

A security release. It closes the proof-cache key collision that was used to
drain the Liquid federation of roughly 4000 BTC on 6 September 2026. 24.7.9
does not close it: a node on 24.7.9 is exposed.

Update your node.

## What changed

A node caches proofs it has already verified so it does not verify them twice.
The cache key was a raw concatenation of the verified fields, with no length
delimiters. Two different inputs whose fields concatenate to the same byte
stream land on the same cache entry — and a cache entry is a stored "this
verified" result, so the second input is returned as valid without ever being
verified.

The rangeproof key is `proof ‖ value commitment ‖ asset commitment ‖
scriptPubKey`. The two commitments are fixed at 33 bytes each, but the proof and
the script are both variable-length, and that is enough. Take bytes off the
front of the script, append them to the proof, and let the two 33-byte windows
slide along with them: a different tuple, the identical byte stream.

So an attacker has one honestly valid output verified and cached — one whose
script is an unspendable data push holding a second value commitment and the
asset commitment — and then presents a second output whose fields are the first
one's bytes divided at different boundaries. The cache lookup comes before any
parsing or verification, so the second output's "rangeproof" is never checked,
and its value commitment is the one planted in the first output's script. A
value commitment with no range proof behind it can hide a negative amount, and a
negative amount balances an inflated one.

24.7.10 serialises every field with a length prefix, so distinct inputs can
never share a key. The surjection-proof cache key is hardened the same way and
additionally now keys on its target set; the transaction id already committed to
that, so this part is belt-and-braces rather than a live gap.

Two regression tests cover it. `blind_tests/rangeproof_cache_length_prefix_test`
builds two different tuples whose raw concatenation is byte-for-byte identical
and asserts that they key differently;
`blind_tests/surjection_cache_vtags_test` asserts that a change in the target
set changes the key.

This tracks Elements upstream (`ElementsProject/elements@94000967f`), released
in Elements 23.3.4.

## Why 24.7.9 is not enough

24.7.9 added the asset commitment and the scriptPubKey to the rangeproof key.
That closes a separate and older defect, in which a proof verified under one
asset or script was handed back as valid under another. But it added the two
fields by plain concatenation, and that concatenation is the defect described
above.

It is the same change (`ElementsProject/elements@c26d719c`) that Liquid's
functionaries were running when the federation was drained. Blockstream's
[incident assessment](https://blog.blockstream.com/liquid-network-security-incident-assessment/)
identifies the missing length prefixes in it as the bug that was exploited.

What it buys an attacker on Sequentia is what the 24.7.9 notes describe:
inflation of an issued asset, and a chain split between nodes whose caches are
warm and nodes whose caches are cold. There is no federation reserve here for it
to reach.

## Deployment

The cache key is internal to each node and reseeded at every startup. It is in
no serialised format and needs no activation height.

It does need a coordinated cutover, for the same reason 24.7.9 did. Absent an
attack, a node on 24.7.10 and a node on 24.7.9 agree about every block. Under
attack they do not: the 24.7.10 node rejects a chain built on the collision and
a 24.7.9 node with a primed cache accepts it. Update nodes together rather than
trickling the release out.

## Not included

`ElementsProject/elements@19d704279` (a `-norangeproofcache` option to disable
the cache outright) is not ported. The `blindpsbt.cpp` PSET-hardening from the
same upstream round remains a separate follow-up.
