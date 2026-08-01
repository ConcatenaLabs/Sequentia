# Bitcoin anchoring & real-time swaps

Every Sequentia block references a Bitcoin block. The referenced Bitcoin height
is monotonically non-decreasing along the Sequentia chain, and the chain
reorganizes *if and only if* Bitcoin reorganizes away a referenced block.
Outside of that one condition, a committee-certified Sequentia block is final.
Binding Sequentia's reorg risk to Bitcoin's in this way is what makes real-time
cross-chain atomic swaps against native BTC possible with no extra
reorg-protection timelock. This chapter specifies the anchor commitment, how
the anchor is chosen, the validation rules and the reorg-following watcher,
what "real-time" precisely means, and the swap mechanism that exploits it.

The anchoring machinery lives in [`src/anchor.h`](../../src/anchor.h) /
[`src/anchor.cpp`](../../src/anchor.cpp). The Bitcoin-RPC transport it builds on
is described in [`01-architecture.md`](01-architecture.md); the consensus that
provides finality is in [`04-proof-of-stake.md`](04-proof-of-stake.md).

## 1. The anchor commitment

Each Sequentia block header carries two anchor fields:

| Field | Meaning |
|---|---|
| `m_anchor_height` | Height of the referenced Bitcoin block. |
| `m_anchor_hash` | The referenced Bitcoin block's header hash (double-SHA256), on Bitcoin's best chain. |

`m_anchor_hash` is the security-bearing field - heights are not unique across
Bitcoin forks - while `m_anchor_height` is carried for cheap monotonicity checks
and indexing. Validation requires `m_anchor_height` to equal the real height of
`m_anchor_hash` on the connected Bitcoin node.

The fields are part of the header behind the global flag `g_con_bitcoin_anchor`,
serialized inside the hashed (`SER_GETHASH`) region in both the dynafed and
legacy header paths, so the block producer signs over the anchor and light
clients see it without parsing the coinbase. They are not part of
`m_signblock_witness`. The same fields are mirrored on `CBlockIndex`
(`src/chain.h`) and persisted with it, so monotonicity checks and the
reorg-following watcher never need to re-read full blocks.

The referenced height is **monotonically non-decreasing** along any chain: a
block's `m_anchor_height` is at least its parent's. The genesis block defines
the initial anchor in chainparams.

## 2. Choosing the anchor

When a producer builds a block, `GetAnchorForNewBlock` selects the anchor by
asking the connected Bitcoin node for its current block count and anchoring to

```
target = btc_tip_height - (anchorminconf - 1)
```

That is, with `-anchorminconf=1` (the default) it anchors to the Bitcoin tip;
higher values anchor that many blocks back from the tip, requiring each anchored
Bitcoin block to already carry that many confirmations. The selected height is
never allowed below the parent block's anchor, preserving monotonicity. If the
Bitcoin node is unreachable, or temporarily reports a tip below the parent's
anchor (for instance while it is still syncing), the producer falls back to
reusing the parent's anchor, which is monotone by construction and already
validated. Anchoring to the freshest available Bitcoin block is what keeps the
Sequentia tip synchronized with Bitcoin's tip within a bounded ~1-block lag
(§4).

Anchoring is governed by these settings:

```ini
con_bitcoin_anchor=1     # enable anchoring (a chain-parameter property)
validateanchor=1         # validate each anchor against the Bitcoin daemon
anchorminconf=1          # confirmations required of the anchored Bitcoin block
anchorpollinterval=5     # seconds between Bitcoin tip/reorg polls
```

The Bitcoin connection becomes mandatory when `con_bitcoin_anchor` is set,
reusing the `-mainchainrpc*` transport. A startup probe analogous to
`MainchainRPCCheck()` refuses to produce blocks if anchoring is required but the
Bitcoin daemon is unreachable. Operational detail and tuning are in
[`05-operating-sequentia.md`](05-operating-sequentia.md).

## 3. Validation rules and the reorg-following watcher

### Validation in `ContextualCheckBlockHeader`

A block's anchor is checked against its parent and against the Bitcoin daemon:

- **Well-formedness.** The fields are present exactly when `g_con_bitcoin_anchor`
  is set; `m_anchor_hash` is non-null on anchored blocks.
- **Monotonicity.** `m_anchor_height` is not less than the parent's. If the
  height is unchanged from the parent, the hash must be unchanged too - a
  producer cannot silently swap which Bitcoin block a height refers to.
- **Bitcoin existence and best-chain membership.** When `g_validate_anchor` is
  set and the anchor differs from the parent's, the node calls
  `CheckMainchainAnchor`, which queries the connected Bitcoin daemon: the header
  must exist, its height must equal `m_anchor_height`, and it must be on the
  daemon's active chain. The check distinguishes `OK`, `NOT_FOUND`, `STALE`
  (the hash is known but no longer on the best chain), `HEIGHT_MISMATCH`, and
  `NO_CONNECTION`.

### The reorg-following watcher

A background watcher polls the Bitcoin daemon every `anchorpollinterval` seconds
for new tips and reorgs, tracking the last seen Bitcoin tip height. When Bitcoin
reorganizes, any Sequentia block whose `m_anchor_hash` is no longer on Bitcoin's
active chain has become invalid. The watcher re-checks anchored blocks (querying
bitcoind outside `cs_main`, with the anchor data captured under the lock first)
and invalidates the lowest-height block whose anchor went stale via the existing
`InvalidateBlock` path; `ActivateBestChain` then disconnects it and all its
descendants. Block production resumes from the last still-valid Sequentia block,
whose anchor is still canonical on Bitcoin, and disconnected transactions return
to the mempool. Rebuilt blocks anchor to Bitcoin's new best chain.

This is a **consensus** invalidation, not a local policy choice: every node
following the same Bitcoin best chain reaches the same decision, because
Bitcoin's best chain is the shared oracle. The only sanctioned reason to discard
a certified Sequentia block is its anchor being reorged away on Bitcoin. As long
as Bitcoin does not reorganize a referenced block, no Sequentia reorg is
possible: committee certification gives immediate finality (see
[`04-proof-of-stake.md`](04-proof-of-stake.md)).

### `validateanchor=0` is a master switch, not a single check

`-validateanchor` reads like one option ("validate anchors") but governs **four**
mechanisms. Setting it to `0` turns off all of them:

| # | Turned off | Where |
|---|---|---|
| 1 | Anchor validation of incoming blocks (rule R3 above) | `ContextualCheckBlockHeader` |
| 2 | **The reorg-following watcher** | `AnchorWatchTask` returns immediately |
| 3 | The escaping-stall parent-chain time gap | `CheckEscapingStallMtpGap` returns `ALLOWED` |
| 4 | The PoS finality reconciliation monitor | requires `-validateanchor` |

Item 2 is the consequential one. The watcher is what tells a node that Bitcoin
orphaned one of its own anchors. Without it a node does not merely stop checking
*other* people's blocks — it can no longer discover that **its own** chain is
built on an orphaned anchor, so it never rolls back, and it keeps announcing that
chain to every peer that syncs from it. The failure is silent and outward-facing:
`getanchorstatus` reports `not_validated`, not a warning, and the node has no way
to notice on its own.

Worked example. Bitcoin is at height 101; a node produces Sequentia blocks 1..6,
all anchored to Bitcoin block 101. Bitcoin then reorganizes: the old 101 is
orphaned (`confirmations: -1`) and the chain continues to 106 on a different
branch, so Sequentia blocks 1..6 are all invalid. With `validateanchor=1` the
watcher notices within `anchorpollinterval` seconds, invalidates them, and the
chain rebuilds on Bitcoin's new best chain. With `validateanchor=0` **nothing
happens**: the node stays at height 6, believing it is healthy, and serves those
six dead blocks to newcomers indefinitely.

Note also that `0` does **not** stop block production. `GetAnchorForNewBlock`
never consults the flag: a producer with a reachable Bitcoin daemon keeps
selecting fresh anchors normally while ignoring every reorg — the worst
combination, since it manufactures new blocks on a branch it cannot detect is
dead.

Legitimate uses are therefore narrow:

- a **follower with no Bitcoin node available** that consciously delegates anchor
  validation to its peers (see [`04-proof-of-stake.md`](04-proof-of-stake.md) §6:
  *finality modulo Bitcoin requires watching Bitcoin*) — such a node must also
  have the finality gate off, or it could never lower its finalized point after a
  Bitcoin reorg and would stall forever;
- **offline validation** of an already-anchored chain.

It is not a supported way to work around a sync failure: it removes the node's
ability to self-correct for as long as it stays set. Never set it on a node that
produces blocks, or that other nodes sync from.

Finally, reaching `0` is now always somebody's decision. When
`con_bitcoin_anchor` is set and the Bitcoin daemon is unreachable at startup the
node **asks**, through `uiInterface.ThreadSafeQuestion`:

- an interactive frontend (`elements-qt`) offers two named choices — close and
  start Bitcoin Core first, which is the default and what closing the window
  does, or start without following Bitcoin;
- every non-interactive frontend, `elementsd` included, answers no, so the node
  refuses to start. That is the right default when there is nobody watching.

A node that took the second option keeps a warning in the status bar for the
whole session, because it otherwise looks perfectly healthy, and the same
warning appears for a deliberately configured `validateanchor=0` node — the
delegation is identical either way.

This replaced an earlier behaviour worth knowing about if you are reading older
logs or an older binary: the node used to decide by itself, and differently
depending on `-server`. Without it (the typical GUI configuration) it started
anyway with `validateanchor` silently forced to `0` for the rest of the session,
even if Bitcoin came up moments later; with it, the node refused to start.
Nobody chose either outcome, and `-server` tracked nothing a user cares about.

### The `getanchorstatus` RPC

`getanchorstatus` (available only when `con_bitcoin_anchor` is enabled) reports
the tip's anchor and the health of the Bitcoin connection:

| Field | Meaning |
|---|---|
| `validateanchor` | Whether anchors are validated against the Bitcoin daemon. |
| `tipheight` | Height of this chain's tip. |
| `anchorheight` | Bitcoin height referenced by the tip. |
| `anchorhash` | Bitcoin block hash referenced by the tip. |
| `anchorstatus` | Result of checking the tip's anchor: `ok`, `not_found`, `stale`, `height_mismatch`, `no_connection`, or `not_validated`. |

`not_validated` is reported when `validateanchor` is off or the tip carries no
anchor; the other values mirror `CheckMainchainAnchor`'s result.

Four more fields describe a node that is *not* watching Bitcoin:

| Field | Meaning |
|---|---|
| `unvalidatedbyprompt` | Validation is off because Bitcoin was unreachable at startup and the user chose to continue, not because it was configured off. |
| `parentbackonline` | Bitcoin has become reachable again since; validation resumes only on restart. |
| `unverifiedescapingstalls` | Sub-quorum blocks accepted without checking the Bitcoin evidence behind them. |
| `lastunverifiedescapingstall` | Height of the most recent one (`-1` = none). |

### Starting without a Bitcoin node

If `con_bitcoin_anchor` is set and the Bitcoin daemon cannot be reached at
startup, the node asks — it does not decide. `AppInitMain` puts the question
through `uiInterface.ThreadSafeQuestion`, so:

- **elementsd** and every other non-interactive frontend answer "no" (there is
  nobody to ask) and the node **does not start**. This is the pre-existing
  behaviour, and it is the right default for a machine nobody is watching. Note
  that the test is now "can this frontend ask a human", not `-server`: a GUI user
  who had enabled the RPC server used to get the daemon's flat refusal, for no
  good reason.
- **elements-qt** shows a dialog with two named buttons — *Close and start
  Bitcoin Core first* (the default, and what closing the window does) and *Start
  without following Bitcoin*.

Choosing to continue sets `validateanchor=0` for the session, exactly as before,
but now as a decision rather than a side effect. Two things follow from it.

**The GUI keeps saying so.** A warning icon stays in the status bar for the whole
session, because a node in this state looks perfectly healthy otherwise: it syncs,
it shows balances, and nothing would ever mention Bitcoin again. Clicking it
repeats what is being delegated and how to undo it. The icon appears for *any*
node running with `validateanchor=0` on an anchored chain, including one
configured that way deliberately — the delegation is identical, and only the
wording differs (a configured node is told to remove the setting; a prompted one
is told to restart, and is notified when Bitcoin becomes reachable again).

**The node reports what it actually had to take on trust.** Most of what
`validateanchor=0` gives up is invisible in the moment — a Bitcoin reorg that is
never noticed leaves no trace. One case is not: a *sub-quorum* (escaping-stall)
block is certified by as little as one committee member, and the only evidence
that the committee genuinely was stalled is the parent-chain time gap, which
lives on Bitcoin. `CheckEscapingStallMtpGap` returns `ALLOWED` unconditionally
when validation is off, so such a block is accepted purely on the network's word.
Every time that happens the node counts it, logs it, and surfaces it in
`getanchorstatus` and in the status-bar tooltip. It is not rejected — rejecting it
would strand a follower that has legitimately delegated — but it is never silent.

**Validation is not resumed in place.** A watcher thread polls Bitcoin every 30
seconds and, when it answers again, tells the user that a restart restores the
checks. It does not simply set the flag back: the blocks accepted meanwhile were
never checked against Bitcoin and are not revalidated retroactively, so a node
that flipped it would be claiming a protection it does not have for its own
history, and the anchor watcher would begin following reorgs from a tip it never
verified. A restart does not revalidate that history either — but it does put the
node back under a live Bitcoin view from a point the user knowingly chose, which
is the honest thing to offer.

## 4. Immediate finality and what "real-time" means

"Real-time" here is *not* "instant" or "zero-latency." It has a precise meaning,
rooted in the two kinds of timelock a cross-chain swap carries:

1. **Refund timelocks intrinsic to the HTLC.** Each leg has a CLTV refund path so
   a party can reclaim funds if the counterparty never completes. Nothing removes
   these; they are part of how an HTLC works.
2. **A reorg-protection buffer**, added *because two unsynchronized chains reorg
   independently*. A confirmation on one chain may vanish after you have already
   acted on the other, so a normal cross-chain swap pads its timelocks to wait
   out that independent reorg risk.

Sequentia drops #2 to zero. A Sequentia block committing Bitcoin anchor height
`A` is final **if and only if** Bitcoin block `A` survives - Sequentia carries no
independent reorg risk of its own, because reorg-following ties its only reorg
condition to Bitcoin's. So there is no second chain reorganizing on its own
schedule, and no buffer is needed to absorb it.

**"Real-time" therefore denotes: no extra reorg-protection timelock beyond
Bitcoin's own confirmation wait**, with the two chains kept *synchronized* - the
Sequentia tip references a current Bitcoin block - to within a bounded ~1-block
lag, since each new block anchors to Bitcoin's tip (§2). That lag is small
relative to the Bitcoin-paced swap clock. A claimant still waits for the leg's
anchor to reach their desired Bitcoin **confirmation depth** - a fresher anchor
is a *shallower* one - but pays no cross-chain buffer on top of that wait.

## 5. Anchor freshness for real-time swaps

Reorg-following gives swaps their *safety*: a Sequentia leg can never outlive the
Bitcoin block it anchors. For swaps to be *prompt* as well, the canonical
Sequentia tip must track Bitcoin's tip with minimal latency, so a swap's
Sequentia leg confirms with an anchor height at or above the Bitcoin leg's height
quickly.

This freshness is delivered by **production**, not by a fork-choice rule.
`GetAnchorForNewBlock` anchors every new block to the freshest Bitcoin block
(tip minus `anchorminconf-1`), so the tip tracks Bitcoin's tip within one
Sequentia block - by *extending* the chain, never by reorganizing it. Among
competing proposals for the same height, the committee backs the freshest-anchored
one (then the lowest leader VRF among equally-fresh proposals), so that is the
block that gets certified - the paper's Principle 7 freshness preference,
implemented in the gossip committee's candidate ordering. This is a
*pre-certification* signing preference, not a fork-choice vote, and the anchor is
deliberately not a key in fork choice: in an immediate-finality system, keying
fork choice on the anchor could let a new Bitcoin block reorder or overwrite
already-certified blocks, which must never happen. The fork-choice and finality details are in
[`04-proof-of-stake.md`](04-proof-of-stake.md).

## 6. Cross-chain atomic swaps

A Sequentia ↔ Bitcoin swap uses a standard hash time-locked contract (HTLC) on
both legs. Each leg locks funds under two conditions:

- a **hashlock** - redeemable by revealing a secret preimage whose hash is
  committed in the script; and
- a **CLTV refund timelock** - reclaimable by the funder after a deadline if the
  swap never completes.

The two legs share the same hash. When the party holding the preimage redeems one
leg, the act of redeeming reveals the secret on-chain, and the counterparty uses
that revealed secret to redeem the other leg. Either both legs are claimed or, on
timeout, both are refunded - the usual atomic-swap guarantee.

The Sequentia value-add is **anchoring consistency**. In a swap against native
BTC, the Sequentia leg lives in a Sequentia block, and that block anchors to a Bitcoin
block. If the BTC leg is reorged away on Bitcoin, the anchor of the Sequentia
block holding the Sequentia leg goes stale, the reorg-following watcher invalidates
that block, and the Sequentia leg reverts with it. Both legs revert together: neither
party is left having paid without being paid. This is exactly the property that
removes the reorg-protection buffer (§4) - there is no independent Sequentia
reorg for the buffer to guard against.

The consistency property is exercised by
[`test/functional/feature_anchor_swap_consistency.py`](../../test/functional/feature_anchor_swap_consistency.py),
which drives a regtest Bitcoin reorg and asserts that the dependent Sequentia
block reverts with it. A self-contained, runnable demonstration of a complete
swap - including the timeout-refund path - is
[`contrib/sequentia/swap-demo.py`](../../contrib/sequentia/swap-demo.py); see
[`05-operating-sequentia.md`](05-operating-sequentia.md) for running it.
