# Testing the open fee market

Sequentia lets a transaction pay its fee in any asset a producer is willing to
price. That makes block space an auction denominated in a common reference unit
rather than a queue ordered by satoshis, and it introduces a kind of disagreement
Bitcoin does not have: **two honest producers can value the same fee differently,
or refuse to value it at all, and must still build the same chain.**

None of that is observable on an idle chain. While there is room in the next
block, every transaction that clears the relay floor is confirmed, whatever it
pays and in whatever asset — so a wallet, a fee estimator or a block assembler can
be badly wrong and every test still passes. The fee market only exists when the
next block is full.

This page describes the standard suite that makes it full on purpose, what each
case asserts and why, and how much of it can be reproduced against a shared
testnet.

---

## 1. The suite

`test/functional/feature_any_asset_fee_market.py`. Run it like any other
functional test:

```bash
test/functional/feature_any_asset_fee_market.py
```

Or as part of a group:

```bash
test/functional/test_runner.py feature_any_asset_fee_market feature_any_asset_fee_congestion
```

It needs a binary built with `--enable-any-asset-fees` **and** with Berkeley DB.
Without BDB the framework falls back to descriptor wallets, the
`anyonecanspendaremine` exception that makes the genesis coins spendable does not
apply, and the suite fails in seconds with `Insufficient funds` — a wallet-format
problem wearing the costume of a fee problem. See `CLAUDE.md` for the configure
line.

### How the wall is built

The block is shrunk rather than the load raised. `-blockmaxweight=8000` leaves
4000 of weight — about 1 kvB — for transactions once the assembler has reserved
its 4000 for the coinbase, so three or four ordinary transactions are a full
block. The auction becomes visible with no load on the machine at all, and a run
takes under a minute.

Two details that are easy to get wrong, and that the suite handles:

- **The limit is applied by restarting.** `-blockmaxweight` is a startup option.
  Funding the wallets and creating the working set under the small limit would
  congest the setup itself, so all of that happens at full size first.
- **The capacity is measured, not assumed.** The suite builds one filler
  transaction, reads its *discount* size — which is what the mempool ranks and the
  assembler counts — and derives how many fit. A change to transaction shape
  therefore cannot silently invalidate the arithmetic.

### The two nodes

`node0` is **producer A**: it holds the only funded wallet, builds every
transaction under test, and has `-blockmintxfee=0`.

`node1` is **producer B**: the same chain, a different opinion. It prices one
asset that A does not price at all, prices another eight times higher than A, and
sets `-blockmintxfee` to a floor of its own. It exists so that every "who decides"
question has two answers to compare.

### Fee rates in this suite

Fee rates are given in **reference atoms per vB** — `fundrawtransaction`'s
`fee_rate` on a chain with the open fee market, where `CURRENCY_ATOM` is `rfa`.
They are *values*, not amounts: two transactions given the same rate pay the same
worth, however many atoms of their own asset that takes. That is the whole point,
and it is why the suite can compare a fee in one asset with a fee in another.

| name | rate | meaning |
|---|---|---|
| `WALL_RATE` | 100 | what every filler transaction pays; the price of entry |
| `UNDER_WALL` | 30 | clears the relay floor, loses the auction |
| `OVER_WALL` | 400 | jumps the queue |
| `UNDER_PRODUCER_FLOOR` | 3 | clears relay, under producer B's `-blockmintxfee` |

---

## 2. The cases

### 1. An asset no producer prices — never confirms

A wallet whose own price server lists an asset builds a perfectly well-formed
transaction paying its fee in it. Every producer values that fee at **zero**, so
it does not merely lose the auction: it never reaches a mempool at all, and is
rejected as `min relay fee not met`.

This is the failure an operator will actually meet, because the wallet and the
producers get their prices from different servers. The suite reproduces it
honestly — it sets the rate, builds the transaction, *removes* the rate, and only
then offers it — rather than by constructing something the wallet would never
produce.

There is no privileged asset here, the policy asset included: an asset absent from
a node's whitelist is not accepted by that node, full stop.

### 2. A listed asset bidding under the wall — waits, then confirms in the gap

Under the wall is **queued, not rejected**, and the difference matters: a test
that only asserted "not in the next block" would pass equally against a node that
had dropped the transaction on the floor.

So the suite mines five consecutive full blocks, asserting each time both that the
transaction is absent from the block *and* that it is still in the mempool. Then
it stops topping the queue up and mines until the transaction lands, asserting
that the block it landed in was **not** full.

That last assertion is the interesting one. Real congestion comes in waves; a
chain of nothing but full blocks would never confirm an under-bidder at all, and
"never" is not the behaviour under test. The suite therefore reproduces the trough
of a wave rather than a permanent siege.

### 3. Under the producer's own floor — not even in an empty block

**Yes, this parameter exists: `-blockmintxfee`.** It is the producer's reserve
price, in `RFU/kvB`, applied in `BlockAssembler` against the fee's *value*, so it
is asset-agnostic by construction. It defaults to 100 rfa/kvB.

It is a different thing from `-minrelaytxfee`, and the distinction is the whole
case: relay uses the relay floor, so a transaction under the producer's floor is
accepted into every mempool and propagates normally. It is simply never mined by
that producer. The suite asserts that producer B leaves the block **half empty**
rather than take it, and that producer A — whose floor is zero — mines the very
same transaction.

In the GUI, the fee acceptance policy is edited in the Fee policy dialog
(`src/qt/feepolicydialog.cpp`), which drives `setfeeexchangerates`: that is the
*which assets and at what price* half of a producer's policy. The reserve price
itself is a daemon setting, so today it goes in the configuration file or on the
command line, not in the interface.

### 4. RBF over the wall — confirmed in the next block

The plain case: same asset, higher bid, `bumpfee`. The suite asserts the original
is gone from the mempool and the replacement is in the next full block.

### 5. An asset only ONE producer prices — and the other still accepts the block

**This is the most important case in the suite.**

Which assets a node accepts fees in is **policy, not consensus**. Producer A will
not relay and will not mine a fee it does not price; producer B does both. Then A
must accept B's block, because block validity accounts for every asset regardless
of any node's whitelist (`ConnectBlock`; the reasoning is spelled out beside the
single-fee-asset check in `MemPoolAccept::PreChecks`).

A regression here does not surface as a rejected transaction. It surfaces as a
**fork**, and a quiet one: two producers with different price servers would simply
stop agreeing. So the suite asserts the whole chain of it — A rejects the
transaction from its mempool, B mines it, and A's best block hash becomes B's
block.

One consequence worth internalising: **relay is hop-by-hop**. A transaction whose
fee asset the intervening nodes do not price cannot reach the producer who would
have taken it, however willing that producer is. The suite sidesteps this by
offering the raw transaction directly to each node, which is also what a wallet
would have to do in production.

### 6. Both price it, one higher — the richer valuation decides

The same bytes, two valuations, one chain. The transaction is built at a rate
producer A values under the wall; B prices that asset eight times higher, so to B
the identical transaction is a rich one. It sits in **both** mempools throughout —
what differs is only who will mine it.

The suite mines a full block on A (excluded), tops the queue back up, and mines on
B (included), then asserts both nodes are still on one chain. No fork is created
at any point: the producers take turns on the same chain rather than racing.

### 7. CPFP, same fee asset

A parent under the wall, then a child that spends the parent's own output and pays
several times over. Both land in the same block.

Spending the parent explicitly is what makes this CPFP: left to choose for itself
the wallet would pick a confirmed input, and the rich child would lift nothing.

### 8. RBF switching fee asset

The replacement pays in a **different** asset. It is worth more while it may well
hold fewer atoms — which is exactly why the mempool compares values rather than
amounts, and why a node that compared amounts would auction block space by an
asset's denomination rather than by what was paid.

### 9. CPFP switching fee asset

The same, through a package: the child pays in another asset than the parent and
still lifts it. The package is valued as one.

---

## 3. What broke while writing this, and why it is in the harness

Three of these are traps rather than bugs, and every one of them produced a
convincing false result before it was found. They are worth knowing before writing
the next fee test.

**Coin selection can invent a CPFP.** Left to choose for itself, the wallet
happily spends its own unconfirmed change. A filler transaction that does so
becomes the *child* of the transaction under test, the mempool scores the pair as
one package, and the under-bidder is dragged into a block as an accidental CPFP —
which is to say, case 2 fails intermittently for a reason that has nothing to do
with the code under test. Every transaction in the suite is therefore funded from
a confirmed output handed out exactly once.

**A blinded input blinds the transaction.** Custom chains keep the Elements
default, so the wallet's change from setup is confidential even when
`-blindedaddresses=0`. Spending one of those forces the whole transaction to be
blinded, and the raw send dies with `output has nonce, but is not blinded` — a
confidentiality error that reads like a fee error. The suite only ever funds from
unblinded outputs.

**Change must be change.** The change destination has to be explicit (a
confidential one cannot be carried by an otherwise transparent transaction) *and*
has to be a real change address: an output paid to a receive address is a payment,
not change, so `bumpfee` cannot shrink it and refuses with `no change destination
provided for asset ...`. The suite uses the unconfidential form of
`getrawchangeaddress`.

A fourth, from the same family: `fundrawtransaction`'s `replaceable` option marks
only the inputs it adds itself. When the inputs are named by the caller, the
BIP125 sequence has to be set on them or `bumpfee` refuses the transaction later.

---

## 4. On a testnet

Everything above is cheap in regtest because the fees are imaginary and the block
is whatever size we say. Neither is true on a shared testnet, and the difference
is not a detail:

- **Fees are paid to whoever produces the block.** On a shared network that is
  somebody else's node, so the money is gone. The budget, not the clock, is what
  ends a run.
- **The block is 400,000 weight** — about 99,000 vB of transactions every 60
  seconds, or roughly 5,940,000 vB an hour to keep full. At a Bitcoin-mainnet-like
  2 US cents per vB that is around $120,000 an hour of value; at the 0.05 cents of
  a quiet Bitcoin, a few thousand.
- **A full block is a full block for everyone.** Holding a shared testnet at a fee
  floor is something to announce to the other operators first, not to discover
  together.

`contrib/sequentia/tx-spammer.py` is the tool for this. Its `--plan` mode prices a
run against the wallet that would pay for it and refuses a duration the budget
cannot cover, naming the rate that would have fitted:

```bash
contrib/sequentia/tx-spammer.py --datadir <datadir> --wallet <name> \
    --rate-cents-per-vb 0.05 --duration 4h --plan
```

Then, once the numbers are agreed:

```bash
# once: split the budget into a working set of independent UTXOs
contrib/sequentia/tx-spammer.py --datadir <datadir> --wallet <name> \
    --budget 20000 --utxos 1500 --fanout

# hold about three blocks of backlog for four hours
contrib/sequentia/tx-spammer.py --datadir <datadir> --wallet <name> \
    --rate-cents-per-vb 0.05 --spread 1.6 --duration 4h --budget 20000 --run
```

### What can and cannot be reproduced there

| case | on a shared testnet |
|---|---|
| 1 — unpriced asset | **Yes.** Issue an asset, do not have it listed, offer the fee. Costs nothing, since nothing is ever accepted. |
| 2 — under the wall | **Yes**, while the spammer holds the floor. The gap has to be waited for rather than arranged. |
| 3 — producer floor | **Only on a producer you control.** `-blockmintxfee` is a per-node setting; you cannot observe another operator's. |
| 4, 8 — RBF | **Yes**, and this is the case most worth doing on a real network: it exercises relay and replacement across real peers. |
| 5, 6 — divergent policy | **Only by arrangement.** It takes two producers with deliberately different price servers, so it needs the other operator's cooperation. Relay being hop-by-hop, the transaction may also have to be handed to each producer directly. |
| 7, 9 — CPFP | **Yes**, same as RBF. |

The rule of thumb: everything about *one* node's policy is reproducible on a
testnet; everything about *disagreement between* nodes needs either a private
network or a partner. For the disagreement cases, the local network launcher is
the honest tool —

```bash
contrib/sequentia/run-local-testnet.py --nodes 2 --slot 10 --no-anchor \
    --blockmaxweight 8000 --initialfreecoins 100000000000000 \
    --client-conf ~/gui-node.conf --basedir ~/seqnet
```

— which is also how a wallet under test (the Qt GUI) joins: `--client-conf` writes
the configuration of a non-producing node carrying the same consensus block. A
node that computes a different genesis never connects, and the symptom reads as a
networking fault rather than a configuration one.

---

## 5. When to run this

- Any change to `src/policy/`, `src/node/miner.cpp`, `src/exchangerates.cpp`,
  `src/feeassets.cpp`, or the wallet's fee selection.
- Any change to what `setfeeexchangerates` accepts or how the price server pushes
  it.
- Before a release, together with `feature_any_asset_fee_congestion.py`, which
  covers the ordering rules underneath these scenarios.

The suite is deliberately blunt about its own preconditions: it asserts the block
really filled before drawing any conclusion from an exclusion. A fee-market test
that quietly stops congesting is a test that passes for the rest of its life
without measuring anything.

---

## 6. Cases 10 and 11: a fee bump must not spend a confidential coin

These two exist because of a failure found by driving the desktop wallet against
this suite's own regtest — a failure none of cases 1–9 could see.

### What went wrong

`bumpfee` switching to a different fee asset produced a replacement the wallet's
own node refused with `bad-txns-in-ne-out`, **after** the wallet had recorded the
bump and told the user the fee was increased. The user is left with a stuck
transaction and a wallet that believes it replaced it — the worst shape a wallet
error can take, because nothing in the interface says anything is wrong.

Decoding the pair shows which side fails to balance:

```
original     1 in,  3 out   change 4997.988692 SPLIT + pay 1.0 SPLIT + fee 0.002056 SPLIT
replacement  2 in,  4 out   change 199.97908863 gasset
                            change 4997.990748 SPLIT + pay 1.0 SPLIT
                            fee 0.003528 gasset
```

The SPLIT side balances exactly, with the old fee correctly returned to change.
The gasset side does not — and its input is **blinded**.

### Why

A bump builds an entirely explicit transaction: `CreateRateBumpTransaction`
rebuilds every recipient without a blinding key and asks for the new fee asset's
change with `add_blinding_key = false`. It then sets `fAllowOtherInputs = true`
and leaves coin selection free. A confidential coin reaches selection as a
`CInputCoin` whose `effective_value`, `value` and `asset` are all left at zero
(`src/wallet/coinselection.h`), so the wallet can fund the transaction with an
input the explicit amount accounting cannot see.

The same function already refuses to bump a transaction whose *outputs* are
blinded. The invariant was simply never applied to the inputs the bump goes and
fetches.

### The fix

`CCoinControl::m_only_explicit_inputs`, honoured in `AvailableCoins`
(`src/wallet/spend.cpp`) and set by the fee bumper. A bump now either funds itself
from explicit coins or fails — it can no longer produce a transaction that cannot
be relayed.

When it fails, it says the true thing. "Insufficient funds" would be a lie to
someone looking at the balance in their own wallet, so a bump that finds the fee
asset present but held only confidentially reports that, and names the way out:
bump in another asset, or unblind the balance first.

### The two cases

- **10** — the wallet holds the fee asset both ways, which is the ordinary state
  of a wallet that has been used. The replacement must be accepted, and every
  input it spends must be explicit. Without the fix this case fails with
  `bad-txns-in-ne-out`: the unfixed wallet reaches for the confidential coin even
  when an explicit one is available, so this is not a corner case.
- **11** — only confidential funds in the new fee asset. The bump must be refused
  with the truthful reason, the original must be left untouched, and bumping in
  the original asset must still work.

### Two traps this cost, worth knowing before writing the next one

**Blinding does not happen just because you asked for a confidential address.**
With `ignoreblindfail` on — the default — a transaction whose blinding cannot be
honoured silently drops it and pays to an explicit output instead. A single
confidential output cannot be balanced, so funding a wallet by sending to one
confidential address produces an explicit coin and a case that passes while
testing the path it meant to avoid. Case 10 funds with **two** confidential
outputs in one transaction, and then asserts the coins really are blinded.

**Paying yourself leaves dust that steers coin selection.** Each filler
transaction paid to the suite's own wallet left a 0.001 coin behind, and one of
those is worth exactly the fee a bump first estimates. Branch-and-bound takes it
as a perfect match, the transaction then grows by an input and a change output,
and the bump dies as `Could not cover fee` — in whichever case happens to draw it.
Filler is now paid out of the wallet. This is a separate weakness of the wallet
worth its own look: a cross-asset bump whose added input makes the transaction
larger than the estimate it selected against does not retry.
