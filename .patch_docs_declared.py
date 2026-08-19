p = "doc/sequentia/05-operating-sequentia.md"
s = open(p).read()

old = """**Monitoring a pool's actual work.** `listpools` reports `blocks_produced`"""
new = """**What counts as a pool.** `listpools` lists a signer only once it has DECLARED
itself one by announcing a payout policy. That is the only deliberate, on-chain
opt-in there is, and it is what separates an operator soliciting delegations from
the many stakers who simply produce blocks for themselves. Anyone may delegate to
any signer, and consensus neither restricts that nor could; but a signer that has
never made a commitment is not running a pool, and listing it as one would put
words in its mouth. A policy that is announced and still serving its notice period
counts, because that is exactly what a new pool looks like while delegators decide.

Pass `include_undeclared` to see every signer with weight, each flagged
`declared`; pass an explicit `signer` to read one whether or not it has declared,
which is how a wallet describes the signer a stake is currently lent to.

**Monitoring a pool's actual work.** `listpools` reports `blocks_produced`"""
assert old in s, "operating doc anchor missing"
s = s.replace(old, new, 1)

old = """| `listpools` | every signer producing blocks: weight commanded, who lent it, what it has committed to paying, announced changes still inside their notice window, and blocks produced against blocks owed |"""
new = """| `listpools` | the declared pools: weight commanded, who lent it, what each has committed to paying, announced changes still inside their notice window, and blocks produced against blocks owed. `include_undeclared` adds the stakers producing for themselves |"""
assert old in s, "monitoring table anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("operating doc updated")

p = "doc/release-notes/release-notes-sequentia-24.3.0.md"
s = open(p).read()
old = """| `listpools` | every signer producing blocks, what it commands, and how reliably it produces |"""
new = """| `listpools` | the declared pools: what each commands, and how reliably it produces |"""
assert old in s, "release notes table anchor missing"
s = s.replace(old, new, 1)

old = """**`listpools` reports `blocks_produced` against `blocks_expected`**, the number"""
new = """**A pool is a signer that declared itself one**, by announcing a payout policy.
That is the only deliberate on-chain opt-in there is, and `listpools` lists
nothing else. Every chain has stakers producing for themselves; calling those
pools would put words in their mouth, and would drown the operators actually
asking for delegations. `include_undeclared` still shows them, each flagged, and
an explicit `signer` is always answered so a wallet can describe whichever signer
a stake is lent to.

**`listpools` reports `blocks_produced` against `blocks_expected`**, the number"""
assert old in s, "release notes body anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("release notes updated")
