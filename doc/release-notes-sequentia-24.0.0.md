# Sequentia Core 24.0.0

This release **changes the block cadence from 30 to 60 seconds** and makes that
cadence a consensus rule instead of a convention of the producer software. It
also adds the missing half of staking: you can now withdraw registered stake.

**This is a hard fork.** Every committee node, the dexnode and the explorer node
must be upgraded together, before testnet height **93,800**. A node still on
23.3.8 at that height will reject blocks its peers accept and fork off.

## Why the cadence changed

Sequentia's 30 seconds between blocks were never written down as a rule. They
lived in one line of `PosProducer::Step`, on the producer's side, where no
validator checks them — the software's good habit, not the network's law. A
producer running modified code could ignore them, and every node would accept
the result.

That is not a theoretical gap. Producers are paid in the fees of the blocks they
lead and in nothing else — there is no block subsidy — so the incentive to run
the chain faster is permanent and grows with congestion. And more blocks in the
same time is more disk, more bandwidth and more validation for every node,
forever.

The sortition time-gate could not close the gap. It delays a leader by its draw
times the slot interval, and that draw scales with the reciprocal of the
staker's weight, so it can never be a uniform speed limit — and the winning draw
rounds down to zero in about 63% of rounds, which is exactly when a brake would
be needed. Measured against the real committee shape: a hostile set holding all
the stake could drive the chain to a **17.5-second** cadence under the old
rules.

So the cadence is now a rule of its own:

```
block.nTime >= parent.nTime + 60        # bad-pos-spacing
```

It compares two timestamps **written in blocks**. The validating node's own
clock never enters, so every node reaches the same verdict regardless of when a
block arrived or how far any local clock has drifted.

Binding written times is enough to bind real time. Producing N blocks costs
`N × 60` seconds of timestamp, and the inherited two-hour limit on how far ahead
of real time a timestamp may sit caps how much of that can be spent in advance.
The long-run rate is therefore **exactly one block per 60 seconds for everyone**,
after a one-off burst of at most 120 blocks that gains nothing, because the
chain then produces nothing until real time catches up.

### What it costs, and what it does not

A Sequentia block is final when it is certified, so the wait for an irreversible
transaction is half an interval: **30 seconds on average**. For comparison,
Liquid produces a block every minute but needs two confirmations, because its
signers only refuse reorgs deeper than one block — about 90 seconds. This chain
stays several times faster while halving its own footprint.

Throughput is unchanged. The block weight cap encodes weight *per second*, held
equal to a saturated Bitcoin, so halving the cadence doubles the cap:

```
400,000 WU / 60 s  ==  4,000,000 WU / 600 s
```

Sequentia is now a tenth of Bitcoin's block at a tenth of its interval, where it
was a twentieth at a twentieth. Total disk growth is identical. It is in fact a
small gain: the ~2,100 weight units of fixed per-block overhead are paid once
per block, so they fall from 1.04% of the cap to 0.52%, leaving marginally more
of the same disk for payload. Everything else that scales per block — index
entries, disk writes, signature verifications during a sync — simply halves.

## The leader time-gate: ten seconds per unit, not thirty

A second change rides the same cutover, and it is the one that pays for itself.

The sortition draw does two jobs: it decides who wins, and it decides how early
each staker may offer a block — `floor(draw) x unit` seconds after the parent.
The exponential race made the draw a RATE rather than a rank, so its minimum
over all stakers is Exponential(1), with an unbounded tail. Multiplying that
tail by a whole 30-second interval silences the chain whenever the draw runs
long.

The cadence change alone improves this a great deal, because a 60-second floor
absorbs one more slot than a 30-second one did: the loss falls from the 18.4%
measured on the live chain to 3.8%. Ten seconds per unit takes almost all of
what is left. Measured over 2,000,000 simulated rounds with twelve equal
stakers at the new cadence:

| unit | blocks late | throughput lost | simultaneous field |
|---|---|---|---|
| 30 s | 1 in 20 | 3.78% | 2.7 of 12 |
| **10 s** | **1 in 1,097** | **0.024%** | **5.3 of 12** |
| 1 s | never | 0.000% | 11.9 of 12 |

Ten takes 99.4% of the available gain. Below it the throughput is already
recovered and only the field keeps widening, which buys nothing and gives the
anchor-freshness key — which orders before the score — more material to
reorder.

**It does not change who wins.** Run over 900,000 rounds on three stake
distributions, with every unit from 30 down to 1 driven off the same draws, the
winner differed in **zero** rounds and the realised shares are identical digit
for digit. The lowest score holds the lowest gate under any unit, so it is
always in the field and always that field's lowest score.

**It is a new parameter, not a lower `-posslotinterval`.** That setting is also
the axis the unbonding requirement is measured on
(`PosRequiredUnbondingSeconds() = period x slot interval`), so lowering it to
10 would have cut the nothing-at-stake lock from ~15 days to ~5 with nothing
saying so. It stays at 30.

## The parameters that had to move with it

Three settings are counted in blocks, so what they mean in real time depends on
the cadence. They were audited one by one; two needed changing and one did not.

| | before | now | why |
|---|---|---|---|
| Payout notice | 2,880 blocks | **1,440 blocks** | Compared in block heights, so it had silently become ~2 days instead of ~1 |
| Coinbase maturity | 100 blocks | **1,000 blocks** | Held equal to Bitcoin in wall-clock time (16 h 40 min), not in block count |
| Unbonding period | 43,200 blocks | **unchanged** | Normalised to seconds already, so ~15 days before and after |
| Leader time-gate unit | 30 s | **10 s** | Its own parameter now; see above |

The unbonding period is the one that looks like it should have moved and must
not. It is compared in *seconds* — `period × slot interval` — and the slot
interval stays at 30, so its 15 days are unaffected by the cadence. Halving the
block count would have cut the nothing-at-stake lock to 7.5 days.

Coinbase maturity matters more here than upstream. With no block subsidy, a
coinbase carries the producer's fee income rather than new issuance, so the
maturity is the delay before a producer can spend what it earned. Its invariant
is written beside the number:

```
coinbase_maturity × pos_block_spacing == 100 × 600
```

so that whoever changes the cadence next sees they have to redo this one too.
That is not hypothetical: the payout notice had already drifted to twice its
documented meaning for exactly this reason.

## Withdrawing stake

Until now `registerstake` could lock SEQ into a staking output and no wallet
command could spend it back. The coins were never locked by the protocol — a
staking output is a bare script with a relative timelock, and anyone could
always have built the spend by hand — but nothing automated it.

- **`withdrawstake ( pubkey amount address )`** finds this wallet's staking
  outputs, judges maturity from the unbonding lock and any vesting lock, signs
  and sends the SEQ back to a fresh address. A partial withdrawal spends whole
  outputs largest-first, so at most one is split and its remainder is re-staked
  to the same key — with a fresh unbonding clock, which the confirmation says
  plainly. Every input is re-verified before anything is broadcast.
- **`liststakeutxos`** is the read-only companion: amount, staker key, funding
  height, whether it is withdrawable now, and if not, when.
- **`bumpwithdrawstakefee`** replaces a withdrawal that is taking too long. The
  wallet's own `bumpfee` cannot: it requires every input to be one the wallet
  recognises, and a staking output is a bare script it does not match.
- The **Staking page** gains a Withdraw card with a confirmation stating the
  outcomes in plain language, Max buttons, a Speed up button while a withdrawal
  is in flight, and an **Unstake** row in Transactions instead of an
  unexplained "Received with".

All of this is wallet, RPC and GUI code. None of it is consensus, so a defect
here can be fixed in an ordinary release — unlike the cadence change above.

## Also fixed

- **Three places read the slot interval as if it were the cadence.** They are
  different numbers now, so all three were wrong: the GUI's initial disk-space
  estimate claimed twice the real growth; the Staking page's tooltip explained
  the draw off a single number when it needs two; and `getposschedule` /
  `getposslot` reported only `slot_interval`, which an operator reads as "a
  block every N seconds". Both RPCs now also report `block_spacing`.
- **`liststakeutxos` dated a stake unlock from the gate unit**, so it told a
  staker with a 43,200-block unbonding left that it would clear in 15 days when
  it is 30 — in the RPC written to answer exactly that question.
- **A producer whose clock trails the network by a second no longer emits a
  block one second too close to its parent.** The producer waited correctly and
  then the block assembler stamped the local clock; a scan of the first 86,357
  testnet blocks found 2,186 closer than 30 seconds to their parent, 2,183 of
  them at exactly 29, still occurring at the tip. Harmless while nothing
  checked, fatal once something does — which is why the clamp follows the
  spacing *value* and takes effect on upgrade, while the rule waits for its
  height.
- **`feature_pos_withdrawstake` now actually runs.** It was never registered in
  the test runner and carried no execute bit, so it ran only by hand.

## Upgrading

1. **Upgrade every node together**, before testnet height 93,800. Mixed binaries
   fork the chain. Stop every node, swap the binary, relaunch every node — the
   committee, the dexnode and the explorer node.
2. **Check the tip before you deploy.** The activation height must still be
   above it. If the chain has moved past ~93,300 by the time you start, raise
   `pos_block_spacing_height` and rebuild rather than proceeding: blocks
   produced between the height and the last node's upgrade come from binaries
   without the clamp, and from the cutover on they are permanently invalid to
   anyone syncing from scratch.
3. After the cutover the cadence shifts to 60 seconds as the fleet upgrades,
   not on a flag day: upgraded nodes space at 60 s immediately while
   un-upgraded ones still offer at 30 and win the race until they are
   outnumbered.

`-posslotinterval` stays at 30 and must not be raised to "match" the new
cadence. It is the leader time-gate unit, not the cadence, and leaving it at 30
is what lets the 60-second floor absorb sortition slots 0, 1 and 2 rather than
just 0 and 1 — worth about 14 points of throughput on its own. On the Sequentia
chains the flag is refused outright.

## Known consequences

- A timestamp pushed toward the two-hour future limit now **stays** there.
  Before, the next producer's stamp of "now" pulled the chain back to real time;
  the floor now keeps each block at least 60 seconds past its parent, so the
  offset persists. It is capped at two hours, cannot grow, does not change the
  real block rate, and unwinds only if the chain stalls long enough for real
  time to catch up.
- A **height-based** stake lock of N blocks is still credited `N × 30` seconds
  while it now really lasts `N × 60`, so stakers using that form over-lock by
  2× relative to those using the time-based form. It demands more, never less,
  so it is safe — but it is wrong, and correcting it changes which outputs
  qualify as stake, so it is left to its own change.
- Producers wait longer to spend their fee income: 16 h 40 min instead of 50
  minutes. That is the point of the coinbase maturity change, not a side
  effect.
