# Fulmen: click-and-play bundled SeqLN — work plan

> **STATUS 2026-07-04: EXECUTED AND SHIPPED (v0.2.0).** All milestones done in
> fulmen `5d37ba2`; artifacts on https://sequentiatestnet.com/download/.
> Deltas from this plan: no Docker available, so the Linux bundle is built on
> the laptop (glibc 2.39 / noble baseline, not jammy) and the WSL rootfs is
> assembled dockerlessly from an ubuntu-base 24.04 tarball + /opt/seqln overlay
> (`build/make-wsl-rootfs.sh`); clnrest came from the existing seqln Rust build.
> Verified headless against live infra: bundled + packaged lightningd synced
> fresh nodes on sequentia-testnet (bundled elements-cli) AND Bitcoin testnet4
> (bundled bitcoin-cli), hsm_secret/emergency.recover created, clean stop.
> NOT verified: GUI click-through (no display here), the whole Windows/WSL2
> path on a real Windows machine (§6.8 still owed), backend flags vs box
> config (box ran no lightningd at ship time; minimal config proven live
> instead).

Goal: Fulmen installs and runs like a normal desktop app, ships SeqLN inside it, and
starts/supervises `lightningd` itself. The user never builds, installs, or configures
SeqLN by hand. External dependencies:

- a **Sequentia node** (elementsd), local or reached over RPC, and
- **optionally a Bitcoin node** (bitcoind) — SeqLN runs on Bitcoin networks too
  (bitcoin / testnet4), and the dual-chain direction (pure-LN asset<->BTC swaps,
  every Sequentia wallet is dual-chain) means Fulmen should be able to manage a
  second SeqLN instance on the Bitcoin side. Optional at onboarding, first-class
  in the architecture.

Targets: Linux (AppImage) and Windows (zip, later installer). Both from this laptop;
Windows click-testing needs a real Windows machine or VM (flagged in M4).

Everything below is grounded in a code survey of `~/fulmen` and `~/seqln`
(`sequentia-stable`, CLN v26.06.2 base) done 2026-07-04.

## 1. Where we are

Fulmen already has most of the process-management skeleton:

- `src/main/node.js` — `NodeManager` spawns and supervises `lightningd`:
  `_startLocal()` (Linux/macOS, unix socket) and `_startWSL()` (Windows: import a
  bundled rootfs tar as WSL2 distro `fulmen-seqln`, run lightningd + clnrest inside,
  talk clnrest over localhost TCP with a freshly minted rune).
- `src/main/main.js:70-80` — `nodeStartConfig()` already looks for a bundled payload:
  `<resources>/seqln/lightningd` (Linux/macOS) or `<resources>/wsl/Fulmen-seqln-rootfs.tar`
  (Windows). Auto-start when `config.mode === 'managed'` (`main.js:108-110`).
- Two transports exist: raw JSON-RPC on the `lightning-rpc` unix socket
  (`src/main/cln.js`) and clnrest HTTP(S)+rune (`src/main/cln-rest.js`).
- electron-builder config in `package.json`: Linux AppImage + Windows zip;
  `extraResources` currently **Windows-only** (`build/wsl/*.tar`).

What is missing is the payload and the wiring around it:

- No SeqLN binaries are staged or shipped for Linux; the WSL rootfs tar has never
  been built.
- The chain-backend side (`bcli` -> Sequentia/Bitcoin RPC) has **no UI, no config
  wiring, and no bundled CLI client**.
- No onboarding, no sync/status UX, no seed (hsm_secret) backup, no app icon.
- Several latent bugs in the WSL path (layout mismatch, stop() strands the distro,
  clnrest not built under the current SeqLN configure flags). Detailed below.

## 2. SeqLN runtime footprint (what the bundle must contain)

Verified against built artifacts in `~/seqln` (`config.vars`: `RUST=0`,
`HAVE_SQLITE3=1`, `HAVE_POSTGRES=0`):

- **Binaries**: `lightningd` plus 10 pure-C subdaemons (`lightning_channeld`,
  `lightning_closingd`, `lightning_connectd`, `lightning_dualopend`,
  `lightning_gossipd`, `lightning_gossip_compactd`, `lightning_hsmd`,
  `lightning_onchaind`, `lightning_openingd`, `lightning_websocketd`), plus
  `lightning-cli` and `lightning-hsmtool` (for CLI health checks and seed backup).
- **Plugins** (C, from `plugins/`): `bcli, pay, spenderp, funder, topology, offers,
  keysend, txprepare, autoclean, chanbackup, commando, recover, exposesecret,
  cln-renepay, cln-xpay, cln-askrene, cln-bwatch, recklessrpc, sql`.
- **Relocatable by design**: `lightningd` resolves subdaemons relative to its own
  binary path (`lightningd/lightningd.c:571-596`). In-tree mode: if all
  `lightning_*` sit **next to** `lightningd`, it uses that dir and loads builtin
  plugins from `../plugins`. So the bundle layout must be:

  ```
  seqln/
    bin/        lightningd, lightning_*  (subdaemons NEXT to lightningd)
                lightning-cli, lightning-hsmtool, elements-cli, bitcoin-cli
    plugins/    bcli, pay, ...           (= bin/../plugins)
    lib/        libsqlite3.so.0, libsodium.so.23    (Linux bundle only)
  ```

- **Shared libs**: only `libsqlite3` and `libsodium` are non-standard dynamic deps
  (libwally and libsecp256k1 are statically linked). Bundle both in `seqln/lib/`
  and spawn lightningd with `LD_LIBRARY_PATH=<resources>/seqln/lib`.
- **Chain backend is `bcli` and it does NOT speak HTTP itself.** It shells out to
  the CLI named in chainparams (`bitcoin/chainparams.c:250,274`):
  - `sequentia-testnet` / `sequentia` -> **`elements-cli`** (cli_args `-chain=test` /
    `-chain=sequentia`)
  - `bitcoin` / `testnet4` -> **`bitcoin-cli`**
  The CLI does the actual HTTP RPC, so elementsd/bitcoind can be fully remote —
  exactly the "only external dep is a node" model. **Both CLI binaries must be
  bundled.** `elements-cli` from this repo's `build-linux/src/elements-cli` has no
  non-standard shared deps (checked with ldd). `bitcoin-cli` comes from a Bitcoin
  Core release build (needs testnet4 support, i.e. v28+; verify its ldd footprint
  the same way, or build static via depends).
- **No Python, no Rust needed for the core Linux bundle.** The Rust plugins
  (clnrest, cln-grpc, wss-proxy...) are not built with `RUST=0`; the old Python
  clnrest is gone upstream. `contrib/holdinvoice-seq/holdinvoice.py` is Python and
  **stays OUT of v1** — it is a swap-daemon primitive, not needed for wallet
  pay/receive/channel flows, and it currently hardcodes
  `/home/aejkohl/seqln/contrib/pyln-client/...` (`holdinvoice.py:34`). Revisit when
  Fulmen grows swap features; fix the path then.
- **Exception for the WSL rootfs**: the Windows path talks to the node via clnrest,
  which is a **Rust** plugin — the rootfs build must run with `RUST=1` (or at least
  build the `plugins/rest-plugin` crate) even though the Linux bundle does not.
- **Networks registered** (`bitcoin/chainparams.c:236,266`): `sequentia-testnet`
  (rpc_port 18332, invoices `lntsqt...`) fully populated; `sequentia` mainnet has
  TODO genesis placeholders — mainnet is a non-goal until that lands. Bitcoin
  networks come from upstream CLN and work unchanged.

Glibc baseline: build the staged binaries in an Ubuntu 22.04 (jammy) container
(seqln ships `contrib/reprobuild/Dockerfile.jammy`) so the AppImage runs on any
2022+ distro, instead of inheriting this laptop's glibc.

## 3. Backend wiring (the part with zero code today)

Fulmen must generate the node's config instead of asking users to write one.

- On managed start, write `<lightning-dir>/config` (mode 0600 — it holds the RPC
  password; never pass the password on argv, it would show in /proc):

  ```
  network=sequentia-testnet
  bitcoin-rpcconnect=<host>
  bitcoin-rpcport=<port>            # 18332 default for sequentia-testnet
  bitcoin-rpcuser=<user>
  bitcoin-rpcpassword=<pass>
  bitcoin-cli=<resources>/seqln/bin/elements-cli
  log-file=<lightning-dir>/log
  ```

  (bcli option names keep the `bitcoin-` prefix on every network; for a Bitcoin-side
  instance the same options point at bitcoind and `bitcoin-cli=<...>/bitcoin-cli`.)
- Settings/onboarding UI needs the four RPC fields per chain plus a **Test
  connection** button that runs the bundled CLI (`elements-cli -rpcconnect=...
  getblockchaininfo`) before ever starting lightningd, so backend errors surface
  as "can't reach your Sequentia node" instead of a lightningd startup failure.
- Defaults to change in `main.js`: `network` default `'bitcoin'` ->
  `'sequentia-testnet'`; new installs with a bundled payload default to
  `mode: 'managed'`.
- M1 task: diff these generated options against the proven SeqLN config in the box
  run dir before freezing them (there may be required experimental/channel flags
  the live deployment uses).

## 4. Dual-chain: optional managed Bitcoin-side node

`NodeManager` is single-instance (`this.proc`). Generalize to one instance **per
network**, because the end state is two managed nodes side by side:

- instance A: `sequentia-testnet`, backend = the user's Sequentia node;
- instance B (optional): `testnet4`/`bitcoin`, backend = the user's Bitcoin node.

Concretely: `NodeManager` -> a map keyed by network with per-instance
proc/socket/transport/lastError; config gains `nodes: {<network>: {...}}`;
`currentTransport()`/`callNode()` in `main.js` take a network/instance argument;
the UI gets a node switcher (or side-by-side balances). Onboarding offers the
Bitcoin node as an opt-in step ("Also run Lightning on Bitcoin? Point me at a
bitcoind"). This is the substrate the pure-LN swap UX (Step 2 / M5 real-BTC-LN)
will sit on later; v1 only needs start/stop/status/pay/receive on both.

Both instances share one bundle — same binaries, different `--network`,
`--lightning-dir` (`~/.fulmen/seqln/<network>` each with its own config file), and
backend CLI. Distinct clnrest ports on Windows (e.g. 9737/9738).

## 5. Linux packaging (M1)

1. **Staging script** `build/make-seqln-bundle.sh` in the fulmen repo:
   - build seqln in a jammy container (`RUST=0`, sqlite3, no postgres);
   - stage the layout from §2 into `build/seqln-linux-x64/`;
   - copy `elements-cli` (from a jammy-built Sequentia, same glibc rule)
     and `bitcoin-cli`;
   - copy `libsqlite3.so.0` + `libsodium.so.23` from the build container into `lib/`;
   - `strip` everything (subdaemons+plugins are ~60-80 MB unstripped; stripping
     roughly halves the AppImage delta);
   - sanity: `ldd` every binary (nothing unexpected), then run the staged
     `bin/lightningd --version` with the bundle's `LD_LIBRARY_PATH`.
2. **package.json**: add Linux `extraResources` `{from: build/seqln-linux-x64, to: seqln}`.
   Confirm electron-builder preserves the +x bits inside the AppImage (it should;
   verify once on the produced artifact).
3. **Code changes**:
   - `nodeStartConfig()` (`main.js:70-80`): bundled path -> `<resources>/seqln/bin/lightningd`.
   - `_startLocal()` (`node.js:56-78`): pass `env` with `LD_LIBRARY_PATH=<resources>/seqln/lib`;
     write the config file from §3 before spawn; stop passing backend settings via
     argv at all.
   - `app.requestSingleInstanceLock()` in `main.js` (lightningd has its own pid
     lock, but a second Fulmen should focus the first window, not fight over the node).
4. **Acceptance**: fresh Linux user (or clean $HOME), download AppImage, run,
   complete onboarding against a reachable Sequentia testnet elementsd, node syncs,
   `getinfo`/`listfunds` render, receive + pay a testnet invoice, quit cleanly
   (socket gone, no orphan processes), relaunch resumes. The existing headless
   smoke test (`node src/main/node.js <path> <dir> <network>`) is the fast loop;
   final check via the real AppImage.

## 6. Windows packaging (M3)

Confirmed: SeqLN is POSIX-only (unix sockets, fork/exec of subdaemons; bcli
fork/execs the CLI). No native Windows lightningd — WSL2 is the right runtime, and
`_startWSL()` already implements the flow. Work items:

1. **Build the rootfs** — `build/make-wsl-rootfs.sh` (Docker is available on this
   laptop): ubuntu:24.04 (or debian-slim) base; build seqln **with `RUST=1`** so
   `clnrest` exists; install to `/opt/seqln/bin` + `/opt/seqln/plugins` (same §2
   layout); include linux `elements-cli` + `bitcoin-cli` in `/opt/seqln/bin`;
   `docker export` -> `build/wsl/Fulmen-seqln-rootfs.tar`. Expect ~100-200 MB;
   debian-slim if size matters.
2. **Fix the layout mismatch** (real bug): `_startWSL` defaults assume
   `/opt/seqln/lightningd`, but with plugins at `/opt/seqln/plugins` the binary must
   live at `/opt/seqln/bin/lightningd` (plugins resolve at `../plugins` relative to
   the binary's dir). Update the defaults in `node.js:90-92` to `/opt/seqln/bin/...`.
3. **Fix stop()** (real bug): killing the `wsl.exe` process does not reliably kill
   lightningd inside the distro. Stop sequence: `wsl -d fulmen-seqln --
   lightning-cli stop` (per running network), wait, then `wsl --terminate
   fulmen-seqln`, then kill the host process. Also run this on `before-quit`.
4. **Backend config inside WSL**: same generated config file (§3), written via
   `wsl -d ... sh -c 'cat > .../config'` with the password piped on stdin, not argv.
   Wrinkle: a **local** node on the Windows host is not `127.0.0.1` from inside
   WSL2 (NAT). Resolution order: try mirrored networking (Win11 makes localhost
   work), else the default-gateway IP from inside the distro; surface a help note
   ("add rpcallowip for the WSL subnet to your bitcoin.conf/elements.conf").
   Remote nodes just work.
5. **WSL preflight UX**: `wsl --status` failure currently throws a raw error.
   Turn it into a guided screen: explain WSL2, offer to run `wsl --install`
   (elevation + reboot required), and always show the fallback: "or connect Fulmen
   to a remote SeqLN over clnrest instead".
6. **clnrest details**: `--clnrest-host=127.0.0.1` + WSL2 localhost forwarding is
   already correct; keep http since it never leaves the machine, rune minted per
   start (persist it in config to survive restarts without an extra createrune).
   Two managed networks -> two ports.
7. **Packaging**: `extraResources` for `build/wsl/*.tar` already configured; un-gitignore
   nothing (the tar is a build artifact, keep it out of git; the release zip carries it).
   Later: NSIS installer instead of bare zip (nice-to-have, not v1).
8. **Acceptance**: on a real Windows 11 machine/VM: unzip, run Fulmen.exe, guided
   WSL setup if needed, first run imports the distro (progress UI — the import
   takes ~1 min), onboarding against a remote Sequentia node, pay/receive works,
   quit leaves no distro processes running (`wsl -l --running` empty), second
   launch skips import. **This step cannot be verified from the Linux laptop** —
   needs a Windows box; budget a session for it.

## 7. Onboarding + lifecycle UX (M2)

First-run wizard (replaces "go to Settings and fill in paths"):

1. Welcome -> choose: **Run a node for me** (default) / Connect to existing SeqLN
   (socket or clnrest — keep the current Settings panels as the advanced path).
2. Network: Sequentia testnet (default; mainnet greyed until chainparams land).
3. Sequentia node RPC: host/port/user/password + Test connection (bundled CLI).
   Optional step: also configure a Bitcoin node for a second instance (§4).
4. Start node -> live progress: WSL import (Windows), lightningd startup log tail,
   then sync progress = `getinfo.blockheight` vs backend `getblockcount` via the
   bundled CLI.
5. **Back up your wallet**: surface `hsm_secret` (and `emergency.recover` from the
   chanbackup plugin) immediately after first start — copy-to-USB flow or at
   minimum a "reveal folder + explain" screen. A managed node quietly generating
   an unbacked-up seed is the one unforgivable wallet sin.

Ongoing lifecycle: status pill per node (stopped/starting/syncing/ready/error with
lastError), log viewer window (NodeManager already captures stdout/stderr), restart
button, crash -> surface + offer restart (no silent auto-restart loops), and a
Sequentia-vs-Bitcoin node indicator once §4 lands.

Branding: no icon exists at all — need `build/icon.png` (512+) and `icon.ico`
wired into electron-builder before release artifacts look legitimate.

## 8. Milestones

- **M1 — Linux bundled node end-to-end**: staging script, extraResources, path/env/
  config-file wiring, defaults (`sequentia-testnet`, managed), single-instance
  lock; headless smoke + AppImage acceptance (§5.4).
- **M2 — Onboarding + lifecycle UX**: wizard, test-connection, sync progress, log
  viewer, hsm_secret backup, status pill, icon.
- **M3 — Windows WSL**: rootfs build (RUST=1/clnrest), layout+stop fixes, config
  injection + host-IP handling, WSL preflight UX, win zip; acceptance on real
  Windows (§6.8).
- **M4 — Dual-chain node manager**: multi-instance NodeManager, per-network config,
  optional Bitcoin node in onboarding, UI switcher.
- **M5 — Release**: strip/size pass, version stamping (Fulmen version + seqln
  commit in About), downloads listed on sequentiatestnet.com, README/user doc
  (public copy: network = "Sequentia", never "SEQ"; fee-rate units per asset,
  never sat/vB for Sequentia-side amounts).

Order rationale: M1 before M2 so the wizard has something real to drive; M3 after
M2 because the WSL path reuses the same onboarding; M4 is independent of M3 and can
run in parallel if a second pair of hands appears.

## 9. Risks / open items

- **Exact lightningd flags**: mirror the box's proven SeqLN run-dir config in M1
  (experimental/channel flags), don't guess.
- **bitcoin-cli sourcing**: pick a pinned Bitcoin Core version with testnet4
  (v28+), prefer a static depends build; verify ldd before staging.
- **electron-builder permission preservation** inside AppImage/zip for extraResources
  binaries: verify once, early in M1 (cheap to check, annoying to discover late).
- **WSL import UX cost**: first run on Windows pays a WSL install (possibly reboot)
  + distro import. Unavoidable; the mitigation is honest progress UI and the
  remote-clnrest fallback.
- **Windows Defender/SmartScreen**: unsigned exe + zip will warn. Signing is out of
  scope for testnet; note it on the download page.
- **holdinvoice-seq / swap features**: out of v1; when swaps come to Fulmen, either
  ship Python + fix the hardcoded path, or port the hold plugin to C/Rust.
- **Mainnet**: blocked on seqln mainnet chainparams (genesis TODO in
  `bitcoin/chainparams.c:265`) — testnet-only until then, say so in the UI.
