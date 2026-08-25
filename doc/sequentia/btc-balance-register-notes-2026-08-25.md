# Notes for the BTC-balance rewrite

*For Alberto and the team working with him on replacing how Sequentia Core sees
its Bitcoin balance, 2026-08-25.*

These are corroboration and integration notes, not a review — I have not seen
the new code. They come out of a night spent in the parent-chain RPC path for an
unrelated reason (the anchor watcher), which happens to be the same road.

## The current implementation is worse than "it rescans"

The diagnosis was that bitcoind does not hold these coins in one of its own
addresses, so it goes looking. That is right, and the specific call makes it
sharper: `getbtcbalance` runs **`scantxoutset`** (`src/wallet/rpc/spend.cpp`), and
so does the send path before building a transaction.

That is not a wallet rescan. It is a sweep of Bitcoin's **entire UTXO set**, and
the difference decides how it scales:

- a rescan is O(chain since the wallet's birthday), so a new wallet is cheap;
- `scantxoutset` is O(current UTXO set) — around 180 million outputs on mainnet
  — **every call, regardless of how new or how small the wallet is**. A wallet
  created this morning with one address pays exactly what a decade-old one pays.

Two sweeps per payment, one to show the balance and one to spend, is the
"20 seconds to scan, then 20 seconds to process" that started this.

It also cannot see unconfirmed outputs at all; the code says so in a comment
beside the call. So incoming Bitcoin is invisible until it confirms, which is a
second "is this broken?" moment on top of the freeze.

**There is no cache that fixes this.** A cached whole-set scan cannot be both
fast and correct, because knowing whether the answer changed costs another scan.
That is why this is a rewrite rather than an optimisation — the conclusion is
forced, not a preference.

## A local register is the only thing that scales

Index the wallet's own keys from block data once, then maintain the set
incrementally as blocks arrive. Balance becomes a lookup in local state instead
of a question put to a daemon that does not know your keys. Every wallet works
this way, and it is the only shape whose cost does not grow with somebody else's
UTXO set.

## Four things from this side of the code

**1. Batching will not help the balance itself — but it will help the sync.**
One `scantxoutset` is a single round trip whose cost is inside bitcoind, so the
batching added in 24.7.5 does nothing for it. What that batching does address is
the other half of the tax: `CallMainChainRPC` opens a **fresh TCP connection and
re-authenticates for every call**. Invisible at one call, ruinous at thousands —
it is what made the anchor walk take 296 seconds where it now takes 48. If the
register pulls blocks or headers over the parent RPC while catching up, it will
meet the same wall. `CallMainChainRPCBatch` (`src/mainchainrpc.h`) sends many
calls in one request; replies come back matched by id, never by position.

**2. Please reuse the reorg detection that is already here.**
A UTXO register is local state derived from the parent chain, so a Bitcoin reorg
can un-confirm entries that are already in it, and the register has to roll back.
This node **already** watches the parent for exactly that: the anchor watcher
runs every tick and computes, in one RPC, the height below which the parent's
chain provably has not changed (`MainchainUnchangedHeight`, `src/anchor.cpp`).

Hooking the register's rollback to that signal is better than giving it its own,
and not only to save code: two reorg oracles in one process can disagree, and the
window where they do is precisely a reorg — the moment when being wrong about
Bitcoin is most expensive. Happy to expose it as a small interface if that helps.

**3. The keys have to be the same keys.**
The dual-chain wallet shares **one** address: the unblinded bech32 address is the
Bitcoin address, byte for byte. So the register must be keyed off the same
descriptors the Sequentia wallet already derives — which is what the present code
hands to `scantxoutset` — and not a separate Bitcoin keychain. "Like any other
asset in the Sequentia wallet" sounds like exactly that, so this is only written
down because it is the property that would break quietly rather than loudly.

**4. Mempool visibility comes free.**
A register fed from blocks and the mempool can show incoming Bitcoin the moment
it is seen, which the current call can never do.

## What moved under you in 24.7.5

Deliberately kept out of the way, but worth knowing:

- `CallMainChainRPC` keeps its signature and behaviour exactly. What changed is
  internal: the transport, authentication and error handling moved into a shared
  static helper so the single-call and batch paths cannot drift apart.
- `CallMainChainRPCBatch` is new and additive.
- `src/anchor.cpp` gained shutdown checks (24.7.3) and the batched prefetch.

If a merge conflicts in `mainchainrpc.cpp`, it will be that extraction, and the
resolution should be to keep both paths on one transport helper rather than to
restore two copies of it.
