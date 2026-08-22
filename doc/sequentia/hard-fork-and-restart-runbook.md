# Sequentia testnet bring-up + hard-fork runbook

Status: operational. The definitive ordered checklist for (a) watching a consensus
activation on the testnet and (b) bringing the whole DEX stack back up correctly after ANY
full restart. Written 2026-07-22 from a live-box audit and kept current since; every
wallet/port/service below was verified present. When a service moves, edit this file.

The 2026-07-22 v23.3.7 cutover restart is what unloaded the sbtc-bridge wallet and left the
LN asset nodes and LSP in a degraded state that took manual probing to find. This runbook
exists so that never costs a manual hunt again.

Box access: `ssh seq` (never `ConnectionAttempts>1`; retry on drop; keep commands simple).
RPC credentials are not written here: the box keeps them in its environment, and the
commands below read them as `$RPCUSER` / `$RPCPASS`.

## 0. Watching a consensus activation

A height-gated rule is a **consensus activation on an already-deployed binary**: the fleet
has been cut over to the fork-aware build well before the height (Section 1), so the
boundary is NOT a mass restart. The risk at the boundary is a **certification stall** —
producers disagreeing on what the next block may look like — not a bring-up. At 60 s per
block, a height N blocks ahead of the tip is about N minutes away.

Activations the testnet has been through, with the build that first shipped each rule:

| Height | Rule | First shipped in |
|---|---|---|
| 44,300 | `pos_exprace` exponential-race leader election | 23.3.7 |
| 89,856 | Simplicity + introspection opcodes (BIP9 bit 21, locked in by signalling) | 24.0.0 |
| 93,800 | 60-s minimum block spacing (`pos_block_spacing`) | 24.0.0 |
| 94,600 | supervised assets (`supervised_assets_height`) | 24.1.0 |
| 101,810 | Simplicity budget x4 (`simplicity_budget4_height`) | 24.3.0 |
| 102,150 | split payouts (`split_payout_height`) | 24.4.0 |

Every height except the Simplicity lock-in is a constant in `src/chainparams.cpp`, bound to
the testnet genesis hash so a re-genesis drops it. Add a row when the next one lands.

**At the boundary, watch for:**
- Block production halting or slowing sharply right at the activation height (rule
  disagreement between producers).
- Committee split on `getbestblockhash` (nodes disagreeing on the tip).
- `bad-posvrf` / certification-fail spam in committee logs.

**Watch commands:**
```
for n in 000 005 010 015 019; do /root/Sequentia/src/sequentia-cli -datadir=/root/seq-testnet/node$n getblockcount; done   # should agree + advance
for n in 000 010 019; do /root/Sequentia/src/sequentia-cli -datadir=/root/seq-testnet/node$n getbestblockhash; done         # should be identical
journalctl -u 'seq*committee*' --since '5 min ago' 2>/dev/null | grep -iE 'posvrf|certif|stall|leader' | tail
```

**If it stalls:** testnet stalls have so far been environmental, not consensus bugs —
mixed binaries across the fleet, a finality/round wedge, or a flapping Bitcoin anchor — and
a uniform committee relaunch on one build (Section 2) clears all three; a finality wedge
often self-heals first. Do NOT ignore Bitcoin anchoring to force it (Principle 1). If the
rule itself misbehaves, that is a code issue for Andreas, not an ops recovery.

## 1. Restart ordering (dependency order)

If a full restart is ever needed, bring services up in this order. Each layer's verify must
pass before the next.

```
Bitcoin testnet4 (the anchor)
  -> Sequentia committee (node000-019)  [node000 = producer]
  -> dexnode (application node, RPC :18300)
  -> explorer node (systemd seq-explorer-node, clone /root/sequentia/Sequentia)
  -> pool-board node (systemd seq-pool-board-node, same clone)
  -> node RPC wallets loaded (EVERY wallet — they do NOT auto-load)
  -> relays (seqobd :9955 systemd + :9965/:9966/:9971)
  -> SeqLN fleet (btc-maker, btc-taker, ln-asset, ln-asset-b, prov nodes, speculad)
  -> LSP (lsp-b5b1 :9981) + sbtc-bridge (:9987)
  -> maker fleets + settlers + seeding
  -> health verification (Section 6)
```

**The sequentiad fleet is 23 nodes, not 20**: the committee plus the dexnode, the explorer
node and the pool-board node, and a binary cutover moves ALL of them in one window. Never
work from a remembered list — the 24.3.0 cutover found the pool-board node missing from
every list anyone held. Derive the fleet live, and let the process table be the authority:

```
ps -eo pid,args | grep '[s]equentiad'                     # every running node + its datadir
systemctl list-units 'seq-*' --type=service --state=running --no-legend
```

Three checks per node before declaring a cutover plan complete:

- **Effective ExecStart comes from `systemctl show -p ExecStart --value <unit>`, never from
  `systemctl cat`.** Drop-ins override, and cat's ordering misleads: at the 24.3.0 cutover
  the explorer unit READ as sharing `/root/Sequentia` while actually running
  `/root/sequentia/Sequentia/src/sequentiad`.
- **Check `/proc/<pid>/exe` against the on-disk binary.** A node keeps running from a deleted
  inode after a rename or rebuild; the dexnode ran the pre-rename path for a full day this
  way, invisible to any path-based audit until its restart.
- Nodes running from `/root/sequentia/Sequentia` need that clone fetched, checked out at the
  tag and BUILT in the same window — one build there covers the explorer and pool-board
  nodes both.

## 2. Committee (node000-019)

```
/root/seq-committee-start.sh                 # idempotent; skips already-up nodes
```
Gotchas (verified, do not relearn the hard way):
- **Build in the RUN dir** `/root/Sequentia` (has BDB wallet support); the clone
  `/root/sequentia/Sequentia` lacks BDB and crashes node000 (holds treasury BDB).
- Committee network subdir is `testnet3`, datadir `-datadir=/root/seq-testnet/nodeNNN` (no
  trailing slash). Count only `sequentiad -datadir=…`, and kill stray `sequentia-cli` waiters
  (they fool the start script's pgrep).
- node000 is THE producer; it is sometimes manually launched and must be explicitly started.

Verify: `getblockcount` agrees across nodes and advances; 20/20 answer RPC.

## 3. dexnode + node RPC wallets (they do NOT auto-load)

dexnode (application node) is launched manually and its wallets must be explicitly loaded.
The 2026-07-22 miss was exactly this — the sbtc-bridge wallet was never reloaded.

**Capture the LIVE set before stopping — the lists below go stale** (node000 held 14 wallets
at the 24.3.0 cutover against the 4 listed here). And capture how each wallet RESOLVES, not
just its name:

```
/root/Sequentia/src/sequentia-cli -rpcport=18200 -rpcuser=$RPCUSER -rpcpassword=$RPCPASS listwallets
/root/Sequentia/src/sequentia-cli -rpcport=18300 -rpcuser=$RPCUSER -rpcpassword=$RPCPASS listwallets
ls /root/seq-testnet/node000/testnet3/wallets/    # the layout half of the capture
```

**The legacy-layout trap (found at the 24.3.0 cutover, would have bitten quietly):** seven of
node000's wallets live directly in `testnet3/` rather than `testnet3/wallets/`.
`loadwallet <name>` cannot see those; loading by ABSOLUTE PATH works but registers the wallet
under that path, so every `-rpcwallet=<name>` in the box scripts (`addstakers.sh`,
`reissue-loop.sh`, `reissue-run.sh`) then fails. The fix that keeps short names working is a
symlink per legacy wallet, then load by name:

```
cd /root/seq-testnet/node000/testnet3/wallets && ln -s ../<name> <name>
loadwallet <name>
```

Never load by absolute path.

**node000 (:18200) wallets:** `treasury  treasury2  compages  sbtc-bridge` (+ the captured rest)
```
for w in treasury treasury2 compages sbtc-bridge; do /root/Sequentia/src/sequentia-cli -rpcport=18200 -rpcuser=$RPCUSER -rpcpassword=$RPCPASS loadwallet $w; done
```

**dexnode (:18300) wallets:** `xmm  seqdex-mm-btc  bridge-taker` (+ load `subtaker submaker
submaker2 speculad-fee seqob-settler` if the LSP/settler need them — verify against
`/etc/sequentia/lsp-b5b1.env` SEQ_WALLET and the settler config; these were NOT loaded at the
last audit and may thin the LSP self-custody paths).
```
for w in xmm seqdex-mm-btc bridge-taker; do /root/Sequentia/src/sequentia-cli -rpcport=18300 -rpcuser=$RPCUSER -rpcpassword=$RPCPASS loadwallet $w; done
```

**bitcoind testnet4 wallets:** `seqdex-mm-btc  w  sell-maker-btc  sell-taker-recv
sbtc-reserve  lsp-bridge-recoup  seqln-btc-channel-test`
```
for w in seqdex-mm-btc sbtc-reserve lsp-bridge-recoup; do bitcoin-cli -testnet4 -rpcuser=$RPCUSER -rpcpassword=$RPCPASS loadwallet $w; done
```

Verify: `listwallets` on each node shows the full set above.

## 4. Relays

- **seqobd :9955** is now a systemd unit with a drop-in (`allflags.conf`) carrying
  `-xsession-deadline 3h -node-rpc 127.0.0.1:18300 -node-rpc-user/-pass $RPCUSER/$RPCPASS -trade-log …`.
  A plain `systemctl restart seqobd` now brings it up correct (3h cross deadline, covenant
  watcher ON, durable trade log). The reseed ExecStartPost refills same-chain books.
  DO NOT hand-launch it (that is what created the rogue duplicate the audit found).
- **:9965 / :9966 / :9971 seqobd** instances (pure-LN/submarine, subasset, subasset-sell)
  come up with their own launchers; their books fill only once the maker fleets reconnect.

Verify: `curl -s 127.0.0.1:9955/v1/markets | python3 -c 'import sys,json;print(len(json.load(sys.stdin)["markets"]))'` shows ~36; a same-chain market's `last_price` is non-zero.

## 5. SeqLN fleet, LSP, sbtc-bridge

- **btc-maker / btc-taker** (the LSP's BTC-LN nodes) — `lightningd-b1a4492`; the bridge
  front-ln needs both up with balanced BTC channels.
- **ln-asset / ln-asset-b** — now systemd units **`seqob-ln-asset` / `seqob-ln-asset-b`**
  (Type=simple, isolated `-16` `asset-bin` binary, enabled). `systemctl start seqob-ln-asset
  seqob-ln-asset-b`. These serve every asset's LN leg (sub-asset + pure-LN + the bridge
  front-ln route). If down, :9966/:9971 books go empty and front-ln fails "getroute".
  Verify: `lightning-cli --lightning-dir=/root/sequentia/lsp/ln-asset --network=sequentia-testnet getinfo` shows channels.
- **prov nodes** (per-user hosted LN) respawn on demand; **speculad** (watchtower) restarts
  with its config.
- **LSP lsp-b5b1 :9981** — `systemctl restart lsp-b5b1`. On boot it re-attaches surviving
  hosted nodes. IMPORTANT: pull the box wallet clone first so the LSP runs the latest
  fund-safety code (`cd /root/sequentia/sequentia-web-wallet && git pull`).
- **sbtc-bridge :9987** — the node service is up, but its Sequentia wallet (`sbtc-bridge` on
  :18200) must be loaded (Section 3) or every peg scan errors "wallet not loaded". Consider
  adding an ExecStartPre loadwallet so this can never silently break again.

## 6. Maker fleets, settlers, seeding

Systemd units (Restart=always): `seqob-scmakers`, `seqob-scmakers-buy`, `seqob-xmakers`,
`seqob-subasset-maker`, `seqob-subasset-sell-maker{,-usdx,-eurx}`, `seqob-submarine-{gold,eurx,usdx}-sell`,
`seqob-submarine-usdx-buy`, `seqob-settler`. Plus the single `supervise-xresume.sh` settler
loop (ensure ONE instance — two were found running concurrently on 2026-07-22).
```
systemctl restart seqob-scmakers seqob-scmakers-buy seqob-xmakers seqob-subasset-maker seqob-subasset-sell-maker seqob-submarine-gold-sell
pgrep -fc supervise-xresume.sh    # must be 1, not 2
```
Same-chain books reseed automatically (seqobd ExecStartPost). Cross/LN/subasset depend on
their maker fleets reconnecting to the relays + (for LN) ln-asset being up first.

## 7. Health verification (run after any bring-up; and as a cron probe)

A green board means: ports listening, wallets loaded, books non-empty, LSP answering, LN
nodes up, bridge alive, no stranded HTLCs.
```
# ports
ss -tlnp | grep -E ':(9955|9965|9966|9971|9981|9987|18200|18300|9740|9741)' | wc -l   # expect ~10
# committee producing
/root/Sequentia/src/sequentia-cli -datadir=/root/seq-testnet/node000 getblockcount
# wallets loaded (each node's set from Section 3)
/root/Sequentia/src/sequentia-cli -rpcport=18200 -rpcuser=$RPCUSER -rpcpassword=$RPCPASS listwallets
# books non-empty + last_price live
curl -s 127.0.0.1:9955/v1/markets | python3 -c 'import sys,json;m=json.load(sys.stdin)["markets"];print(len(m),"markets",sum(1 for x in m if x.get("last_price")),"priced")'
# LN asset nodes
for d in ln-asset ln-asset-b; do systemctl is-active seqob-$d; done
# LSP alive (needs Bearer LSP_TOKEN from /etc/sequentia/lsp-b5b1.env)
systemctl is-active lsp-b5b1
# sbtc-bridge not erroring
tail -1 /root/sequentia/sbtc-bridge/bridge.log
# single xresume
pgrep -fc supervise-xresume.sh
```
Section 7 is the basis for the standing health-probe cron (`/root/seq-health-probe.sh`) that
writes a status line the operator can watch — see the DEX gap-closure plan P0.8.

## 8. Known post-restart gotchas (do not relearn)

- Wallets never auto-load — always run the Section 3 loops.
- ln-asset uses the isolated `-16 asset-bin` binary (NOT the shared `b1a4492`) to avoid the
  subdaemon version-mismatch reexec loop; the systemd units already point at it.
- A seqln binary change is a FULL consistent cutover: stop the whole LN fleet (makers,
  takers, asset nodes, prov nodes, speculad), swap the binary once, relaunch everything on
  that build. Do not mix versions — a mixed fleet fails in ways that look like channel
  problems.
- Never hand-launch seqobd :9955 (creates a port-holding duplicate that blocks the systemd
  unit); use `systemctl restart seqobd`.
- `bitcoin-cli -testnet4` works; `-chain=testnet4` prints an error banner.
- peer-stall-check throws transient hits during the restart window itself: inbound peers at
  headers -1 (unknown), under a minute old. That is connection churn, not the
  frozen-below-a-height stall signature; it clears on its own. Confirm a cutover by version
  and height across all 23 nodes, never by the script alone.
- From 24.5.2, the first start after an upgrade sweeps supervision-invalidated residue out of
  a reloaded mempool.dat before RPC opens (`Supervision: evicting …` lines at startup are that
  sweep working, not a problem). 24.3.0 through 24.5.1 carried a sweep that raced the registry
  rebuild and did nothing when it lost; a node on those builds can still be holding dead
  spends, and this cutover is what clears them.
