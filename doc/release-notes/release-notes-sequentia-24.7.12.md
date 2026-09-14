# Sequentia Core 24.7.12

A registry fix that keeps every node's view of the committee identical
through a reorg. It changes what a running node believes about committee
membership in one situation, so the fleet should move to it together rather
than one node at a time.

## What was wrong

A staker's committee BLS key rides in a staking output. The registry dropped
the key only when the staker's *last* output of any kind was gone. A staker
can hold outputs without the key beside one that carries it, which is exactly
how a stake registered without a key is repaired: by funding one more output
that has it. When that registered output was then spent, or reorged away by a
Bitcoin reorg, a running node kept the key while the weight of the other
outputs remained; a node started afterwards rebuilt the registry from the UTXO
set and did not have it. The two then disagreed on who sits on the committee,
and so on which certificates are valid, until the output was mined again.

This was reached on the testnet on 2026-09-14: a repair registration confirmed
at height 134289 was reorged out by a 115-block Bitcoin-driven reorg while the
staker's original output stayed.

## What changed

The registry counts, per staker, the unspent outputs that carry its key, and
drops the key when the last of those leaves, whether by spend or by reorg. A
rebuild from the UTXO set produces the same count, so running and restarted
nodes agree at every height.

## Deployment

No activation height: the rule was always that the registry is a pure
function of the UTXO set, and this makes the incremental path honour it.
Nodes on 24.7.11 and 24.7.12 differ only while a registered output is
reorged out beside a BLS-less one, so switch the committee over in one step,
the way every committee upgrade is done.
