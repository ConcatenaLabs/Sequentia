# Challenge 2 — Bitcoin anchoring

**Goal.** Every Sequentia block references a Bitcoin block. The referenced
Bitcoin height is **monotonically non-decreasing** across the Sequentia chain. If
Bitcoin reorgs away a referenced block, Sequentia reorgs the dependent blocks;
otherwise Sequentia has **immediate finality**. This enables real-time
cross-chain atomic swaps with no extra reorg-protection timelocks.

> **Status: implemented** (`src/anchor.{h,cpp}`, header fields
> `m_anchor_height`/`m_anchor_hash`, validation in `ContextualCheckBlockHeader`,
> the `getanchorstatus` RPC; options `-con_bitcoin_anchor` / `-validateanchor` /
> `-anchorminconf` / `-anchorpollinterval`; tested in
> `feature_bitcoin_anchoring.py` and `feature_anchor_swap_consistency.py`).
> The text below is the original design; where it names a recency window
> (`-anchormaxlag`/`-anchormaxlead`) or an `-anchorbtc` switch, those were
> folded into the options above or deferred — see doc 05, Milestone 3 item 6.

## 1. The anchor datum

Each Sequentia block commits to:

```
struct BitcoinAnchor {
    uint32_t btc_height;     // height of the referenced Bitcoin block
    uint256  btc_blockhash;  // its header hash (double-SHA256, on Bitcoin's best chain)
};
```

`btc_blockhash` is the security-bearing field (heights are not unique across
forks); `btc_height` is carried for cheap monotonicity checks and indexing, and
must be validated to equal the real height of `btc_blockhash` on the connected
bitcoind.

## 2. Where the anchor lives — header vs. coinbase

Two options:

1. **In the block header** (recommended). Add `btc_height` + `btc_blockhash` to
   `CBlockHeader`, behind a new global flag `g_con_bitcoin_anchor`, serialized in
   the hashed region of **both** the dynafed and non-dynafed branches (see doc 01
   §4). *Pros:* the federation signs over the anchor (it's inside `SER_GETHASH`);
   light clients see it without the coinbase; matches the conceptual model
   ("blocks anchor to blocks"). *Cons:* a header-format change ⇒ new genesis /
   fresh chain (acceptable: PoC is a new network).
2. **In the coinbase** (an `OP_RETURN` commitment, like merge-mining tags /
   witness commitment). *Pros:* no header-format change. *Cons:* not signed as a
   first-class header field; every validator must parse the coinbase; weaker fit
   with "header references header".

**Decision: header field behind `g_con_bitcoin_anchor`.** Because the PoC is a
brand-new chain we are free to change the header; doing it properly (signed,
first-class) is cleaner than a coinbase commitment.

### Serialization placement

In `CBlockHeader::Serialize/Unserialize`, emit the two fields immediately after
`block_height` and before the `proof`/`nBits` branch, in **both** the dynafed and
legacy paths, guarded by `g_con_bitcoin_anchor`. Keep them inside the
`SER_GETHASH` region so they are committed to and signed; they are **not** part of
`m_signblock_witness`. Bump the chain's genesis accordingly. Mirror the field in
`CBlockIndex` (`src/chain.h`) and its (de)serialization so monotonicity checks
don't require re-reading the block.

## 3. Validation rules

### R1 — Anchor well-formedness (`CheckBlockHeader`, context-free)
- Fields present iff `g_con_bitcoin_anchor`.
- `btc_blockhash != 0`.

### R2 — Monotonicity (`ContextualCheckBlockHeader`, needs parent)
- `btc_height(this) >= btc_height(parent)`.
- Genesis defines the initial anchor (chainparams).

### R3 — Bitcoin existence & best-chain membership (contextual, needs bitcoind)
- Query the connected Bitcoin node (doc 01 §5): the header `btc_blockhash` must
  exist **and** `getblockheader(btc_blockhash).height == btc_height` **and** it is
  on bitcoind's active chain (`confirmations >= 0`, i.e. not on a stale branch).
- Enforce a **maturity/recency window**: reject anchors whose height is implausibly
  far ahead of, or behind, the validator's view of Bitcoin's tip
  (`-anchormaxlag` / `-anchormaxlead` — **deferred**, see the status note above), bounding how stale the chain may get
  (the paper's cross-chain-consistency concern, principle 7) and rejecting
  anchors to not-yet-seen Bitcoin blocks.

### R4 — Reference-tip availability for *block production*
- A producer sets the anchor to the **most recent Bitcoin block its bitcoind has**
  that satisfies maturity, and never below the parent's anchor (preserving R2).

## 4. Reorg-following — the core behaviour

Sequentia must reorg when Bitcoin reorgs away a referenced block. Mechanism:

1. **Track Bitcoin's tip.** A background watcher polls bitcoind
   (`getblockchaininfo` / `getbestblockhash`, or ZMQ `hashblock` if available)
   for new tips and reorgs. Reuse the `mainchainrpc` transport.
2. **Map anchors → Bitcoin blocks.** Maintain an index from each Sequentia
   `CBlockIndex` to its `(btc_height, btc_blockhash)` (already stored per R2/§2).
3. **On a Bitcoin reorg,** determine the set of Bitcoin block hashes that left the
   best chain. Any Sequentia block whose `btc_blockhash` is now stale (no longer on
   bitcoind's active chain) is **invalid**. Invalidate the **lowest-height** such
   Sequentia block via the existing `InvalidateBlock` path; Elements'
   `ActivateBestChain` then disconnects it and all descendants, and the federation
   resumes building from the last still-valid Sequentia block (whose anchor is
   still canonical on Bitcoin).
4. **Re-validation.** Disconnected Sequentia transactions return to the mempool
   per existing logic; blocks are rebuilt with anchors to the new Bitcoin best
   chain.

Because honest producers only anchor to Bitcoin blocks they consider buried-enough
(R3 maturity window), routine 1-block Bitcoin reorgs need not cascade if the
window is set conservatively; the operator tunes the window to trade finality
latency against reorg exposure. **As long as Bitcoin does not reorg the referenced
blocks, no Sequentia reorg is possible** (signed blocks give immediate finality),
which is precisely the paper's principle 6 + principle 5 guarantee.

### Interaction with consensus finality

With strong-federation signed blocks, Sequentia blocks are final on signature.
The *only* sanctioned reason to discard a signed Sequentia block is R3 failing
because Bitcoin reorged its anchor away. This must be encoded as a **consensus**
invalidation (all nodes following the same bitcoind-confirmed best chain reach the
same decision), not a local policy choice — so the watcher feeds
`InvalidateBlock`, and acceptance of a *replacement* block at the same Sequentia
height is gated on its parent anchor having become stale. Nodes connected to
honest bitcoind instances converge because Bitcoin's best chain is the shared
oracle.

### Anchor-freshness fork choice (PoS, real-time swaps)

Reorg-following gives the swap *safety* (a Sequentia leg can never outlive the
Bitcoin block it anchors). For real-time, **timelock-free** swaps we also want
the canonical Sequentia tip to track Bitcoin's tip with minimal *latency*, so a
swap's Sequentia leg confirms with `anchor_height >= the Bitcoin leg's height`
promptly. Under PoS, `CBlockIndexWorkComparator` (`src/validation.cpp`)
therefore prefers, among **equally-certified same-height** blocks, the one
referencing the **fresher (higher) Bitcoin anchor** (then the lower leader VRF
score). This is ordered *after* the countersignature key, so it never displaces
a finalized full-threshold block; it only resolves ties toward Bitcoin's tip. A
newly-arrived Bitcoin block is picked up within one Sequentia block (~1 slot),
and referencing a *stale* Bitcoin block can only lose — so there is no
proposer-grinding incentive. See doc 10 §7; tested in
`feature_pos_anchor_freshness.py`.

## 5. Node configuration

- Make the Bitcoin connection **mandatory** when `g_con_bitcoin_anchor` (today
  `-validatepegin` defaults on only when `has_parent_chain`). New settings:
  as implemented: `-con_bitcoin_anchor=1`, reusing `-mainchainrpc*` for the
  connection, plus `-validateanchor` / `-anchorminconf` / `-anchorpollinterval`
  (the designed `-anchormaxlag` / `-anchormaxlead` window is deferred).
- Startup probe analogous to `MainchainRPCCheck()`: refuse to start (or run
  validation-only) if anchoring is required but bitcoind is unreachable.

## 6. Failure modes & answers

| Failure | Behaviour |
|---|---|
| bitcoind unreachable at startup | Refuse to produce blocks; optionally header-validate with cached data; loud warning. |
| bitcoind behind the network | Anchors look "ahead"; R3 lead-window rejects until local bitcoind catches up — fail safe, no bad accept. |
| Deep Bitcoin reorg past maturity window | Cascade invalidation of dependent Sequentia blocks; expected and correct (the swaps those blocks protected are also gone on Bitcoin). |
| Producer anchors too far in the past (fee-securing incentive, paper principle 7) | Bounded by anchor monotonicity and R3 (the designed `-anchormaxlag` window is deferred); PoS adds incentive weighting. For the federated PoC, a policy "anchor to tip-minus-`anchorminconf`" suffices. |

## 7. Test strategy

- Unit: header (de)serialization round-trip with/without `g_con_bitcoin_anchor`;
  `CBlockIndex` anchor persistence; monotonicity predicate.
- Functional with a **regtest bitcoind** (the Elements functional framework can
  already spin one up for peg-in tests):
  - happy path: Sequentia blocks anchor to advancing regtest BTC tips; heights
    monotonic; chain finalises.
  - reorg: invalidate a regtest BTC block (`invalidateblock`), mine a competing
    longer BTC branch, assert Sequentia invalidates exactly the dependent blocks
    and rebuilds.
  - guard: reject a block whose anchor height < parent's; reject an anchor to a
    non-existent / stale BTC hash; reject anchors outside the lag/lead window.
