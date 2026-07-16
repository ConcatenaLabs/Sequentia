# Note for Andreas & Saba — the public testnet is mining empty blocks

**From:** Alberto (with bubu)
**Date:** 2026-07-16
**Where:** the public testnet (`sequentiatestnet.com`), committee on the box.

---

## What's happening

I issued a real asset (tADLT) from the new Core build to test the issuance flow
end to end. The transaction has sat in the public mempool **unconfirmed for nine
hours**. Looking closer, it is not my transaction — **the network is confirming
nothing but coinbases.**

Evidence from your own explorer API:

- My issuance tx `7ab291f54d1b2bae3be262418090c29642f2f7f8fafd1368434e4ba22446a43d`
  is in the mempool, `status.confirmed = false`, since ~00:39 UTC.
- The chain has advanced ~600 blocks since then (tip height 25619 as I write).
- **Every recent block has `tx_count = 1`** — the coinbase and nothing else.
  I sampled heights 20000, 22000, 24000, 24800, 25000, 25200, 25619: all 1 tx.
  So this is not a spike; the committee has been producing empty blocks for a
  long stretch, possibly always on this chain.
- The public mempool holds **18 transactions**, total vsize ~25.9 kvB, that are
  all stuck. So it is not specific to my asset — real traffic is accumulating and
  none of it is being included.

## The likely cause (for you to confirm on the box)

Every stuck transaction shares one property: a **near-zero fee rate**. The
mempool's own histogram is `[[0.0, 25868]]` — every byte in it is in the
lowest fee bucket. My tx pays 41 atoms of tSEQ for 406 vB ≈ **0.1 atoms/vB**.

Core's block assembler skips transactions below `blockmintxfee`
(`DEFAULT_BLOCK_MIN_TX_FEE = 100`, i.e. 100 atoms/kvB = 0.1 atoms/vB —
`src/policy/policy.h:26`, made configurable by commit `daec955fd` "Introduce
-blockmintxfee"). My fee is right at that boundary, and the others may be below
it. If the committee nodes run with the default (or a higher `-blockmintxfee`),
they will assemble empty blocks whenever the mempool only holds
at-or-below-threshold transactions — which is exactly what the histogram shows.

But that is a hypothesis about **wallet-side fee estimation producing fees too
low for the miner policy**, and the fix is on the box, not in the node source, so
I'm flagging rather than guessing. Worth checking on a committee node:

1. `getblocktemplate` — does it include the mempool txs or drop them? If it drops
   them, it is a policy threshold.
2. The committee's `-blockmintxfee` / `-minrelaytxfee` settings versus what the
   wallet actually attaches. The wallet defaults are
   `DEFAULT_TRANSACTION_MINFEE = 100`, `DEFAULT_PAY_TX_FEE = 0`,
   `DEFAULT_FALLBACK_FEE = 0` (`src/wallet/wallet.h:77-83`) — with no fallback and
   any-asset fees valued through the price server, an under-estimate is very
   plausible.
3. Whether the price the fee estimator uses for tSEQ lines up with what the miner
   expects. The price server does serve SEQ (`/prices` returns
   `SEQ: {price: 0.0515…}`), so estimation has an input; the question is whether
   the resulting atom-denominated fee clears `blockmintxfee`.

## Why it matters beyond my test

This blocks the whole issuance demo: an issuer can create an asset, publish the
proof and press Register, but if the issuance never confirms, the registry's
on-chain check fails and the asset never gets its name. The last mile of
everything we built this week runs through a transaction actually getting mined.

## The good news, separately

The issuance itself is correct. The contract Core committed on chain hashes to
exactly what it saved — `94a8aecb8ac9a3b8c65597d17354186d87386467513e9c750df6283040da1294`,
byte-for-byte, in the right order — so the moment a block includes it and the
proof is up, tADLT will verify cleanly. It is purely stuck at the mempool, not
malformed.

Could you check the committee's fee policy on the box, and whether the assembler
is dropping the mempool on a threshold? If the demo needs a lower
`-blockmintxfee` on the committee to match what the GUI attaches, that is a
one-line change on your side — but you should decide it, since it is your call
what the testnet's fee floor should be.

— Alberto
