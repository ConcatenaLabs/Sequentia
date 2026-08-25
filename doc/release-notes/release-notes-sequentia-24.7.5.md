# Sequentia Core 24.7.5

The anchor watcher asks the parent chain about many anchors per round trip
instead of one. On a 100,000-block chain the first tick after a start drops from
**296 seconds to 48**, and the anchor walk itself from thousands of requests to
seven.

There is no consensus change here, and no change to what the node verifies.

## What was slow

`CallMainChainRPC` opens a fresh connection per call and closes it again, so
asking N questions costs N connects, N authentications and N round trips. That
is invisible at N=1 and ruinous at the scale the anchor walk works at: the
verdict cache lives in memory only, so the first tick after a start needs a
verdict for every distinct anchor on the whole chain — 1,592 of them on the
testnet chain as it stands, and growing with it.

## What was considered and rejected

The obvious fix is to make the verdict cache survive a restart. It was rejected,
and the reason is worth writing down.

The cache can only ever record that an anchor **is** canonical — never that it
is not. A persisted entry therefore fails in exactly the forbidden direction: a
block whose anchor Bitcoin reorganised away while the node was down would be
waved through on the strength of a verdict that was true yesterday. Anchoring is
consensus law here, and "probably still true" is not a basis for it.

There is a sound version — persist the parent tip alongside the verdicts, spend
one RPC at startup working out where the parent's chain still agrees, drop
everything above that, and drop everything when the move cannot be classified.
That is the same argument the running node already relies on to survive a missed
or coalesced parent reorganisation. But it buys speed by making anchoring
correctness depend on one more thing being right forever, and there is a way to
have the speed without spending anything at all.

## What was done instead

The verification is unchanged and unchanged in scope: every anchor, every tick,
from ground truth, to any depth, with no floor and nothing remembered across a
restart. Only the transport changed.

The walk already builds its whole list before it asks the parent anything, which
makes it a natural batch. `CallMainChainRPCBatch` sends one HTTP request
carrying many calls, and the walk pre-fills its caches from the result before
running exactly as it did before.

Two properties worth stating:

- **replies are matched by id, never by position.** A JSON-RPC batch may come
  back in any order, and pairing an answer with the wrong question here would
  judge one anchor by another anchor's verdict and then invalidate a block on
  the strength of it. Any id that is missing, repeated or out of range throws
  rather than being guessed at, and the matching is a pure function with tests
  that feed it reversed and shuffled replies.
- **it is best-effort.** Anything that goes wrong in the batch leaves the caches
  untouched and the per-anchor path re-asks the daemon the old way. A batching
  failure costs speed, never correctness.

A batch is also a single item in the parent daemon's work queue rather than
hundreds, so this reduces pressure on `rpcworkqueue` rather than adding to it —
which matters when a whole committee restarts at once and points all of it at
one gateway.

## What is now the slow part

Of the 48 seconds that remain, most is the checkpoint scan, which pulls up to a
hundred whole parent-chain blocks one at a time on its first pass. It is
sequential by construction — each block names its predecessor — so batching it
means restructuring how it walks, which is a change to checkpoint scanning
rather than to transport, and is deliberately not bundled here.
