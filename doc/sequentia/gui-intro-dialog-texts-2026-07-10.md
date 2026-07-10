# GUI: first-run intro dialog texts fixed and expanded (2026-07-10)

The Qt first-run dialog ("Welcome" / data-directory chooser, `src/qt/intro.cpp`
+ `src/qt/forms/intro.ui`) still carried Bitcoin Core wording and Bitcoin
constants. This change fixes the inherited bugs and rewrites the texts for
Sequentia.

## Bugs fixed

- **"the full 2009 block chain"**: `intro.cpp` kept an orphaned `.arg(2009)`
  from Bitcoin's string (which had a `%3` year placeholder). Our `.ui` string
  had placeholders `%1 %2 %4`, so `2009` landed in `%4` (the chain-name slot)
  and the following `.arg(tr("Sequentia"))` was silently discarded. The whole
  `.arg` chain was rebuilt against new strings; no year, no hardcoded chain
  name.
- **Prune "days of backups" estimate used Bitcoin constants**
  (`nPowTargetSpacing = 600 s`, expected block data 2.25 MB). It now derives
  from the selected chain's params at runtime — `Intro` is only constructed
  after `SelectParams()` (see `Intro::showIfNeeded()`), so `Params()` and
  `g_pos_slot_interval` are available: 30 s slots and the
  `consensus.nMaxBlockWeight = 200,000` WU cap (0 = global
  `MAX_BLOCK_WEIGHT` fallback, matching the enforcement in `validation.cpp`).
  Expected on-disk footprint per block keeps Bitcoin Core's 9/16 byte-per-WU
  ratio → 112,500 B per full block, ~324 MB/day, e.g. a 2 GB prune target
  covers ~6 days.
- **`m_assumed_blockchain_size` for the testnet was 40 GB** — inherited
  verbatim from Bitcoin's testnet3. Set to an honest 2 GB (young,
  lightly used chain; even a saturated chain adds only ~53 GB/year).
  Mainnet's 1 GB was already sensible.
- **Hardcoded "Sequentia"** in the storage warning label: the network name is
  now derived from the selected chain's id (`Sequentia`, `Sequentia testnet`,
  or the raw chain id on regtest/signet/custom chains).

## New texts

- **Main explanation** now states the size ("approximately %3 GB") and, on the
  Sequentia chains only, that this is an independent Bitcoin sidechain — *not*
  a download of the Bitcoin block chain.
- **New growth paragraph** (`lblExplanation1b`), computed at runtime from the
  chain's slot interval and block-weight cap: with 30 s blocks and the
  200,000 WU cap the chain cannot grow more than **~53 GB/year** even fully
  saturated (hard ceiling, `31,557,600 s / 30 s × 50,000 B`), and sustained
  Bitcoin-peak-level usage would add **~24 GB/year** (45% observed fill
  ratio, commented in the code). Formats as GB or TB as appropriate.
- **Pruning explanation** (`lblExplanation3`, shown only when the prune box is
  checked) rewritten: pruning does not reduce security (every block is still
  fully verified); the node stops serving historical blocks, and importing a
  wallet older than the retained window makes the node stop with a clear
  warning and require re-downloading the chain. This matches the actual
  wallet behaviour (`CWallet::AttachChain`, `src/wallet/wallet.cpp`: "Prune:
  last wallet synchronisation goes beyond pruned data. You need to
  -reindex…") — the wallet refuses to load rather than showing a wrong
  balance.
- **Prune checkbox tooltip** rewritten to the same effect.

## Translations

`src/qt/locale/bitcoin_it.ts` updated with Italian translations for all new
and changed strings (the stale upstream-Bitcoin sources it still carried were
replaced). Other languages fall back to the English source strings —
accepted. `bitcoin_en.ts` / `bitcoin_en.xlf` are lupdate-generated and will be
refreshed on the next `make translate`.

## Verification

Qt linguist tools are not available on this Windows machine, so instead of a
full GUI build the change was verified by script: XML validity of `.ui`/`.ts`,
byte-identical match between `.ui`/`tr()` sources and `.ts` `<source>`
entries, placeholder coherence (source vs translation) for every message in
the `Intro` context, and simulation of every `QString::arg` chain with the
real Sequentia parameters (no unresolved placeholders; B renders "53 GB" /
"24 GB" in both languages). **A CI/full Qt build should still compile
`intro.cpp` before release.**

## Reminder

**The installer binaries must be regenerated** (Windows installer and any
other packaged GUI builds) for users to see the corrected first-run dialog —
this change is compiled into `sequentia-qt`, including the `.qm` translation
embedded from `bitcoin_it.ts`.
