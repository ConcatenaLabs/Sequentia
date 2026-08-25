# Sequentia Core 24.7.3

`sequentiad stop` could sit for ten minutes without exiting, holding the data
directory lock the whole time — long enough that reopening the wallet, or
restarting a node during a fleet cutover, failed with a lock error for no
apparent reason.

There is no consensus change here.

## The anchor watcher ignored shutdown once a tick had begun

The watcher's loop checked `ShutdownRequested()` between ticks and slept
interruptibly, so it looked responsive. Nothing checked it *inside* a tick, and
shutdown joins that thread — so a tick already running had to finish first,
however long it took.

Two parts of a tick take a long time, and both take longest on exactly the tick
most likely to be interrupted: the first one after a start.

- **The whole-chain anchor walk.** Every verdict the walk cannot serve from
  `g_anchor_ok_cache` is a round trip to the parent daemon, and that cache lives
  in memory only. On the first tick after a start it is empty, so the walk asks
  about *every distinct anchor on the chain* — around ten thousand of them on a
  100,000-block chain. Measured: **over ten minutes**, still running.
- **The checkpoint scan.** It pulls whole parent-chain blocks, and with no scan
  cursor yet it pulls the entire window — 100 Bitcoin blocks by default.
  Measured: **about 40 seconds** over a tunnelled RPC.

Both now check for shutdown each time round, and give up promptly. Measured on
the same 100,000-block chain: **stop to "Shutdown: done" in 9 seconds**, of
which 8 are one already-issued request finishing.

Abandoning a tick costs nothing. It re-derives every verdict from ground truth
each time and carries nothing across the point where it stops — which is why the
walk already abandoned itself partway whenever the parent daemon was
unreachable. The only loss is work the next tick, or the next start, does again.

Two details worth recording, because both are easy to get wrong:

- the checkpoint scan **returns** rather than breaking. Breaking would fall
  through to the line that records the new parent tip as scanned, when most of
  the window had not been, and those blocks' checkpoints would be skipped for
  good. Returning leaves the cursor alone and the next start rescans.
- the invalidation retry loop is checked at the top only, never between
  `InvalidateBlock` and `ActivateBestChain`. Those two are one step, and a
  shutdown wedged between them would leave a block invalidated with the chain
  never reactivated onto its replacement.

## Not fixed here

The reason that first tick is so expensive is that the anchor verdict cache does
not survive a restart, so every start re-asks the parent chain about the whole
history it already knew. That is worth addressing on its own, and it is a change
to what the node trusts across restarts rather than a shutdown fix, so it is not
bundled in here.
