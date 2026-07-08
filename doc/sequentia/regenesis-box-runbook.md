# Re-genesis box runbook (public BLS committee)

> **STATUS: EXECUTED 2026-07-05.** This runbook was carried out and the
> resulting chain (genesis `ddd11d54…`) is the live public testnet. It is kept
> as the historical record of how the re-genesis was performed; it is not a
> procedure to re-run.

The chain-side work is DONE and validated (branch `committee-public-selection`,
new testnet genesis `ddd11d54…`). This runbook is the box-side execution, in the
order the user asked for: **take the 100 nodes down first (to de-flake the box),
re-genesis with FEWER nodes, bring the rest up slowly at the end.**

Everything below runs on the box (159.195.15.140). The laptop→GitHub→box-pull
pipeline is unchanged; do NOT scp source/binaries.

## 0. Prerequisite: get the code onto the box's build clone

Either merge PR #3 into the working branch and pull it, or pull the feature
branch directly:

    cd /root/sequentia/Sequentia        # the build CLONE (not the run dir)
    git fetch origin
    git checkout committee-public-selection && git pull    # or the merged working branch

Confirm the new genesis is baked (a sanity grep, not a full build yet):

    grep ddd11d54c87a2bd94400fd31ce05d8e1110bb4b78e7103f738342086fc4ea92e src/chainparams.cpp

## 1. Take the 100 committee nodes DOWN first (de-flake the box)

They have no business running through a re-genesis; stopping them frees the box
so SSH/build stop thrashing.

    systemctl stop 'seq-committee@*'   # or whatever the committee unit glob is - confirm with: systemctl list-units 'seq*'
    # stop the maker/relay/services too so nothing writes to the old chain:
    systemctl stop seqobd seqob-maker compagesd seq-price-server seq-mock-price-api || true
    # leave testnet4 bitcoind UP (the anchor parent) and Caddy UP (web).

## 2. Build the re-genesis binary ON the box

    cd /root/sequentia/Sequentia
    ./autogen.sh && ./configure <the box's usual flags> && make -j$(nproc)
    # sanity: the binary must abort if the genesis is wrong; a clean start on
    # --chain test proves the ddd11d54 assert holds.

## 3. Wipe the old chain state and re-bootstrap with FEWER nodes

This is the irreversible step. The old chain (`c2a0a99b`) is abandoned.

    # back up (do NOT delete) any host-only secrets first: wallets, *.conf, RPC creds.
    # then clear the committee RUN dirs' chain data (testnet3 subdir) for the fresh genesis.

    cd /root/sequentia/Sequentia
    python3 contrib/sequentia/bootstrap-autonomous-testnet.py \
      --chain test --public-committee --committee-cap 250 \
      --nodes <SMALL, e.g. 5-10> \
      --run-seconds 0   # keep running; committee stays up
    # anchors to the box's testnet4 bitcoind. Founder self-certifies block 1
    # (size-1 committee), registers the stakers, committee goes autonomous.
    # Validated locally at N=4: heights 3/4/5 certified by quorum-3. NO WBTC.

The founder/treasury key is unchanged: WIF `cURsyjY6…n9LB4` / pub `028f88c9…`
(placeholder, kept for testnet - see [[sequentia-founder-treasury-wallet]]). Its
BLS registration is baked into genesis, so it is committee member #1 from block 1.

## 4. Re-issue the 5 assets (NO WBTC), all REISSUABLE

From the founder/treasury wallet, issue GOLD, USDX, EURX, SILVR, OILX - each
reissuable (keep the reissuance token, re-blind it before spending, see
[[sequentia-reissuance-needs-blinded-token]]). Record every NEW asset id; they
change with the genesis and every downstream system keys off them.

## 5. Fund treasury + faucet

Treasury = the founder's genesis remainder (~399M tSEQ). Faucet wallet
(`livetest`/`treasury-clean`, node-gw) holds a large tSEQ balance + the
reissuance tokens. Verify mints hit the mempool.

## 6. Repopulate every downstream system with the NEW asset ids

(Each keys off the asset ids from step 4 - do this AFTER issuance.)

- **Mock price API** (`/root/price-demo/mock-price-api.py`): 5 assets, DROP WBTC,
  but KEEP a `BTC` price (~64000) for "real" testnet4 BTC (user directive).
- **Price server** (`/root/price-demo/gen-price-config.py`): node_rpcs + asset
  list, drop WBTC; regenerate config.json; restart `seq-mock-price-api` +
  `seq-price-server`; confirm dexnode `getfeeexchangerates` matches node000.
- **Asset registry** (served at `159.195.15.140/registry`): new ids + metadata.
- **Faucet** (`sequentia-explorer/serve-public.js`, FAUCET_WALLET): new native/asset refs.
- **DEX** (`~/seqdex`): daemon asset refs, SEQDEX_NODE_RPC, dexnode fee whitelist,
  re-fund the 15 same-chain + 6 cross-chain BTC↔asset markets, MM wallets.
- **Compages bridge**: USDX id + vault config.
- **SWK web wallet** + **Ambra** + **explorer** + **desktop GUI**: asset meta/registry.

## 7. Bring the rest of the committee + services up SLOWLY

Start the remaining committee nodes in small batches (watch the box load and the
anchor status settle between batches), then the services.

**LN backend (user directive 2026-07-04):** give the LN/LSP daemons their OWN
`txindex=1` node - do NOT let them piggyback on the DEX maker's node (`:18300`).
Boot a dedicated txindex node for the LN backend.

## 8. Update memories

New genesis hash `ddd11d54`, the new asset ids, and where each wallet's funds
live - update [[sequentia-testnet-reset-2026-06]] and
[[sequentia-founder-treasury-wallet]].
