# Sequentia Core 24.7.7

A checkpoint scan that was cut short recorded the whole window as read, and the
parent blocks it never reached were then skipped for good.

There is no consensus change here.

## The bug

The scan walks a window of parent-chain blocks down from the tip, looking for
committed PoS checkpoints, and afterwards records the tip it scanned from. The
next pass starts at the new tip and stops when it reaches that mark.

If a block could not be fetched — a parent daemon that stopped answering for a
moment is enough — the walk broke out of its loop, and then recorded the mark
anyway. The blocks below the break had never been read, but the next pass would
stop at the mark and never go back for them. Any checkpoint committed in those
blocks was lost, silently, and the only trace was one line about a failed fetch.

The same hazard was already understood for shutdown: the check added in 24.7.3
deliberately *returns* rather than breaking, with a comment explaining that
breaking would fall through and record the mark. The break on a fetch failure,
a few lines further down, did exactly that.

## The fix

A pass now says how it ended, and only a pass that ran to completion moves the
mark. Cut short by shutdown, by a parent that stopped answering, by anything —
the mark stays where it was and the next pass covers the same ground again.

Being wrong in this direction costs a rescan. Being wrong in the other direction
loses checkpoints permanently, which is why the two are not a trade.

## On making that scan faster

It was also batched — fetching the window's hashes by height in one request,
then the blocks in chunks — and then removed again, because it did not earn its
place. The numbers, since they are the point:

- Against a parent on **loopback**, which is what a node running beside its own
  bitcoind has, a hundred blocks takes **0.83 s** fetched one at a time and
  **0.18 s** batched. Both are already nothing.
- Where the scan does take tens of seconds, the constraint is **bandwidth, not
  round trips**: the window is a few megabytes of block data, and a node reading
  its parent over a slow or tunnelled link pays for those megabytes however they
  are requested. Batching moves none of it.

So the saving was 0.65 s on the machines that matter, in exchange for the code
needed to prove that a window addressed by height really is the ancestry of the
tip it claims to descend from. The sequential walk stays, and the reasoning is
recorded above it so the next person measures before rewriting it.
