# Sequentia Core 24.7.10

Follow-up hardening to the rangeproof cache fix in 24.7.9. Defence in depth,
not a fix for a live exploit: 24.7.9 already closed the bug that drained the
Liquid federation. This release is safe to adopt at your own pace — a node on
24.7.10 and a node on 24.7.9 agree about every block.

## What changed

A node caches proofs it has already verified so it does not verify them twice.
The cache key was a raw concatenation of the verified fields, with no length
delimiters. When two of those fields are variable-length, two different inputs
whose fields happen to concatenate to the same byte stream would land on the
same cache entry — and a cache entry is a stored "this verified" result, so a
collision could return "valid" for something never actually verified.

On 24.7.9 this was not reachable: in the rangeproof key the two variable-length
fields (the proof and the output script) are separated by two fixed 33-byte
commitments, so no colliding pair can be constructed. 24.7.10 removes the
dependency on that layout entirely by length-prefixing every field, so distinct
inputs can never share a key. The surjection-proof cache additionally now keys
on its target set; the transaction id already committed to it, so this too is
belt-and-suspenders rather than a live gap.

This tracks Elements upstream (`ElementsProject/elements@94000967f`), which
added the same hardening after 24.7.9 shipped.

## Deployment

The cache key is internal to each node and reseeded at every startup. It is in
no serialised format, needs no activation height, and does not change what any
block means. Unlike 24.7.9, there is no coordinated cutover: update whenever
convenient.

## Not included

`ElementsProject/elements@19d704279` (a `-norangeproofcache` option to disable
the cache outright) is not ported. The `blindpsbt.cpp` PSET-hardening from the
same upstream round remains a separate follow-up.
