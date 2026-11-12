# Releasing an asset on a proof of a bitcoin payment

A Sequentia output can be locked so that it opens only for a spender who proves
that a given bitcoin payment is in Bitcoin's chain: an output paying a named
script at least a named amount, mined in a Bitcoin block within heights the lock
names. A refund path opens once the chain's anchor has passed a later Bitcoin
height.

This is the whole of a swap of a Sequentia asset against on-chain bitcoin. The
bitcoin side is an ordinary payment: no hash lock, no secret, no script a Bitcoin
wallet has to support. The Sequentia side is checked by every validating node
against the chain's own Bitcoin anchors, so a claim is valid only in a block whose
anchor chain contains the payment. If the payment leaves Bitcoin's history, the
claim leaves Sequentia's with it and can never be mined again. If the payment is
mined again in another block, anyone can attach a fresh proof to the same claim,
and a node that watches Bitcoin does so itself.

This page specifies the rule for node developers and the swap for wallet
developers. It assumes [`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md): the
anchor fields, rules R1 to R3 and the reorg-following watcher.

## 1. The rule in one paragraph

Three tapscript opcodes, `OP_INSPECTBTCPAYMENT` (`0xe5`), `OP_INSPECTBTCOUTPUT`
(`0xe6`) and `OP_CHECKANCHORHEIGHTVERIFY` (`0xe7`), read two kinds of record that
a spender places in the input's per-input witness slot: a **payment proof** (a
Bitcoin transaction, the block it is in and the paths from both it and the
block's coinbase to that block's merkle root) and an **anchor floor** (a Bitcoin
height). The script reads them through the opcodes; the node checks them against
the block, the spent coin and the anchor, outside the script, the way it checks
`nLockTime` for `OP_CHECKLOCKTIMEVERIFY` and `nSequence` for
`OP_CHECKSEQUENCEVERIFY`. A payment proof is valid in a block only if headers it carries
link its Bitcoin block to an anchor that block or one of its ancestors recorded. An anchor floor is valid in a block only
if the block's anchor height is at least the floor. Neither record is under any
signature.

## 2. The form

### 2.1 A tapscript opcode over a node-checked record

| | Tapscript opcode | Simplicity jet | Witness field the node checks, no script |
|---|---|---|---|
| Only tightens | yes: the opcodes are `OP_SUCCESS` today (`src/script/script.cpp:451-459` covers 229 to 254), and the slot is ignored by consensus | no: a new jet is a program old nodes refuse to parse, so a spend using it is a relaxation | yes |
| What scripts can do with it | anything tapscript does around it: keys, deadlines, Elements introspection, 64-bit arithmetic | the most, once written | nothing: the node checks one fixed condition |
| Code reused | the interpreter's `OP_SUCCESS` path, the Bitcoin transaction and header types of the peg-in code, and the anchors every block already records | the same node side, plus changes inside the vendored interpreter | the same node side |

**Decision: the tapscript opcode.** It is the only form that both tightens and
composes. The node does all checking that depends on the chain; the opcodes only
read what the node has checked.

**Why the opcodes do not read the chain themselves.** A script's result is cached
under a key made from the witness transaction id and the script flags
(`src/validation.cpp:2158-2162`) and reused when the transaction is mined, and
scripts are checked in parallel with no block in hand. A script that read the
containing block's anchor would be cached valid from the mempool and reused
under a block with a lower anchor. So every script stays a pure function of the
transaction and its spent outputs: the opcode compares the record with constants
in the script, and the node compares the record with the block. This is exactly
how `OP_CHECKLOCKTIMEVERIFY` divides the work with `nLockTime`.

### 2.2 The records are under no signature

The records go in the **peg-in witness of a non-peg-in input**: a per-input
witness stack every Elements transaction serializes (`src/primitives/transaction.h:426`
and `:493`), covered by the witness transaction id and by the block's witness
commitment (`src/primitives/txwitness.cpp:22`), covered by no signature hash, and
today refused only by the mempool (`extra-pegin-witness`,
`src/validation.cpp:1041-1047`, in `MemPoolAccept::PreChecks`); a block carrying
bytes there is valid.

Keeping the proof outside every signature is what lets the swap run with nobody
online:

- **The buyer signs its claim before it pays**, and anyone completes it once the
  payment is mined: the buyer's wallet, a service, the counterparty, a node.
- **A Bitcoin reorganization that moves the payment to another block** needs no
  new signature: the claim's transaction id is unchanged and only its proof is
  replaced. Whoever holds the signed claim and the buyer's payment builds the
  fresh proof. The buyer's completion service does so after every reorganization
  until the claim's block is final (section 8.5): the completion service is
  the guarantee the swap relies on. A node that watches Bitcoin also re-links a
  fresh proof itself for the claims it holds (section 6.3: at most 1,000, held
  by fee per virtual byte, each for at most 144 Bitcoin blocks), from the
  variants it kept and, when none of them stands, from a search of Bitcoin's
  recent blocks for any payment to the script those variants name. That search
  is what brings a held claim back with nobody online, as whitepaper section 5.3
  says a reorganized transaction does.
- **Wallet libraries** sign the claim exactly as any taproot script-path spend.

The taproot annex, which every signature commits to, was rejected: with it, each
reorganization that moves the payment, and the first claim itself, would need
the buyer's key, and a buyer who had closed its wallet would lose the asset at
the refund height. That is a watcher duty on the buyer, which is the duty this
rule exists to remove.

What being unsigned costs, and how each cost is closed:

- **Anyone can change the records.** A change that makes the proof invalid makes
  a transaction with another witness id, which is refused on its own. The reject
  filter keys a failure of that kind by witness id only
  (`src/net_processing.cpp:3520-3546`; proof failures are never reported as
  `TX_INPUTS_NOT_STANDARD`, the one result that also files the transaction id),
  so the valid transaction is still fetched from other peers.
- **A valid change could make the claim heavier**, lowering its fee rate to hold
  it back until the refund. So every record has **exactly one valid encoding**
  for a given fact and recorded anchor (section 3; a proof may link to any
  anchor recorded within 2,016 blocks above its block, so one payment has a
  valid record per such anchor, of different weights), relay refuses anything else in the slot,
  including any record the input's script does not read (section 6.1), and the
  mempool lets a lighter witness (raising the fee rate by at least the
  incremental relay rate) or a fresher one replace a pooled one with the same
  transaction id (section 6.2). What remains: a padding that lowers the claim's
  fee rate by less than that incremental rate; a block's own producer can pad the
  slot with records of an unknown type, which consensus ignores, and a script
  that reads a witness value it does not constrain admits equal-weight
  variants, as any tapscript does.
- **A Simplicity jet** would see the slot through the change that adds the jet;
  the records do not move.

## 3. The records

On a non-peg-in input the slot is either empty or holds records, one stack item
each, whose first byte is the type. A record of a known type must be valid and
may appear at most once. An item of any other type is ignored by consensus, so
a later record type can be added as a tightening (read by a new opcode on a new
`OP_SUCCESS` slot), and refused by relay policy, which accepts only the known
types, in ascending order. So relayed transactions carry one encoding, and only
a block's own producer can add ignored items.

**Type `0x01`, payment proof.** One item:

| Field | Bytes |
|---|---|
| type `0x01` | 1 |
| Bitcoin height `h` of the block, which the node checks is the recorded anchor's height less `n` | 4, little-endian |
| coin anchor `C`: the anchor height of the block that created the coin this input spends | 4, little-endian |
| the block's 80-byte header | 80 |
| `SHA256(coinbase)`: the single SHA-256 of the block's coinbase transaction | 32 |
| depth `d`, the length of both branches | 1, from 1 to 24 |
| payment index | 4, little-endian, below `2^d` |
| payment branch | `32 × d` |
| coinbase branch | `32 × d` |
| number of linking headers `n` | 2, little-endian, at most 2,016 |
| linking headers | `80 × n` |
| the payment transaction without its witness | the rest of the item, at most 100,000 bytes |

The payment transaction comes last, so its length is the item's remainder and
needs no length prefix. Its bytes are the ones its transaction id hashes, so they
admit no second encoding. The 100,000-byte limit is Bitcoin's standardness
ceiling for any relayed transaction; the buyer's own wallet makes the payment, so
it never comes near it.

**Type `0x02`, anchor floor.** One item: `0x02` and a 4-byte little-endian
Bitcoin height.

**Two plain branches, not a partial merkle tree.** A branch of equal length for
the payment and for the coinbase is one encoding with a few lines to verify, and
it is what light Bitcoin servers already return: Electrum's
`blockchain.transaction.get_merkle` and Esplora's `/tx/:txid/merkle-proof` give
the payment's branch and position, and Electrum's
`blockchain.transaction.id_from_pos(height, 0, true)` or Esplora's
`/block/:hash/txid/0` then `/tx/:txid/merkle-proof` give the coinbase's. The
coinbase itself (`blockchain.transaction.get`, `/tx/:txid/hex`) is hashed once
locally, and the header comes from `blockchain.block.header` or
`/block/:hash/header`. A wallet that talks to Bitcoin Core builds both branches
from `getblock <hash> 1`. Bitcoin Core's `gettxoutproof` partial merkle tree was
rejected: it has several encodings of the same proof and needs a traversal to
verify.

## 4. What the node checks

### 4.1 The proof, on its own

These depend only on the record's bytes:

1. The item decodes exactly as above, within its limits.
2. Hashing the payment up its branch by its index, and `SHA256(SHA256(coinbase))`
   up its branch by index 0, both give the header's merkle root.
3. **One encoding of the index:** at any level where a node's sibling equals the
   node itself (the duplicated last node of an odd level), the index bit at that
   level is 0. The payment's index is above 0.
4. The payment parses as a Bitcoin transaction without witness, consuming every
   byte; it is not a coinbase; and it is not 64 bytes long.

**The 64-byte weakness.** Bitcoin's merkle tree hashes a leaf and an inner node
the same way, so 64 bytes can be read either as a transaction or as two child
hashes. A forger can claim the payment at the wrong depth in either direction;
proving the coinbase at the same depth closes both:

- *Too shallow:* the "payment" is an inner node. Its only known preimage is
  64 bytes long, which check 4 refuses. Any other preimage is a second preimage
  of SHA-256d.
- *Too deep:* an invented transaction sits under a real 64-byte transaction one
  level down. The coinbase branch then has the same extra level, so the real
  coinbase would have to be 64 bytes too, with its first 32 bytes equal to
  `SHA256(s)` for the `s` the forger supplies. Those 32 bytes are a 4-byte
  version, the input count `0x01` and 27 zero bytes of the null prevout: 28 fixed
  bytes of a SHA-256 output. Two levels down needs a 64-byte value whose double
  hash has the same 28 fixed bytes.

Carrying `SHA256(coinbase)` rather than the coinbase keeps the proof a constant
size whatever the coinbase. Carrying the coinbase in full would let a miner who
is also a seller make every payment in its block unprovable by giving the block
a coinbase above any size limit. `SHA256(coinbase)` is necessary as well as
sufficient for the deep case: a bare leaf hash at index 0 needs no preimage at
all. It does nothing for the shallow case, since an inner node has a 32-byte
preimage too (the single SHA-256 of its 64 bytes), and check 4 covers that case.

**A deployed precedent.** tBTC v2's Bitcoin proof carries `coinbasePreimage`, the
single SHA-256 of the coinbase, and a coinbase proof of the same length as the
transaction's, for the reason above: a 2023 report against that bridge forged a
proof through a 64-byte coinbase (the structure is `BitcoinTx.Proof` in
`keep-network/tbtc-v2`, `solidity/contracts/bridge/BitcoinTx.sol`, and the
Immunefi bug-fix review "Threshold Transaction Malleability"). If Bitcoin makes
64-byte transactions invalid, these checks stay correct.

**No proof-of-work check on the header.** A header's own `nBits` is the header's
claim, and a header at the lowest difficulty costs nothing to make, so such a
check adds no security. The header is trusted exactly as far as the anchor chain
vouches for it (section 4.3).

### 4.2 The proof, in its block

For a transaction in a block, each input carrying a payment proof for Bitcoin
block `H` at declared height `h`, with `n` linking headers:

5. **The linking headers** each commit to the one before (`hashPrevBlock`), the
   first to `H` itself, so they are `H`'s real descendants up to the hash they
   end at, `E` (`H` itself when `n` is 0). At most 2,016 (`bad-btcpay-links`,
   `bad-btcpay-encoding`).
6. **`E` is an anchor this branch recorded, by the first block recording it:**
   the claim's block, or one of its ancestors, carries `E` as its anchor at
   height `h + n`, and that block's parent does not carry `E`
   (`bad-btcpay-not-in-anchor-chain`). So `H` is the ancestor of that anchor at
   height `h`, and the declared height is true. The second condition matters
   where rule R2's converse (a hash has one height) is not consensus, before
   `pos_hardening_height`: there a block may repeat its parent's anchor hash at
   a larger height, and a proof linking to that record would declare a height
   larger than the real one and open a floor its payment does not meet. A block
   recording an older hash again further on, not right after it, is refused by
   rule R3 on every node that watches Bitcoin (`bad-anchor-height-mismatch`),
   since its hash differs from its parent's.
7. **The coin anchor is true:** `C` equals the anchor height of the block that
   created the coin this input spends, found by the coin's height on this branch,
   as for relative time locks.

### 4.3 How every validating node reaches the same verdict

Checks 5 to 7 read only the record and the chain the block extends. No Bitcoin
daemon is asked, so the verdict is a function of the block and its ancestors,
the same on every node, at any time; and a block refused is refused for good.

Finding `E` costs a binary search over the block's ancestors. Anchor heights
never decrease along a chain, and a block anchored at the same height as its
parent carries the same hash (rule R2), so the first ancestor anchored at or
above `h + n` decides: its anchor must be exactly `E` at exactly `h + n`.

What depends on a node's view of Bitcoin stays where it was: rule R3, when a block
records a new anchor, and the watcher, which disconnects every block whose anchor
leaves Bitcoin's best chain. If `H` leaves Bitcoin's chain, so does every anchor
above it on its branch, `E` included, so the watcher disconnects the claim's
block, and the claim cannot be mined on the new branch because no anchor there
descends from `H`.

**Earlier designs asked the daemon** whether `H` was on its best chain. A proof
review split two honest nodes that way: one answered from a cached verdict a
moment after a Bitcoin reorganization and accepted the claim, the other asked
afresh and refused it for good. Linking `H` to the chain's own anchors removes
the question.

**A first sync** checks old blocks the same way, with no daemon and no index.
**A node that does not watch Bitcoin** (`-validateanchor=0`) checks proofs fully
as well; what it gives up is R3 and the watcher, as described in
[`03-bitcoin-anchoring.md`](03-bitcoin-anchoring.md), section 3. It therefore
takes every anchor on trust: when a producer records a fabricated anchor, such a
node accepts a claim whose payment exists in no Bitcoin block, and keeps it
until a chain with more work replaces that block. **Such a node must not be used
to decide that a swap is complete.**

### 4.4 The anchor floor

For an input carrying a floor `F` in a block anchored at `A`: `F <= A`
(`bad-anchorlock-not-reached`). This reads only the block header.

### 4.5 The opcodes

All three exist only on a tapscript leaf, under the script flag
`SCRIPT_VERIFY_BTCPAYMENT`, from the activation height (section 7). Script
numbers they read must be minimally encoded.

- **`OP_INSPECTBTCPAYMENT`** takes nothing and pushes the payment's height `h`,
  then the coin anchor `C`, as script numbers. It fails without a payment proof
  (`SCRIPT_ERR_BTCPAY_MISSING`).
- **`OP_INSPECTBTCOUTPUT`** pops an output index and pushes that output of the
  proven transaction: its value as 8 bytes little-endian (the form of Elements'
  64-bit arithmetic opcodes), then its script in the form
  `OP_INSPECTOUTPUTSCRIPTPUBKEY` uses: the witness program and then the witness
  version for a segwit output, the SHA-256 of the script and then `-1` for any
  other. It fails without a proof or for an index out of range
  (`SCRIPT_ERR_BTCPAY_OUTPUT`). Any output can be read, so a script can bind a
  claim to data in the payment (section 8.4) with no further rule.
- **`OP_CHECKANCHORHEIGHTVERIFY`** reads the top stack item and fails unless the
  input carries an anchor floor exactly equal to it
  (`SCRIPT_ERR_ANCHORLOCK_MISSING`, `SCRIPT_ERR_ANCHORLOCK_UNSATISFIED`). It
  leaves the stack unchanged. Equality, rather than "at least" as in
  `OP_CHECKLOCKTIMEVERIFY`, is possible because the floor belongs to one input,
  and it gives the floor one valid encoding.

Both `OP_INSPECTBTC*` opcodes check the record (checks 1 to 4) themselves, so a
script is never sound only because the node checked something elsewhere. An
input's records are parsed and checked once, however many times its script reads
them, and each output script's hash is computed once; an execution then costs a
constant.

**Floors: absolute in a script, or relative to the coin.** The node guarantees
`C`, so a script chooses its own floor:

- `h > F` for a constant `F` written in the script: the **absolute** floor. The
  two-party swap uses it (section 8.1).
- `h > C`: the **relative** floor, in the shape of `OP_CHECKSEQUENCEVERIFY` (a
  value in the record that the node checks against the chain, and a script that
  demands it). A covenant that re-creates its own output after each fill uses
  it: each re-created output has a later `C`, so one payment can never fill it
  twice, and a coin created in the same block as its claim can never be claimed
  there, since `h` would have to be both above `C` and at most `C`.

The two differ under a Bitcoin reorganization that reaches below the coin's
block (proof review PV13, case E18). When only the payment moves, a claim under
either floor is mined again. When the reorganization reaches below the lock, the
lock is mined again in a block with a later anchor, so `C` rises: a payment
mined again at or below the new `C` still opens the absolute floor, and no longer
opens the relative one, which then leaves the seller with both sides. So a
two-party trade uses the absolute floor, and the relative floor belongs to
covenants whose next fill a later payment can make.

## 5. Cost and limits

**Size.** A proof is 128 bytes, plus 64 bytes per tree level (12 to 13 levels on
a full block today, about 800 bytes), plus 80 bytes per linking header, plus the
payment, typically 200 to 400 bytes. A claim completed as soon as its payment is
recorded needs no links: about 1.2 kilobytes. A claim completed later links to
the lowest anchor recorded at or above its payment, typically a few headers. It
is witness data, so it counts one weight unit per byte toward the block's weight
limit and the transaction's virtual size, paid at the fee rate of whichever
accepted asset pays the claim's fee. The 2,016-link cap bounds a proof at about
161 kilobytes of headers.

**Work.** Checks 1 to 7 hash the payment, up to 48 tree nodes and the linking
headers, and run one binary search over the block's ancestors: no network. On a
node with an empty cache, a full block (weight 360,829) of 296 claims, each naming
a different Bitcoin block, spent 0.46 ms in these checks. A script that reads its
records 50,000 times over an 80-kilobyte proof (transaction weight 181,006) was
accepted in 0.02 s.

**No limit on distinct proven blocks**: nothing in the check grows with them but
the hashing, which the block weight already bounds.

## 6. The mempool and block assembly

### 6.1 Acceptance and assembly

- A transaction carrying a payment proof is accepted only when checks 1 to 7
  pass against the chain tip, that is, when its links end at an anchor the active
  chain has recorded. Validity in the mempool is then validity in the next block.
  A claim whose payment is above every recorded anchor waits, about one block,
  until a block anchors at or above it, and is completed then.
- A proof whose coin is still unconfirmed is refused (`btcpay-coin-unconfirmed`),
  so a claim never shares a block with its lock.
- **Relay refuses a record no executed opcode read**: every record in an input's
  slot must have been read by an opcode the input's script executed
  (`OP_INSPECTBTCPAYMENT` or `OP_INSPECTBTCOUTPUT` for a proof,
  `OP_CHECKANCHORHEIGHTVERIFY` for a floor). A record on an input that is not a
  tapscript spend can never be read, so it is refused too, as is one read only on
  a branch the script did not take. Without this anyone could add a floor or a
  whole payment record to an ordinary input of someone else's transaction: the
  variant pools first, refuses the original as the same transaction, and may be
  many times heavier. The rule is relay policy (the standard script flag
  `SCRIPT_VERIFY_BTCPAYMENT_RECORDS_READ`, refused as `Bitcoin payment record not
  read by any executed opcode`), not consensus: a block may still carry such a
  record. It holds on every input, a spend of a stake record included. Its
  verdict reads only the transaction and the outputs it spends, so
  the same bytes get the same answer on every node with the rule active, at any
  tip.
- A transaction carrying a floor is accepted, and may replace another, only
  when the floor is at most the anchor height of the chain tip, never Bitcoin's
  tip. Anchor heights never fall along a branch, so the next block can mine it.
  Everything the mempool holds can be mined now, and a spend that cannot be
  mined yet never displaces one that can: a refund whose `R` Bitcoin has reached
  but no block has anchored waits, and the claim pooled before it is mined first.
  The refund costs one block after `R` is first anchored.
- The producer chooses its anchor before its transactions, and takes only those
  whose floors it meets. It never rewrites a proof.

### 6.2 One transaction id, a better proof

The mempool otherwise refuses a second transaction with the same id
(`txn-same-nonwitness-data-in-mempool`). When the pooled transaction carries
records, it accepts a valid newcomer in its place when the newcomer is lighter
by enough that its fee rate rises by at least the incremental relay rate
(`-incrementalrelayfee`, 100 per thousand virtual bytes by default), whether it
carries other records or none, or when the pooled one's records are no longer
valid. A newcomer without records is how the original of a spend
displaces a variant someone else added records to. A seller who completes the
buyer's pre-signed claim with a heavy proof of some other qualifying payment is
then displaced by the lighter proof of the buyer's own payment. Each replacement
must raise the fee rate or replace a proof that is no longer valid, so a chain
of replacements cannot loop, and since a replacement re-checks the claim's
descendants it is never free: the fee is the same for every variant (the same
transaction id has the same outputs), so a claim paying `f` per virtual byte at
its lightest is replaced at most `f` / 0.1 times in all (fees valued as the
mempool values them, in `rfa`, the reference unit's 10⁻⁸). That rule cannot keep
a buyer's better proof out by more than the incremental rate: the pooled variant
pays at most 0.1 `rfa` per virtual byte less than the best one, so whoever pads
the claim lowers its rate by less than that, and the buyer's wallet covers it by
paying 0.1 `rfa` per virtual byte more: about 30 `rfa` for a claim of 304
virtual bytes. `sendrawtransaction` submits such a transaction to
the mempool instead of treating its transaction id as already known. The pooled
transaction's descendants spend its outputs, which the replacement leaves
unchanged with its transaction id, so they stay in the pool: a better proof never
costs a buyer its onward spend or a child paying for the claim's inclusion.

**A proof is preferred over a floor.** Among conflicting spends of one coin:

- a spend that read only an anchor floor never replaces one whose script read a
  payment proof (`btcpay-floor-replaces-proof`), whatever it pays;
- a spend that read a proof replaces spends that read only a floor, whatever
  they pay and whether or not they signal replacement;
- every other pair follows the usual replacement rules, by fee: two spends that
  read a proof, and a spend with no records against either kind. A spend that
  read a proof counts as signalling replacement whatever its `nSequence`: on a
  keyless leaf its spender, not the coin's owner, chooses that field.

In the swap this decides the race after `R`: the refund never displaces a claim,
and the claim displaces a refund pooled before it, so between honest producers
the buyer who paid by `D` is mined. The third rule is what keeps it from pinning
a coin: a coin that a key can also spend is never held by a cheap proof-path
spend, signalling or not, since the key spend replaces it by paying more. Two
payers racing to claim a keyless leaf compete by fee in the same way. Relay refuses records no
opcode read (6.1), so a spend cannot claim the preference with a proof its script
does not need. Block assembly takes its transactions from the mempool, which holds
one spend per coin, so it follows the same order; a producer that is itself the
seller can still mine its own refund after `R`, which consensus allows.

**Valid variants are kept, paid for by their claim's own fee.** A newcomer that
is valid but does not replace the pooled one (it is not lighter) is still checked
in full, kept beside it, and then refused as the same transaction. Which proof a
node pools depends on what reached it first and on who sent a lighter one, both of
which an outsider controls; a claim mined with one proof also comes back alone
from a disconnected block. If that proof's payment then leaves Bitcoin's chain,
another kept variant whose payment stands brings the claim back. So the node
keeps, per transaction id, variants it has seen valid (accepted, validated beside
the pooled one, or returned from a disconnected block), within bounds that make
the claim's own fee, never anyone else's, pay for what it stores:

- **the variant the claim is pooled or was mined with is always kept,** whatever
  the bounds below say, as one more beside them: it is the proof the node needs
  after a reorganization, whoever sent it and whatever was seen first;
- **its fee pays for the rest:** the kept variants' total virtual size, at the
  minimum relay rate, is at most the claim's fee;
- **one variant per proven Bitcoin block** on each input, and at most 8 in all. A
  variant proving the same payments as a kept one takes its place (its links
  reach a later anchor);
- **the newest seen is the one not kept** when a bound is reached, so nothing a
  later sender adds can push out a proof already kept;
- **at most 32 MiB in all:** over that, the transaction id paying the least per
  stored virtual byte is dropped first, as the mempool evicts, and claims waiting
  for re-attachment go last.

A variant the node would not keep is refused at once, without its scripts being
run. To push one claim's kept proofs out of the 32 MiB store, an outsider stores
about 8.4 million virtual bytes of its own at a higher fee per byte than that
claim pays: about 14,000 times the fee of a claim storing two proofs.

**No rule over stored variants can beat a seller who is first.** The node cannot
tell the buyer's payment from the seller's payment to itself: both pay the same
script the same amount. A seller whose payment sits in the same Bitcoin block as
the buyer's, or who fills the bounds first, keeps the buyer's proof from being
stored at all. So the node does not rely on what it stored: when every kept
variant fails, it searches Bitcoin for any payment to the script those variants
name (section 6.3), and finds the buyer's payment whatever the store holds.

The bounds and the search are not consensus: they are relay help, and the
buyer's wallet or completion service can always submit its own proof. **The
completion service is the guarantee** (section 8.5): it watches the claim until
its block is final and resubmits a fresh proof of the buyer's own payment after
every reorganization that disconnects it. **The node's search is what brings a
claim it holds back with nobody online.** What remains for an attacker is what
section 9.3 already accepts: taking the buyer's payment itself out of Bitcoin's
chain, or making it pay another script, which leaves the buyer with its
bitcoin; a buyer's payment older than the search's window (section 6.3) when the
reorganization comes; and a claim kept out of a node's waiting set by 1,000
claims that each pay more per virtual byte (section 6.3), which costs about 830
times the claim's own fee at every reorganization it is aimed at. In the
last two, the claim comes back only when the buyer's wallet or completion
service submits it, and is lost if both stay offline until `R`.

### 6.3 Reorganizations, and the proof re-linked

- **A Sequentia reorganization** (including every one the watcher makes after a
  Bitcoin reorganization): transactions of disconnected blocks return through the
  normal checks, and the mempool re-checks every claim it holds against the new
  tip. A claim whose links end at an anchor the new branch did not record, whose
  lock left the chain, or whose coin anchor moved is evicted.
- **A Bitcoin reorganization** changes a claim's validity only through the
  Sequentia reorganization it causes, since validity reads only the chain.
- **The node re-links a fresh proof itself** (node policy, on every node that
  watches Bitcoin; every block producer does). It holds each claim evicted after
  a reorganization, keyed by transaction id, with the variants of section 6.2,
  in a waiting set of at most 1,000 claims:
  - **held by fee per virtual byte.** When the set is full, the held claim
    paying the least per virtual byte is dropped for a newcomer paying more,
    and a newcomer paying no more than every held claim is not held at all;
  - **forgotten at once when its lock coin is spent** on the active chain
    (checked whenever the chain tip moves): such a claim can never be mined on
    that chain;
  - **forgotten after one window:** a claim not re-attached within 144 Bitcoin
    blocks of being held is dropped with its variants. By then the search has
    covered the window below the height it was held at and every block mined
    since, so every payment that could still complete it has been looked for.

  A claim the node did not hold, or has forgotten, comes back only when someone
  submits it: the buyer's wallet or its completion service (section 8.5), which
  is the guarantee. To keep one claim out of a node's waiting set an attacker
  needs 1,000 claims of its own held there at the moment the reorganization
  evicts it, each paying more per virtual byte. A claim is held only when a
  reorganization evicts it from the pool or from a disconnected block, so those
  claims must have been pooled or mined just before it, offering their fees:
  1,000 times the claim's fee per virtual byte times their own size. With the
  smallest claims (252 virtual bytes, a leaf any proof opens, against 304 for a
  buyer's claim) that is about 830 times the claim's own fee, paid in full
  whenever the Bitcoin block the attacker bets on is not orphaned. A
  held claim lasts one window at most and goes as soon as its coin is spent, so
  the cost is paid again for every reorganization aimed at.

  The work runs on its own thread, beside the anchor watcher and never on its
  path, so nothing it does delays an anchor-driven reorganization:
  - held claims are tried highest fee per virtual byte first;
  - a claim is tried again only when Bitcoin's tip or the chain tip has moved
    since its last try, and each tick stops after 500 ms of work, the rest
    waiting for the next tick. The budget is checked before every step: each
    stored variant submitted, each batch of daemon calls, each block the search
    reads, each candidate it tries. What was read and how far each claim got
    are kept, so the next tick goes on from there, and no tick runs past its
    budget by more than one such step;
  - each kept variant is first submitted as it is, which asks the Bitcoin daemon
    nothing: a Sequentia reorganization that records the anchor again makes the
    stored proof valid again;
  - then each variant gets a fresh proof of the payments it names. The node
    looks for each payment once per Bitcoin tip, however many claims name it:
    first `gettxout` on its outputs (in batches of 64), whose reply gives the
    block's depth, and if they are all spent, among the transactions of the last
    144 blocks, read once
    per tip from a cache of block contents keyed by block hash. It links that
    block to the lowest anchor the active chain recorded at or above it, with the
    headers along Bitcoin's best chain (read in batches and kept per Bitcoin
    tip), refuses if they do not end at that anchor, fills in the coin anchor,
    and submits the claim;
  - **then a search by paid script.** For each claim whose every variant failed,
    the node takes the outputs its variants' proofs name (an output of the
    payment whose index the input's witness carries, which the leaf reads with
    `OP_INSPECTBTCOUTPUT`) and searches Bitcoin for any other transaction paying
    the same script. Every such output is a candidate, largest first, whatever
    it pays: the node does not know the price the script asks, and a seller
    paying itself a little more must not hide the buyer's exact payment. For
    each candidate the node builds a fresh proof, makes the witness item that
    named the old output name the new one, and submits it; the mempool runs the
    script, so a wrong candidate costs one refusal. The window is the last 144
    Bitcoin blocks, reaching down to the floor a claim carries when it carries
    one, and never more than 2,016, since a proof links at most 2,016 headers to
    a recorded anchor. Only blocks of Bitcoin's best chain inside the window are
    read, never an orphaned one: a payment an orphaned block held that was mined
    again is in the window, and what stayed behind in the orphan can complete
    nothing. Candidates are the transactions the scan itself found there, with
    the block it found them in; their proofs are built from what the scan read
    (the block's transaction ids, its header and its coinbase), so a candidate
    costs no daemon call, and the work is bounded by the budget, not by how many
    outputs pay the script.

  The search runs once every waiting claim has been tried at the current tips,
  and one scan serves all of them: each block is read once (`getblock`) for the
  scripts it was not read for yet, matched against every waiting script, and the
  budget is checked between blocks, so a scan stopped by it goes on at the next
  tick from what it has read. It asks the Bitcoin daemon nothing while a proof is
  validated: like the rest of this section it is relay help, and a claim's
  validity stays a pure function of the chain.

  Held claims and variants live in memory only; after a restart the buyer's
  wallet submits its own proof again.

  Measured with claims waiting for a payment that was double-spent (PV13's case),
  an anchor-driven reorganization disconnects the orphaned block in 1.0 s with
  200 waiting and 1.1 s with 1,000, against 0.5 s with none, and the Bitcoin
  daemon serves at most one read of the recent blocks per tip (144 `getblock`
  calls in 20 s with 1,000 waiting). With claims that each name a script of
  their own, so that every one is searched for, the anchor-driven disconnect
  stays at 0.5 s with 200 and with 1,000 waiting; one scan reads the 145 blocks
  of the window once for all of them (289 `getblock` calls in 20 s, the recent
  blocks and the scan). Against 2,000 outputs in the window paying a waiting
  claim's script, each refused by its script, the candidates take two ticks,
  the longest 501 ms; 10,000 outputs paying it in an orphaned block cost nothing
  (one `gettxout` call, a 45 ms tick).

## 7. Activation

The rule only tightens: a block valid under it is valid for a node that does not
know it, because the three opcodes are `OP_SUCCESS` to such a node and the slot is
ignored on a non-peg-in input. It rejects blocks that were valid before, so it is
height-gated as [`CONTRIBUTING.md`](../../CONTRIBUTING.md) requires, by
`Consensus::Params::btc_payment_height` and `btc_payment_chain_genesis`, read
through `BtcPaymentActiveAt(height)` in the shape of `SimplicityBudget4ActiveAt`
(`src/consensus/params.h:485-502`):

- every chain not yet started (mainnet, regtest, every custom chain) has it from
  genesis; a custom chain can move it with `-con_btc_payment_height`, which the
  functional tests use to show the behaviour before activation;
- the running testnet gets a height bound to its genesis hash, chosen by the
  owner, together with the version bump, the release notes and an all-at-once
  cutover of every node. Until then the testnet does not have it;
- the running chains that are not this project's (Bitcoin's `main` and `signet`,
  and the Liquid chains) never have it. On them 0xe5 to 0xe7 stay `OP_SUCCESS`.

`src/test/sequentia_chainparams_tests.cpp` pins each of these.

## 8. The swap

### 8.1 The lock

The seller locks the asset in a taproot output with an unspendable internal key
and two leaves:

```
claim:   <buyer key> OP_CHECKSIGVERIFY      # witness: <i> <signature>
         OP_INSPECTBTCPAYMENT OP_DROP       # i  h
         OP_DUP <F> OP_GREATERTHAN OP_VERIFY
         <D> OP_LESSTHANOREQUAL OP_VERIFY   # i
         OP_INSPECTBTCOUTPUT                # value  program  version
         <version> OP_EQUALVERIFY
         <program> OP_EQUALVERIFY
         <amount, 8 bytes LE> OP_GREATERTHANOREQUAL64

refund:  <R> OP_CHECKANCHORHEIGHTVERIFY OP_DROP
         <seller key> OP_CHECKSIG
```

- `F`, the floor: the anchor height of the Sequentia tip when the seller builds
  the lock, read from the tip's header. The seller needs no Bitcoin view and
  guesses nothing, and a payment older than the lock, even to an address used
  before, is below it.
- `program` and `version`: the seller's receiving script (section 8.3).
- `amount`: the price in satoshis.
- `D`: the last Bitcoin height at which the payment may be mined.
- `R`: the first anchor height at which the seller may take the asset back.

**Two deadlines on one clock.** Both are Bitcoin heights, the shared clock of the
two chains. A payment at `h <= D` can be claimed from the first block anchored at
`h`, and the refund cannot be mined before a block anchored at `R`. Between them
only the claim is valid. `R` exceeds `D` by a margin covering every way the
chain's anchor can lag Bitcoin's tip while the claim waits:

- 1 block: the normal lag of a new block's anchor behind Bitcoin's tip;
- 2 blocks: the contest window, within which a producer holds its anchor back from
  a contested Bitcoin tip;
- 3 blocks: the anchor gap after which a stalled committee's chain resumes with an
  escaping-stall block.

**So `R = D + 6`.** Without `D`, a payment mined just before `R` would race the
refund. Once the claim is signed, its completion service and every node that
watches Bitcoin bring it back after a reorganization (sections 6.3, 8.5), so the
margin covers only the chain's own lag.

**Who signs what.** The seller signs the lock, from its own coins, and the refund
if it comes to it. The buyer signs the claim. With the default signature hash the
claim's signature commits to the claim's inputs and outputs, to the leaf and to
the chain's genesis, so it spends only this lock, through this leaf, on this
chain. It does not commit to the proof, which is the point (section 2.2): any
proof of any payment that meets the leaf's conditions completes it.

The lock must not signal replacement. The claims do signal it (`nSequence`
`0xfffffffd`), so that a higher fee tier of the same claim (section 8.2) replaces a
lower one. A refund never replaces a claim in honest mempools, whatever either
pays (section 6.2).

**The claim's fee is paid in an asset block producers accept.** Producers take
only the fee assets on their lists, and a node values each at broadcast, so a
claim paying its fee in an asset no producer accepts is never mined, and the
seller refunds at `R`. The claim therefore pays its fee:

- in the locked asset itself, when producers accept it, which is the default
  (the fee comes from the asset being moved);
- otherwise from a second input in an accepted asset: a small output the seller
  adds to the lock under the same two leaves, priced into the offer, or a coin of
  the buyer's own, which the buyer's wallet keeps unspent until the claim is mined
  or `R` has passed.

The buyer's wallet checks before it pays that producers accept the claim's fee
asset (section 8.2), and offers the trade only when one of these is available. No
asset is the fallback: the Sequence token is used only if it is one of the
accepted assets the claim can draw on, like any other.

### 8.2 The timeline

1. The two sides agree the asset, the amount, the price, `D` and `R`, and the
   seller's key. The seller's wallet sets `D = (Bitcoin tip) + k + 6` and
   `R = D + 6`, where `k` is the buyer's wait below.
2. The seller sends the lock. It is mined in a block anchored at `L`.
3. **The buyer's wallet checks before it pays:**
   - the lock's script, leaf for leaf, against the agreed terms, and that the
     receiving script is the seller's template (section 8.3);
   - that `F <= L`, and that `D` leaves room to confirm a payment;
   - that producers accept the asset the claim's fee is paid in (section 8.1);
   - that the lock's block is final: at or below the node's finalized height
     (`getposfinality`'s `finalized_height`), so it is certified by a full quorum
     and no sibling block can replace it except through Bitcoin;
   - that Bitcoin has added `k` blocks on top of `L`, by the default-mode policy:

     | Price | `k` | Wait, typically |
     |---|---|---|
     | below 0.01 BTC | 0 | none: the buyer pays as soon as the lock's block is final |
     | below 0.1 BTC | 1 | 10 minutes |
     | below 1 BTC | 2 | 20 minutes |
     | 1 BTC or more | 5 | 50 minutes |

     The page meanwhile says "Getting your purchase ready. This usually takes a
     few minutes." The wait is the buyer's policy, not what the rule's safety
     rests on (section 9.4). **What `k = 0` risks, for the owner to weigh against
     the ten minutes it saves:** a one-block Bitcoin reorganization that replaces
     `L`, which takes the payment's block with it. With the absolute floor, honest
     producers mine the lock again and the swap completes; the buyer loses only if
     the seller also controls the producer of the block that would mine the lock
     again and has it take a respend of the lock's inputs instead. A natural
     one-block reorganization is rare on Bitcoin's main network and common on its
     test networks, and a seller who controls a producer is the rarer half.
4. **The buyer signs the claim, and a cancel, before it pays.** The claim pays
   the asset to the buyer, its fee in an accepted asset (section 8.1). It signs
   three tiers of the same claim (section 8.5): the going rate with headroom, ten
   times that, and a top tier sized so that keeping the claim out costs more than
   the swap is worth. The cancel spends the payment's own inputs back to the buyer
   at a high fee. The wallet hands all of them to its completion service and to
   the counterparty.
5. **The buyer pays** the seller's address at least the amount, with an ordinary
   Bitcoin transaction, at a fee for the next block.
6. **Anyone completes the claim** once the payment is mined at `h` and a
   Sequentia block has anchored at `h` or above, usually one Sequentia block
   later: attaches the proof, linked to that anchor, and the coin anchor, and
   submits it. It is mined in the next block, so the claim lands about two
   Sequentia blocks after the payment confirms. The completion service keeps it
   alive across reorganizations until its block is final, and every node that
   watches Bitcoin helps (sections 6.3, 8.5). **A wallet shows the claim as
   complete only once its block is final** (certified, and its anchor buried),
   never on its first confirmation: until then a reorganization, of Sequentia or
   of Bitcoin beneath it, can still disconnect it.
7. **Or the seller refunds**: if no claim has been mined by the time the anchor
   reaches `R`, and the seller's wallet sees no payment to its address mined at a
   height above `F` and at most `D`. The seller's wallet, too, shows the refund as
   complete only once its block is final.

The same steps serve both directions of a trade. Whoever holds the asset locks it
and whoever holds the bitcoin pays: when a maker sells an asset the maker locks
and the taker pays, and when a maker buys one the taker locks and the maker pays.

### 8.3 The seller cannot pin the payment

A seller could spend its own still-unconfirmed output of the buyer's payment with
a large, low-fee child. Bitcoin Core then prices any replacement of the payment
(a fee bump, or the buyer's cancel) at the child's whole fee as well, which can
cost more than the trade is worth, and the seller waits for `D`. Proof review PV10
measured how decisively pinning works under Bitcoin Core 28's policy.

So the lock names a receiving script the seller cannot spend before it confirms:

```
wsh(and_v(v:pk(<seller key>),older(1)))
```

a P2WSH script, `<seller key> OP_CHECKSIGVERIFY OP_1 OP_CHECKSEQUENCEVERIFY`. The
buyer's wallet derives the address from the seller's key and checks that the lock
names exactly it. A plain Bitcoin wallet still pays it as an ordinary address.
Bitcoin Core's descriptor wallet holds and spends it. The payment's only other
output is the buyer's change, so nobody but the buyer can attach a child before
it confirms. Making the payment a version-3 (TRUC) transaction was weighed and
not taken: it limits the seller's child to 1,000 virtual bytes but does not
remove it.

### 8.4 Who may claim

In the form above only the buyer can claim, because the claim needs the buyer's
signature. A payment by anyone else enriches the seller and opens nothing for the
payer.

**An offer open to anyone** (no buyer key) must bind the claimant to the payer,
or anyone who sees a claim copies its proof into a claim of their own. With
`OP_INSPECTBTCOUTPUT` reading any output, a script does it with no further rule:
the payer adds an output to its payment carrying the claimant's Sequentia script
(an `OP_RETURN` of the script's witness program), and the open leaf checks that
the proven payment's output at a second index is that `OP_RETURN` and that the
claim pays the asset to that script (Elements' `OP_INSPECTOUTPUTSCRIPTPUBKEY`,
with `OP_CAT` to build the expected bytes). Binding by the paying input's key was
rejected: the proof carries the payment without its witness, so the key would
need a second proof through the block's witness commitment.

Two payers racing for one open offer: one claim wins; the other payer's bitcoin
reaches the seller and buys nothing. The open form suits offers where that race
is acceptable or ruled out.

### 8.5 The completion service and the cancel as a service

A portal operator runs a service that holds no key. It receives each buyer's
signed claims, signed cancel and the payment's transaction id, and then:

- completes the claim as soon as a Sequentia block has anchored at the
  payment's block or above;
- **watches the claim until its block is final** (certified, and its anchor
  buried as the swap requires), not merely mined; after every reorganization
  that disconnects it, resubmits a fresh proof of the **buyer's own** payment,
  linked to the lowest anchor the active chain recorded at or above it, and
  keeps doing so, at a rising tier, until `R`. Nodes re-link proofs themselves
  (section 6.3), but the service never relies on what they kept: this duty is
  the swap's guarantee;
- raises the claim's fee tier if it is not mined: to the second tier after 10
  Sequentia blocks, to the top tier after 20, well inside the margin of 6
  Bitcoin blocks (about 60 Sequentia blocks);
- if the payment is still unconfirmed two blocks before `D`, broadcasts the
  cancel. Under Bitcoin Core's full replacement the cancel replaces the payment
  if its fee is higher; the template of section 8.3 keeps anyone from making that
  replacement costlier.

The buyer's wallet does the same while it is open. Every node that watches
Bitcoin does the first part as policy (section 6.3). Every tier pays its fee in
an accepted asset (section 8.1); a wallet holding two accepted assets signs the
tiers in both, so a fee asset dropped from the producers' lists before the claim
is mined leaves a tier that still is.

**The top tier's rule.** A seller who wants the refund can try to keep the claim
out by filling every block of the margin with transactions paying a higher fee
rate. It must outbid the claim on every block's whole weight, the buyer on one
claim. So the top tier's fee is the value of the asset locked, times the claim's
virtual size, divided by the virtual size of all the blocks in the margin:

    top fee = V × v_claim / (B × N)

with `V` the asset amount locked, `v_claim` the claim's virtual size, `B` a
block's virtual-size limit and `N` the Sequentia blocks in the margin. With the
chain's limit of 89,999 virtual bytes, 60 blocks and a claim of 300 virtual
bytes, the top tier costs the buyer V / 18,000 (0.0056 % of the value), paid in
the claim's fee asset, and a seller who outbids it on every block pays more than V.
The rule is in the fee asset's own units, so it holds whatever that asset is
worth. `N` counts the blocks the claim can be mined in, from the first block
anchored at or above the payment to the last one anchored below `R`; the mempool
judging floors against the tip's anchor (section 6.1) keeps a stall from
shortening it.

Two routes make outbidding cheaper for the seller (proof review PV13, case E15),
and the rule takes both:

- **A seller that produces blocks** fills its own blocks for nothing. If it
  produces a share `s` of them, outbidding costs it `1 - s` of the sum above. A
  staker with half the blocks or more controls the chain outright, so the rule
  takes `s < 1/2`.
- **Fillers paid in a fee asset whose published rate is above what the seller
  pays for it**, for example a stale or illiquid one: producers value the
  fillers at the published rate, the seller pays the market price, and its cost
  falls by the ratio `m` of the two.

So the top tier is

    top fee = V × v_claim × m / (B × N × (1 - s))

With `s = 1/2` and `m = 2` it is V / 4,500 (0.022 % of the value) for the figures
above. `m` has no bound the chain enforces: it is bounded by how closely the
producers' rates follow the market, and a wallet sets it from the age and
liquidity of the rates it reads.

### 8.6 One payment, one lock

A single bitcoin output paying a script at least an amount opens every lock that
names that script, an amount no larger, a floor below its height and a deadline
at or after it. Only the side that wrote those locks is hurt: two locks naming one
receiving address are two sales paid once. That is always the seller's own
address, so the seller's wallet prevents it by deriving a fresh key for every
lock and never reusing one. The chain keeps no list of spent payments. A rule
refusing two proofs of one output in one transaction was rejected: two
transactions would do the same.

The floor covers the rest: a payment made before a lock existed never opens it,
and a covenant using the relative floor cannot be filled twice by one payment.

## 9. What each side can lose, and to whom

**The one risk this design has that a hash-locked swap does not:** a bitcoin
payment that confirms after the deadline belongs to the seller, because an
ordinary payment has no refund path of its own, and only the buyer's own fee
bumps and cancel, made in time, keep that from happening. **The one it removes:**
the asset can never go to someone whose payment is not in Bitcoin's chain,
whatever Bitcoin reorganizes, with nobody watching.

The chain guarantees one thing: a claim is valid exactly when a payment meeting
its leaf is in the Bitcoin history that the claim's block is anchored to.
Everything below follows from it, and the risk is not zero.

**What the check from the chain alone costs.** A proof links its block to an
anchor the chain has already recorded (section 4.3), so a node never asks Bitcoin
anything to judge it and honest nodes cannot disagree. The one cost: a claim
waits about one Sequentia block after its payment is first anchored. The mempool
takes it once a block has anchored at or above the payment, and the next block
mines it.

**9.1 The payment confirms after `D`.** The buyer has paid and cannot claim; the
seller refunds at `R` and holds both. *The buyer's wallet and its service* pay at
a next-block fee, bump by replacement if the payment is unconfirmed after two
blocks, and broadcast the cancel two blocks before `D`. Any version that confirms
by `D` still opens the lock, since every version pays the same output. *What
remains:* neither the payment nor the cancel confirms by `D`, and the payment
confirms later. Then only the seller's goodwill returns it.

**9.2 The buyer never pays.** The asset is parked until `R`, about `k + 12`
Bitcoin blocks after the lock, and then goes home.

**9.3 A reorganization with the payment double-spent.** The payment's block leaves
Bitcoin's best chain. Every Sequentia block anchored at or above it leaves too,
with any mined claim, and the claim cannot re-enter the mempool or any block, now
or later. The buyer keeps its bitcoin; the seller refunds at `R`. Nobody loses.

**9.4 A reorganization with the payment mined again.** The claim's transaction id
is unchanged; a node or the service attaches the new block's proof, and the claim
is mined again, if the payment's new height is above `F` and at most `D`. The
floor is absolute, so a reorganization that also reaches the lock's block changes
nothing: the lock is mined again, the coin anchor changes, the node fills it in,
and the swap completes at any depth. What can still cost the buyer its payment:

- the payment is mined again above `D` (as in 9.1);
- the reorganization reaches below `F` and the payment is mined again at or
  below `F`, which needs a Bitcoin reorganization deeper than every block since
  the lock was built;
- a dishonest seller **respends the lock's inputs** while the lock is back in the
  mempool. The lock does not signal replacement, so honest mempools refuse the
  respend, and the seller needs a block producer that takes it, plus a Bitcoin
  reorganization reaching the lock's anchor block: `k` blocks deep or more by
  step 3.

So the buyer's risk after paying is a deep Bitcoin reorganization, which the
buyer prices by its `k` exactly as it would price accepting any bitcoin payment
of that value, combined with a seller who controls a block producer. Without the
producer, no reorganization of any depth costs the buyer anything.

**9.5 A seller who fills the blocks of the margin.** The seller pays higher fees
than the claim's on every block from the payment to `R`, so that the refund
becomes valid with the claim still unmined. The completion service raises the
claim to its top tier (section 8.5), which the seller can outbid only by paying
more than the asset is worth. What remains: a seller willing to lose more than
the swap's value to deny it, and a fee asset whose accepted rate collapses at
the producers before the top tier is mined; the buyer then needs a tier in
another accepted asset, which a claim with a second input can pay.

**9.6 The seller.** Once the lock is mined the seller cannot cancel, and needs
nothing but the refund at `R`. A claim cannot be mined without a payment in the
anchor chain, so the seller loses the asset only against bitcoin in the chain its
own validating node follows. A producer, or a whole committee, that includes a
claim with a forged or orphaned proof makes a block every validating node that
watches Bitcoin refuses. A node that does not watch Bitcoin takes anchors on
trust (section 4.3) and can be shown a fabricated one; a producer's own template
can also build, for up to one watcher interval, on a parent whose anchor its
Bitcoin node already shows replaced, a block the network then disconnects. Both
are why a wallet counts a swap as done only once its block is final.

**9.7 After `R` both leaves are valid.** Honest mempools prefer the claim: a
refund never replaces a pending claim, whatever it pays, and a claim replaces a
refund pooled before it (`btcpay-floor-replaces-proof`, section 6.2). Before a
block has anchored at `R` the mempool refuses the refund outright
(`anchorlock-not-reached`, judged against the tip's anchor, never Bitcoin's tip).
Completion by anyone puts the claim in mempools from the payment's first anchor
on, so in the normal case it is mined long before `R`. A seller that produces
blocks can still include its refund in place of a claim pending at `R`, which
consensus allows, and that happens only after a stall: Sequentia producing no
block anchored at or above the payment for the whole margin. A stall longer than
the margin hands the choice to whichever producer makes the first block anchored
at `R` or above, because nothing caps how far one block's anchor may advance over
its parent's. A stall of that length means the committee has been short of its
quorum for an hour; its cause is in [`04-proof-of-stake.md`](04-proof-of-stake.md).

**9.8 A reorganization at each step.** Before the lock is mined, nothing is at
stake. Between the lock and the payment, the buyer has not paid; if the lock's
block leaves the chain, the buyer waits for it to be mined again and for depth `k`
again. With the payment unconfirmed, nothing on either chain refers to it. After
the payment, as in 9.3 and 9.4. After the refund, it is mined again in the first
block anchored at or above `R`, unless a claim gets in first, which needs a valid
payment that nobody completed for the whole margin.

## 10. The collision class

One output satisfying two covenants has been found five times in this project,
so this section lists what can share a transaction with a claim or a refund.

| Shares the transaction | Can one proof or one output serve both? |
|---|---|
| a second claim of a lock naming the same address | yes, and it is the seller's own error (section 8.6) |
| a refund or a claim of another lock | no: each leaf reads only its own input's records, and each is authorized by a signature over the whole transaction |
| a covenant that checks "some output pays X" (resting orders, guarded orders, landings, enclaves, escrows, fee pots) | not through the claim or refund: neither leaf counts Sequentia outputs, so neither can mistake another covenant's output for its own |
| a Simplicity program on another input | it cannot read this input's slot, and the node checks every record anyway |

The two-party form leaves nothing to collide because both leaves are signature
paths over the whole transaction. A keyless form (an open offer, a resting order
filled by a payment) must check its own payment and outputs by the pattern that
leaves no output to share: the order is input 0 and pays output 0, alone, or an
equivalent one-to-one pairing.

## 11. What a user sees

In a wallet's default mode, buying an asset with bitcoin is a payment with a wait,
and selling one is choosing an amount. No protocol, rail, lock or proof is named,
and nothing is offered as a choice.

- **Buying.** "Buy 100 XYZ for 0.0123 BTC." One button. While the wallet waits for
  the seller's side: "Getting your purchase ready. This usually takes a few
  minutes." Then "Paying 0.0123 BTC. Your XYZ arrives a minute or two after Bitcoin
  confirms the payment, usually within 20 minutes." Then "Bitcoin confirmed your
  payment. Receiving 100 XYZ", and the purchase shows as done, with the balance,
  only once the claim's block is final.
- **Paying from another wallet:** an address, an amount and a time: "Send exactly
  0.0123 BTC to this address within the next hour."
- **Selling.** "Sell 100 XYZ." The wallet shows the price and "Waiting for the
  buyer's bitcoin. This usually takes 20 to 30 minutes." Then the bitcoin balance.
  A refund shows as done only once its block is final.
- **A payment that did not confirm in time, cancelled:** "The payment did not
  confirm in time, so your wallet cancelled it. Nothing was spent."
- **A late payment, bought from the portal's own seller:** "Your payment confirmed
  after the deadline, so this purchase did not go through. We return late
  payments; your bitcoin will be back within an hour." This is the operator's own
  promise and the operator keeps it.
- **A late payment, bought from anyone else:** "Your payment confirmed after the
  deadline, so this purchase did not go through. The bitcoin reached the seller,
  and only the seller can send it back."
- **The buyer did not pay:** "The buyer did not pay in time. Your 100 XYZ are
  back."

Developer mode shows the lock, the deadlines in Bitcoin heights, the proof and its
block.

## 12. What it replaces and what else it serves

It replaces the hash-locked swap against on-chain bitcoin wherever one is built:

- seqdex's on-chain cross-chain swap (maker and taker, both directions);
- the node wallet's convert (`src/wallet/xchainconvert.cpp`);
- the Sequentia Wallet Kit and the wallets built on it (the web wallet, Ambra
  browser, Ambra mobile);
- the restricted-asset swap against bitcoin, whose claim leaf keeps the issuer's
  co-signature beside the buyer's.

Hash-locked swaps over Lightning are unaffected: they settle without waiting for a
Bitcoin confirmation.

Further uses, not built, none needing a further rule:

- **resting sell orders filled by a payment:** a covenant that releases asset in
  proportion to the bitcoin proven paid (`OP_INSPECTBTCOUTPUT` and 64-bit
  arithmetic) and re-creates the rest under the relative floor, with the open
  form's binding (section 8.4) so that a stranger cannot take a fill;
- **sales for bitcoin:** a token sale that releases on payment;
- **repayment in bitcoin:** a loan whose collateral is released on a proof that
  the repayment was paid.
