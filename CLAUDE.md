# Working on Sequentia

Notes for AI coding agents and new contributors. These are the conventions that
are not obvious from the code, and where getting them wrong is expensive.

[CONTRIBUTING.md](CONTRIBUTING.md) covers the parts that are shared with Bitcoin
Core — commit hygiene, PR titles prefixed by area, squashing, atomic commits —
plus this project's rules on consensus activation heights. That document is
authoritative for everything it covers; this one does not repeat it. One thing it
inherits from upstream does **not** apply here: the Bitcoin Core review and
"decision making" process in its second half describes the upstream project. See
below for what actually happens to a pull request in this repository.

<!-- BEGIN SHARED AGENT CONVENTIONS: identical in every Sequentia repo. Change it in all of them together. -->
## Working with git and GitHub here

These rules are the same in every Sequentia repository. They are repeated in each
one because this file is the only thing an agent is guaranteed to read, whatever
machine it is working from.

**Nothing pushed to GitHub credits Claude, Anthropic, or any AI tool.** No
`Co-Authored-By: Claude` trailer, no `Claude-Session:` trailer or `claude.ai`
link, no "Generated with Claude Code" in a commit message or a pull request body,
no `claude/*` branch names or session ids, and no mention in source, comments,
docs or issue text. Agent tooling offers several of these by default; compose the
message without them rather than stripping them afterwards.

**Author every commit as**
`GracedEternalKingCabbageMan <151803062+GracedEternalKingCabbageMan@users.noreply.github.com>`.
Never a personal address.

**Every change lands through a pull request that you merge yourself, at once.**
There is no reviewer on this project; the pull request exists so the reasoning is
recorded beside the diff. Branch, push, open it, merge it, delete the branch, all
in one sitting. Pushing straight to the default branch is the rule most often
broken here, and it is the one that costs the record. A pull request stays open
only when the repository owner asks for that specific one, and that never carries
over to the next.

**Name branches `area/short-description`**: `fix/`, `doc/`, `feature/`, `test/`,
`build/`, or the component being changed. Never a tool name, a session id, or
`worktree-*`.

**Write the subject as `area: what changed`**, one line, 72 characters at the
outside and 50 where you can manage it. Put the reasoning in the body, and
explain why rather than what.

**These repositories are public and world-readable.** Never commit private keys,
seeds, `wallet.dat`, RPC credentials, `.env` files or API tokens. Read the diff
before every commit. Secrets belong on the server and in offline backups.

**A file belongs to the repository whose code it describes.** Decide which repo
owns it before writing it; if it landed in the wrong one, move it rather than
deleting it.

**Push the same day you commit.** The testnet server pulls only from GitHub, so a
branch left on one laptop is invisible to every other machine and to the box.
<!-- END SHARED AGENT CONVENTIONS -->

Two things this repository adds to the rules above. Merging is not deploying:
even a consensus change is merged straight away, because the step that needs
coordination is the node cutover rather than the merge. And every consensus
change bumps the version in the same pull request, then gets a matching git tag,
without which the download page keeps serving the old build; `CONTRIBUTING.md`
carries the reasoning.

## Deploying to the testnet server

The server pulls operational code from GitHub only. Never edit source on the
server, and never copy binaries onto it — `git` checkout there, then build there.

Two clones must stay consistent: `/root/Sequentia` (the run directory,
the one the nodes execute) and `/root/sequentia/Sequentia`. They are
frequently on detached HEAD at different commits, so `git pull` may not do what
you expect; fetch and check out explicitly.

## Upgrading the committee: all at once, never gradually

The testnet runs a set of committee nodes (currently 20) plus a dexnode and an
explorer node. Mixed binaries select different anchors and fragment share-locks,
which stalls the chain. Stop every node, swap the binary, relaunch every node.

Process management is mixed: the committee and dexnode run as ordinary processes
started by a script; the explorer node is a systemd unit. Stopping "everything"
therefore takes more than one mechanism.

Restarting drops wallets that were loaded dynamically and appear in no
configuration file. Capture the loaded wallet set before stopping, and reload it
afterwards.

Never use `pkill -f <pattern>` over SSH. The SSH command line contains the
pattern, so the command matches and kills itself.

## Consensus changes

Read the consensus sections of [CONTRIBUTING.md](CONTRIBUTING.md) before writing
one. In short:

- A new rule should be active from genesis on every fresh chain — regtest, a new
  testnet, mainnet. Height-gate only what the already-running testnet requires.
- A one-time, chain-specific rule must bind to that chain's genesis hash as well
  as to a height, so that a re-genesis or a different chain drops it
  automatically rather than inheriting it.
- A rule that only ever accepts more — one that relaxes validation — needs no
  activation gate at all, because no previously accepted block can fail under it.
  It can still be a hard fork for the live network, which is a deployment
  question, not an activation-height one.

## Chains

`CreateChainParams` in `src/chainparams.cpp` selects these by name:

- `test` — the live testnet, and the binary's default chain when `-chain` is not
  given (`CBaseChainParams::DEFAULT`).
- `sequentia` — mainnet parameters. Not running anywhere yet; the genesis is a
  placeholder and the node refuses to start on it without
  `-allowplaceholdergenesis`.
- `main`, `signet`, `regtest` — Bitcoin-mode chains (`g_con_elementsmode =
  false`): a single asset and no issuance.
- `liquidv1`, `liquidv1test`, `liquidtestnet` — inherited from Elements. Not
  something this project develops against.
- Any other name is a custom chain (`CCustomParams`), where genesis and the
  consensus rules come from configuration. `elementsregtest` is the custom chain
  the functional test suite runs on by default
  (`test/functional/test_framework/test_framework.py`), and the one the local
  network scripts in `contrib/sequentia/` use.

## Design rules that are easy to break by accident

**Transparent by default.** Unlike Elements, addresses here are unblinded bech32
and confidentiality is opt-in. An operation that works only on the confidential
path is a bug, not a design. Note that this is baked into the real chains
(`m_default_blinded_addresses = false` for both `test` and `sequentia`) but *not*
into custom chains, which keep the Elements default — a regtest node reproducing
production behaviour needs `-con_default_blinded_addresses=0`.

**No privileged asset outside staking.** Only SEQ can stake. For everything else
it is one asset among equals — fees are payable in any accepted asset. A silent
fallback to the policy asset is a privilege, and privileges are bugs.

**Bitcoin anchoring is supreme.** Every block references a Bitcoin block header.
If that anchor is reorged away, this chain reorgs too, in real time. Immediate
finality is modulo Bitcoin: a certified block is final against all internal
competition, including a better-certified sibling, but finality never blocks an
anchor-driven reorg. The gate is only enforced on nodes that watch Bitcoin
(`-validateanchor`), because that watcher is the mechanism that lowers the
finalized point again; a node without it must fall back to plain most-work fork
choice, which follows the reorg transitively. The reasoning is spelled out in the
comments around the immediate-finality gate in `ContextualCheckBlockHeader`
(`src/validation.cpp`) and in `doc/sequentia/03-bitcoin-anchoring.md`.

## Building

A fresh worktree has no *built* `depends/` output, so a plain `./configure` fails
with "Boost is not available!". Point it at an existing checkout's prebuilt
dependencies:

    export CONFIG_SITE=<checkout>/depends/x86_64-pc-linux-gnu/share/config.site
    ./autogen.sh
    ./configure --enable-any-asset-fees --with-gui=no --disable-bench \
      --disable-fuzz-binary
    make -j$(nproc)

**`export` it, rather than setting it for `configure` alone.** Whenever
`configure.ac` is newer than the generated build system — which every version
bump makes true — `make` re-runs `configure` itself, and that re-run inherits
only the environment. Set for the one command, it is gone by then, and the build
dies partway through with the same "Boost is not available!" (or, on a GUI
build, "Qt5Core >= 5.9.5 not found") that it was there to prevent. The failure
lands *after* a successful `./configure`, which makes it read like a broken
checkout rather than a lost variable.

`./autogen.sh` is not only for a fresh worktree: run it after anything that
touches `configure.ac`, so `configure` is newer than it and `make` has no reason
to regenerate at all.

`--enable-any-asset-fees` is not optional. It is a compile-time flag, distinct
from the runtime `-con_any_asset_fees` chain parameter, and it is the only thing
that defines `ANY_ASSET_FEES`. Under that define `CURRENCY_UNIT` and
`CURRENCY_ATOM` in `src/policy/feerate.h` are `RFU` and `rfa` rather than `BTC`
and `sat`, which is the whole of its effect, and every RPC string quoting a fee
unit follows from it. A binary built without it fails `wallet_basic`,
`wallet_send`, `rpc_psbt`, `rpc_fundrawtransaction` and `interface_bitcoin_cli`
on the unit spelling alone. Those failures look real and are not.

The unit test binary is `src/test/test_bitcoin` (there is no `test_elements`
target); build it with `make -C src test/test_bitcoin`.

The functional tests are run with `test/functional/test_runner.py`; the
Sequentia-specific ones are `test/functional/feature_pos_*`,
`feature_any_asset_fee*` and `feature_bitcoin_anchoring*`.
