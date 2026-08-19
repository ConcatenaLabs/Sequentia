# Sequentia Core 24.3.0

Two things: a consensus change with a flag day on the live testnet, and delegated
staking becoming usable from a wallet.

**If you run a node against the live testnet, upgrade before height 101,750.** The
consensus change below activates there, and a node that has not upgraded by then
forks off.

## The consensus change: four weight units of Simplicity budget per witness byte

A Simplicity program whose static cost bound exceeds what its own witness pays for
cannot be spent at all, so the only way to buy the difference was to carry inert
bytes. Real covenants hit this at once: the OpenDAMP verifier's functional witness
is about four kilobytes against a cost bound of twenty thousand weight units, so
under Elements' one-weight-unit-per-byte rule it hauled 22,528 bytes of padding
whose entire purpose was to be counted, 84% of its witness. That transfer measured
7,459 vB against 89,999 vB of block payload: twelve per block.

`SIMPLICITY_BUDGET_PER_WITNESS_BYTE = 4` removes the padding. The same transfer is
1,582 vB, fifty-six per block.

Four is argued against the block, which is what actually binds: total Simplicity
cost in a block is bounded by the block's weight times this multiplier, so a
Sequentia block at 400,000 WU admits at most 1,600,000 WU of Simplicity work.
Liquid, at a 4,000,000-weight block and a multiplier of one, already accepts
4,000,000, so Sequentia at four stays at 40% of the per-block validation cost the
interpreter this code came from has always allowed.

### The flag day: height 101,750 on the testnet

The rule only ever accepts more, which is why it needs no activation gate for
*correctness*. It is still a hard fork for a running network, and ungated it would
be an **unscheduled** one: it fires the instant anyone broadcasts a spend the old
budget cannot pay for, and the date is set by whoever first runs the new tooling
rather than by anyone who gets to plan for it.

`consensus.simplicity_budget4_height` therefore gates it on **height and the genesis
hash** of the chain the flag day was written for, so a re-genesis drops the delay
instead of waiting out a height that no longer means anything. Only `test` carries
one; regtest, mainnet and every custom chain keep the rule from genesis.

Before the height this binary enforces the old budget, so upgrading early is safe
and cannot split the chain. After it, a node that has not upgraded forks off.

Because this is a relaxation, the new script flag deliberately stays out of
`STANDARD_SCRIPT_VERIFY_FLAGS`: before the flag day that would make mempool policy
more permissive than consensus, and the mempool would fill with spends no block will
accept. Policy reads the same height gate consensus reads.

Also: an annex is now standard on a Simplicity leaf spend and nowhere else, capped
at 100,000 bytes, policy only. It is the one place a program can be given budget
without also being given bytes it must read, and Simplicity's `sigAllHash` commits
to it, so a relay node cannot strip it without invalidating the spend.

## Delegated staking, from a wallet

The primitives for staking pools shipped in 23.3.5: a delegation record lends a
staker's block-signing weight to a pool without moving the coins, and a payout
record commits a producer to how its blocks pay out, behind a notice period. What
never shipped was any way to use them. Delegating meant taking a script from
`getdelegationscript` and hand-assembling a payment to a bare output. Five RPCs
close that:

| RPC | |
|---|---|
| `delegatestake` | fund a delegation record. Called again with another signer it **re-points**: the old record is spent and the new one created in one transaction |
| `undelegatestake` | spend the record back. Unilateral, no lock, no notice |
| `announcepayout` | the operator side. Refuses an activation inside the notice period rather than letting the node reject the block later |
| `listdelegations` | where this wallet's stake signs, and what the pool holding it has committed to |
| `listpools` | the declared pools: what each commands, and how reliably it produces |

None of this changes consensus. Every rule these work within was already enforced,
so old and new nodes agree on validity.

**Re-pointing has to be one transaction.** Consensus permits at most one live record
per controller, so reclaiming and re-delegating as two loose transactions could be
mined in the order that leaves two live records and invalidates the block carrying
the second.

**`listdelegations` is the delegator's watch.** A pool's payout policy cannot change
without being announced a notice period in advance, but that only protects a
delegator who *sees* the notice. Every announced-but-not-yet-binding change against
this wallet's stake is now reported with the blocks remaining and a plain warning.
Since leaving is instant and unilateral, seeing it is the entire protection.

**A pool is a signer that declared itself one**, by announcing a payout policy.
That is the only deliberate on-chain opt-in there is, and `listpools` lists
nothing else. Every chain has stakers producing for themselves; calling those
pools would put words in their mouth, and would drown the operators actually
asking for delegations. `include_undeclared` still shows them, each flagged, and
an explicit `signer` is always answered so a wallet can describe whichever signer
a stake is lent to.

**`listpools` reports `blocks_produced` against `blocks_expected`**, the number
nobody advertises: a pool holding a tenth of the weight should produce about a tenth
of the blocks, and well under that means its delegators are earning nothing for what
they lent.

The desktop wallet's Staking tab gains both sides. A **Staking pool** card picks a
pool from the live listing, delegates, reclaims, and shows any pending policy change
as a banner. A **Run a staking pool** card announces a payout policy; that one is
node-only on purpose, because running a pool means being online with the signing key
on the machine that produces the blocks, which a browser or phone wallet cannot
promise.

`listpools` is also a published contract: it feeds the public pool board at
[sequentiatestnet.com/pools/](https://sequentiatestnet.com/pools/), whose source is
in its own repository, and `feature_pos_pools.py` asserts every field that page
renders.

### One thing worth knowing before you delegate

A delegation record pays the fee to spend itself out of its own value, so
`delegatestake` refuses an amount below the dust floor **plus** one spend fee. Dust
here is tens of atoms while that fee is thousands, and a record funded in the gap
between them would relay, delegate correctly, and then never be reclaimable.

## Upgrading

Stop the node, replace the binary, start it again. Committee operators upgrade every
node in one pass, as with any consensus release: mixed binaries select different
anchors and fragment share-locks.

Do it **before height 101,750** on the testnet, then confirm behaviourally rather
than by version alone:

    contrib/sequentia/peer-stall-check.sh

Every peer should report as following the chain. A peer frozen just below a
consensus-change height is running a binary without that rule, whatever version it
reports.

### The inclusion probe gates the cutover, not the activation

Before stopping anything, have an independent operator broadcast a fresh
transaction funded from confirmed coins, with the fee in tSEQ, and watch it
confirm within three blocks. Run it *after* the new binary is built on the box
and *before* the committee is stopped.

That ordering is the whole point. A probe that gates the activation leaves the
chain committed with nothing to unwind if it fails; a probe that gates the
cutover fails while the old binaries are still running and still producing, and
costs a delay rather than an intervention. If the operator running the probe is
unreachable inside the window, whether to proceed is a human decision, not one
for whoever happens to be executing the cutover.

### If the flag day has to be held after the cutover

The height is a single constant, so a hold is deliberately cheap and is written
down here rather than improvised:

1. `consensus.simplicity_budget4_height` in `CTestNetParams`, re-derived as
   `round_up_to_10(tip + 1440)`.
2. The matching bounds in `src/test/simplicity_policy_tests.cpp`, and the three
   heights in this file.
3. Tag `v24.3.1`, rebuild, and run the same full cutover.

It is a second stop/start of the whole committee inside the notice window, so it
is a decision for the maintainers rather than for an operator or an agent acting
alone. Nothing about it is exotic; the point of writing it out is that it is
followed rather than invented under time pressure.
