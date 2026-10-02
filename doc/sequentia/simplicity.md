# Simplicity on Sequentia

This page is for developers writing Simplicity programs for the Sequentia
network: how the node validates a Simplicity spend, what it charges for one,
where Simplicity is active, and the mistakes that lose money.

## What Simplicity is here

Simplicity is a typed, non-Turing-complete language for spending conditions.
A program is a DAG of combinators and *jets* (native implementations of common
functions: arithmetic, hashing, signature checks, transaction introspection).
Its cost is bounded statically before it runs, over the whole program, so a
node knows the worst case of every spend before executing it.

The node vendors the Elements Simplicity interpreter and its jets unchanged in
`src/simplicity/` (a git subtree; `doc/simplicity-c-code-update.md` describes
how it is updated). Only the Elements half is compiled. Sequentia changes how
much execution a spend may buy and where the annex is standard, both described
below; the language, the jets and their costs are those of Elements.

The node validates Simplicity spends and does nothing else with them. The
wallet cannot build, sign or spend a Simplicity output: `tr()` descriptors add
every leaf as tapscript, and the signer refuses any leaf version other than
tapscript (`src/script/sign.cpp`). Programs are written in SimplicityHL and
compiled, satisfied and pruned outside this repository; transactions spending
them are assembled by hand and submitted as raw transactions.

## The output and the witness

A Simplicity program lives in a taproot leaf with leaf version `0xbe`
(`TAPROOT_LEAF_TAPSIMPLICITY` in `src/script/interpreter.h`), beside tapscript's
`0xc4`. The leaf's "script" is the program's 32-byte commitment Merkle root
(CMR), not the program, and the leaf hash is the Elements tagged hash
`TapLeaf/elements(0xbe || 0x20 || CMR)`. The same taptree may mix Simplicity
and tapscript leaves.

A script-path spend of a Simplicity leaf carries exactly this witness stack,
bottom to top:

| Item | Content |
|---|---|
| 1 | the witness: the bit-packed values of the program's `witness::` names |
| 2 | the program, pruned to the branches this spend takes |
| 3 | the CMR, 32 bytes |
| 4 | the control block |
| 5 (optional) | the annex: a last item whose first byte is `0x50` |

With the annex removed, the stack must have exactly four items and the leaf
script exactly 32 bytes, or the spend fails with `SCRIPT_ERR_SIMPLICITY_WRONG_LENGTH`
(`src/script/interpreter.cpp`, the `TAPROOT_LEAF_TAPSIMPLICITY` branch of
`VerifyWitnessProgram`). The node then decodes the program, checks that its
root matches the CMR, type-checks it, fills the witness values (trailing bytes
or bad padding are refused), and runs it under the budget.

Pruning is part of building the spend, not an optimisation: a program that
still contains a `fail` node (what `panic!` compiles to) does not decode. The pruned bytes depend on
which branches the transaction takes, so the final witness size is known only
once the transaction is.

The program's environment includes the chain's genesis hash, so a signature
over `sig_all_hash` is bound to this chain as well as to the transaction.

## The budget

A program is accepted only if its static cost bound fits the budget its own
input pays for. The budget, in weight units, is

    budget = min(4 × serialized witness stack bytes + 50, 4,000,050)

(`SIMPLICITY_BUDGET_PER_WITNESS_BYTE`, `VALIDATION_WEIGHT_OFFSET` and
`SIMPLICITY_BUDGET_MAX` in `src/script/script.h`). The serialized size is that
of the whole witness stack of the input: the item count, every length prefix,
the witness, the program, the CMR, the control block and the annex. The cost
bound is computed over the whole pruned program, untaken `case` branches
included, in milli weight units; it must be at most `budget × 1000`. Nothing is
metered at run time, and a witness larger than the program needs is not
refused.

Worked example, measured on a regtest node: a looping program with a cost bound
of 15,664,298 milli-WU (15,665 WU) was refused with a 3,903-byte witness stack
(budget 4 × 3,903 + 50 = 15,662 WU) and accepted with 3,904 bytes (budget
15,666 WU). The refusal reads
`mempool-script-verify-flag-failed (Program's execution cost could exceed budget)`.
A single-signature program needs no help: its 178-byte stack buys 762 WU
against a bound of about 53 WU.

The multiplier of four is set against the block: a 400,000 weight unit block
admits at most 1,600,000 weight units of Simplicity work. The comment on the
constant in `src/script/script.h` gives the reasoning. On the live testnet the
multiplier applies from height 101,810 (`simplicity_budget4_height` in
`src/chainparams.cpp`, bound to that chain's genesis); on every other chain,
mainnet parameters included, it applies from genesis.

## The annex

A program whose cost exceeds what its witness buys can buy the difference with
an annex: bytes the program need not read, counted in the budget like any other
witness byte.

- An annex is standard only on a Simplicity leaf spend, up to 100,000 bytes
  (`MAX_STANDARD_SIMPLICITY_ANNEX_SIZE`, `src/policy/policy.h`), which buys
  400,000 weight units. One byte more is refused by relay with
  `bad-witness-nonstandard`; it is still valid in a block. An annex on a
  key-path spend or a tapscript leaf remains non-standard.
- The annex is part of the transaction environment the program sees
  (`jet::input_annexes_hash`) and is committed to by `sig_all_hash`, so a relay
  node cannot strip it without invalidating a signed spend.
- The node passes the annex to the program; the `simplicity-lang` Rust library
  used by SimplicityHL tooling passes none. A program that reads the annex, or
  that checks a signature over a `sig_all_hash` computed by that library, will
  therefore disagree with the node once an annex is attached: a spend the Rust
  Bit Machine refuses can be valid on chain, and a signature it produced can be
  refused. Compute `sig_all_hash` yourself if you attach an annex to a signed
  spend.
- Relay weight is capped at 400,000 (`MAX_STANDARD_TX_WEIGHT`). Budget beyond
  what a 100,000-byte annex buys has to come from the other witness bytes: the
  program and witness data it consumes, since a witness with bytes the program
  does not read is refused.

## Activation

Simplicity is a version-bits deployment named `simplicity` on bit 21. On
`sequentia`, `test` and custom chains the deployment's start and timeout are
block heights, not times. A block enforces Simplicity when the deployment is active after its
parent.

| Chain | Setting |
|---|---|
| `sequentia` (mainnet parameters) | always active, from genesis |
| `test` (the live testnet) | BIP9 by signalling: start height 0, period 144, threshold 108, no timeout |
| custom chains, `elementsregtest` included | never active, unless set on the command line |
| `regtest`, `main`, `signet` | never active |

`getdeploymentinfo` reports the state. On the testnet, `bip9.since` under
`deployments.simplicity` is the height from which it is enforced. On a chain
where the deployment is never active, `simplicity` is missing from the list
altogether.

To turn Simplicity on from genesis on a custom chain, start the node with

    -evbparams=simplicity:-1:::

(`-vbparams=simplicity:-1:1` does the same). `getdeploymentinfo` then reports
`"active": true, "height": 0`. The format is
`deployment:start:timeout:period:threshold`, and a start of `-1` means always
active.

A start of `0` does not mean "from genesis". `-evbparams=simplicity:0:::` only
begins BIP9 signalling under the custom-chain period and threshold of 128: the
deployment is `defined` to height 127, `started` from 128, `locked_in` from 256
if all 128 blocks of that period signal, and `active` from height 384. Until
then the chain does not enforce Simplicity at all.

Before funding any Simplicity output, check that `getdeploymentinfo` reports
`simplicity` active on that chain.

## Traps

**An unactivated chain.** Where Simplicity is not enforced, a `0xbe` leaf is an
unknown leaf version, and an unknown leaf version is anyone-can-spend by
consensus. Measured on `elementsregtest` started with
`-evbparams=simplicity:0:::`: at height 201 a spend of a one-key program with a
one-byte garbage program and an empty witness, no key involved, was refused by
the mempool but mined by `generateblock`. The same spend at height 401 was
refused in a block.

**The mempool applies Simplicity as policy, so only a block proves a rule.**
`SCRIPT_VERIFY_SIMPLICITY` is in the standard script flags unconditionally
(`STANDARD_SCRIPT_VERIFY_FLAGS`, `src/policy/policy.h`), so the mempool runs
every `0xbe` leaf as Simplicity even where consensus does not; that is why the
garbage spend above was refused by relay and accepted in a block. A mempool
refusal therefore says nothing about consensus. Prove every negative case by
forcing the transaction into a block with `generateblock` and a raw
transaction. A block refusal reports only `block-validation-failed`; the
script-level reason (`Assertion failed inside jet`, `Assertion failed`,
`Program's execution cost could exceed budget`, and the like) appears only in
the mempool's answer.

**The four relative-timelock jets do not bind the input being spent.**
`check_lock_distance`, `check_lock_duration`, `tx_lock_distance` and
`tx_lock_duration` compare against the largest relative lock carried by *any*
input of the transaction, not the current one; the node's sources name them
`BROKEN_DO_NOT_USE_*` (`src/simplicity/elements/decodeElementsJets.inc`) and
keep them only for consensus compatibility. They still decode and run,
and SimplicityHL tooling may expose them under their plain names without a
warning. Measured: a coin locked by `check_lock_distance(10)` was spent one
block after it was created, its own `nSequence` final, because a second input
(an old wallet coin) carried `nSequence = 10`. The same bypass works against
`check_lock_duration`.

The safe pattern reads the current input's own sequence and requires a version
under which BIP 68 binds it:

```rust
fn main() {
    assert!(jet::le_32(2, jet::version()));
    let d: u16 = match unwrap(jet::parse_sequence(jet::current_sequence())) {
        Left(blocks: u16) => blocks,
        Right(units: u16) => panic!(),
    };
    assert!(jet::le_16(param::N, d));
}
```

Consensus then enforces the age of this input itself. The bypass above is
refused by it in the mempool and in a block, as are a version-1 spend and a
lock of the other kind (swap the `Left` and `Right` arms for a time lock in
units of 512 seconds); a spend before the lock matures is refused as
`non-BIP68-final` (mempool) and `bad-txns-nonfinal` (block). The
`version >= 2` check is not optional: under version 1 consensus does not
enforce `nSequence` at all.

**`check_lock_height` and `check_lock_time` need a non-final input.**
`tx_lock_height` and `tx_lock_time` return `nLockTime` only when at least one
input has `nSequence` below `0xffffffff`; otherwise they return 0, as
consensus ignores `nLockTime` in that case.

**`lbtc_asset` is Liquid's asset, on every chain.** The jet returns a
constant, the Liquid L-BTC asset id (`simplicity_lbtc_asset` in
`src/simplicity/elements/elementsJets.c`), whatever chain it runs on. On
Sequentia that is no asset at all. Fees here are payable in any accepted
asset, so a program that cares about the fee must read it from the transaction
(`output_is_fee` tells whether an output is a fee output; `total_fee` takes the
asset id to total), and a program that wants a particular asset must name its
id as a parameter.

**Asset ids are in internal byte order.** Every jet that reads or returns an
asset id uses the byte order of the serialized transaction, the reverse of the
hex the RPC interface prints.

## Tests in this repository

- `src/test/simplicity_policy_tests.cpp`: the budget arithmetic, the clamp, the
  testnet flag day and annex standardness.
- `src/test/sequentia_chainparams_tests.cpp`: pins activation on `sequentia`
  and `test`.
- `test/functional/feature_elements_simplicity_activation.py`: the BIP9 state
  machine on a custom chain.
- `test/functional/feature_taproot.py`: a minimal Simplicity spend among the
  taproot cases, run with `-evbparams=simplicity:-1:::`.
