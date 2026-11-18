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
Sequentia tip synchronized with Bitcoin's tip within a ~1-block lag (§4).

**Never on a parent whose anchor was replaced.** Before choosing, the producer
reads the Bitcoin node's block at the parent's anchor height. If another block is
there now, the parent's anchor has left Bitcoin's best chain: the anchor watcher
is about to disconnect the parent, and every node that watches Bitcoin already
refuses it. So the producer builds nothing on it (`CreateNewBlock` fails with
"the parent block's anchor ... is no longer on the parent chain's best chain")
and wakes its anchor watcher at once, which disconnects the parent and lets
production resume on the canonical branch. The check needs an answer it can
trust: an unreachable Bitcoin node, or one behind the parent's anchor height,
gives none, and the producer then proceeds as above. It applies only to a node
that watches Bitcoin (`-validateanchor`), the one whose watcher will act.

**Backing off a contested parent height.** One condition lowers that target.
When Bitcoin has live competing branches at or near its own tip, the tip block
may be the one that loses, and a Sequentia block anchored to it has to be
reorganized out when it does. With `-anchoravoidcontested` (on by default) the
producer therefore asks the Bitcoin node for its chain tips and backs the target
down to the last height every live contender still agrees on: the lowest fork
point (`tip height - branchlen`) among competing branches whose tip is no more
than `-anchorcontestwindow` blocks (default 2) *below* the active tip. The test
is one-sided: a branch standing at or above the active tip always counts,
however far ahead it runs, and only a branch that has fallen further than the
window behind is treated as losing the race and ignored. So an old resolved fork
never drags the anchor down, and with no live fork the uncontested height *is*
the tip and the target is unchanged. The back-off only ever lowers the target,
and never below the parent block's anchor: if the uncontested height falls below
it, the producer reuses the parent's anchor instead, so monotonicity is never at
risk. The selection arithmetic is the pure function `AnchorUncontestedHeight`,
unit-tested without a Bitcoin daemon. This is block-producer policy and not a
consensus rule; §5 covers what it costs and why it is worth paying.

**Whether the anchor keeps moving during a contest depends on the rivals**, and
both regimes occur. `AnchorUncontestedHeight` takes the lowest fork point among
the rival branches currently inside the window, and a branch's fork point is
where it diverged from the active chain: extending that branch raises its tip
height and its `branchlen` together, so the fork point it pins does not move. A
single rival that persists inside the window therefore holds the target at one
fixed height, and the anchor sits there while Bitcoin's tip advances past it,
the trail growing by one for every parent block. The window is what ends that: a
rival stops counting once the active tip runs more than `-anchorcontestwindow`
blocks beyond the rival's own tip, after which the uncontested height is set by
whichever rivals remain, or is the tip itself when none do, and the anchor steps
forward. In the ordinary case rivals appear near the tip and expire behind it,
so the anchor advances in steps rather than block for block, and once no rival
is live it is back at the tip.

**Once committed to a branch, the producer follows it.** When Bitcoin has rival
branches, Sequentia anchors to the latest uncontested Bitcoin block (the last
block those branches share) only if the latest Sequentia block is itself
anchored to an uncontested Bitcoin block. If it is already anchored to one of
the Bitcoin branches, Sequentia keeps following that branch for as long as it
remains Bitcoin's best chain. The reason is that holding back only helps while
the chain is still on shared ground. Once the parent block's anchor sits above a
rival's fork point, the chain is committed against that rival: if the rival
wins, the parent block is unwound whatever the new block anchors to. Backing
off would protect nothing, and since the target would fall below the parent's
anchor it would freeze the anchor for as long as the rival kept pace, and with
it the escaping-stall relaxation (§5). In code, `AnchorUncontestedHeight` skips
every rival whose fork point lies below the parent block's anchor, provided the
Bitcoin node still reports that anchor at its height. If it does not, the
parent's anchor has been reorganized away, every rival counts again, and the
reorg-following watcher of §3 takes over. Rivals that fork at or above the
parent's anchor are still backed off from as before.

Anchoring is governed by these settings:

```ini
con_bitcoin_anchor=1     # enable anchoring (a chain-parameter property)
validateanchor=1         # validate each anchor against the Bitcoin daemon
anchorminconf=1          # confirmations required of the anchored Bitcoin block
anchorpollinterval=5     # seconds between Bitcoin tip/reorg polls
anchoravoidcontested=1   # hold the anchor off a contested parent-chain height
anchorcontestwindow=2    # a rival stops counting once the active tip runs
                         # more than N blocks beyond it
```

The Bitcoin connection becomes mandatory when `con_bitcoin_anchor` is set,
reusing the `-mainchainrpc*` transport. A startup probe analogous to
`MainchainRPCCheck()` refuses to produce blocks if anchoring is required but the
Bitcoin daemon is unreachable. Operational detail and tuning are in
[`05-operating-sequentia.md`](05-operating-sequentia.md).

## 3. Validation rules and the reorg-following watcher

### Validation in `ContextualCheckBlockHeader`

A block's anchor is checked against its parent and against the Bitcoin daemon:

- **Well-formedness (R1).** The fields are present exactly when
  `g_con_bitcoin_anchor` is set; `m_anchor_hash` is non-null on anchored blocks.
- **Monotonicity (R2).** `m_anchor_height` is not less than the parent's. If the
  height is unchanged from the parent, the hash must be unchanged too - a
  producer cannot silently swap which Bitcoin block a height refers to.
- **Bitcoin existence and best-chain membership (R3).** When `g_validate_anchor`
  is set and the anchor differs from the parent's, the node calls
  `CheckMainchainAnchor`, which queries the connected Bitcoin daemon: the header
  must exist, its height must equal `m_anchor_height`, and it must be on the
  daemon's active chain. The check distinguishes `OK`, `NOT_FOUND`, `STALE`
  (the hash is known but no longer on the best chain), `HEIGHT_MISMATCH`, and
  `NO_CONNECTION`.

Only `HEIGHT_MISMATCH` is permanent: a Bitcoin block's height never changes.
The other refusals depend on this node's own Bitcoin daemon, which may be behind
the network, still in its initial block download, unreachable, or in the middle
of a reorganization, so they are rejected as `BLOCK_RECENT_CONSENSUS_CHANGE`
(`anchor-unknown`, `anchor-stale`, `anchor-unverifiable`): nothing is marked
invalid and the peer is not penalized. The log says which state the daemon is
in: for `anchor-unverifiable`, whether it is behind (with its height and the
anchor's), still syncing, or unreachable. Such a block is not remembered, so the
node asks for it again: when the watcher sees the daemon's tip move, or the
daemon answer again after failing to, and the daemon has reached the highest
anchor left aside, it sends every peer a `getheaders`. The block is then taken
within about one `anchorpollinterval` of the daemon catching up, rather than at
the next block's announcement. One whose anchor is still not verifiable (the
daemon reached the height on another branch) is left aside again until the next
move.

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

When Bitcoin returns to a branch it showed before, the blocks the watcher
invalidated for that branch are valid again. The watcher keeps every block it
invalidated in a recovery set (the block index marks them, so a restart keeps
the set) and checks their anchors on every tick. A block whose anchor is
canonical again is reconsidered together with the blocks above it, but only as
far as Bitcoin's anchors allow: before it clears anything, the watcher checks the
anchor of every block the reconsideration would bring back, and in the same step
it keeps invalid each block above whose own anchor is still off Bitcoin's best
chain, with everything above that one. Those blocks wait in the recovery set for
their own anchor. A block whose ancestors off the active chain include one with
an orphaned anchor waits for that ancestor. So no block whose anchor Bitcoin
does not have is ever connected, not even for the moment before the walk would
disconnect it again; and an anchor the Bitcoin daemon cannot judge right now
revives nothing until a later tick can. The `reconsiderblock` RPC follows the
same rule on a node that watches Bitcoin: it clears an operator's
`invalidateblock` as before, but every block it would revive whose anchor is
off Bitcoin's best chain (the block named included) stays invalid, with the
blocks above it, and waits for its anchor in the recovery set. When the Bitcoin
daemon does not answer, a block the watcher itself invalidated stays invalid
until the watcher can judge its anchor, and every other mark is cleared as
usual, so an operator can still undo an `invalidateblock` during an outage.

This is a **consensus** invalidation, not a local policy choice: every node
following the same Bitcoin best chain reaches the same decision, because
Bitcoin's best chain is the shared oracle. The only sanctioned reason to discard
a certified Sequentia block is its anchor being reorged away on Bitcoin. As long
as Bitcoin does not reorganize a referenced block, no Sequentia reorg is
possible: committee certification gives immediate finality (see
[`04-proof-of-stake.md`](04-proof-of-stake.md)).

### Transactions of a lost block

When the watcher disconnects a certified block because Bitcoin replaced an
anchor on its branch, the block's transactions do not go back to an open
mempool where whoever leads next may respend their coins. They are carried
into the chain that replaces it. This is what keeps a swap against on-chain
bitcoin whole across a Bitcoin reorganization: the seller's lock, certified
before the buyer paid, is mined again before anything that conflicts with it,
whoever leads.

The rule has two layers.

1. **Records and bindings are validity.** Every node checks them, from the
   chain alone, for ever.
2. **The carry-over is a duty of the nodes that hold the lost block.** They do
   not produce, sign, or take as their tip a block that skips it. They do not
   resist a chain that a quorum has certified past them.

#### Definitions

- A *lost block* L is a block off the active chain that carries its parent's
  full certification quorum, or is an ancestor of such a block on the same
  branch. Its branch includes a block whose anchor is not on Bitcoin's best
  chain.
- The branch leaves the active chain at its *fork point* F. a_L is L's anchor
  height and H_L its height.
- The *window* W is 6 Bitcoin blocks, a consensus value. `-poslostwindow`
  sets it on custom chains only (the functional tests); the testnet and
  mainnet ignore it.
- A lost block anchored more than W above its fork point's anchor is not
  carried: whatever forked that long ago left the window before it was lost.
  A holder does not take it as evidence (`fork-too-deep`), does not protect
  it, and a record of it is not valid.
- *Certified* means a certificate that verifies, everywhere the rule reads
  it: the BLS aggregate under the committee its parent's anchor seeds,
  reaching the seat quorum. For a branch shown from outside the chain (a
  record, or evidence), every block's certificate is verified in order from
  F: block F+1's committee is seeded from F's anchor, on the chain, and each
  later block's from its verified parent's anchor. No committee is seeded by
  a value whoever shows the branch chooses. Every such committee is drawn
  from the stake registry as it stood at F, the registry block F+1's
  committee was really drawn from: the node rewinds the stake changes of the
  blocks above F from their undo data. So no key registered after the seed
  of the branch's first committee was public can sit on any of them.
- *The lapse.* A transaction of L stays protected for a candidate B on a
  chain containing F until B's chain has a block R at height H_L and B's
  anchor is more than W above both a_L and R's anchor. This is a function of
  B's chain alone. A transaction that is not yet valid (an `nLockTime`, a
  relative lock, a maturity) is valid again by H_L at the latest, so it is
  never left unprotected before it can return, however fast Bitcoin moves.
- A node *holds* L when it connected L and saw it leave, or when it was shown
  L (P2P `poslost`, or `submitlostblock`) and judged it lost and certified.
  It appends L to `lostblocks.journal` and syncs it before the disconnect
  releases the chain lock, so before it signs anything afterwards; the
  maintenance pass folds the journal into `lostblocks.dat`. It reads both
  back before it connects a block or meets a peer. A holder judges whether
  L's anchor is off Bitcoin's best chain from its own Bitcoin daemon: the
  best chain has another block at that height. The daemon need not have
  seen the stale block.

#### The duty of a holder

A holder judges a candidate block B that extends its active tip, has no
descendant whose certificate verifies, and arrives outside initial block
download. The duty applies when B is built on a chain containing F and L has
not lapsed for B.

The pending lost transactions are the non-coinbase transactions of every lost
block whose fork point is in B's chain, in lost order: the loss they belong to
(an earlier loss first), then height, then position. Each is classified
against B's parent, with the lost block's own witnesses:

- **due**: valid there (inputs, amounts, scripts, record rules, payment
  proofs, and the bindings of the records in B's chain). Each due transaction
  is judged after the ones before it.
- **later**: valid only later as it is. This covers an `nLockTime` or a
  relative lock that the shift made non-final, a coinbase spend not yet
  mature, an unbonding output not yet unlocked, an input that a record of B's
  chain binds to another transaction, and the child of a later one.
- **failing**: not valid as it is. This covers a payment proof that names a
  stale Bitcoin block, an anchor floor not reached, a record rule bound to a
  height (a payout announced without its notice at the new height, a pot
  claim, a conflicting delegation or payout record), a peg-in, and the child
  of a failing one.
- **gone**: an input is spent on B's chain, because the transaction is already
  mined there or something it conflicts with is.

The classification uses validity's own checks, the records' bindings
included, so a block that does what the duty asks is valid.

The holder refuses B (`bad-lost-replay`) unless B begins, right after its
coinbase, with the due transactions in lost order: the longest run of them,
from the first, that fits the consensus limits. The weight left for them is
the block's maximum weight less 8,000 (the header and the coinbase), less the
certificate's maximum size, less 100,000 when B owes a record, and less 640
for each fee asset the run pays in (one coinbase output at the largest payee
script). Their signature operations, counted as `ConnectBlock` counts them
(legacy, P2SH and witness), stay within the block's 80,000 less 4,000. These
reserves are fixed, so the run is a function of the chain and the lost blocks
alone: a leader cannot shorten it by padding its own coinbase. When the due
transactions do not all fit, B carries nothing else; the rest are due in the
next block, and ordinary transactions follow in the first block whose run
takes the last of them.

It refuses B (`bad-txns-lost-respend`) if any other transaction in B spends an
outpoint that a pending lost transaction spends. Which of two conflicting lost
transactions stands is decided by lost order alone.

It refuses B (`bad-lost-unrecorded`) when B does not record the failing
transactions it owes. A failing transaction is owed when a record of it would
be valid in B, judged by the record check itself (below). Until it is
recorded on B's chain, each block owes the longest run of the owed failing
transactions, in lost order, whose records fit the block's 25,000 record
bytes; the rest are owed by the next block. A failing transaction whose
record cannot fit 25,000 bytes even alone is never owed. The holder logs it
("can never be recorded"); the holders' refusal is its only protection, and
the transactions after it are still owed.

A later transaction is protected by the same refusal until it falls due, and
needs no record: it becomes valid as it is by H_L at the latest, and the duty
mines it then.

The refusal is soft. No peer is punished, no mark that a restart keeps is
written, and the refused block is judged again whenever the holder's evidence
changes. The holder drops L once its active chain has lapsed for L (or, when
its active chain no longer contains F, once Bitcoin is 2W above a_L: a bound on
memory, not a rule), and then judges every block it refused by validity
alone.

A producer that holds L builds exactly what the duty asks. The due
transactions go first, whatever its own fee list and floor say, and the
records go in the coinbase. Its mempool refuses a spend that conflicts with a
pending lost transaction (`txn-lost-conflict`).

#### Records and bindings

A record is a coinbase output `OP_RETURN 'SQLR' <record>`, value 0. The record
contains:

- a version (2);
- the lost branch's headers from F's child, each with its certificate;
- one entry per recorded transaction: the index of its lost block, its
  position, the merkle branch to that block's merkle root, and the transaction
  without its witness.

A block carries at most 25,000 record bytes, and a record at most 256 lost
headers; both are consensus values. The records count toward block weight
like any coinbase output. The coinbase is under the block's merkle root, so
the record needs no commitment of its own and the block format is unchanged.
A block that carries a record is marked in the block index, so the bindings
are rebuilt after a restart from the marked blocks, however far back.

A record is valid in a block B when all of the following hold:

- Its first header's parent is the block of B's chain at that height, and the
  headers link one to the next at consecutive heights, with anchors that obey
  R2 from F and are at most W above F's anchor.
- For each entry:
  - its position is below 2 to the power of its branch's length, and not 0;
  - the merkle branch leads from the transaction id to its block's merkle
    root;
  - its lost block has not lapsed for B.
- Every header up to the highest entry carries a certificate that verifies in
  order from F under the registry at F (see "Certified" above).
- For each entry, a header at or below it has an anchor at a Bitcoin height
  at most B's anchor height that is not B's anchor's ancestor at that height.
  The node reads that ancestry by walking B's anchor's parents in its Bitcoin
  daemon (at most 2W steps). It is a fact of Bitcoin's header tree: the same
  on every node that knows B's anchor, whatever its daemon's best chain is at
  the moment, and unchanged by any later reorganization.

Otherwise B is invalid (`bad-lost-record`), on every node alike. The one
exception is a daemon that does not know B's anchor, or does not answer: B is
then left aside (`lost-record-unverifiable`), never marked invalid, and taken
when the daemon can say.

Once a record is in a chain, a later block of that chain, or B itself, is
invalid (`bad-txns-recorded-respend`) if the recorded transaction's lost block
has not lapsed for it and it spends an input of the recorded transaction
through any other transaction. The same transaction id with another witness is
that transaction. A record binds only coins still unspent: a spent coin cannot
be spent again.

#### What this guarantees, and to whom

- **With an honest quorum, a lost transaction stays protected even when the
  seller leads every height.** It comes back at the height it falls due, at
  the latest when the lost blocks' heights are rebuilt (about ten minutes per
  stale Bitcoin block). It is then in the chain for good. A failing
  transaction is bound from the first block whose anchor shows its branch
  lost, until it lapses. A payment-proof claim is the usual one, and it keeps
  its id when it is proven again: the proof is in the peg-in witness slot.
  When its payment is mined again, anyone can attach a fresh proof and mine
  it. When the payment is gone for good, the lock is released at the lapse
  and its refund path is open again.
- **A block that skips the duty cannot be certified unless at least two of
  the lost block's own signers sign against what they hold on disk.** Any two
  quorums share at least two members.
- **A dishonest quorum can certify a block that skips the duty.** The holders
  refuse it until it has a certified descendant whose certificate verifies,
  and take its chain then: a quorum that goes on certifying is followed, and
  one that stops is outrun by the holders' chain through reconciliation (§6
  of [`04-proof-of-stake.md`](04-proof-of-stake.md)). Against a dishonest
  quorum the rule protects nothing.
- **A dishonest quorum can also make a spend invalid.** On a chain without
  this rule a dishonest majority can only censor a spend: it refuses to
  certify blocks carrying it, and escaping-stall blocks that carry it lose to
  its certified siblings, but the spend is never invalid. With records, a
  quorum can certify a fabricated lost branch on a fork point of the chain,
  with an anchor it makes up, and record a transaction of its own choosing;
  the coins that transaction spends can then be spent by nothing else, on
  every node, until the record lapses. That is a new kind of power, bounded in
  time, and the price of judging records from the chain alone. The same holds
  for a coalition that holds a seat quorum at some height by the draw: besides
  certifying that height alone, as it could anyway, it can record a fabricated
  transaction and freeze a coin until the lapse. The coalition can pick the
  fork point among the heights of a window (about 2W Bitcoin blocks), but
  not its committee's members: they come from the registry at the fork
  point, so it cannot register identities after the seed is known and grind
  their seats. On the testnet's committee of 12 seats (quorum 7, two stakers
  holding about 38 % and 36 % of the registered stake), the smallest
  coalition with a seat quorum at some height of every window holds 39.4 %
  of the stake. With a committee at its cap of 250 seats (quorum 126), a
  coalition that splits its stake into many identities at the floor stake
  reaches a seat quorum at some height of a window with about 42 to 44 % of
  the stake, and at some height of a day with about 40 to 42 %, whether 250,
  400 or 1,000 honest stakers hold the rest in equal or unequal stakes.
  Below a seat quorum nothing can be recorded at all.
- **Every node judges records alike.** A late node, one syncing from nothing
  with a Bitcoin daemon that never saw the stale block, and one whose daemon
  goes back and forth between a block's header and its body all reach the
  verdict of the nodes that were there, because the record is judged against
  the judged block's own anchor, not against what a daemon happens to know.
- **Bitcoin stays supreme.** Nothing here keeps a block whose anchor Bitcoin
  replaced, delays the watcher, or undoes an accepted block. Evidence shown
  late never touches history. It binds only candidates for the tip, and only
  coins still unspent.

#### Costs

- **The fee market.** A proposer's fee list is its own choice everywhere
  except here. A due lost transaction is mined whatever asset and rate it
  pays, so a producer carries transactions whose fee asset or rate it would
  not accept itself. This is required for safety: if a producer could skip a
  lock whose fee asset it does not list, a seller whose own producer alone
  lists that asset would get back the gap after W. No consensus rule judges a
  fee asset or rate, so a carried transaction is never invalid for its fee.
  The carried transactions were accepted once by a producer and certified by
  the committee; the open fee market governs their first inclusion only. A
  producer claims the fee of a carried transaction as it claims any fee, in
  whatever asset it is paid. A producer that must not hold an asset it does
  not list sets `-poslostleaveunlisted`: its coinbase then leaves the fees of
  carried transactions in unlisted assets unclaimed, the block stays valid
  (consensus lets a coinbase claim less than the block's fees), and that fee
  is claimed by nobody. What the producer gives is block space.
- **What a producer can make the others carry.** A producer that fills its
  own certified block with its own transactions, paying fees in an asset only
  it lists, makes the others carry them once if that block is lost: the lost
  block's space, at the next heights, and never more. Bitcoin going there,
  back and there again mines each of them once, because a carried transaction
  already in the chain is spent there and is not due again. A producer that
  leads a share s of the heights leads about s of the Sequentia blocks one
  stale Bitcoin block takes with it, so it can make the others carry about
  s × 10 blocks of its own transactions per stale Bitcoin block, at 60-second
  blocks. It cannot cause a stale Bitcoin block without mining Bitcoin.
- **Honest conflicts wait.** A holder's own replacement of a pending lost
  transaction (a fee bump, a refund) waits until it is due, or until the
  lapse. The
  holder's mempool refuses it (`txn-lost-conflict`). A wallet's `bumpfee`
  still records the replacement and returns its id: the wallet shows it
  unconfirmed and in conflict with the lost transaction until that is mined
  again, in the first block of the replay when it is due, and the bump is
  then a conflicted transaction that never confirms.
- **Throughput while replaying.** The lost transactions take back the space
  they had. A deep reorganization replays its blocks at their heights. While
  it does, a block full of due transactions carries nothing else.
- **Stake changes in the lost branch.** A lost branch whose certificates do
  not all verify under the registry at F cannot be recorded, because stake
  changed inside the branch before the entry: the registry at a lost block's
  own parent is not in B's chain until the lost stake transactions are
  replayed. A stake change on B's chain after F does not stop a record. Its failing transactions are then
  protected only by the holders' refusal until the lapse, never by nothing:
  the holders bind every outpoint of a lost block whatever its record. A
  stake change is itself a lost transaction, due and replayed in lost order,
  so the registry the next blocks are judged with converges on the lost
  branch's as the replay proceeds.
- **How long conflicts wait.** A holder's own replacement of a pending lost
  transaction, and a refund after a claim that failed for good, wait until
  the lapse: W Bitcoin blocks after the chain rebuilds the lost block's
  height. For a shallow loss that is about W; for a deep one, W after the
  replay ends.
- **Record bytes and the check's cost.** A record carries no Bitcoin header.
  Judging one walks B's anchor's parents (at most 2W header reads, cached per
  anchor), rewinds the stake registry from B's parent to F (one block and
  its undo data read per height between them, cached per fork point and
  parent), and verifies one certificate per lost header up to its highest
  entry.
- **The holders' check per candidate.** The plan (the due run and the
  records owed) is computed once per parent on each holder and shared by the
  template, every proposal on that parent and the block's connection. It costs
  about 0.2 ms per pending transaction, almost all of it reading the coins its
  inputs spend: about 1 second with one stale Bitcoin block of full blocks
  pending (4,800 transactions) and about 3 seconds with three (14,400). It is
  paid in the first round at each height of the replay. At the testnet's
  round lengths (9.45 s rounds, a 6.75 s collection window) the autonomous
  committee replays three stale Bitcoin blocks of full blocks with every
  height certified in its first round, 7 to 17 seconds apart.
- **A deeper fork point.** A lost branch whose transactions' window has
  passed is not carried.

#### A member that binds what it signed

`-poslostbindsigned` (off by default) is signing behaviour, never part of
what the rule guarantees. A member running it holds every block it signs on
disk (`lostsigned.dat`) before its signature leaves. When such a block's
anchor goes stale and the block never certified, the member refuses, until
the block lapses and at most until Bitcoin's height passes its anchor plus
W, a candidate that spends an
outpoint one of its transactions spends (`bad-txns-signed-respend`), and its
mempool refuses such a spend. It owes no replay and no record, and it relays
nothing. It narrows the case of two blocks certified at one height on two
live Bitcoin branches: honest members that signed one side do not sign a
conflicting other. Its cost is a pause. A block that carries a conflicting
transaction cannot be certified while the members that signed the lost
candidate are at least as many as the committee less the quorum, and the
height waits for a leader whose block does not carry it. A member whose every
candidate at a height is refused or set aside for a stale anchor proposes
again only once Bitcoin's tip moves, so with fewer signers the height can
also wait for the next Bitcoin block, and in some runs for the whole window.

#### For a buyer's wallet

A wallet pays against a lock only when `checklock "txid"` answers
`payable: true`. It runs the three checks the lock must pass, each instant
except during a live Bitcoin fork, and otherwise lists the reasons in words a
wallet can show:

- It is certified and final: it has stood the observation window (§6 of
  [`04-proof-of-stake.md`](04-proof-of-stake.md)) with no competing
  certificate at its height.
- Bitcoin's tip is uncontested in the buyer's Bitcoin view: no other branch
  within the contest window (§5), as `getchaintips` shows it.
- Its unburied ancestry holds no transaction that a reorganization can make
  fail for good, and was examined whole. The unburied ancestry is the lock
  and every ancestor confirmed in a block anchored fewer than W + 1 Bitcoin
  blocks below the buyer's Bitcoin tip. A payment-proof claim, a peg-in, and
  a transaction that creates or spends a stake record are never carried over
  a reorganization, so nor is a lock funded from one: its seller could make
  that parent fail by double-spending a Bitcoin payment it made to itself.
  The walk examines at most 1,000 unburied transactions (and 20,000 lookups
  in all); a lock whose ancestry is larger is not payable, because a failing
  ancestor could sit beyond what was examined. A lock funded from older
  coins passes. `checklockancestry` runs this check alone.

The first two close the one case the duty cannot: two blocks certified at one
height on two live Bitcoin branches, neither set of signers knowing of the
other. The third closes a lock whose own funding can be made to fail.

#### Activation

The rule is active from genesis on a fresh chain. On a running chain it
starts at a height bound to the chain's genesis hash
([`CONTRIBUTING.md`](../../CONTRIBUTING.md)), with every committee member
upgraded. A node on the old rules reads a record as an ordinary `OP_RETURN`
output and follows the upgraded committee's chain. It parts from it only if
a quorum certifies a block that breaks a binding.

`getdeploymentinfo` lists the rule as `lost_blocks`, with its window `W`, on a
Proof-of-Stake chain; `active` is false on a node that does not watch Bitcoin,
since such a node holds no lost block.

Tested in `feature_pos_lost_blocks.py`, `feature_pos_lost_attacks.py` (a
transaction not yet due while Bitcoin outruns the window, a descendant whose
certificate does not verify, a record whose first header carries no
certificate, a record certified with keys registered after its fork point
and its control), `feature_pos_lost_autonomous.py`
(the autonomous committee at the testnet's round lengths: the gap, a deep
replay, the member's switch), `feature_pos_lost_replay.py` (the
due run and its floor, the producer's opt-out, a producer's own-asset block,
a fork point too deep, kill -9 during a replay, full lost blocks) and,
together with the payment proof, `feature_pos_lost_swap.py`.

### What the watcher costs the Bitcoin daemon

The watcher re-examines the **whole** Sequentia chain on every tick, down to
height 1, with no depth floor: a block is valid if and only if its anchor is on
Bitcoin's best chain, at any depth, so there is no height below which checking
can stop. That walk is in memory. What it must not do is turn into traffic.

Two things keep the traffic flat as the chain grows.

**Anchors are shared.** Sequentia produces a block every 60 seconds and Bitcoin
one every ~10 minutes, so about ten consecutive Sequentia blocks carry the
same anchor. The walk collapses each such run and asks about *distinct anchors*,
of which there is roughly one per Bitcoin block the chain spans.

**Verdicts survive what cannot invalidate them.** Each distinct anchor's verdict
— canonical, or definitively off the best chain — is cached, and a cached verdict
is discarded exactly when Bitcoin does something that could have changed it.
Extending Bitcoin cannot: appending blocks leaves the best chain below the old
tip untouched, so every verdict already held is still correct. A reorganization
can, but only above its fork point. So on each Bitcoin tip change the watcher
spends one `getblockheader` asking whether its previous Bitcoin tip is still on
the best chain: if it is, the move was an extension and nothing is re-checked; if
it is not, it walks back to the fork point (one call per block of reorg depth)
and drops only the verdicts above it.

The result: a tick where Bitcoin stood still costs one `getbestblockhash`, and a
tick where Bitcoin produced a block costs a small constant more — regardless of
whether the chain is a day or a decade old.

This matters more than it looks. Every call opens a **fresh TCP connection**
(`mainchainrpc.cpp` sends `Connection: close`), and on testnet every node that
has not configured `-mainchainrpchost` points at the same shared gateway. Before
this was fixed the watcher discarded all its verdicts on every Bitcoin block,
extension included, and re-asked about every distinct anchor on the chain: some
three thousand calls per Bitcoin block on a chain a few weeks old, growing with
every day the chain lived, from every node, forever.

`getmainchainrpcstats` reports the calls this node has made to the Bitcoin
daemon since startup, in total and per method. Sample it twice around a known
interval to see the standing load; `getbestblockhash` is issued once per watcher
tick and by nothing else, so its count is also the tick count and the difference
between the two is everything the anchor walk actually cost.
`feature_anchor_rpc_cost.py` asserts that difference does not grow with the
number of distinct anchors on the chain.

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

- an interactive frontend (`sequentia-qt`) offers two named choices — close and
  start Bitcoin Core first, which is the default and what closing the window
  does, or start without following Bitcoin;
- every non-interactive frontend, `sequentiad` included, answers no, so the node
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

`recoveryset` lists the blocks the anchor watcher invalidated because their
Bitcoin anchor was orphaned and still tracks, so that it can restore them if
Bitcoin reorganizes back. An entry leaves once its branch reconnects to the
active chain. A block restored after the chain has moved on to a branch that
fork choice prefers never reconnects, so it stays listed until the node
restarts; that costs nothing.

### The `getmainchainrpcstats` RPC

`getmainchainrpcstats` (also only when `con_bitcoin_anchor` is enabled) reports
how many RPC calls this node has made to the Bitcoin daemon since startup:

| Field | Meaning |
|---|---|
| `calls` | Total calls, failures included — a failed call still cost a connection attempt. Every call is one fresh TCP connection, so this is also the number of connections opened. |
| `bymethod` | The same total broken down by method name. |

It makes no Bitcoin call of its own, so sampling it does not perturb what it
measures. See *What the watcher costs the Bitcoin daemon* above for how to read
the numbers.

### Starting without a Bitcoin node

If `con_bitcoin_anchor` is set and the Bitcoin daemon cannot be reached at
startup, the node asks — it does not decide. `AppInitMain` puts the question
through `uiInterface.ThreadSafeQuestion`, so:

- **sequentiad** and every other non-interactive frontend answer "no" (there is
  nobody to ask) and the node **does not start**. This is the pre-existing
  behaviour, and it is the right default for a machine nobody is watching. Note
  that the test is now "can this frontend ask a human", not `-server`: a GUI user
  who had enabled the RPC server used to get the daemon's flat refusal, for no
  good reason.
- **sequentia-qt** shows a dialog with two named buttons — *Close and start
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
Sequentia tip references a current Bitcoin block - to within a ~1-block lag,
since each new block anchors to Bitcoin's tip (§2; §5 covers the one exception,
a contested parent tip). That lag is small relative to the Bitcoin-paced swap
clock. A claimant still waits for the leg's anchor to reach their desired
Bitcoin **confirmation depth** - a fresher anchor is a *shallower* one - but
pays no cross-chain buffer on top of that wait.

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
one (then, among equally-fresh proposals, the lowest exponential-race election
score from `pos_exprace_height` onward, and the lowest raw leader `beta` below
it), so that is the block that gets certified - the paper's Principle 7
freshness preference, implemented in the gossip committee's candidate ordering.
This is a *pre-certification* signing preference, not a fork-choice vote, and
the anchor is deliberately not a key in fork choice: in an immediate-finality
system, keying fork choice on the anchor could let a new Bitcoin block reorder
or overwrite already-certified blocks, which must never happen. The fork-choice
and finality details are in [`04-proof-of-stake.md`](04-proof-of-stake.md).

### The exception: a contested parent chain

Tracking Bitcoin's tip within one Sequentia block is the ideal, and it is what
the chain does whenever its parent chain has a single undisputed tip - the
normal condition on Bitcoin mainnet, which sees far fewer contested blocks than
testnet4. The exception is a **contested parent tip**. While rival branches are
live at or near Bitcoin's tip, the producer holds the anchor at the last
parent-chain height every contender agrees on instead of advancing onto ground
that may be reorganized away, for as long as the chain itself is still anchored
on that shared ground (§2). The trade is deliberate: a Sequentia block
anchored to a losing parent block must be unwound when the parent fork resolves,
and unwinding certified Sequentia history is far more disruptive than carrying a
slightly older anchor for a few blocks. The policy was adopted after the
2026-07-11 testnet4 anchor-split incident, to reduce exactly that unwind; the
related finality work is in
[`incident-2026-07-17-finality-partition.md`](incident-2026-07-17-finality-partition.md).

**The back-off is producer policy, not a consensus rule.** `-anchoravoidcontested`
and `-anchorcontestwindow` (with `-anchorminconf` setting the baseline target)
change only which anchor a node picks for the blocks *it* produces, never which
blocks it accepts, so operators may set them differently without splitting the
network. `-anchorcontestwindow` does have one further node-local effect, and it
is worth knowing before changing it: finality reconciliation reads the same
window. Before releasing its own finalized point for a rival branch, a node
recomputes the uncontested parent height through the same
`GetMainchainUncontestedHeight` and refuses to release while the rival's
certifying block anchors above it (`src/anchor.cpp`), so a release can never
fire into a live parent-chain fork
([`04-proof-of-stake.md`](04-proof-of-stake.md) §6). Widening the window counts
more branches as live contests and so lowers the uncontested height, making the
node both slower to advance its own anchor and slower to release finality;
narrowing it does the reverse, and lets a release fire while the parent chain is
less settled. Either way the node's *validity* rules are untouched: what changes
is what it produces and how fast it converges.

*Anchor* validity is exactly the three rules of §3: the anchor is present (R1);
anchor heights are monotonically non-decreasing along the chain, with an
unchanged height forcing an unchanged hash (R2); and the anchor is on the
Bitcoin best chain at the claimed height (R3). R1 and R2 are settled from the
block and its parent alone; R3 is the one that needs a live Bitcoin daemon, and
it is skipped when the block's anchor is unchanged from its parent's
already-validated one. It is also skipped entirely under `-validateanchor=0`, a
setting that leaves only R1 and R2 enforced and therefore abandons the rule
every other guarantee in this chapter rests on: a node run that way no longer
checks that the Bitcoin block its chain commits to is real and canonical.
Otherwise, anchoring to the naked Bitcoin tip stays fully valid, and repeating
the previous anchor is valid indefinitely. There is no minimum-depth rule and no
maximum-advance rule on the anchor, so nothing in consensus bounds how far an
anchor may trail, and no node rejects a block *on its anchor* for trailing
further back than it would have chosen itself.

**One consensus rule elsewhere does read the anchor height**, and it runs the
other way: the escaping-stall relaxation that lets a block be certified below
the committee quorum. Such a block must additionally anchor at least
`POS_ESCAPING_STALL_ANCHOR_GAP` (3) parent heights past its parent's anchor, and
must show `-posescapestallmtpgap` seconds of parent-chain median-time-past
between the two anchors (default 600, one Bitcoin block interval). The time gap
is there because the height gap alone is not evidence of a stall: a parent-chain
block storm can supply three heights in seconds with the chain fully alive
(`src/pos.h`), and requiring parent-chain time to have passed as well is what
stops that from being enough. A block failing either gap is rejected. Both
bundled Sequentia chains run the BLS committee by default
(`src/chainparams.cpp`), so the code they actually emit for
a sub-quorum block without the height gap is `bad-posbls-agg-quorum`;
`bad-posvrf-agg-quorum` is the same rejection on the legacy MuSig2-aggregate
path. Either way, a block that clears the height gap but not the time gap is
rejected as `bad-pos-escape-stall-too-soon`, which is common to both paths. A
block carrying a full quorum is never subject to either test. The rule is in
[`04-proof-of-stake.md`](04-proof-of-stake.md) §5.

**Only the height half of that rule survives `-validateanchor=0`.** The anchor
heights are read straight from the block and its parent, but the
median-time-past evidence has to come from the parent chain, so
`CheckEscapingStallMtpGap` returns *allowed* outright once anchor validation is
off (`src/anchor.cpp`) - the same reasoning as the R3 skip, since the time
evidence rides on the very daemon such a node has stopped consulting. A node run
that way therefore enforces only the 3-height gap, and is blind to exactly the
case the time gap was added for: a parent-chain block storm supplying three
heights in seconds while the chain is fully alive.

**The practical consequence is a longer wait, never a weaker guarantee.** While
a contest is live, the tip's anchor can trail Bitcoin's tip by more than one
Bitcoin block. An application that needs a Sequentia block anchored at or
above a specific parent height - most concretely a purely on-chain cross-chain
swap, whose Sequentia leg must reference the Bitcoin block its BTC leg confirmed
in - waits for the contest to clear before that anchor appears. Nothing about
the swap becomes unsafe: reorg-following still binds the two legs to a single
fate (§6), and the reorg-protection buffer is still zero. It simply takes longer,
rarely on mainnet, routinely on testnet4.

How much longer is not something this chapter quantifies. The trail is set by
how long rival branches persist near the parent tip (§2), which is a property of
the parent chain on the day and not of any rule here, and no measurement harness
for it is carried in the tree.

The wait has a second, less obvious form. Because the escaping-stall relaxation
is keyed on the anchor advancing 3 parent heights past the parent block's
anchor, holding the anchor at the uncontested height also delays reaching that
gap: a committee that has fallen under quorum takes longer to certify its way
out of a stall precisely while the parent chain is contested. That is the same
direction the 2026-07-17 fix pushed when it added the parent-chain
median-time-past requirement to the same relaxation, and it is the intended
trade in both cases - a sub-quorum block should be hard to mint - but it is a
liveness cost, and it lands exactly when the parent chain is least settled.

The cost is bounded by the rule of §2 that a producer follows the branch it is
already committed to. The back-off can delay an escaping-stall block only while
the parent block's anchor is still on ground the rivals share. As soon as one
block anchors above a rival's fork point, that rival stops holding the anchor
back, and the next escaping-stall block is due as soon as the parent chain
supplies the height and time gaps. Without that rule, a rival that forked just
below the parent's anchor and kept pace with the tip would pin the target under
the parent's anchor, and a committee below quorum could not certify anything
until the rival dropped out of the window.

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
