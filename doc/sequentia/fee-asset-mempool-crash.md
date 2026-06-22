# Fee-asset mempool crash (`RecomputeFees` / `m_total_fee`)

## Symptom

With the open fee market enabled (`con_any_asset_fees`), a transaction that pays
its fee in a non-policy asset (e.g. USDX) was accepted into the mempool, then —
within ~30 s, around the next rate refresh / block — the node became briefly
unreachable and the transaction "totally disappeared": not mined, not in the
mempool. Observed on the explorer-backend node (where the web wallet broadcasts).

## Root cause

The node was **crashing (SIGABRT, core-dump)** and being restarted by systemd
(`seq-explorer-node.service` had `NRestarts=9`, every exit `code=dumped,
status=6/ABRT`). The exact assertion:

```
txmempool.cpp:941: void CTxMemPool::check(...) const:
  Assertion `m_total_fee.GetValue() == check_total_fee.GetValue()' failed.
```

`m_total_fee` is the mempool-wide running total of every entry's fee **value**
(the reference-unit valuation used for fee-asset txs). It is adjusted only when an
entry is **added** (`m_total_fee += entry.GetFeeValue()`) or **removed**
(`m_total_fee -= it->GetFeeValue()`).

The open-fee-market code re-values mempool fees whenever exchange rates change, in
`CTxMemPool::RecomputeFees()` (called on every `setdynamicfeerates` push — the
price server publishes every 30 s). For each entry it updates the per-entry fee
value via `UpdateFeeValue()` and propagates the delta to ancestor/descendant
aggregates — **but it never adjusted `m_total_fee`.** So after the first rate
refresh while a fee-asset tx sat in the mempool, `m_total_fee` was stale, and the
next periodic `CheckMempool()` consistency check (enabled via `-checkmempool`)
re-derived `check_total_fee` from the new per-entry values, mismatched, and
`assert`ed — aborting the node.

Because the abort is abnormal, `DumpMempool()` does not run, so on restart
`mempool.dat` does not contain the tx (`Imported … 0 succeeded`) and it is lost —
hence "disappeared." The restart also wipes the in-memory dynamic rates (they are
ephemeral, re-pushed every 30 s), which is what made `esplora`/electrs briefly
unreachable.

## Fix

`src/txmempool.cpp`, in `RecomputeFees()`, keep the running total in sync with the
per-entry fee-value change (one line, alongside the existing `mapTx.modify(...,
update_fee_value(...))`):

```cpp
mapTx.modify(it, update_fee_value(newFeeValue));
m_total_fee += feeValueDelta;   // SEQUENTIA: m_total_fee is otherwise only adjusted
                                // on add/remove; without this the next CheckMempool()
                                // aborts on m_total_fee == sum(GetFeeValue()).
```

Evicted entries (`newFeeValue <= 0`, removed via `removeRecursive`) already update
`m_total_fee` through the normal removal path, so only the *updated* (delta ≠ 0)
entries needed this adjustment.

## Verification

After rebuilding `elementsd` and restarting the explorer node, a fee-in-USDX tx
was pushed and monitored for 75 s spanning multiple 30 s rate pushes and 3 block
connects:

```
t=5s  tx=IN_MEMPOOL  NRestarts=0  height=2302
...
t=75s tx=IN_MEMPOOL  NRestarts=0  height=2304
```

No crash (`NRestarts` stayed 0), and the transaction survived every rate refresh
and block connect — versus a SIGABRT within ~30 s before the fix.

## Notes

- **Rate durability is intentionally not persisted.** Dynamic fee-asset rates are
  ephemeral (the price server is the source of truth, publishing every 30 s; see
  `02-open-fee-market.md`). Persisting them would reload stale data. With the crash
  fixed the node no longer restarts, so rates stay populated; a *clean* operator
  restart has a ≤30 s window before the next push, after which fee values are
  corrected (now without crashing). Reduce `poll_interval_secs` in the price-server
  config if faster post-restart recovery is wanted.
- **Producer rollout (done).** The same crash affects any node that holds a fee-asset
  tx across a rate refresh, so the fix was built and the committee restarted onto it
  (block production resumed normally). Both the explorer node and the producers now run
  the fixed binary.
- **Relay gap (fixed).** The PoS committee mesh does not relay externally-submitted
  transactions to producers, so a tx that only reached the explorer node's mempool was
  never mined (it showed `"unbroadcast": true`); the workaround was to `sendrawtransaction`
  it to a miner by hand. `explorer/serve-public.js` now intercepts `POST /api/tx` and
  forwards the raw tx straight to a producer (which accepts, mines and relays it) plus the
  explorer node (immediate electrs indexing), returning the txid like esplora. Verified:
  a fee-in-USDX tx broadcast through the normal web-wallet path was mined without any
  manual step (recipient received the asset, confirmed on-chain). BTC (`/testnet4/api/tx`)
  is untouched — it relays on the real testnet4 network.
