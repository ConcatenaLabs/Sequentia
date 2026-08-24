# Converting staking rewards automatically

A staker is paid in whatever assets the blocks it earns from happened to carry
fees in. That is the open fee market working as designed, and it is also, for
most stakers, an inconvenience: a solo producer accumulates a long tail of small
balances in assets it never chose to hold, and a pool delegator receives the same
tail one claim at a time. The staker wanted income, and got inventory.

This document specifies **reward auto-conversion**: an opt-in rule that takes
every staking reward a wallet receives, in whatever asset it arrives, and sells
it on SeqDEX for **one asset the staker picked** — provided a market for that
pair exists.

**Native Bitcoin is the default and sits at the top of the picker**, because it
is what most stakers converting an unwanted tail of assets are converting *to*,
and because it is the one asset that is not an opinion about anything. It is not
the only choice: a staker paid in five assets who wants USDX, or GOLD, or simply
more SEQ to grow a stake with, is doing the same thing for the same reason. The
top seat is the whole of Bitcoin's privilege here — the same arrangement the Send
picker already uses, and the same principle: outside staking itself, no asset is
privileged over another.

The target is never SBTC when it is "Bitcoin". SBTC is a narrow opt-in peg
(`sbtc-peg-design.md`); a staker who asks for Bitcoin gets native parent-chain
BTC, and one who genuinely wants SBTC can pick it from the list like any other
asset.

The rule is the same in every wallet — the node's Qt wallet, the web wallet, the
browser extension, Ambra. This document is the single definition, because four
independent implementations of "which coins are rewards" would disagree on the
first edge case, and disagreeing about which coins to sell is the expensive kind
of disagreement.

## The three layers

Auto-conversion is not new machinery. It is a **rule that fires machinery that
already exists**: SeqDEX already sells one asset for another, same-chain over a
covenant or HTLC and cross-chain to native BTC over an HTLC or Lightning, and a
wallet that offers a manual swap already contains every primitive a conversion
needs. What is missing is the two layers above it.

| Layer | What it answers | Where it lives |
| --- | --- | --- |
| **Attribution** | Which of my coins are staking rewards? | node RPC `liststakingrewards`; SWK for light wallets |
| **Policy** | Which of them should be sold, into what, and when? | shared, pure, in SWK |
| **Execution** | Sell it | the wallet's existing SeqDEX take path |

Keeping the layers apart is what makes the feature testable: attribution is a
pure function of the wallet's coins, policy is a pure function of attribution
plus the book, and only execution touches the network.

## Layer 1: attribution

A staking reward is a coin the chain paid the staker for producing blocks, or
for lending stake to whoever did. There are exactly two shapes, because there are
exactly two ways the consensus rules pay a staker.

**A coinbase output the wallet owns.** Sequentia has no subsidy, so a coinbase
carries only the block's fees, one output per fee asset, and
`PosRequiredCoinbaseScript` decides who they pay:

- `LEADER` — P2WPKH of the elected leader. If that leader's key is this wallet's,
  the wallet produced the block itself: **solo**.
- `DIRECT` — the script the pool operator committed to. If it is ours, the
  operator is paying us directly: **direct**.
- `LOTTERY` — P2WPKH of one participant drawn from the block's election seed.
  When the draw lands on us: **lottery**.
- `SPLIT` — pays the pot, which is not ours and never becomes ours. A split
  pool's rewards reach a delegator through the second shape, below.

**An output the wallet owns in a pot claim.** Under a `split` policy the
rewards accumulate in `<"SEQPOT"> OP_DROP <signer> OP_DROP OP_TRUE` and
`claimpoolrewards` distributes them, paying each delegator at
`P2WPKH(controller)` — the delegator's own staking key. From the delegator's
side this is an ordinary incoming payment: **split**.

Both shapes are recognisable from wallet data alone, which matters because a
light wallet has no chainstate to consult:

```
coinbase && IsMine(out)                     -> reward (solo | direct | lottery)
!coinbase && !IsFromMe(tx) && IsMine(out)
    && out.scriptPubKey == P2WPKH(k)
    && k is a staking key of this wallet    -> reward (split)
```

A **staking key** here is a key whose `P2WPKH` this wallet can spend and which is
either known to the stake registry as a staker, held by a stake output in the
wallet, or named as the controller of a delegation record in the wallet. The
registry route is the one that matters in practice: a node configured with
`-staker=` holds weight without the wallet holding a stake output at all, which
is how the committee runs.

The second rule is deliberately narrow. It requires the payment to land on a
*staking* key — never a key the wallet hands out as a
receive address — and it requires the wallet not to have sent the transaction
itself, which excludes a delegator's own re-pointing or withdrawal. A stranger
who deliberately paid a staker's staking address would be mistaken for a pool
payout; that is a payment nobody makes by accident, and the consequence of the
mistake is that it gets converted along with the rest.

**Maturity is part of attribution, not an afterthought.** Coinbase value is
spendable only `COINBASE_MATURITY` blocks deep. A reward that is not yet mature
is reported, so a staker can see it coming, and is not eligible for conversion.
Pot-claim outputs are ordinary outputs and mature immediately — but the pot they
came from was itself coinbase value, which `claimpoolrewards` already respects.

**Two implementations, one test vector.** The node implements attribution in
`liststakingrewards` (`src/wallet/rpc/spend.cpp`), SWK implements it for the
light wallets, and both are pinned by the same fixtures — the arrangement that
already keeps the delegation-record script from drifting between the two
codebases.

## Layer 2: policy

Attribution produces a set of reward coins. Policy decides what to do with them,
and it is the layer where a naive implementation loses the staker money.

**Sell the asset, not the coin.** Rewards arrive small and often. Converting each
one as it lands would pay a swap's fixed costs per reward and, on a quiet chain,
convert dust at a loss. Policy therefore accumulates: reward coins of the same
asset are batched, and a batch is converted only when it is worth converting.

**A batch converts when all of these hold:**

1. the asset is not the target itself (nothing to do) and is not excluded by the
   user;
2. a SeqDEX market for `ASSET/TARGET` exists and has depth to fill the batch —
   this is the user's own "as long as there is a market for that trading pair",
   and it is checked against the live book, not a static list of pairs. No market
   is not an error: the batch waits, and converts if one appears;
3. the batch's expected proceeds clear the **floor**: the swap's cost plus the
   configured minimum receive, both denominated in the target asset. A batch that
   does not clear the floor is not a failure and is not retried in a loop; it
   waits for the next reward in the same asset, indefinitely if that is how long
   it takes;
4. the price the book offers is within the configured **slippage cap** of the
   reference price. A market that exists but is being quoted 40% away is not a
   market the staker meant to sell into.

**What policy must never do:**

- **never convert more than staking has paid.** The batch is the bound, and it is
  the AMOUNT that is bounded, not the identity of the coins. An asset is fungible,
  so selling the batch is satisfied by any coins of that asset, and none of the
  take paths on any wallet accepts a list of specific UTXOs to spend. An earlier
  draft of this document promised UTXO-level selection; that promise could not be
  kept and was worth less than it sounded, because the guarantee that matters is
  the total.
- **never touch stake, delegation records, or payout records.** Those are coins
  in the wallet that look spendable and are load-bearing. This one holds by
  construction rather than by care: they are bare scripts, which no wallet's coin
  selection can spend at all.
- **never block staking.** Conversion runs behind block production, not in front
  of it; a wallet that cannot convert must still produce.
- **never quietly convert away the thing the staker is accumulating.** Rewards in
  the target asset are left alone by rule 1, and everything else is converted by
  default, because that is the feature as asked for. A staker growing a stake
  toward the 40,000 SEQ floor either sets the target to SEQ — in which case every
  other asset is converted *into* stake, which is exactly right — or excludes SEQ
  from conversion. Both are one setting, and the setting says what it costs.

**Idempotence.** Every conversion is recorded against the reward coins it
consumed, so a wallet that restarts mid-conversion, or runs two windows, cannot
sell the same reward twice. The ledger is also the user-facing record: what was
sold, for how much, and when.

## Layer 3: execution

Execution hands the batch to the wallet's existing SeqDEX take path as a
**Market sell** of `ASSET` for the target. There is no new settlement primitive
and no new crypto — and which primitive runs follows from the target, not from
anything auto-conversion introduces:

- a **Sequentia-asset target** is a same-chain swap, settled by the covenant or
  HTLC path the manual composer already uses, and delivered to the wallet's own
  address;
- a **native BTC target** is a cross-chain swap, settled over the cross book's
  HTLC or over Lightning, and delivered to the wallet's own parent-chain address.
  Every Sequentia wallet is dual-chain, so that address already exists;
- rail choice stays a settlement concern, never a matching one — the conversion
  posts against the unified book and the settlement router bridges if the two
  sides' rails cross, exactly as a manual swap does;
- the swap's refund path is the swap's own. A conversion that fails leaves the
  reward coins where they were, marked as not converted, and the next pass tries
  again — but only when the failure is DEFINITE. An executor that threw may have
  paid before it threw, and a wallet that released those coins on an ambiguous
  failure would sell the same reward twice; those records stay claimed, visibly
  stuck, until a human or a resume pass settles them;
- a cross-chain conversion has an order, and the order is the safety. Nothing of
  the staker's moves until the maker's Bitcoin is locked, confirmed and verified
  against the parent chain by the wallet itself. The asset is funded only once
  the wallet's own Bitcoin anchor has reached the height that lock confirmed at —
  the block that confirms the funding commits to an anchor and that number
  freezes on confirmation, so waiting afterwards can never repair it. And the
  lock is checked once more immediately before spending, because one parent-chain
  reorg is all it takes for a lock that was confirmed at the first check to be
  gone by the second. After funding, the asset is recoverable by timelock
  whatever the maker does, and the refund needs nobody's cooperation;
- a conversion is never a Limit order. A resting order that the staker forgot is
  worse than a reward that did not convert.

**The node's wallet converts too, and how it manages to is worth knowing.** It
has no interactive DEX client and cannot hold a co-signing conversation with a
maker — but for the same-chain case it does not need one. A covenant offer is a
Taproot output whose FILL leaf spends nothing and signs nothing: it only
*inspects* the transaction spending it, and passes when that transaction pays the
maker at the committed rate and returns any remainder to an identical output. So
the node builds one transaction and broadcasts it, with the maker offline. It
rebuilds the maker's committed leaf byte for byte and refuses to spend unless the
result hashes to the Taproot output the offer is actually funded by on chain —
the relay is never trusted about terms, value, or even that the offer is still
there.

The cross-chain case *is* a conversation, so the node grew the parts to have one:
a minimal RFC 6455 WebSocket client, AES-256-GCM to open the courier's sealed
payloads (pinned against the NIST vectors; nothing in consensus, P2P or wallet
storage uses it), and the HTLC both legs share. What it does NOT have is TLS —
Core dropped OpenSSL and nothing replaced it — so the relay endpoint is
configured with `-seqoburl` as a plain `http://` URL. What crosses that wire is a
signed public order book and sealed courier payloads: nothing secret, and nothing
trusted with funds. Adding a TLS dependency for a wallet convenience would be a
reproducible-build change out of all proportion to the feature; running a relay
locally, or terminating TLS in front of the node, is the operator's call.

## Settings

One set of settings, the same names in every wallet:

| Setting | Default | Meaning |
| --- | --- | --- |
| `enabled` | off | opt-in, always. Nobody's rewards are converted because they upgraded. |
| `target` | native BTC | what to convert into. Any asset with a market; BTC is offered first. |
| `exclude` | empty | assets to keep as they are, on top of the target itself. |
| `min_receive` | 0.0001 BTC, or the target's equivalent | the floor a batch must clear, in the target asset. This is the "minimum value before anything is sold" dial: below it a batch simply waits, indefinitely if that is how long it takes. |
| `max_slippage_bp` | 200 (2%) | how far from the reference price a fill may land. |
| `destination` | the wallet's own address for the target | where the proceeds go — parent-chain for BTC, Sequentia otherwise. |

Off by default is not timidity. Converting someone's rewards is irreversible and
the staker may have chosen those assets deliberately; the feature has to be asked
for. Changing the target does not retroactively re-convert anything: it applies
to the next batch.

## Where it is implemented

| | attribution | policy | execution |
| --- | --- | --- | --- |
| node / Qt | `liststakingrewards` | `wallet/rewardconvert` | covenant fill + cross-chain HTLC, on the node's scheduler |
| web wallet | SWK (wasm) | SWK (wasm) | `takeMarketWalk` / `driveReverse` |
| extension | SWK (wasm) | SWK (wasm) | covenant fill / the Lightning rail |
| Ambra | SWK (`api::rewards`) | SWK (`api::rewards`) | the phone's existing swap services |

Two implementations of attribution and policy exist — the node's C++ and SWK's
Rust — and they are pinned to the same cases, in the same order, with the same
numbers. A node wallet and a light wallet watching one staker's keys must reach
the same verdict about that staker's coins; two implementations that were never
compared have already drifted.

## What this is not

It is not a rebalancer, a DCA schedule, or a portfolio tool. It converts staking
rewards, in one direction, into the single asset the staker said they wanted.
Everything else is the DEX, where the staker can do it by hand.
