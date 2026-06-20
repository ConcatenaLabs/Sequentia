# Sequentia testnet (chain=test) — local runbook

Stand up the **genuine Sequentia testnet** locally: `chain=test` (CTestNetParams) —
the real baked genesis founder, the **autonomous BLS committee on by default**, 30-second
slots, ~15-day unbonding, the 40,000-SEQ stake floor — bootstrapped from the founder via
escaping stall and anchored to a **local Bitcoin testnet4 node**.

This is the real branded chain, not the `elementsregtest` custom chain used for fast
experiments. The autonomous committee is now the default certification model on `chain=test`
and `chain=sequentia` (`-posbls`); the manual MuSig2 coordinator is the `-posbls=0` fallback.

Tool: [`contrib/sequentia/bootstrap-autonomous-testnet.py`](../../../contrib/sequentia/bootstrap-autonomous-testnet.py)
with `--chain test`.

## Prerequisites (already satisfied on this machine)

- A built `src/elementsd` / `src/elements-cli` containing the latest consensus + launcher
  changes. Rebuild if in doubt: `make -j$(nproc)` (no sudo). Sanity:
  `strings src/elementsd | grep -q posdebugroundskewms && echo ok`.
- A local Bitcoin **testnet4** node, RPC reachable. Config (`~/.bitcoin/bitcoin.conf`):
  `testnet4=1`, `[testnet4]` `rpcport=48332`, `rpcuser=seq`, `rpcpassword=seq`,
  `rpcthreads=64`, `rpcworkqueue=8192`. Start: `~/bitcoin-28.0/bin/bitcoind -daemon`.
  testnet4 genesis: `00000000da84f2bafbbc53dee25a72ae507ff4914b867c565be350b0da8bf043`.

## Step 0 — Free resources (if an old run is up)

A large committee plus 100+ daemons is memory-heavy. Stop any prior run first:

```bash
# stop the old elementsregtest 100-node run, if present:
python3 ~/SequentiaByClaude/contrib/sequentia/bootstrap-autonomous-testnet.py --stop --basedir ~/seq-bootstrap100 2>/dev/null
pgrep -c elementsd        # confirm it drops
```

## Step 1 — Self-contained smoke (~1 min, no testnet4 needed)

Validates the whole `chain=test` flow with a throwaway parent it advances on demand:

```bash
cd ~/SequentiaByClaude
ulimit -n 65535
python3 contrib/sequentia/bootstrap-autonomous-testnet.py \
  --chain test --local-parent --nodes 5 --run-seconds 200 \
  --basedir /tmp/seq-testnet-smoke
rm -rf /tmp/seq-testnet-smoke
```

Success: Phase 1 "founder registered from genesis stake: True", Phase 2 a registration tx,
Phase 3 "block 1 ... countersignatures=1", then `height min=N max=N fork=no anchor=ok@btcN`
climbing (autonomous BLS certification on chain=test).

## Step 2 — The real run: chain=test anchored to local Bitcoin testnet4

```bash
cd ~/SequentiaByClaude
ulimit -n 65535
nohup python3 contrib/sequentia/bootstrap-autonomous-testnet.py \
  --chain test --nodes 10 --run-seconds 0 \
  --basedir ~/seq-testnet \
  --parent-rpchost 127.0.0.1 --parent-rpcport 48332 \
  --parent-rpcuser seq --parent-rpcpassword seq \
  --parent-genesis 00000000da84f2bafbbc53dee25a72ae507ff4914b867c565be350b0da8bf043 \
  > ~/seq-testnet.log 2>&1 &
echo $! > ~/seq-testnet.pid
tail -f ~/seq-testnet.log
```

Notes:
- `--nodes 10` = founder + 9 registered stakers, quorum 6. Raise toward `--nodes 100` for
  the full 51-of-100 committee (the hard cap is 100). Each member is staked 50,000 SEQ from
  the founder's genesis remainder.
- **30-second slots** (baked on chain=test) — blocks are ~30s apart; be patient.
- **Phase 3 waits for Bitcoin testnet4 to advance ≥3 blocks** (the escaping-stall rule) before
  the founder can mine the registration block — ~30 min on real testnet4, variable. The log
  prints "waiting for the Bitcoin parent to advance >= 3 blocks". This is expected, not a hang.
- Keys are saved to `~/seq-testnet/committee_keys.json`. The founder is the testnet placeholder
  (`cURsyjY6…`); regenerate genesis for a real public launch.
- `--run-seconds 0` runs until you stop it.

What success looks like (after Phase 3): the log climbs
`height min=2 max=2 fork=no anchor=ok@btc<testnet4 height>`, the committee certifying full
blocks autonomously with no coordinator.

## Step 3 — Watch / inspect (second terminal)

```bash
B="$HOME/SequentiaByClaude/src/elements-cli -chain=test -rpcuser=seq -rpcpassword=seq"
D=$HOME/seq-testnet
q(){ $B -datadir=$D/node$1 -rpcport=$((18200+10#$1)) "${@:2}" 2>&1; }
for n in 000 001 005 009; do echo -n "node$n: "; q $n getblockcount; done
q 000 getanchorstatus
q 000 getstakerinfo | grep -c ':'      # registered staker count
```

## Step 4 — Stop / clean up

```bash
kill -INT $(cat ~/seq-testnet.pid); sleep 5
python3 ~/SequentiaByClaude/contrib/sequentia/bootstrap-autonomous-testnet.py --stop --basedir ~/seq-testnet
rm -rf ~/seq-testnet ~/seq-testnet.log ~/seq-testnet.pid
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| Phase 3 sits at "waiting … advance >= 3 blocks" | Normal on testnet4. Confirm it is mining: `~/bitcoin-28.0/bin/bitcoin-cli -testnet4 getblockcount` rising. |
| Phase 2 "registration tx rejected" | Stale binary — `make -j$(nproc)` and retry. |
| `--nodes must be between 1 and 100` | Committee hard cap is 100; lower `--nodes`. |
| `min`/`max` heights diverge and stay split | A fork (would indicate a stale binary on some node). On one machine you should never see it. |
| Founder not registered / can't anchor | bitcoind testnet4 not reachable on 127.0.0.1:48332 with seq/seq. |

## Faithfulness notes

- `chain=test` uses the real testnet consensus: autonomous BLS committee, anchored to Bitcoin,
  escaping-stall bootstrap, 30s cadence, 40k-SEQ floor, ~15-day unbonding — identical in shape
  to `chain=sequentia` mainnet (which differs only in network magic, the founder/genesis seed,
  and that its committee size is fixed at 100).
- `-posbls` is a **network-wide consensus rule**, not a per-node toggle: every node and producer
  on the chain must agree, or a node set to `-posbls=0` will fork off the BLS chain.
