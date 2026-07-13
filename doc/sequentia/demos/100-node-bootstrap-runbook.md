# 100-node mainnet-style bootstrap runbook

> **STATUS (2026-07-08):** historical demo record. It predates the 2026-07-05
> re-genesis (public fixed-size committee; Bitcoin **testnet4** parent) and
> describes a 100-node threshold-sortition bring-up anchored to testnet3. The
> tool it drives (`contrib/sequentia/bootstrap-autonomous-testnet.py`) is
> current; see `doc/sequentia/05-operating-sequentia.md` for today's flags.

Stand up a **100-member autonomous Sequentia committee the way mainnet launches**:
from a single genesis founder, through escaping-stall solo-mining, to a
self-running quorum — anchored to **live Bitcoin testnet3** at **mainnet cadence**
(30-second slots, ~15-day unbonding). Everything here is exact and assumes a
single Linux box (Pop!_OS/Ubuntu).

Tool: [`contrib/sequentia/bootstrap-autonomous-testnet.py`](../../../contrib/sequentia/bootstrap-autonomous-testnet.py).

## What "mainnet-style" means here (read once)

This reproduces the real launch **economics and liveness**: one founder seeded in
genesis (`-con_genesis_stake`), funding and registering the other 99 stakers
on-chain, gated by the escaping-stall rule (a sub-quorum block needs Bitcoin to
advance ≥ 3 blocks), then a quorum forming and certifying autonomously. The
bundled `chain=sequentia`/`chain=test` mainnet params **default to BLS autonomous
gossip** (`-posbls=true`, the gossip-and-sign layer that lets 100 nodes self-run),
so the 100-node bootstrap works on the bundled path as well as the Elements
**custom-chain** path. Consensus rules, anchoring, cadence, unbonding, stake floor
and bootstrap flow are identical across both.

## The four phases you will watch

| Phase | What happens |
|---|---|
| 1 | Genesis seeds **one** staker (the founder) + a spendable `initialfreecoins` output. No `-staker` config. |
| 2 | The founder spends `initialfreecoins` into one CSV-locked staking output per new staker — funding+registering all 99 on-chain in one tx. |
| 3 | The founder **solo-mines ONE escaping-stall block** to confirm them. Against real testnet this **waits for Bitcoin to advance ≥ 3 blocks** (~30 min, variable) — the mainnet liveness gate doing its job. |
| 4 | The 99 staker nodes come online; a quorum (51) now exists, so the committee certifies full blocks **autonomously** over BLS gossip. The founder stops solo-mining. |

---

## Step 0 — No tmux needed

`tmux` isn't installed and you don't need it — use `nohup`, which is built in. The
run survives closing the terminal; you watch via the log.

If you already started a run (a `~/seq-bootstrap100` dir exists, or a previous
attempt is mid-flight), clear it first:

```bash
# stop the orchestrator if it's running, kill stray daemons, remove the dir:
pkill -f bootstrap-autonomous-testnet.py 2>/dev/null
python3 ~/Sequentia/contrib/sequentia/bootstrap-autonomous-testnet.py \
  --stop --basedir ~/seq-bootstrap100 2>/dev/null
pkill -9 elementsd 2>/dev/null
rm -rf ~/seq-bootstrap100
```

---

## Step 1 — Latest code, build green

```bash
cd ~/Sequentia
sudo chown -R $USER:$USER ~/Sequentia   # undo any earlier `sudo make`
git pull origin master
make -j$(nproc)                                  # NO sudo
strings src/elementsd | grep posdebugroundskewms # must print the knob
ls contrib/sequentia/bootstrap-autonomous-testnet.py
```

---

## Step 2 — Start + verify your Bitcoin-testnet proxy

```bash
python3 ~/seq-rpc-proxy.py          # own terminal, leave running

export PHOST=127.0.0.1
export PPORT=18332                   # match your proxy
export PUSER=seq
export PPASS=seq

# must return the current testnet height (~4.4M) and the testnet3 genesis:
curl -s --user $PUSER:$PPASS --data-binary \
  '{"jsonrpc":"1.0","id":"t","method":"getblockcount","params":[]}' \
  -H 'content-type: text/plain;' http://$PHOST:$PPORT/ ; echo
curl -s --user $PUSER:$PPASS --data-binary \
  '{"jsonrpc":"1.0","id":"t","method":"getblockhash","params":[0]}' \
  -H 'content-type: text/plain;' http://$PHOST:$PPORT/ ; echo
```
The genesis must be `000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943`.
Note the height — Phase 3 unblocks at height + 3.

---

## Step 3 — Smoke A: full flow, self-contained, ~1 min

Validates the whole bootstrap with a throwaway parent it advances on demand:

```bash
ulimit -n 65535
cd ~/Sequentia
python3 contrib/sequentia/bootstrap-autonomous-testnet.py \
  --local-parent --nodes 5 --slot 2 --run-seconds 60 \
  --basedir /tmp/seq-bootA
rm -rf /tmp/seq-bootA
```
Pass = all four phases print, block 1 shows `countersignatures=1`, then
`height min=N max=N fork=no anchor=ok@btc...` with min==max.

---

## Step 4 — Smoke B: real testnet, small (5 nodes)

Same flow against live testnet; **Phase 3 will wait** for testnet + 3.

```bash
ulimit -n 65535
cd ~/Sequentia
nohup python3 contrib/sequentia/bootstrap-autonomous-testnet.py \
  --nodes 5 --slot 10 --anchorpoll 15 \
  --basedir ~/seq-bootB \
  --parent-rpchost $PHOST --parent-rpcport $PPORT \
  --parent-rpcuser $PUSER --parent-rpcpassword $PPASS \
  --parent-genesis 000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943 \
  > ~/seq-bootB.log 2>&1 &
echo $! > ~/seq-bootB.pid
tail -f ~/seq-bootB.log         # Ctrl-C stops watching, not the run
```
Expect Phases 1-2 instant, Phase 3 sitting at "waiting … advance >= 3 blocks",
then block 1 and autonomous production with `anchor=ok@btc<~4.4M>`. Stop it:
```bash
kill -INT $(cat ~/seq-bootB.pid); sleep 5
python3 contrib/sequentia/bootstrap-autonomous-testnet.py --stop --basedir ~/seq-bootB
rm -rf ~/seq-bootB ~/seq-bootB.log ~/seq-bootB.pid
```

---

## Step 5 — The full 100-node run, mainnet cadence

30-second slots, ~15-day unbonding, the 40,000-SEQ stake floor (stakers seeded at
50,000 to clear it):

```bash
ulimit -n 65535
cd ~/Sequentia
nohup python3 contrib/sequentia/bootstrap-autonomous-testnet.py \
  --nodes 100 \
  --slot 30 --posunbonding 43200 --posminstake 4000000000000 --stake-seq 50000 \
  --anchorpoll 30 --run-seconds 0 \
  --basedir ~/seq-bootstrap100 \
  --parent-rpchost $PHOST --parent-rpcport $PPORT \
  --parent-rpcuser $PUSER --parent-rpcpassword $PPASS \
  --parent-genesis 000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943 \
  > ~/seq-bootstrap100.log 2>&1 &
echo $! > ~/seq-bootstrap100.pid
tail -f ~/seq-bootstrap100.log
```

Notes:
- `--run-seconds 0` runs until you stop it; Phase 3's wait has a 2-hour cap.
- Founder + 99 registered = a **100-member committee, quorum 51**, funded from one
  `initialfreecoins` output in one registration tx, confirmed by one escaping-stall block.
- Keys saved to `~/seq-bootstrap100/committee_keys.json` (founder + 99 WIFs) — keep them.
- **Timeline:** Phases 1-2 instant → Phase 3 waits ~30 min for testnet + 3 → block 1
  registers all 99 → Phase 4 starts 99 daemons (1-2 min) → full blocks every ~30 s.
  Budget ~45 min to first autonomous block.
- Simpler variant (no stake floor): drop `--posminstake … --stake-seq …` and it uses
  1-SEQ stakes; still 30 s cadence and the full bootstrap.

---

## Step 6 — Watch / inspect (second terminal)

```bash
ND=$HOME/seq-bootstrap100/node000
CLI="$HOME/Sequentia/src/elements-cli -datadir=$ND -chain=elementsregtest -rpcport=18200 -rpcuser=seq -rpcpassword=seq"
watch -n 3 "pgrep -c elementsd; echo '--- stakers registered:'; $CLI getstakerinfo | grep -c ':'; echo '--- anchor:'; $CLI getanchorstatus"
```
`getstakerinfo` shows **1** during Phase 3 (founder only), then **100** after block 1;
`getanchorstatus` reads `anchorstatus: ok` once block 1 lands.

---

## Step 7 — Stop / clean up

```bash
kill -INT $(cat ~/seq-bootstrap100.pid); sleep 5     # clean shutdown of all daemons
python3 contrib/sequentia/bootstrap-autonomous-testnet.py --stop --basedir ~/seq-bootstrap100
rm -rf ~/seq-bootstrap100 ~/seq-bootstrap100.log ~/seq-bootstrap100.pid
```

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `No rule to make target 'blst/build/assembly.S'` | Stale checkout — `git pull` (the vendored blst is committed in the tree), rebuild. |
| `strings … posdebugroundskewms` empty | Build didn't finish — `make clean && make -j$(nproc)`, no sudo. |
| Step 2 curl errors | Proxy down / wrong port / missing method. Anchor needs `getblockcount`, `getblockhash`, `getbestblockhash`, `getblockheader`, `getblock`. |
| `founder registered from genesis stake: False` | Stale tool — `git pull` (the genesis lock is now time-based). |
| Phase 3 waits a long time | Normal: testnet must mine 3 blocks. Re-check the Step 2 `getblockcount` is rising; testnet has ~20-min min-difficulty gaps. |
| Phase 3 `could not produce the escaping-stall block` | Proxy went unreachable before block 1, or 2-h cap hit. Fix proxy, clear basedir, rerun. |
| `min`/`max` diverge and stay split | A real fork → stale binary. Confirm the `strings` check. On one synced box you should never see it. |
| Out of RAM at 100 | `--nodes 50` (founder + 49, quorum 25) — same flow. |

The `~/seq-bootstrap100.log` reaching Phase 4 with climbing heights, `fork=no`, and
`anchor=ok@btc<testnet height>` is the proof: a 100-member committee that
bootstrapped from one founder via escaping stall and now self-certifies, anchored
to live Bitcoin testnet3.
