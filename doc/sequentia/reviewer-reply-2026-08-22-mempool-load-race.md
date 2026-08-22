# You are right: #143 held on scheduler luck, and 24.5.2 makes it hold on ordering

Reply to the reviewer's letter of 22 August ("#143 does not hold"). Everything below was
reproduced here today, deterministically, and the fix is tagged as v24.5.2.

## The diagnosis, confirmed

Your reading of the code is exact. The sweep ran on the loadblk thread straight
after `LoadMempool`; the registry it needs was rebuilt on the init thread
afterwards; `removeStaleSupervision` returns on an empty registry because an
empty registry is also what a chain with no supervised assets looks like. When
the import finished first, the sweep had nothing to sweep and the comment above
it promised something the code could not deliver.

Why it passed here and not there: on this machine the rebuild beat the import by
59 microseconds (`registry loaded` at `.388093`, `Imported mempool` at
`.388152`), every run. That is not a margin, it is a coin that happens to land
the same way on one box. Forcing the other outcome — a 1.5 s delay inserted
before the rebuild — makes the 24.3.0–24.5.1 code fail the regression test on
every run, with your log signature exactly: `2 succeeded`, the registry arriving
later, and no eviction line after it.

## There was a second hole behind the first

Moving the sweep after the rebuild on the init thread was the obvious repair,
and it was not enough on its own: `SetRPCWarmupFinished()` sat *above* the
registry rebuild in `AppInitMain`. With the race forced, the sweep ran and
evicted the spend — and the test still failed, because RPC had answered
`getrawmempool` before the sweep ran. A client, or the node's own wallet, could
read and build on a pool the sweep had not cured yet, with the window as long
as whatever sat between warmup and the rebuild (the peg-in reachability check,
which can wait on an absent Bitcoin node).

## What 24.5.2 does

I took your second direction, with one change to its premise. The sweep now runs
on the init thread directly after `RebuildSupervisionRegistry`, under the same
`cs_main` hold — and it does not need the mempool to be loaded by then.
Acceptance takes `cs_main` too, so every entry the import admitted against the
empty registry is resident before this thread takes the lock, and every entry
it processes after the rebuild is refused by acceptance outright. Whichever
side wins, the invariant holds from the sweep onward.

And the whole block — rebuild and sweep — now runs *before* RPC leaves warmup,
so "RPC answers" implies "the pool is cured". The sweep on the loadblk thread is
gone; a sweep that does nothing in exactly the case it exists for is worse than
none, and the comment there now says why.

The reorder you describe first (rebuild before loadblk starts) was tried for
#143 and dropped: an early flush on a fresh chain killed the node at startup.
This placement stays after the loadblk start and the genesis wait, so that
hazard does not return; `wallet_basic`, the test that caught it then, passes
both wallet types.

Verification, both ways, on the same forced race: 24.5.1's code fails 4 of 4;
24.5.2 passes 6 of 6 with `Supervision: evicting …` logged on each, then 8 of 8
unforced, plus `mempool_persist`, the supervision suite and the PoS basics.

Your node: a first start on 24.5.2 clears whatever is resident. Expect one
`Supervision: evicting` line per dead spend in `debug.log`, and nothing else to
do.

## Checksums

Both of yours match what the page serves, fetched and hashed independently here:

| file | sha256 |
|---|---|
| `sequentia-core-24.3.0-linux-x86_64.tar.gz` | `199027a42e584ed31822f95f280f104076c92e094b97fd05122ecbde185ea1cb` |
| `sequentia-core-24.5.1-linux-x86_64.tar.gz` | `5b5827f3fc669ef542efeb4ada713a34aed0fed6f24a67651b53a2b4fc4f208e` |

From the 24.5.2 publish onward the publisher writes
`sequentiatestnet.com/download/SHA256SUMS` beside the artifacts, covering
everything the page serves, regenerated on every run:
`sha256sum --ignore-missing -c SHA256SUMS`.

And signed, from the run after that: `SHA256SUMS.asc` is a detached OpenPGP
signature, `sequentia-release-signing-key.asc` beside it is the public key. The
private half exists only on the build host. The fingerprint, pinned in the
repository at `contrib/sequentia/release-publisher/README.md` so you are not
trusting the same server for both the file and the key:

```
B4F5 7796 7E32 25D5 8FF6 3DFC 5974 FD59 E609 F11F
```

```
gpg --import sequentia-release-signing-key.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS
```
