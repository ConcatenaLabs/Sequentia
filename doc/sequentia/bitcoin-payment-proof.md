# Releasing an asset on a proof of a bitcoin payment

A Sequentia output can be locked so that it opens only for a spender who proves
that a given bitcoin payment is in Bitcoin's chain: an output paying a named
script at least a named amount, mined in a Bitcoin block after the one the lock
itself was anchored to, and no later than a deadline the lock names. A refund
path opens once the chain's anchor has passed a later Bitcoin height.

This is the whole of a swap of a Sequentia asset against on-chain bitcoin. The
bitcoin side is an ordinary payment: no hash lock, no secret, no script on
Bitcoin, nothing a Bitcoin wallet has to support. The Sequentia side is checked
by every validating node against the chain's own Bitcoin anchors, so a claim is
valid only in a block whose anchor chain contains the payment. If the payment
leaves Bitcoin's history, the claim leaves Sequentia's with it and can never be
mined again; if the payment is mined again in another block, the claimant
attaches a fresh proof. No watcher, no secret and no delay are involved.

This page specifies the rule for node developers and the swap for wallet
developers. It assumes [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md):
the anchor fields, rules R1 to R3 and the reorg-following watcher.

## 1. The rule in one paragraph

Two tapscript opcodes, `OP_INSPECTBTCPAYMENT` (`0xe5`) and
`OP_CHECKANCHORHEIGHTVERIFY` (`0xe6`), take meaning from two kinds of record a
spender places in the input's taproot annex: a **payment proof** (a Bitcoin
transaction, the output it pays, the block it is in and the path to that
block's merkle root) and an **anchor floor** (a Bitcoin height). The script
reads them through the opcodes; the node checks them against the block, the
spent coin and the anchor, outside the script, the way a node checks
`nLockTime` and `nSequence` for `OP_CHECKLOCKTIMEVERIFY` and
`OP_CHECKSEQUENCEVERIFY`. A payment proof is valid in a block only if its
Bitcoin block is in that block's anchor chain and above the anchor of the block
that created the coin being spent. An anchor floor is valid in a block only if
the block's anchor height is at least the floor.

## 2. The form: a tapscript opcode over a node-checked annex record

Three forms were weighed.

| | Tapscript opcode | Simplicity jet | Witness field the node checks, no script |
|---|---|---|---|
| Only tightens | yes: the opcodes are `OP_SUCCESS` today, and the annex is ignored | no: a new jet is a program old nodes refuse to parse, so a spend using it is a relaxation | yes |
| What scripts can do with it | anything tapscript does around it: keys, deadlines, Elements introspection, 64-bit arithmetic | the most, once written | nothing: the node checks a fixed condition and scripts cannot compose it |
| Code reused | the interpreter's `OP_SUCCESS` path, the Bitcoin transaction and merkle-block types and parser of the peg-in code, `CheckMainchainAnchor` and its verdict cache | the same node side, plus changes inside the vendored interpreter | the same node side |

**Decision: the tapscript opcode.** It is the only form that both tightens and
composes. The two opcodes are kept small: the node does all checking that
depends on the chain, and the opcodes only read what the node has checked.

**Why the opcodes do not read the chain themselves.** A script's result is
cached by witness transaction id in the script execution cache and reused
when the transaction is mined, and scripts are checked in parallel with no block in
hand. A script that read the containing block's anchor would be cached valid
from the mempool and reused under a block with a lower anchor. The two-part
design keeps every script a pure function of the transaction and its spent
outputs: the opcode compares the record with constants in the script, and the
node compares the record with the block. This is exactly how
`OP_CHECKLOCKTIMEVERIFY` divides the work with `nLockTime`.

**Why the annex.** The record must be per input, readable by the node without
running the script, covered by the witness transaction id (so a cached script
result belongs to exactly that proof), and larger than a tapscript stack item
may be (a proof is around a kilobyte). Two places qualify:

- the **taproot annex**, the last witness element when it starts with `0x50`;
- the **peg-in witness** of a non-peg-in input, which every Elements
  transaction serializes, which consensus ignores today, and which only the
  mempool refuses (`extra-pegin-witness`).

The annex is chosen. The taproot signature hash commits to it, so the claimant's
signature covers the proof and the floor it carries: nobody relaying the
transaction can swap the proof or raise the floor to hold it up, and the signer
knows exactly which Bitcoin block it is claiming against. A Simplicity program
can see the annex as well (its transaction environment includes it), so a jet
reading the same records can be added without moving them. The peg-in slot is
outside every signature and invisible to Simplicity; it was rejected for those
two reasons.

The cost of that choice: after a Bitcoin reorganization that mines the payment
in a different block, the claimant re-signs its claim with the new proof. The
claimant is the party that wants the claim mined, so this asks nothing of anyone
else.

The records are read only on a **tapscript leaf** spend (leaf version `0xc0`).
On a Simplicity leaf the annex already has a meaning: it is padding that buys
execution budget, and its contents are arbitrary. Records there would need their
own marker; that is left to the jet, when one is written.

**Rejected:** a lock that names the payment in a data output and a rule that the
lock's block carry the proof (it binds the lock rather than the claim, so a
payment must exist before the asset is locked, which is the wrong order for a
sale, and it does not compose); the proof as an ordinary tapscript stack item
(stack items are limited to 520 bytes by consensus).

## 3. The records

An annex on a tapscript leaf spend is read as `0x50` followed by records, each
`type` (one byte), `length` (CompactSize) and `length` bytes of value. An annex
that does not parse this way carries no records. Unknown types are ignored by
consensus, so further types can be added later as tightenings, and refused by
relay policy. Each known type may appear at most once per input.

**Type 1, payment proof.** In order:

| Field | Encoding | Limit |
|---|---|---|
| height | 4 bytes, little-endian | the Bitcoin height of the block |
| vout | 4 bytes, little-endian | index of the paying output |
| payment transaction | CompactSize length, then the transaction without its witness | at most 100,000 bytes |
| coinbase transaction | the same, for the block's coinbase | at most 100,000 bytes |
| merkle block | CompactSize length, then a Bitcoin merkle block | at most 4,000 bytes |

The merkle block is exactly what Bitcoin Core's
`gettxoutproof '["<payment txid>","<coinbase txid>"]' <block hash>` returns: the
80-byte header and a partial merkle tree matching the two transactions. The
100,000-byte limit is Bitcoin's own standardness ceiling for any relayed
transaction, so no payment a Bitcoin node relays is too large to prove.

**Type 2, anchor floor.** A 4-byte little-endian Bitcoin height.

## 4. What the node checks

### 4.1 The proof, on its own

All of these depend only on the record's bytes:

1. The value decodes exactly, within the limits above, with no trailing bytes.
2. The merkle tree's root equals the header's merkle root, and the tree matches
   exactly two transactions: the coinbase at index 0 and the payment at an index
   above 0.
3. The coinbase transaction hashes to the match at index 0, is a coinbase (one
   input, null prevout) and is not 64 bytes long.
4. The payment transaction hashes to the other match, is not a coinbase, is not
   64 bytes long, and has an output at `vout`.
5. The header's proof of work meets its own `nBits`, within the parent chain's
   proof-of-work limit (`CheckParentProofOfWork`).

**The 64-byte weakness.** Bitcoin's merkle tree hashes a leaf and an inner node
the same way, so 64 bytes can be read either as a transaction or as two child
hashes. That allows two forgeries: presenting an inner node as a transaction,
and presenting an invented transaction as a leaf one level below a real 64-byte
transaction. Both put the claimed leaf at the wrong depth. The tree's depth is
not committed in the header, but every real leaf of a block is at the same
depth, and the coinbase is always the leftmost one. Proving the coinbase in the
same partial tree as the payment puts both at the depth the tree's transaction
count implies, and the coinbase cannot be forged one level down: that would need
a real 64-byte coinbase whose first 32 bytes, which hold the version, the input
count and most of the all-zero null prevout, equal the hash of the forged one.
So check 2 with check 3 fixes the depth, and check 4 refuses the first forgery
directly. Checking only that the payment is not 64 bytes would not be enough,
because the second forgery's invented transaction can have any size. If Bitcoin
makes 64-byte transactions invalid, check 3 stays correct and becomes redundant.

**Duplicated subtrees** (the weakness behind CVE-2012-2459) are refused by the
partial merkle tree's own traversal, which rejects identical left and right
branches.

### 4.2 The proof, in its block

For a transaction in a block whose anchor is at Bitcoin height `A` with hash
`A_hash`, each input carrying a payment proof for block `H` at height `h`:

6. **Below the anchor:** `h <= A`. A plain comparison.
7. **After the lock's anchor:** `h` is above the anchor height of the block
   that created the coin this input spends. The node finds that block by the
   coin's height on the chain being connected, as it does for relative time
   locks. A coin created in the same block as the claim therefore can never be
   claimed in it: `h` would have to be both at most `A` and above `A`.
8. **In the anchor chain:** `H` is `A_hash` or one of its ancestors.

Check 7 is relative on purpose. The seller names no height and needs no
Bitcoin view to lock; a payment older than the lock can never open it; and a
covenant that re-creates its own output after each fill (a resting order sold
for bitcoin) gets a later floor with every fill, so one payment can never fill
it twice. An absolute floor written into the script was rejected: a covenant
cannot know the Bitcoin height when it re-creates itself, so it could not stop
the same payment filling it again. Section 9.4 states what the relative floor
costs after a reorganization below the lock.

### 4.3 How every validating node reaches the same verdict on check 8

"`H` is an ancestor of `A_hash`" is a fact about two Bitcoin blocks and never
changes. A node answers it from its Bitcoin daemon with one question asked of
the best chain: is `H` on it, at height `h`? When `A_hash` is on the best chain
(rule R3 checked it, and the watcher keeps checking it), `H` on the best chain
at a height at most `A` is the same thing as `H` being its ancestor. Bitcoin's
best chain is the shared oracle here exactly as it is for the anchor itself.

The node asks through `CheckMainchainAnchor(h, H)`, the call R3 uses, with the
same verdict cache and the same invalidation: a verdict is dropped only when
Bitcoin reorganizes at or below its height. The answers map as R3's do:

| Daemon says | Verdict | Effect |
|---|---|---|
| `H` on the best chain at `h` | valid | — |
| `H` on the best chain at another height | invalid for good (`bad-btcpay-height-mismatch`) | a hash has one height |
| `H` unknown or off the best chain | re-asked in one fresh read with `A_hash`; if `A_hash` is on the best chain in that read, invalid for good (`bad-btcpay-not-in-anchor-chain`); otherwise unverifiable for now | an off-chain anchor is the watcher's business |
| no answer | unverifiable for now (`btcpay-unverifiable`) | — |

"Unverifiable for now" is `BLOCK_RECENT_CONSENSUS_CHANGE`, which this node
already uses for the anchor (`anchor-unknown`, `anchor-stale`) and for the
escaping-stall time gap: the block is neither marked invalid nor stored as
failed, and is retried, at connect time through `ConnectTip`'s stall. A verdict
that depends on this node's view of Bitcoin is never written down as permanent;
a permanent verdict is written only when the node has seen, in one read, a
canonical anchor that `H` is not under.

**A Bitcoin node that is behind.** If it does not know `A_hash`, rule R3 has
already deferred the block (`anchor-unknown`). If it knows `A_hash` it knows the
header of every ancestor, `H` included, so check 8 never needs a new answer for
a lagging daemon.

**A first sync.** Old blocks are checked exactly like new ones: one
`getblockheader` per distinct proven block, cached, as R3 asks one per distinct
anchor. Only headers are needed, so a pruned Bitcoin node serves.

**A node that does not watch Bitcoin** (`-validateanchor=0`) runs checks 1 to 7
and skips check 8, as it skips R3. It follows most work and relies on its peers
for the rest, as [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md) section 3
describes.

**When `H` leaves Bitcoin's chain later.** Every anchor at or above `H` on that
branch leaves with it, `A_hash` included, so the watcher disconnects the claim's
block as it disconnects any block with an orphaned anchor. Nothing new is needed
in the watcher for blocks.

### 4.4 The anchor floor

For an input carrying a floor `F` in a block anchored at `A`: `F <= A`
(`bad-anchorlock-not-reached`). This reads only the block header, so every node,
including one that does not watch Bitcoin, reaches the same verdict.

### 4.5 The opcodes

Both are active only on a tapscript leaf, under the script flag
`SCRIPT_VERIFY_BTCPAYMENT`, from the activation height (section 7).

**`OP_INSPECTBTCPAYMENT`** takes nothing and pushes, in order:

1. the payment's Bitcoin height `h`, as a script number;
2. the paying output's value, as 8 bytes little-endian (the form Elements'
   64-bit arithmetic opcodes read);
3. the paying output's script, in the form `OP_INSPECTOUTPUTSCRIPTPUBKEY` uses:
   the witness program and then the witness version for a segwit output; the
   SHA-256 of the script and then `-1` for any other.

It fails (`SCRIPT_ERR_BTCPAY_MISSING`) when the input has no payment proof. It
re-runs checks 1 to 5 on the record, which cost microseconds, so a script is
never sound only because the node checked something elsewhere.

**`OP_CHECKANCHORHEIGHTVERIFY`** reads the top stack item as a script number,
like `OP_CHECKLOCKTIMEVERIFY`, and fails unless the input carries an anchor floor
at least that high (`SCRIPT_ERR_ANCHORLOCK_MISSING`,
`SCRIPT_ERR_ANCHORLOCK_UNSATISFIED`). It leaves the stack unchanged.

So a script states the conditions, and the node guarantees the facts they read:
that the payment is real, in the anchor chain and after the lock; and that the
chain's anchor has reached the floor.

## 5. Cost and limits

**Size.** A typical proof is a 200–400 byte payment, a 150–400 byte coinbase and
a merkle block of 80 bytes plus 32 bytes per tree level for each of the two
leaves, a little over a kilobyte in all. It is witness data, so it counts one
weight unit per byte toward the block's weight limit and the transaction's
virtual size, and the claimant pays for it at the fee rate of the fee asset it
chooses, like any other witness byte. A deep tree costs about a kilobyte more; an
unusually large coinbase costs its size, up to the 100,000-byte limit.

**Work.** Checks 1 to 7 hash the two transactions and about thirty tree nodes:
microseconds. Check 8 is one Bitcoin-daemon lookup per distinct proven block,
cached and dropped only by a Bitcoin reorganization at or below it.

**No extra limit.** The block weight limit already bounds the number of proofs
in a block to a few hundred. Check 7 bounds how many distinct Bitcoin blocks
those proofs can name: a payment can only be proven against a lock created
before it, so naming many distinct Bitcoin blocks needs locks made long before
and a payment mined in each of those blocks. The lookups for one block are
batched into one round trip, as the watcher's are.

## 6. The mempool and block assembly

- **Acceptance.** A transaction carrying a payment proof is accepted when
  checks 1 to 7 pass against the chain tip and `H` is on Bitcoin's best chain.
  A proof whose coin is still unconfirmed is refused
  (`btcpay-coin-unconfirmed`): a claim can never share a block with its lock.
  A transaction carrying a floor is accepted when the floor is at most the
  Bitcoin tip, so the next honest block can satisfy it.
- **Block assembly.** The producer selects its anchor first, then takes only
  the transactions whose proofs are at or below that anchor and whose floors it
  meets. A claim is therefore mined in the first block anchored at or above its
  payment.
- **A Sequentia reorganization.** Transactions of disconnected blocks return to
  the mempool through the normal checks, which now include checks 1 to 8. A claim
  whose payment has left Bitcoin's chain is refused there. A claim still in the
  mempool whose lock was in a disconnected block now spends an unconfirmed coin
  and is removed, as non-final transactions are.
- **A Bitcoin reorganization that leaves the Sequentia tip alone** (it forks
  above every anchor) cannot touch a mined claim, but it can make a pooled
  claim's proof stale. On every Bitcoin reorganization it sees, the anchor
  watcher removes from the mempool each transaction whose proven block is no
  longer on the best chain.
- **A fresh proof.** The claimant re-signs with a proof of the block that now
  holds its payment and submits again. The transaction id is unchanged; only the
  witness differs.

## 7. Activation

The rule only tightens: a block valid under it is valid for a node that does
not know it, because both opcodes are `OP_SUCCESS` to such a node and it ignores
the annex. It still rejects blocks that were valid before, so it is height-gated
as [`CONTRIBUTING.md`](../../CONTRIBUTING.md) requires, by
`Consensus::Params::btc_payment_height` and `btc_payment_chain_genesis`, read
through `BtcPaymentActiveAt(height)` in the shape of
`SimplicityBudget4ActiveAt`:

- every chain not yet started (mainnet, regtest, every custom chain) has it
  from genesis; a custom chain can move it with `-con_btc_payment_height`, which
  the functional tests use to show the behaviour before activation;
- the running testnet gets a height bound to its genesis hash, chosen by the
  owner, with the version bump, the release notes and an all-at-once cutover of
  every node. Until then the testnet has it never.

## 8. The swap

### 8.1 The lock

The seller locks the asset in a taproot output with two leaves:

```
claim:   <buyer key> OP_CHECKSIGVERIFY
         OP_INSPECTBTCPAYMENT              # h  value  program  version
         <version> OP_EQUALVERIFY
         <program> OP_EQUALVERIFY          # h  value
         <amount, 8 bytes LE> OP_GREATERTHANOREQUAL64 OP_VERIFY
         <D> OP_LESSTHANOREQUAL            # h <= D

refund:  <R> OP_CHECKANCHORHEIGHTVERIFY OP_DROP
         <seller key> OP_CHECKSIG
```

and an unspendable internal key. `program` and `version` are a fresh Bitcoin
address of the seller's, `amount` the price in satoshis, `D` the last Bitcoin
height at which the payment may be mined, and `R` the first anchor height at
which the seller may take the asset back.

**Two deadlines on one clock.** Both are Bitcoin heights, the shared clock of
the two chains. A payment at `h <= D` can be claimed from the first block
anchored at `h` onward, and the refund cannot be mined before a block anchored
at `R`. Between them the claim alone is valid. `R` must exceed `D` by a margin
covering every way the chain's anchor can lag Bitcoin's tip while the claim waits
to be mined, so that a payment at `D` always gets blocks in which only its claim
is valid:

- 1 block: the normal lag of a new block's anchor behind Bitcoin's tip;
- 2 blocks: the contest window, within which a producer holds its anchor back
  from a contested Bitcoin tip;
- 3 blocks: the anchor gap after which a stalled committee's chain resumes
  with an escaping-stall block.

**The margin is therefore 6 Bitcoin blocks: `R = D + 6`.** Without a deadline a
payment mined just before `R` would race the refund, which is why `D` exists at
all.

**Who signs what.** The seller signs the lock (from its own coins) and, if it
comes to it, the refund. The buyer signs nothing on Sequentia until its claim.
The claim's signature, with the default signature hash, commits to the claim's
inputs and outputs, to the leaf, to the chain's genesis and to the annex. So it
spends only this lock, through this leaf, on this chain, against this proof.

The lock must not signal replacement, and the buyer checks that before paying.

### 8.2 The timeline

1. The two sides agree the asset, the amount, the price, `D` and `R`, and the
   seller's fresh address. The seller's wallet sets
   `D = its Bitcoin tip + k + 6` and `R = D + 6`, where `k` is the depth below.
2. The seller sends the lock. It is mined in a block anchored at `L`.
3. **The buyer waits** until Bitcoin block `L` has `k` confirmations: the
   depth at which the buyer would accept any bitcoin payment of that value (the
   whitepaper's single risk model, section 9.4). Its wallet checks the lock's
   script leaf for leaf against the agreed terms, and that `D` leaves room to
   confirm a payment.
4. **The buyer pays** the seller's address at least the amount, with an ordinary
   Bitcoin transaction from its own wallet, at a fee for the next block.
5. **The buyer claims** as soon as the payment is mined at `h`: it builds the
   proof with `gettxoutproof`, signs the claim and sends it. The claim is mined
   in the first block anchored at `h` or above, usually within one Sequentia
   block.
6. **Or the seller refunds:** if no claim has been mined by the time the anchor
   reaches `R`, and the seller's wallet sees no payment to its address mined at
   or below `D` and above `L`, it takes the asset back.

The same steps serve both directions of a trade. Whoever holds the asset locks
it and whoever holds the bitcoin pays, so when a maker sells an asset the maker
locks and the taker pays, and when a maker buys one the taker locks and the
maker pays.

### 8.3 Who may claim

In the form above only the buyer can claim, because the claim needs the buyer's
signature. A payment by anyone else enriches the seller and opens nothing for
the payer.

**An offer open to anyone** (no buyer key) would need the proof to bind the
claimant to the payer; otherwise anyone who sees a claim in the mempool copies
its proof into a claim of their own. Two bindings work:

- **data in the payment:** an output of the bitcoin payment commits to the
  claimant's Sequentia script, and the claim must pay the asset to that script
  (Elements introspection on the Sequentia side). This needs only a second
  inspection opcode reading another output of the proven transaction.
- **the payer's key:** in a dual-chain wallet the key that signs the payment's
  input is also a Sequentia key. But a proof carries the payment without its
  witness, so the key would have to be proven through the block's witness
  commitment as well, which costs a second path and the coinbase's witness.

Data in the payment is the simpler and is the one to build when an open form is
wanted. The record keeps the whole payment transaction for that reason. Two
payers racing for one open offer: one claim wins; the other payer's bitcoin
reaches the seller and buys nothing, and only an honest seller's wallet returns
it. An open form is therefore for offers where that race is acceptable or
impossible.

### 8.4 One payment, one lock

A single bitcoin output that pays a script at least an amount opens every lock
naming that script, an amount no larger and a deadline at or after its height,
if it is above each lock's anchor. Only the side that wrote those locks is hurt:
two locks naming the same receiving address are two sales paid once. That is
always the seller's own address, so the seller's wallet prevents it by deriving
a fresh address for every lock and never reusing one. The chain keeps no list of
spent payments. A rule refusing two proofs of one output inside one transaction
was rejected: two transactions would do the same.

Check 7 covers the rest: a payment made before a lock existed never opens it,
and a covenant that re-creates its output cannot be filled twice by one payment.

## 9. What each side can lose, and to whom

The chain guarantees one thing: a claim is valid exactly when its payment is in
the Bitcoin history that the claim's block is anchored to, mined after the
lock's anchor and no later than `D`. Everything below follows from it, and the
risk is not zero.

**9.1 The payment confirms after `D`.** The buyer has paid and cannot claim; the
seller refunds at `R` and holds both. The chain makes the outcome certain rather
than a race. *The buyer's wallet* pays at a next-block fee, bumps the fee
(replace-by-fee) if the payment is unconfirmed after two blocks, and two blocks
before `D` double-spends its own payment back to itself at a high fee. Any
version that confirms by `D` still opens the lock, since every version pays the
same output. *What remains:* neither the payment nor the cancel confirms by `D`,
and the payment confirms later. Only an honest seller's wallet returns it: a
seller's wallet returns every payment to a lock address mined above `D` to the
buyer's agreed address, less the Bitcoin fee.

**9.2 The buyer never pays.** The asset is parked until `R`, about
`k + 12` Bitcoin blocks after the lock, and then goes home. Nobody loses more
than the time.

**9.3 A reorganization with the payment double-spent.** The payment's block
leaves Bitcoin's best chain. Every Sequentia block anchored at or above it
leaves too, a mined claim with them, and the claim cannot re-enter the mempool or
any block, now or later, because no block on the new chain is in its proof's
history. The buyer keeps its bitcoin; the seller refunds at `R`. Nobody loses.

**9.4 A reorganization with the payment mined again.** If the payment is mined
again at `h' <= D` and above the lock's anchor, the buyer attaches a fresh proof
and claims. Two cases lose the buyer its payment:

- the payment is mined again above `D` (as 9.1);
- the reorganization also reaches the lock's anchor block `L`. The lock returns
  to the mempool and is mined again in a block anchored at the new Bitcoin tip;
  if the payment is already in the new branch at or below that anchor, check 7
  fails for good. The same reorganization also lets a dishonest seller respend
  the lock's inputs instead of letting it be mined again. Either way the seller
  holds the bitcoin and the asset.

The second case is the price of check 7, and it needs a Bitcoin reorganization
deeper than the `k` confirmations the buyer waited for at step 3, plus the
blocks until the payment was mined. That is exactly the risk the buyer takes in
accepting any bitcoin payment at `k` confirmations, which is the whitepaper's
claim of one risk model, made precise. If the payment is not yet in the new
branch when the buyer sees the reorganization, the buyer's wallet double-spends
it back to itself and loses nothing. An honest seller's wallet returns a
payment whose claim a reorganization has made impossible.

**9.5 The seller.** Once the lock is mined the seller cannot cancel, and needs
nothing but the refund at `R`. A claim cannot be mined without a payment in the
anchor chain, so the seller loses the asset only against bitcoin in the chain
its own validating node follows. A producer, or a whole committee, that includes
a claim with a forged or orphaned proof makes a block every validating node
refuses.

**9.6 A reorganization at each step.** Before the lock is mined, nothing is at
stake. Between the lock and the payment, the buyer has not paid; if the lock's
block leaves the chain, the buyer waits for it to be mined again and for depth
`k` again. With the payment unconfirmed, nothing on either chain refers to it.
After the payment and the claim, as in 9.3 and 9.4. After the refund, the refund
is mined again in the first block anchored at or above `R`, unless the buyer's
claim gets in first, which happens only if a valid payment exists and the buyer
had not claimed for the whole margin.

## 10. The collision class

One output satisfying two covenants has been found five times in this project,
so this section lists what can share a transaction with a claim or a refund.

| Shares the transaction | Can one proof or one output serve both? |
|---|---|
| a second claim of a lock naming the same address | yes, and it is the seller's own error (section 8.4) |
| a refund or a claim of another lock | no: each leaf reads only its own input's records, and each is authorized by a signature over the whole transaction |
| a covenant that checks "some output pays X" (resting orders, guarded orders, landings, enclaves, escrows, fee pots) | not through the claim or refund: neither leaf counts outputs, so neither can mistake another covenant's output for its own |
| a Simplicity program on another input | it can read the claim's annex only through hashes and gains nothing from a record it has not checked; the node checks every record anyway |

The two-party form leaves nothing to collide because both leaves are
signature paths over the whole transaction. A future keyless form (an open
offer, a resting order filled by a payment) must check its own payment and
outputs by the pattern that leaves no output to share: the order is input 0 and
pays output 0, alone, or an equivalent one-to-one pairing.

## 11. What a user sees

In a wallet's default mode, buying an asset with bitcoin is a payment with a
wait, and selling one is choosing an amount. No protocol, rail, lock or proof is
named, and nothing is offered as a choice.

- **Buying.** "Buy 100 XYZ for 0.0123 BTC." One button. The wallet waits for the
  seller's side to settle and pays from its bitcoin balance: "Paying 0.0123 BTC.
  Your XYZ arrives a minute or so after Bitcoin confirms the payment, usually
  within 20 minutes." Then "Bitcoin confirmed your payment. Receiving 100 XYZ",
  then the balance.
- **Paying from another wallet** (no dual-chain wallet at hand): an address, an
  amount and a time: "Send exactly 0.0123 BTC to this address within the next
  hour."
- **Selling.** "Sell 100 XYZ." The wallet shows the price and "Waiting for the
  buyer's bitcoin. This usually takes 20 to 30 minutes." Then the bitcoin balance.
- **A late payment.** "Your payment confirmed after the deadline, so this
  purchase did not go through. The seller's wallet returns late payments
  automatically; your bitcoin should be back within an hour." A purchase whose
  payment the wallet cancelled in time: "The payment did not confirm in time, so
  your wallet cancelled it. Nothing was spent."
- **A seller's side that never comes:** "The buyer did not pay in time. Your 100
  XYZ are back."

Developer mode shows the lock, the deadlines in Bitcoin heights, the proof and
its block.

## 12. What it replaces and what else it serves

It replaces the hash-locked swap against on-chain bitcoin wherever one is built:

- seqdex's on-chain cross-chain swap (maker and taker, both directions);
- the node wallet's convert (`src/wallet/xchainconvert.cpp`);
- the Sequentia Wallet Kit and the wallets built on it (the web wallet, Ambra
  browser, Ambra mobile);
- the restricted-asset swap against bitcoin, whose claim leaf keeps the issuer's
  co-signature beside the buyer's.

Hash-locked swaps over Lightning are unaffected: they settle without waiting for
a Bitcoin confirmation.

Further uses, not built:

- **resting sell orders filled by a payment:** a covenant that releases asset in
  proportion to the bitcoin proven paid and re-creates the rest; check 7 stops a
  payment from filling it twice, and the open form's binding (section 8.3) stops
  a stranger from taking a fill;
- **sales for bitcoin** (a token sale that releases on payment);
- **repayment in bitcoin:** a loan whose collateral is released on a proof that
  the repayment was paid.
