# Sequentia Core 25.2.3

25.2.3 is 25.2.2 plus two fixes in how the node reports a script it cannot
check or that fails. It changes no consensus rule: every block and transaction
25.2.2 accepts, 25.2.3 accepts, and the reverse. The fork at height
**163,000** is the same.

## Signing a transaction with a Simplicity input

`signrawtransactionwithwallet` and `signrawtransactionwithkey` stopped the node
when handed a transaction with an input whose parent the node does not know
beside an input already carrying a Simplicity witness. The second RPC needs no
wallet, so anyone with RPC access could stop the node this way.

A Simplicity program may read the spent output of every input, so it can only be
run when the node knows all of them. The RPCs now report the unknown input as
`Input not found or already spent` and the Simplicity input as
`Simplicity input cannot be checked without the spent output of every input`,
sign what they can, and the node keeps running. `signrawtransactionwithkey`
takes the missing outputs in its `prevtxs` argument, and then checks the
program.

## Script failures in a block

A block refused because a transaction's script fails was reported as
`mempool-script-verify-flag-failed (…)`, which reads as relay policy. The block
was checked under consensus rules, and the reason now says so:
`mandatory-script-verify-flag-failed (…)`, with the script error consensus
found. A transaction the mempool refuses is still reported as
`mempool-script-verify-flag-failed (…)`, under the mempool's stricter flags,
whose script error can differ.

The script error is named only when scripts are checked on the validating
thread (`-par=1`); with parallel checks a block's script failure reads
`block-validation-failed`, as before.

Tools that match a block's refusal by its whole reason, such as
`TestBlockValidity failed: mempool-script-verify-flag-failed (…)` from
`generateblock`, need the new label.

## Upgrading

Stop the node, replace `sequentiad`, `sequentia-cli` and `sequentia-qt`, start
it. Check that `sequentiad -version` reports v25.2.3.
