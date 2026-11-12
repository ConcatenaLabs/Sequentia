# A standing offer filled by a payment

This note belongs beside [`bitcoin-payment-proof.md`](bitcoin-payment-proof.md)
and uses its terms: the payment proof, the coin anchor `C`, the absolute and
relative floors (section 4.5), the per-trade lock (section 8.1) and the open
form's binding (section 8.4). It asks whether a swap for bitcoin can run with
no wait at any amount, built from the three payment opcodes as they are and
from the tapscript opcodes the chain already has, with no new rule. It
answers no, and names the one rule that would make the answer yes.

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

### 6.1 The opcode: a reading, not a proof

**`OP_INSPECTBTCINPUT`**, on the next free `OP_SUCCESS` slot (`0xe8`), under
`SCRIPT_VERIFY_BTCPAYMENT` and from the same activation height as the other
three. It pops an input index and pushes that input's previous output from the
proven payment: the transaction id (32 bytes, in the order the transaction
serializes it), then the output index as a script number. It fails without a
payment proof (`SCRIPT_ERR_BTCPAY_MISSING`) and for an index out of range (a
new `SCRIPT_ERR_BTCPAY_INPUT`). Reading it counts as reading the proof for the
relay rule of section 6.1 of the specification.

It reads a record that is already checked, and adds no proof. The type `0x01`
record already carries the whole payment without its witness, and the
interpreter already parses it into a Bitcoin transaction (`proof.tx` in
`src/script/interpreter.cpp`, the object `OP_INSPECTBTCOUTPUT` reads
`vout` from). Checks 1 to 4 tie those bytes, inputs included, to the header's
merkle root, and checks 5 to 7 tie the header to the anchor chain. The opcode
reads `proof.tx->vin[i].prevout` from the same object. It does not check that
the input was unspent or properly signed. Bitcoin did that when it mined the
block, which is the same trust the proof already places in Bitcoin's blocks.

### 6.2 The slot, and who decides its fate

The maker makes, for each lot:

- **on Bitcoin, a coin `U`** of a few hundred sat to a taproot key of its own,
  and a key-path signature over spending `U` with
  `SIGHASH_SINGLE|ANYONECANPAY`. Under that signature, output `i` of the
  spending transaction, where `i` is `U`'s input index, must pay the price to a
  receiving script `S` used for this slot alone. The maker publishes the
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
fill can open. If the maker spends `U` itself to cancel, only the take-back can
open. A Bitcoin history holds one spend of `U`, so on any branch of Sequentia
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

### 6.3 What each side can lose, and to whom

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
  its node's chain that spends `U` and pays it the price. A stale quote (6.5)
  is the one cost it carries.
- **Two honest buyers.** Both build a spend of `U`, and Bitcoin mines one. The
  other transaction never confirms, and that buyer spent nothing. A third party
  who copies a buyer's spend with a higher fee buys the slot itself and pays
  the price itself, so the buyer loses only the attempt.
- **A reorganization at each step (9.8).** Before `U` is spent, nothing is at
  stake. With the payment unconfirmed, the maker's cancel can win, and then
  the buyer spent nothing. After the payment, as above. After a take-back, the
  take-back stands or falls with the maker's own spend of `U`.

### 6.4 What the buyer's wallet must build

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

### 6.5 The maker's side

- **For each lot:** one Bitcoin output, about 43 virtual bytes to create in a
  batch, holding a few hundred sat. That value passes to the buyer, so the
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
  takes one at once carries the per-trade gap of 6.3 until the slot is old.
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

### 6.6 Before activation or after

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

## 7. Verdict

The standing offer does not close the gap; it widens it. With the per-trade lock
and no wait, a buyer can lose its payment only if Bitcoin reorganizes the very
block the seller's lock is anchored to and the seller also runs a block
producer that drops the lock. With a standing offer, the seller takes the
buyer's payment whenever it likes: it pays itself in the same Bitcoin block and
its claim wins on fee, with no reorganization and no producer (run on regtest,
SO7). Even an honest seller's offer turns away honest buyers who pay close
together, and their bitcoin is gone (SO5, SO6, SO10). What it would cost, for
nothing: a fill of about 400 virtual bytes plus the proof, and for the maker a
wait of about two hours to reprice, a wait that holds nobody. So the wallets
should use the per-trade lock, and by default pay with no wait at any amount. A
wait for Bitcoin confirmations stays available as a setting, off by default,
for a buyer who wants to cover the one remaining case. Closing that case
without any wait would take one new opcode that reads the inputs of the proven
bitcoin payment (section 6). Bitcoin's own rule that a coin is spent once then
decides between buyers. A buyer could then lose a payment only to a
reorganization deeper than the offer is old, there would be no deadline and so
no late payment lost, and the maker could cancel within one Bitcoin
confirmation. The cost is that the buyer's wallet must build the payment around
the maker's signed input, so a payer that can only send to an address keeps
the per-trade lock.

## 8. The runs

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
