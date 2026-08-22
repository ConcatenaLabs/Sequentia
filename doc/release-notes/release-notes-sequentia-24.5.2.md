# Sequentia Core 24.5.2

No consensus change. One fix, to a fix: the `mempool.dat` sweep that 24.3.0
introduced held or failed on scheduler timing, and this release makes it hold.

## A pause-invalidated spend could survive `mempool.dat` loading after all

24.3.0 added a sweep of the loaded mempool against the supervision registry, so
that a node restarting with a `mempool.dat` written before a pause confirmed —
or by a build that predates supervision eviction — would not come back up
holding spends the registry forbids. The sweep ran on the import thread,
immediately after the load. The registry it needs is rebuilt on the init
thread, later. When the import finished first, the sweep found an empty
registry, took that to mean a chain with no supervised assets, and did nothing.

Which side won was down to the scheduler: the regression test written for the
sweep passed 8 of 8 on one machine and 4 of 8 on another, with the same code.
A node on the losing side kept every invalidated spend resident across every
restart, re-announced them on request, and let its wallet chain children onto
their change — the retention behind the 19 August incident, still present on a
binary that carried the fix.

Two things change:

* The sweep now runs on the init thread, right after the registry rebuild, under
  the same `cs_main` hold. Acceptance takes `cs_main` too, so every entry the
  import admitted against the empty registry is resident by the time the sweep
  runs, and every entry it processes afterwards is refused by acceptance
  outright. The ordering no longer matters.
* The rebuild and sweep now run **before RPC leaves warmup**. They used to run
  after it, so a client — or the wallet — could read and build on a pool the
  sweep had not cured yet. Once RPC answers, the invariant holds.

Verified both ways: with the race forced against the sweep (a delay inserted
before the rebuild), the previous code fails the regression test on every run
and this code passes on every run, with the eviction logged each time.

**If you run a node that has restarted on 24.3.0 through 24.5.1 since a pause
you hold a spend of**, this release clears it at first start; look for
`Supervision: evicting` in `debug.log`. Nothing else is needed.

## Checksums on the download page

Every publish now writes a `SHA256SUMS` file beside the artifacts at
`sequentiatestnet.com/download/SHA256SUMS`, covering everything the page
serves. Verify a download with `sha256sum --ignore-missing -c SHA256SUMS` in
the directory you saved it to.
