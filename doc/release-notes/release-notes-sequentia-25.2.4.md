# Sequentia Core 25.2.4

25.2.4 is 25.2.3 plus one relay policy rule: an annex on a Simplicity spend
relays only when the program needs it, and only as large as it needs. It
changes no consensus rule: every block 25.2.3 accepts, 25.2.4 accepts, and the
reverse. The fork at height **163,000** is the same.

## An annex on a Simplicity spend

A Simplicity spend buys execution budget with its witness bytes, four weight
units a byte, and an annex (a last witness item tagged `0x50`) is how a program
buys budget without carrying bytes it has to read. Relay took any annex up to
100,000 bytes on a Simplicity leaf. Only a program that signs `sig_all_hash`
commits to its annex, so on a spend that signs nothing, a SeqDEX order fill for
one, whoever relayed the transaction could append an annex under the same txid.
A fill of 930 vB grew to 25,932 vB, its fee rate fell from 6,445 to 231 atoms
per 1,000 vB, and a node holding that copy kept it when the honest one arrived,
so the spend sat at the floor fee rate.

Relay now takes an annex on a Simplicity leaf only when every byte after the tag
is zero and the program's cost bound exceeds the budget of the same witness with
the annex one byte shorter, or with none when the annex is the tag alone. A
spend has one standard witness size: no annex when its witness pays for the
program, else the smallest annex that does. Any other annex is refused with

    mempool-script-verify-flag-failed (Simplicity annex is larger than the program's cost needs)
    mempool-script-verify-flag-failed (Simplicity annex must be zeros after its tag)

An annex above 100,000 bytes is still refused with `bad-witness-nonstandard`.
Blocks are not affected: a block may carry any annex, as before.

A wallet that pads a program does so with the smallest zero annex that covers
the program's cost at four weight units a byte, counting the annex's length
prefix, and signs after. SWK's contract signer and the Simplex SDK fork already
do. A tool written for Elements' one weight unit a byte pads about four times
too much, and its spend is refused. `doc/sequentia/simplicity.md` gives the
rule and the arithmetic.

The rule applies with `-acceptnonstdtxn=1` too, as every standard script check
does. A test that relays a Simplicity spend with an annex it does not need must
mine it with `generateblock` instead.

## Upgrading

Stop the node, replace `sequentiad`, `sequentia-cli` and `sequentia-qt`, start
it. Check that `sequentiad -version` reports v25.2.4. Nodes on 25.2.3 and 25.2.4
agree on every block; a node still on 25.2.3 relays the annexed copies this rule
refuses, so the protection is complete once the nodes a transaction passes
through have upgraded.
