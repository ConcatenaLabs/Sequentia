# Sequentia Core 25.2.1

25.2.1 is 25.2.0 plus a fix to how a node treats block times when clocks
disagree. It changes no consensus rule: 25.2.0 and 25.2.1 agree on every block,
and the fork at height **163,000** is the same. Run 25.2.1 before 163,000 if you
can; a node already on 25.2.0 is not at risk of a split.

## Clocks and block times (audit A12)

Consensus accepts a block stamped up to two hours after the receiving node's
clock, and every following block must be stamped after it. So a producer whose
clock runs fast stamps its blocks ahead of real time, and the network has to
live with that stamp.

- **Producers no longer wait for a stamp in the future.** A producer counted the
  minimum spacing and its slot from its parent's stamp, so after a parent stamped
  ahead of its clock it waited, up to two hours, for its clock to catch up. It
  now counts them from when the parent arrived, and keeps producing at the
  usual pace.
- **The committee keeps block times close to real time.** A committee member
  whose clock agrees with its peers' (within a minute) does not back a proposal
  stamped more than five minutes ahead of both its clock and the earliest time
  consensus allows. A member whose clock disagrees with its peers' backs
  proposals as before, so a wrong clock never takes a member out of the
  committee; it only turns this check off for that member.
- **A node says when clocks disagree.** A warning (log, `getnetworkinfo`, GUI)
  appears when the node's clock is more than a minute off its peers', or the
  newest block is stamped more than a minute after the node's clock. Setting the
  system date, time and time zone right clears it.

## Upgrading

As for 25.2.0: stop the node, replace `sequentiad`, `sequentia-cli` and
`sequentia-qt`, start it. Check that `sequentiad -version` reports v25.2.1.
