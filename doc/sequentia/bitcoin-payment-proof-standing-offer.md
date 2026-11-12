# A standing offer filled by a payment

This note belongs beside [`bitcoin-payment-proof.md`](bitcoin-payment-proof.md)
and uses its terms: the payment proof, the coin anchor `C`, the absolute and
relative floors (section 4.5), the per-trade lock (section 8.1) and the open
form's binding (section 8.4). It asks whether a swap for bitcoin can run with
no wait at any amount, built from the three payment opcodes as they are and
from the tapscript opcodes the chain already has, with no new rule. It
answers no, and then weighs what would close the gap: a fourth opcode (section
6) or a signing rule for the committee (section 7). Section 8 compares every
form.

## 1. The gap

In the per-trade form the seller makes the lock moments before the buyer pays.
If Bitcoin reorganizes the block that the lock's Sequentia block is anchored to,
the lock goes back to the mempool. Honest producers mine it again, and because
the floor is absolute the swap completes. The buyer loses its payment only if
the seller also controls the producer of the block that would mine the lock
again, and that producer takes a respend of the lock's inputs instead. Without
a wait before paying, that reorganization needs to be only one Bitcoin block
deep.

A form closes the gap if:

1. the buyer pays as soon as it has decided, at any amount, after waiting for
   nothing on either chain;
2. no Bitcoin reorganization costs the buyer its payment, unless it is deeper
   than the seller's offer is old, even when the seller controls block
   producers;
3. the seller loses the asset only against bitcoin in the chain its own node
   follows;
4. no honest party loses to another honest party.

## 2. The candidate: a standing offer

The asset sits in an output made earlier, so it is already old when a buyer
pays. Anyone who proves a bitcoin payment of at least the price to the seller
releases asset in proportion to the payment, to the destination that the
payment itself names. The output re-creates the rest under the same script.

The prototype's fill leaf, keyless, with witness `<o> <p>`: `p` is the index of
the payment output that pays the seller, and `o` is the index of its `OP_RETURN`
output, which names the buyer's destination.

```
OP_PUSHCURRENTINPUTINDEX 0 OP_EQUALVERIFY            # input 0 alone: outputs 0 and 1 are its own
OP_INSPECTBTCPAYMENT                                 # o p h C
OP_GREATERTHAN                                       # relative floor h > C
                                                     # (absolute: OP_DROP <F> OP_GREATERTHAN)
OP_NOTIF OP_RETURN OP_ENDIF
OP_INSPECTBTCOUTPUT 0 OP_EQUALVERIFY <program> OP_EQUALVERIFY   # o value paid to the seller
<K> OP_MUL64 OP_VERIFY                               # o release = value x K
OP_DUP <input value> OP_SWAP OP_SUB64 OP_VERIFY      # o release rest
<output 1 value> OP_EQUALVERIFY                      # output 1 holds the rest,
<output 1 script> = <input script>                   # under this covenant,
<output 1 asset> = <input asset>                     # in this asset
<FEEMAX> OP_SUB64 OP_VERIFY
<output 0 value> OP_SWAP OP_GREATERTHANOREQUAL64 OP_VERIFY      # the fee takes at most FEEMAX
<output 0 asset> = <input asset>
<output 0 script: version 1> 0x6a2101 OP_SWAP OP_CAT OP_SHA256  # OP_RETURN 01 <program 0>
OP_SWAP OP_INSPECTBTCOUTPUT -1 OP_EQUALVERIFY OP_NIP OP_EQUAL   # is the payment's OP_RETURN
```

The opcodes it uses all exist on this chain:
`OP_PUSHCURRENTINPUTINDEX`, `OP_INSPECTINPUT*` and `OP_INSPECTOUTPUT*`
(`0xc7` to `0xd1`), `OP_SUB64` and `OP_MUL64` (`0xd8`, `0xd9`),
`OP_GREATERTHANOREQUAL64` (`0xdf`), `OP_CAT` (`0x7e`, left enabled by
`IsOpSuccess` in `src/script/script.cpp`), `OP_TWEAKVERIFY` (`0xe4`) and the
three payment opcodes (`0xe5` to `0xe7`). The leaf is 129 bytes. On regtest, a
fill of a payment in a block of three transactions had a 373-byte proof and a
virtual size of 391 bytes. On a full Bitcoin block the proof is about
1.2 kilobytes (specification, section 5).

The floor is the only check that fails with `OP_RETURN`, so a block refused
with `OP_RETURN was encountered` was refused by the floor and by nothing else.

## 3. Replay against concurrency

The chain keeps no list of spent payments (section 8.6), and the script cannot
read a payment's transaction id or its place in its block. All it can know of a
payment is `h` and the payment's outputs.

**The relative floor stops replay and shuts out honest buyers.** Each fill
re-creates the coin with a later `C`: the anchor of the block that mined the
fill, which is at least the height of the payment it proved. So one payment
cannot fill twice (SO2). By the same stroke, the floor refuses any payment mined
at or below that `C`. A second buyer's payment is lost:

- in the same Bitcoin block as the first buyer's (SO5);
- in the next block, when the first fill is mined after both payments
  (SO6). It usually is, because a fill waits one Sequentia block after its
  payment is first anchored;
- after a Bitcoin reorganization that mines the two payments again together.
  Both fills had been mined; afterwards the second can never be (SO10), with
  nobody acting against anyone.

The relative floor therefore admits at most one fill per Bitcoin block at best,
and in practice fewer. Each extra payment reaches the seller and buys nothing.

**The absolute floor allows replay.** One payment that the seller makes to
itself filled the offer three times and left less inventory than a buyer's
payment needed (SO8).

**What could take the place of a list of spent payments, and what each leaves:**

- *State carried by the covenant.* The covenant could commit to a sparse merkle
  tree of consumed payments, keyed by the hash of the payment's `OP_RETURN`
  output (which `OP_INSPECTBTCOUTPUT` already pushes, and which includes the
  destination and a buyer's nonce). Each fill proves the key is absent, then
  re-creates the output with the key added, rebuilding the taproot key with
  `OP_CAT`, tagged hashes and `OP_TWEAKVERIFY`. The key must be at least
  128 bits, or a seller grinds a payment onto a buyer's key, so each fill
  carries 128 or more tree levels: several kilobytes of witness. That would
  answer replay against concurrency, since fills commute and an honest second
  buyer is not shut out. It was not built, because section 4 makes it moot.
- *An address derived per buyer and per payment*, with `OP_TWEAKVERIFY` over
  the seller's key and the buyer's destination. This binds the payment to the
  destination, as the `OP_RETURN` already does, but it adds no uniqueness:
  under the absolute floor one payment still replays, and the seller derives
  addresses for destinations of its own.
- *Independent slots*: one-shot outputs, each with its own receiving script.
  These stop replay, because a coin is spent once, but two payments can still
  pay one slot's address (SO9).

## 4. The seller is always a buyer

Each of these forms has the same flaw, and it decides the question. **The
seller can pay its own receiving script.** Doing so costs it only the fees,
since the bitcoin comes back to it, and the fill releases asset to a
destination of its own. The script cannot tell this payment from a buyer's.
Both pay the same script the same amount, and the payment is carried without
its witness, so no key of the payer's can be read (section 8.4 rejects
binding by the paying input's key for this reason).

SO7 runs the result. The buyer pays. The seller sees the payment in Bitcoin's
mempool and pays itself the same amount, and both are mined in one Bitcoin
block. Both fills are valid. The seller's fill pays the higher fee and
displaces the buyer's in an honest mempool, and an honest producer mines it.
The buyer's fill is then refused against the spent coin
(`bad-txns-inputs-missingorspent`), and against the re-created one by the floor
(`OP_RETURN was encountered`). The buyer's bitcoin has reached the seller and
buys nothing. The seller needed no Bitcoin reorganization and no block
producer. Under the absolute floor it does not even need a second payment
(SO8), and with slots it takes the slot (SO9).

**This holds for every keyless form, not only the ones tried.** To be safe at
the moment it pays, the buyer needs inventory that only its own payment can
take. Inventory that any payment meeting the script can take is inventory the
seller can take, at the cost of fees. Making the inventory the buyer's alone
needs a Sequentia transaction that names the buyer, made after the buyer is
known, which is after the buyer has decided. Such a transaction is fresh, and
a Bitcoin reorganization one block deep removes it while a seller's producer
mines a conflicting spend. That is exactly the per-trade lock's gap. So the
opcodes as they are offer two choices: a buyer-bound lock that is fresh when
the buyer pays, or an old offer that anyone can drain. The four conditions
need a lock that is both bound to the buyer and old, and nothing made from
these opcodes is both. A seller bond paying any buyer left without inventory
does not help either, because the seller drains the bond with the same
payments to itself.

## 5. The obstacles, one by one

- **Two buyers, one offer, not enough inventory.** An ordinary payment has no
  refund, so the second buyer is not made whole, and the race cannot be
  prevented (section 4). Under the relative floor it happens even when
  inventory is ample (SO5, SO6).
- **One payment, several offers of one seller.** A fresh receiving script per
  offer binds a payment to one offer, as section 8.6 requires, which protects
  the seller. The buyer is then exposed whenever that offer is drained or
  withdrawn, and section 4 lets the seller do both at once. No binding closes
  both ends.
- **Taking inventory back and repricing.** An announce-then-wait under an
  anchor floor cannot hold the seller, because a payment to itself withdraws
  inventory at once (SO8). If it did hold, the wait would need to cover the
  slowest payment that a wallet could have made in good faith. That is the
  per-trade form's `D` and `R`, about 12 Bitcoin blocks for each reprice. A
  buyer's wallet could check for an announcement in a block, but not for a
  payment the seller makes to itself.
- **A late payment.** The standing offer has no deadline. Under the relative
  floor, a slow payment is the most exposed of all: any fill mined at an
  anchor at or above its height shuts it out for good.
- **Reorganizations, step by step.**
  - *Before the offer is old enough:* nobody has paid against it.
  - *After a fill, when the payment is mined again:* under the relative floor,
    the next fill's floor moves with the reorganization (SO10; PV13's case E18
    for covenants). Under the absolute floor the fill stands, and so does
    replay.
  - *Payment double-spent:* the fill leaves the chain, the inventory returns,
    and the buyer keeps its bitcoin. Nobody loses.
  - *Between an announcement and its maturity:* moot, as above.
- **The claim's fee.** The fill pays its fee out of the released asset. The
  script caps that fee at `FEEMAX`, so whoever completes a fill cannot give the
  buyer's asset to the fee (SO4). When producers do not accept the asset, the
  completer adds an input in an asset they do accept. The covenant reads only
  input 0 and outputs 0 and 1, so that input collides with nothing, and no
  asset is privileged.
- **Restricted assets.** The issuer's co-signature cannot tell a buyer from the
  seller paying itself, so it does not stop the drain. It also requires the
  issuer to be online for every fill.

## 6. What would close it: a fourth opcode

Bitcoin already has the rule this needs. **A Bitcoin output can be spent once.**
Two transactions can create equal outputs, but only one can spend a given
input. A slot that only a payment spending one particular Bitcoin output can
open is therefore a mutex, and Bitcoin enforces it. What follows is a design
for the owner to decide on. Nothing of it is built or run.

### 6.1 The seller holds no bitcoin

The owner's constraint: **a seller of an asset for bitcoin never needs to hold
bitcoin, not even dust, before it is paid.** The payer's wallet may build any
Bitcoin transaction the form needs. Everything in this subsection is reasoned,
not run.

**What the coin `U` is for.** It provides uniqueness: one spend, so one
outcome. It need not be the seller's money. A buyer must be able to spend `U`
only by paying the price, and the `SIGHASH_SINGLE|ANYONECANPAY` signature over
`U` is what enforces that. Without such a signature `U` is spendable by anyone,
for nothing: anyone can kill the slot, and fee scavengers sweep a coin of a few
hundred sat whenever spending it costs less than its value. So `U` needs a
**signer**: whoever holds `U`'s key and signs it. The signer and the funder can
be anyone:

- **A service, or any maker, paid by the seller in any asset**, funds `U` to its
  own key or to a MuSig2 key of itself and the seller, and signs it. The seller
  holds no bitcoin.
- **What the signer can do.** It can spend `U` itself. That kills the slot: the
  lot goes back to the seller through the take-back leaf, which needs only the
  seller's Sequentia key. If a buyer's payment is in flight, that payment
  becomes invalid and the buyer keeps its bitcoin. The signer can also buy the
  slot, by paying the price like anyone else. It can never fill without paying
  the price, so it can stall, censor or cancel a slot, and it can take nothing
  from either side. Under a MuSig2 key it cannot even kill the slot alone.
  Instead it can refuse to co-sign a cancel and hold the offer hostage. A
  script path on `U` that lets the seller spend it alone after a timeout bounds
  that, with the fee paid from `U`'s own value (or by a child of an anchor
  output). So the funder puts in a few thousand sat, priced to the seller in
  the asset the seller pays with.
- **When `U` must exist.** Its outpoint is written into the slot, so it must be
  fixed (its funding transaction signed and non-malleable) before the slot is
  made. It should be confirmed before the slot has aged. If `U` never comes to
  exist, no payment can spend it (a payment that tries is invalid, and its
  payer keeps its bitcoin), but the lot can only come home by a long expiry on
  the slot. Near that expiry the specification's 9.1 returns, so a buyer's
  wallet does not pay a slot close to it.
- **A coin made by the payer at trade time brings the gap back exactly.** A
  slot made then, to bind that coin, is made from the seller's free coins, and
  the seller's producer can replace it with a slot bound to a coin of its own.

**A fresh output carries no gap when a covenant forces it.** The gap of
section 1 is a fresh output whose inputs the seller can spend some other way.
An output re-created by a fill has no such alternative. The only valid spend of
slot `k` is the fill by the one payment that spent `U_k`, and the covenant fixes
the remainder output. After a reorganization, any completer mines that fill
again (perhaps with another fee, and so another transaction id), and the same
remainder appears. If the payment is double-spent instead, the coins it created
vanish with it. This opens a **chained offer**:

- payment `k` carries, beside the price and the destination, the next coin
  `U_{k+1}` as an output. Its key is one the covenant names (the seller's
  signer), or buyer `k`'s own key, in which case the covenant checks only that
  the output is there;
- the fill releases in proportion and re-creates the rest bound to
  `U_{k+1} = (payment k's transaction id, that output's index)`. The covenant
  rebuilds the next leaf with `OP_CAT` and the streaming SHA-256 opcodes and
  checks output 1 with `OP_TWEAKVERIFY`. For this, the opcode of 6.2 also
  pushes the proven payment's own transaction id (index `-1`), which checks 1
  to 4 already compute;
- only the offer's first coin comes from outside the chain, so there is one
  funded coin per offer, not one per lot. After its first fill the seller holds
  bitcoin anyway, and the constraint is spent;
- each later coin still needs its signature over `U_{k+1}`. That comes from the
  seller's online signer, a service the seller delegates to, or buyer `k`'s
  own wallet at payment time. Whichever signs can stall the offer, never take
  from it. Two buyers who both build a spend of `U_{k+1}` are decided by
  Bitcoin, and the one that is not mined spent nothing. A buyer may even spend
  `U_{k+1}` while payment `k` is unconfirmed, as its child. If payment `k` is
  replaced, the child is invalid and that buyer keeps its bitcoin.

**No coin tied to the seller at all: an HTLC payment, with the three opcodes.**
The payer's wallet pays an output the seller can spend with the buyer's secret
`s`, and the buyer alone can spend after a timeout `T`. The fill leaf rebuilds
that P2WSH program from the hash in the witness, the seller's key and the
buyer's refund key with `OP_CAT` and `OP_SHA256`, and compares it with
`OP_INSPECTBTCOUTPUT`. A buyer who never reveals `s` gets its bitcoin back by
itself, whether the offer was drained (SO7, SO8), the buyer raced (SO9) or the
floor shut it out (SO5, SO6, SO10). The seller sweeps with `s`, paying the fee
from the output itself, so it needs no bitcoin either. Revealing `s` is what
commits the buyer, so the fill runs in two steps: first it reserves the lot
without `s`, then it claims with `s`. A buyer that revealed `s` in a fill that
was then not mined would hand `s` to the seller. A reorganization before the
reveal costs the buyer only time. One after it, when the seller controls a
producer, is the gap of section 1. So this form moves the wait to after the
payment, for as long as the buyer chooses, rather than removing it. The seller,
or a watchtower acting for it, must sweep before `T`, and the buyer must
refund after `T`. It needs no new rule, and it was not run.

**The two directions.**

- *A maker selling from its inventory to a person paying bitcoin.* The maker
  holds bitcoin anyway, so it funds its own coins or buys them from a service.
  The person pays at once, with the full guarantee of 6.4.
- *A person or an issuer with no bitcoin selling to a payer, usually a maker.*
  The exposed party is the payer. It carries the gap by its own choice with
  the per-trade lock of the specification's section 8 and the wait it turns on
  (`w`). The seller makes the lock, waits for nothing and holds nothing. No
  fourth opcode is needed for this direction. The chained offer serves it too,
  with a service funding the first coin.

**What the best form under the constraint gives up.** Buyers give up nothing:
no wait at any amount, and a payment is lost only to a reorganization deeper
than the offer's first slot is old. What is given up is liveness. Whoever signs
the first coin and each later one decides whether the offer stays open, and
with a service as signer the seller relies on it to cancel quickly. The
timeout path on each coin bounds that reliance.

### 6.2 The opcode: a reading, not a proof

**`OP_INSPECTBTCINPUT`**, on the next free `OP_SUCCESS` slot (`0xe8`), under
`SCRIPT_VERIFY_BTCPAYMENT` and from the same activation height as the other
three. It pops an input index and pushes that input's previous output from the
proven payment: the transaction id (32 bytes, in the order the transaction
serializes it), then the output index as a script number. With index `-1` it
pushes the proven payment's own transaction id, which the chained offer of 6.1
needs. It fails without a
payment proof (`SCRIPT_ERR_BTCPAY_MISSING`) and for an index out of range (a
new `SCRIPT_ERR_BTCPAY_INPUT`). Reading it counts as reading the proof for the
relay rule of section 6.1 of the specification.

It reads a record that is already checked, and adds no proof. The type `0x01`
record already carries the whole payment without its witness, and the
interpreter already parses it into a Bitcoin transaction (`proof.tx` in
`src/script/interpreter.cpp`, the object `OP_INSPECTBTCOUTPUT` reads
`vout` from). Checks 1 to 4 tie those bytes, inputs included, to the header's
merkle root, and checks 5 to 7 tie the header to the anchor chain. The opcode
reads `proof.tx->vin[i].prevout` from the same object, and the payment's
transaction id is the hash that check 2 already computes. It does not check that
the input was unspent or properly signed. Bitcoin did that when it mined the
block, which is the same trust the proof already places in Bitcoin's blocks.

### 6.3 The slot, and who decides its fate

For each lot there is:

- **on Bitcoin, a coin `U`**, funded and signed as 6.1 describes (by the maker,
  by a service, or by the payment of the previous fill), and a key-path
  signature over spending `U` with
  `SIGHASH_SINGLE|ANYONECANPAY`. Under that signature, output `i` of the
  spending transaction, where `i` is `U`'s input index, must pay the price to a
  receiving script `S` used for this slot alone. The signer publishes the
  signature with the offer;
- **on Sequentia, an output** holding the lot under an unspendable internal key
  and two leaves. Both take the input index `i` from the witness:
  - **fill**: the proven payment's input `i` spends `U`, its output `i` pays at
    least the price to `S`, and output `i + 1` is an `OP_RETURN` naming a
    destination. The claim pays the lot to that destination. Nobody signs;
  - **take-back**: the proven payment's input `i` spends `U`, and its output
    `i` does not pay at least the price to `S`. The maker signs.

**The slot's fate follows whichever Bitcoin transaction spends `U`.** No
Sequentia transaction decides it. If the buyer's payment spends `U`, only the
fill can open. If `U`'s signer spends it to cancel, at the maker's request or
after the timeout of 6.1, only the take-back can open. A Bitcoin history holds one spend of `U`, so on any branch of Sequentia
exactly one of the two leaves can ever open, and nothing on the Sequentia side
is left for the maker to race. A maker that spends `U` and pays itself the price
at output `i` opens the fill to a destination of its own, which is a take-back
by another name. Two slots naming one `U` would be two sales paid once, and
that is the maker's own error, as in section 8.6, so the maker's wallet makes a
fresh `U` and a fresh `S` for every slot.

A payment that pays the price but names no valid destination at `i + 1` opens
neither leaf. The maker keeps the price and the lot stays where it is for good.
Only the payer who built the payment wrongly loses, and nobody else can build
it that way, because spending `U` requires the maker's signature and that
signature requires the price.

### 6.4 What each side can lose, and to whom

Here, in the form of the specification's section 9, the "payment" is the
buyer's Bitcoin transaction that spends `U`.

- **The payment confirms late (the specification's 9.1).** This loss
  disappears. There is no deadline, so a payment that spends `U` owns the slot
  whenever it confirms. If something else spends `U` first, whether the maker's
  cancel or another buyer, the buyer's transaction is invalid on Bitcoin and
  its inputs stay the buyer's. Nobody can receive the buyer's bitcoin without
  the buyer receiving the lot.
- **The buyer never pays (9.2).** Nothing is parked, and the slot stays open for
  the next buyer. There is no refund, because none is needed.
- **The payment is double-spent in a reorganization (9.3).** If the new history
  spends `U` in another transaction, the fill's Sequentia block leaves the
  chain with its anchor, and the slot follows the new spend. Whoever made that
  spend now holds the slot: another buyer gets the fill, the maker gets the
  take-back. The original buyer's transaction is invalid, so the buyer keeps
  its bitcoin. If the new history spends `U` nowhere, the slot is open again
  and the buyer's inputs are unspent. Nobody loses.
- **The payment is mined again (9.4).** It still spends `U`, so the fill is
  completed again with a fresh proof (the node's re-attacher, the completion
  service), at any depth. The slot's Sequentia output is older than the
  reorganization and stands. The maker's producer cannot spend the slot,
  because both leaves need a proof of a spend of `U`, and the only one in the
  chain is the buyer's. **What remains:** a Bitcoin reorganization deeper than
  the slot's Sequentia output is old, together with a maker who controls the
  producer that would mine that output again and has it respend the output's
  inputs instead. This is the per-trade gap, with the lock's age in place of
  the minutes between lock and payment. The buyer's wallet bounds it by
  choosing old slots. A reorganization deeper than `U`'s own creation does not
  hurt the buyer: either `U` is created again and the payment re-mined, or `U`
  never exists and the payment is invalid.
- **The maker fills the margin (9.5) and both leaves open after `R` (9.7).**
  Both disappear. There is no `R`, and no leaf becomes valid with time, so
  delaying the fill gains the maker nothing. The fill needs only an ordinary
  fee, and no top tier.
- **The maker (9.6).** It loses the lot only against a Bitcoin transaction in
  its node's chain that spends `U` and pays it the price. A stale quote (6.6)
  is the one cost it carries.
- **Two honest buyers.** Both build a spend of `U`, and Bitcoin mines one. The
  other transaction never confirms, and that buyer spent nothing. A third party
  who copies a buyer's spend with a higher fee buys the slot itself and pays
  the price itself, so the buyer loses only the attempt.
- **A reorganization at each step (9.8).** Before `U` is spent, nothing is at
  stake. With the payment unconfirmed, the maker's cancel can win, and then
  the buyer spent nothing. After the payment, as above. After a take-back, the
  take-back stands or falls with the maker's own spend of `U`.

### 6.5 What the buyer's wallet must build

The buyer's wallet builds a transaction around the maker's signed input:

- `U` at input `i`, with the maker's signature and the `nSequence` it signed;
- the price to `S` at output `i`, and the `OP_RETURN` at `i + 1`;
- its own inputs and change;
- the transaction version and `nLockTime` that the signature commits to (a
  taproot signature commits to both under any sighash), so the wallet turns
  off anti-fee-sniping.

Bitcoin Core does this with `createrawtransaction`, then `fundrawtransaction`
with `input_weights` for the foreign input, then `signrawtransactionwithwallet`,
which keeps the maker's signature. A PSBT library does the same. Every
Sequentia wallet holds bitcoin itself (every wallet is dual-chain), so the web
wallet, Ambra and anything built on the Sequentia Wallet Kit can build this
transaction.

**Who can no longer pay this way:** any payer that can only send an amount to an
address, such as an exchange withdrawal, a custodial wallet or most
hardware-wallet flows. **What stays for them:** the per-trade lock of section 8,
paid by an ordinary payment to an address, with a deadline. Both forms run side
by side: the slot for wallets that build the transaction, the lock for
everything else.

### 6.6 The maker's side

- **For each lot:** one Bitcoin output, about 43 virtual bytes to create in a
  batch, holding a few hundred sat (a few thousand where it must also pay for
  its own cancel, as in 6.1). A maker with no bitcoin buys it from a service in
  any asset; a chained offer needs it only for its first slot. That value passes to the buyer, so the
  price accounts for it. The lot also needs one Sequentia output and one
  signature. A ladder of lots at several prices is a ladder of slots.
- **Cancelling** is one Bitcoin transaction that spends the `U`s back to the
  maker, about 58 virtual bytes per slot in a batch (a taproot key-path input).
  It takes effect when that transaction confirms, about ten minutes, and the
  take-backs on Sequentia follow, one input each with its own proof. Until it
  confirms, a buyer can still win a `U` at the old price, which is a fair race
  on Bitcoin's own rules.
- **Repricing** is the same transaction, which also creates the new `U`s:
  about 100 virtual bytes per slot, or about 1,000 sat per slot at
  10 sat per virtual byte. New slots on Sequentia are fresh, so a buyer who
  takes one at once carries the per-trade gap of 6.4 until the slot is old.
  The maker can create slots before it needs them so that buyers find old
  ones. These sizes are the standard taproot sizes and were not measured here.
- **A stale quote.** A signature at an old price stays valid until `U` is spent.
  Bitcoin gives no upper bound in time to a signature, and no Sequentia-side
  expiry may be added, because a buyer whose payment then confirmed would lose
  it, which brings back the specification's 9.1. Only the maker is hurt, when
  the market moves against the price. Three things bound the loss: the lot
  sizes, the time to cancel (one Bitcoin confirmation, during which the cancel
  competes with buyers on fee), and how many lots the maker exposes at once.
  The per-trade lock's quote lives until `D`, at least six Bitcoin blocks,
  which is longer.

### 6.7 Before activation or after

The rule is not active on any chain yet. The testnet gets it only at a height
the owner chooses (specification, section 7).

- **Added before activation**, the opcode is a few lines beside
  `OP_INSPECTBTCOUTPUT`, reading an object that is already parsed. It needs one
  new script error, unit and functional tests, and a share of the proof review
  the rule gets anyway. It costs no extra version bump, cutover or activation:
  one tightening activates once.
- **Added after activation**, it is a second tightening on another `OP_SUCCESS`
  slot. That means its own activation height, its own version bump and release
  notes, a second all-at-once cutover of the testnet's nodes, and wallets that
  must wait for that height before they offer the slot form. Mainnet has not
  started, so it would get the opcode from genesis either way.

## 7. Certification as the guard

This section asks whether the gap can be closed where blocks are certified
rather than in a script. It is a feasibility reading of the code and of
[`04-proof-of-stake.md`](04-proof-of-stake.md). Nothing in it is built or run.

**The rule.** After an anchor-driven disconnect, a committee member does not
sign a block that spends an outpoint already spent by a transaction it holds
as **protected**. A protected transaction is one from a certified block that
the node itself disconnected, and the rule applies unless the block spends the
outpoint with that same transaction id (a fresh proof keeps the id). Like the
share-lock (section 9 of the PoS document), this is local signing policy, not a
validity rule: no block becomes invalid because of it, and every node still
accepts a conflicting block that a quorum did certify.

**7.1 Where a member decides to sign.**
- **The gossip committee.** In `PosProducer::DriveRound` (`src/pos_producer.cpp`)
  a member picks the round's proposal with `BackedForRound`, then validates it
  lazily just before signing: `TestBlockValidity` against the active tip, under
  `cs_main`. An invalid proposal adds its leader to `m_excluded` and the next
  candidate is backed. The check belongs exactly there, beside
  `TestBlockValidity`. A conflict excludes the leader the same way, and the
  next leader, who holds the protected transaction in its mempool, mines it.
- **The other signing paths** (`musigpartialsign` and `generateposblock`, which
  coordinators use) need the same check.
- **What the member has at that moment.** It has the mempool. `InvalidateBlock`
  re-admits every disconnected transaction on anchored chains
  (`MaybeUpdateMempoolForReorg`; the `add_to_mempool` line is uncapped when
  `g_con_bitcoin_anchor` is set). It also has the waiting set of claims
  (specification, section 6.3).
- **What it lacks.** Neither of those records where a transaction came from,
  and a transaction the mempool later evicts or refuses would drop out. So the
  protected set must be a new local record: the transaction ids and spent
  outpoints of the disconnected certified blocks. It is filled from the
  disconnect pool after the disconnect, never on its path. No consensus data is
  needed.

**7.2 Who must be dishonest.** The replacement block's committee is drawn again
from the new state, but any member that holds the protected set refuses to
sign. A conflicting block therefore needs a quorum made only of members that
are dishonest, or unaware of the disconnected block (offline through it,
restarted since, or never having had it):

- **Public committee** (the testnet: 250 seats, quorum 126): dishonest plus
  unaware members must hold at least 126 seats, a strict majority of the stake
  behind the committee. That is the same coalition that can already certify two
  blocks at one height. The gap falls back to the chain's own safety
  assumption, from one producer plus a Bitcoin reorganization.
- **Private-VRF committee** (quorum: a strict majority of the expected size,
  51 of 100): the drawn set varies. With 40 % of stake dishonest or unaware, a
  draw holds 51 or more of them about 1.6 % of the time. The attacker also has
  to lead that slot, and it has only the slots before an honest leader re-mines
  the protected transaction, usually one or two.
- **A restart.** The set lives in memory, so a member that restarts between the
  disconnect and the vote is unaware. Persisting it at shutdown, as the mempool
  is persisted, closes that.

**7.3 The escaping-stall path is the hole the rule must also cover.** A Bitcoin
reorganization `d` blocks deep leaves Sequentia's tip anchored at or below the
fork point, while Bitcoin's new tip is at least `d + 1` above it. From `d = 2`,
the first new block can therefore escape a stall (an anchor gap of 3 or more,
PoS document section 5) and be certified by **a single signer**: the seller's
producer, carrying the respend. Refusing to sign is then not enough. The rule
needs a second arm:
- a member refuses to sign any block whose own transactions, or those of its
  ancestors above the disconnect point, conflict with the protected set. So it
  never extends that block;
- it signs a sibling at that block's height, on that block's parent, which is
  what the share-lock's rival-parent arm already does for same-height rivals.

A certified sibling beats an escaping-stall block at the same height (the
comparator's first key, PoS document section 6), so every node, including
those without the rule, moves to it. This works when the honest and aware
members hold a quorum themselves. If they hold fewer than a quorum but more
than `K` minus the quorum, neither side certifies at full quorum. The
conflicting block then stays a tip that no honest member extends: finality
stalls until stake moves, and the respend never becomes final.

**7.4 Bounds.**
- **Size.** A reorganization `N` Bitcoin blocks deep disconnects about `10N`
  Sequentia blocks of at most 89,999 virtual bytes each. That is at most about
  8,200 transactions per Bitcoin block of depth at the smallest transaction
  size, so about 82,000 for `N = 10`. Each entry is one transaction id plus its
  spent outpoints, a few megabytes in all. Filling the set is a hash insertion
  per input, negligible beside the 0.3 ms per transaction that re-admission
  already costs (Sequentia #351), and it is done after the disconnect so as
  not to lengthen it. Checking a proposal is one lookup per input.
- **What removes an entry:**
  - the transaction is mined on the active chain;
  - the original branch returns;
  - the transaction becomes invalid for good. A claim is invalid for good
    when its payment is double-spent in Bitcoin's best chain, which the node
    checks with its daemon. A claim whose payment is merely above the anchor
    stays protected;
  - a window ends: 144 Bitcoin blocks, as for the waiting set.
- **What protection costs a seller:** inside the window, a legitimate refund
  is delayed while a claim that conflicts with it may still return.

**7.5 Anchoring stays supreme.** The rule changes only which new blocks a member
signs. The watcher disconnects exactly as now, nothing holds a block whose
anchor Bitcoin replaced, and following a reorganization is not delayed: the set
is filled after the disconnect. A conflicting proposal costs at most one round
before the next leader. Nothing protected can resist a later Bitcoin
reorganization. Every protected transaction is still checked against the new
chain, and one whose Bitcoin facts moved stops being valid there.

**7.6 The contested-anchor jump (proof review KR4, F1).** During a sustained
Bitcoin fork, producers keep recording the shared ancestor. The anchor can then
jump from below the payment to above `R` in one block, and a seller producing
that block mines its refund. A signing rule ("do not certify a refund while
holding a valid claim for the same coin") does not close this, for two
reasons:
- members do not hold the claim before the jump. The mempool refuses it with
  `btcpay-not-in-anchor-chain`, and the waiting set holds only claims
  disconnected by a reorganization;
- a jump of 3 or more lets the jump block escape a stall with one signer, as
  in 7.3.

The fix belongs in the floor, and it is free before activation, since the rule
is active nowhere. Judge an anchor floor not against the block's own anchor but
against the anchor of the block `M` places below it. A refund is then valid only
once `M` blocks anchored at or above `R` have passed, in each of which the claim
was valid and preferred by every honest mempool. A seller who also produces
must then produce all `M` of them, which happens with probability `s^M` for a
seller with a share `s` of the blocks. The check is deterministic, needs no
member's state, and reuses the ancestor search that check 6 already does. A
script cannot do this, because a script is a pure function of the transaction
and cannot read an ancestor's anchor.

## 8. Verdict

| Form | What it closes | What it rests on | Cost |
|---|---|---|---|
| Per-trade lock, no default wait (three opcodes, as specified) | Front-running, races, the seller's self-payment (the claim needs the buyer's key) | No Bitcoin reorganization through the lock's anchor while the seller controls the producer that re-mines it; the opt-in wait covers that for a buyer who wants it | Nothing new. The seller is online for each trade. Needs the KR4 F1 floor fix (7.6) |
| Certification rule (signing policy, no opcode) | The per-trade gap, with both arms of 7.3 | Members holding a quorum honest and aware of the disconnected block: the chain's own safety assumption | Node policy in `DriveRound` and the signing RPCs, a protected set of a few MB persisted across restarts, a refund delayed up to the window. Nothing for wallets; sellers hold nothing; nobody waits. Needs F1's floor fix |
| Bitcoin-coin mutex (fourth opcode), one coin per lot | The gap, drains, races, late payments (no deadline, so no 9.1), filled margins, and F1 (there is no `R`) | Bitcoin's one-spend rule; the slot older than the reorganization | One opcode (free before activation). A coin per lot, which a service can fund. A payer able to build transactions. Cancelling takes one Bitcoin confirmation. Liveness depends on the coin's signer |
| The same, chained (6.1) | As above, with proportional fills | As above | One funded coin per offer; index `-1` (the payment's transaction id); a signer online per fill; throughput bounded by Bitcoin's unconfirmed chains |
| HTLC payment (three opcodes) | Drains, races and late payments, which all become refunds | The buyer reveals its secret only after its reservation is as old as it chooses | The gap moves to after the payment rather than going. Two-step fills; the seller or a watchtower sweeps before `T`; the buyer refunds after `T` |

**In plain words.** The standing offer built from today's opcodes does not
work: the seller takes the buyer's payment by paying itself (SO7). The cheapest
real fix needs no new opcode and no change for anyone's wallet. Committee
members refuse to certify a block that undoes a certified transaction which a
Bitcoin reorganization disconnected, and they refuse to build on such a block.
Making the seller's attack work then takes a majority of the committee, which
is what the whole chain already trusts. It must also cover blocks that escape a
stall, or a single producer still suffices after a reorganization two Bitcoin
blocks deep. The bitcoin-coin mutex is stronger, because it rests on Bitcoin
alone and also ends late-payment losses and the anchor-jump refund. In return
it needs a fourth opcode, a payer able to build transactions, and a signer for
each coin, which a service can be, so the seller holds no bitcoin. For the
wallets now: the per-trade lock with no default wait, the certification rule in
the node, and F1's fix in the floor. The fourth opcode is for the owner to add
before activation if he wants the guarantee to rest on Bitcoin alone.

## 9. The runs

`test/functional/feature_btc_payment_standing_offer.py` uses the pinned
binaries of `f9386f36`, Bitcoin Core 28 as the parent chain, `-par=1` and
`-validateanchor=1`. Every negative case is forced into a block with
`generateblock`.

| Case | What happens | Block refused with |
|---|---|---|
| SO1 | offer made, filled by a real payment, filled again by another | mined, twice |
| SO2 | payment 1 again against the re-created coin (relative) | `OP_RETURN was encountered` |
| SO3 | the payment's `OP_RETURN` names another destination than output 0 | `Script evaluated without error but finished with a false/empty top stack element` |
| SO4 | a completer gives more than `FEEMAX` to the fee | `Script failed an OP_VERIFY operation` |
| SO5 | two honest buyers in one Bitcoin block (relative) | second: `OP_RETURN was encountered` |
| SO6 | two honest buyers in adjacent blocks, first fill mined after both | second: `OP_RETURN was encountered` |
| SO7 | seller pays itself beside the buyer; its fill wins in an honest mempool | buyer's: `bad-txns-inputs-missingorspent`, then `OP_RETURN was encountered` |
| SO8 | absolute floor: one payment by the seller fills three times, then the buyer's cannot | three mined; buyer's: `Script failed an OP_EQUALVERIFY operation` |
| SO9 | one-shot slot taken by the seller's own payment | buyer's: `bad-txns-inputs-missingorspent` |
| SO10 | two filled payments mined again together after a reorganization | second: `OP_RETURN was encountered` |
