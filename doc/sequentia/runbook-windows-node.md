# Runbook - running a Sequentia testnet node on Windows (stake, create assets, run asset tests)

This is a complete, do-it-yourself guide for a tester (e.g. Alberto) to run a
Sequentia node on a Windows laptop, join the shared testnet, stake tSEQ, issue
their own assets, and exercise the asset/fee features by hand. It assumes no
prior Sequentia experience. Commands are shown for the **Windows Command Prompt
(cmd.exe)**; in PowerShell they work the same, only the line-continuation and
`%VAR%` syntax differ.

> Naming: the binaries are `elementsd.exe` (the node) and `elements-cli.exe`
> (the command-line tool). Sequentia is an Elements-based chain, so the programs
> keep the Elements names.

---

## A. What the testnet operator gives you

Before you start, the person running the testnet (the "operator") gives you
**two things**:

| # | Item | Looks like | Used for |
|---|------|-----------|----------|
| 1 | The installer | `SequentiaTestnetSetup-x.y.z.exe` (or a ZIP of the binaries), built from the current branch (post-2026-07-05 re-genesis) | installing the node |
| 2 | Your tSEQ | sent to an address you generate in §4, **after** your node is synced | staking, fees, asset issuance |

The network endpoints are built into the node: on `chain=test` it defaults to
the shared gateway as a peer (`addnode=159.195.15.140:18444`) and to a shared
Bitcoin testnet4 RPC endpoint for anchor validation. If the operator gives you
your own peer or testnet4 endpoint, set those in §2 instead - an explicit
value always overrides the built-in default. The operator sends your tSEQ
(item 2) once you confirm your node is synced (end of §3).

---

## B. System requirements

- Windows 10 or 11, 64-bit.
- ~5 GB free disk (chain data is small today but grows; leave headroom).
- A stable internet connection. The node should stay running to stake reliably.
- Administrator rights to run the installer once.

---

## 1. Install

**Installer path (recommended).** Double-click `SequentiaTestnetSetup-x.y.z.exe`
and accept the defaults. It installs to `C:\Program Files\Sequentia\` and creates
a data directory at `%APPDATA%\Elements\` with a starter `elements.conf`.

**Manual path (ZIP).** If you were given a ZIP instead: unzip it to a folder of
your choice (e.g. `C:\Sequentia\`), and create the data directory yourself:

```
mkdir "%APPDATA%\Elements"
```

Add the install folder to your `PATH` for convenience, or just `cd` into it
before running commands.

To check the binaries run:

```
elements-cli.exe --version
```

---

## 2. Configure

Open the config file in Notepad:

```
notepad "%APPDATA%\Elements\elements.conf"
```

Paste in the following and **choose your own `rpcpassword`**:

```ini
chain=test

[test]
# --- consensus params: now the chain=test defaults (issue #3 fix); shown
#     explicitly so the config is self-documenting and correct on older binaries ---
pospubliccommittee=1
poscommitteesize=250

# --- your node's local RPC (used by elements-cli on this machine only) ---
server=1
rpcuser=alberto
rpcpassword=<choose-a-long-random-password>
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=18201

# --- OPTIONAL: your own peer / Bitcoin testnet4 anchor source. The node
# defaults both to the shared public-testnet endpoints; only set these if the
# operator gave you your own, or if you run your own Bitcoin testnet4 node
# (see §2b - the recommended fallback when the shared endpoint is unreachable).
# Explicit values override the defaults.
#addnode=<SEQ_PEER_HOST:PORT>
#mainchainrpchost=<TESTNET4_RPC_HOST>
#mainchainrpcport=<TESTNET4_RPC_PORT>
#mainchainrpcuser=<TESTNET4_RPC_USER>
#mainchainrpcpassword=<TESTNET4_RPC_PASSWORD>
```

Save and close. (Leave the staking lines out for now - you add them in §5,
*after* you have a funded staking output.)

> Why `pospubliccommittee` and `poscommitteesize`? They are network-wide
> consensus rules of the current chain (the 2026-07-05 re-genesis runs the
> public fixed-size committee, cap 250). As of the issue #3 fix these ARE the
> binary's `chain=test` defaults (they were always pinned on the mainnet chain),
> so a bare config now reaches consensus. On earlier binaries a node without them
> rejected the network's blocks — silently: peers stay connected, but every
> header fails `block-proof-invalid` and the tip never moves. Keep them in the
> config anyway: it is self-documenting, stays correct on any binary, and if you
> ever rewrite this file from scratch (e.g. while debugging the mainchain RPC
> settings) it guarantees these two lines survive.

> Why a Bitcoin RPC? Every Sequentia block commits a Bitcoin testnet4 block as
> its anchor; your node checks that anchor against testnet4, and when you stake
> it anchors the blocks *you* produce to fresh testnet4 blocks. Without it the
> node can't validate the tip or produce. See `doc/sequentia/03-bitcoin-anchoring.md`.

---

## 2b. Running your own Bitcoin testnet4 node (Windows)

If the shared testnet4 RPC endpoint is unreachable (or you simply want your own
anchor source — the more robust setup), run Bitcoin Core locally on testnet4
and point the `mainchainrpc*` options at it. Three Windows-specific pitfalls
make this the single most error-prone step of the whole setup, so follow this
section exactly.

**1. Install and start Bitcoin Core on testnet4.** Download Bitcoin Core
(v28 or later) from bitcoincore.org and launch it on the testnet4 chain:

```
"C:\Program Files\Bitcoin\bitcoin-qt.exe" -testnet4
```

**2. Find the REAL data directory before writing bitcoin.conf.** Older guides
say the Windows data directory is `%APPDATA%\Bitcoin` (a `Roaming` path).
**Recent Bitcoin Core versions default fresh installs to `%LOCALAPPDATA%\Bitcoin`
instead** (a `Local` path), and the GUI's first-run dialog stores whatever you
picked there in the registry — not in any file you can grep. A `bitcoin.conf`
placed in `%APPDATA%\Bitcoin` is then **silently ignored**: the node runs,
syncs, and looks healthy, but none of your settings (in particular `server=1`)
apply, and the RPC port never opens. The reliable way to locate the directory
in use: in bitcoin-qt open **Window → Information (or Help → Debug window)**
and read the *Datadir* field, or **Settings → Options → Open Configuration
File**, which creates/opens the `bitcoin.conf` the node will actually read.

**3. Write this into that bitcoin.conf** (choose your own password):

```ini
testnet4=1
server=1
rpcuser=<user>
rpcpassword=<choose-a-long-random-password>
rpcallowip=127.0.0.1

[testnet4]
rpcport=48400
```

Without `server=1` bitcoin-qt does not serve RPC at all (`bitcoind` does).
Note that `rpcport` must sit under the `[testnet4]` section header; in the
global section it would apply to mainnet only. Restart bitcoin-qt after
editing — the file is read once at startup. Verify the RPC is up:

```
curl.exe -s -u <user>:<password> --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"t\",\"method\":\"getblockchaininfo\",\"params\":[]}" -H "content-type: text/plain;" http://127.0.0.1:48400/
```

You should get a JSON reply with `"chain": "testnet4"`. "Failed to connect"
means the RPC is not listening — almost always the wrong-datadir pitfall above.

**4. Point Sequentia at it** in `elements.conf` (§2):

```ini
mainchainrpchost=127.0.0.1
mainchainrpcport=48400
mainchainrpcuser=<user>
mainchainrpcpassword=<the-same-password>
```

**5. Let Bitcoin Core finish its initial sync before expecting Sequentia to
sync.** Sequentia blocks anchor to *recent* testnet4 blocks. While your local
Bitcoin Core is still in initial block download it does not yet know those
blocks, so your Sequentia node rejects every incoming header with
`anchor-unknown` ("not (yet) known to the parent chain daemon; rejecting block
for now" in the log). This is deliberate and transient — nothing is banned and
the headers are re-accepted automatically — but the practical effect is that
**Sequentia stays at 0 blocks until `getblockchaininfo` on the Bitcoin node
shows `"initialblockdownload": false`**. Testnet4 is small (well under 10 GB);
first sync typically takes under an hour on a normal connection.

Disk note: a full testnet4 node currently needs ~10 GB. If that is a problem,
add `prune=2000` to the bitcoin.conf above (limits block storage to ~2 GB).
Pruning is fine for anchor validation, which only queries headers/tip.

---

## 3. Start the node and verify it syncs

Start the daemon (it runs in the background):

```
elementsd.exe -daemon
```

Watch it find the network and catch up:

```
elements-cli.exe getblockchaininfo
elements-cli.exe getconnectioncount
elements-cli.exe getpeerinfo
```

You're synced when `getblockchaininfo` shows `"initialblockdownload": false` and
`blocks` equals the height the operator reports for the tip. Confirm the committee
view too:

```
elements-cli.exe getposschedule
elements-cli.exe getstakerinfo
elements-cli.exe getanchorstatus
```

`getanchorstatus` should read `"anchorstatus": "ok"` - that confirms your
testnet4 RPC is reachable and the tip's anchor checks out. **Tell the operator
once you're synced; they'll send your tSEQ.**

Stop the node any time with `elements-cli.exe stop`; restart with
`elementsd.exe -daemon`.

---

## 4. Create a wallet and receive your tSEQ

Create a wallet (use a legacy wallet - it makes the staking key step in §5
straightforward):

```
elements-cli.exe -named createwallet wallet_name=main descriptors=false
```

Generate an address and give it to the operator to receive your tSEQ:

```
elements-cli.exe -rpcwallet=main getnewaddress
```

When the tSEQ arrives, confirm it:

```
elements-cli.exe -rpcwallet=main getbalance "*"
```

You should see a `"bitcoin"` balance (that's the policy asset, the Sequence token (SEQ) - it carries
the Elements default label "bitcoin"). You can also see individual coins with
`elements-cli.exe -rpcwallet=main listunspent`.

---

## 5. Become a staker

Staking is **opt-in**: holding tSEQ does nothing on its own. You must lock some
tSEQ into a **staking output**, after which its weight counts continuously (for
as long as the output stays unspent - indefinitely; the lock is only the
*unbonding delay* before you could withdraw). See
`doc/sequentia/04-proof-of-stake.md` §2 and `05-operating-sequentia.md` §8.

Registering stake takes three ingredients, all from your own node:

1. **A staker key.** Generate one and note its pubkey and WIF (private key):
   ```
   elements-cli.exe -rpcwallet=main getnewaddress
   elements-cli.exe -rpcwallet=main getaddressinfo <that-address>
   elements-cli.exe -rpcwallet=main dumpprivkey <that-address>
   ```
   (`getaddressinfo` shows the `pubkey`; `dumpprivkey` the WIF.)
2. **Your committee BLS registration**, derived from the staker key (the
   public committee names its signers by their registered BLS keys):
   ```
   elements-cli.exe getblsregistration "<staker-WIF>"
   ```
   Note the `blspubkey` and `pop` values it returns.
3. **The staking script** for that key with the ~15-day unbonding lock:
   ```
   elements-cli.exe getstakescript "<staker-pubkey>" null 1296384 <blspubkey> <pop>
   ```
   It returns the `script` (hex) your stake must pay to.

The staking script is a **bare script with no address form**, so an ordinary
`sendtoaddress` cannot pay it; the funding transaction has to be built raw
(the repo tool `contrib/sequentia/bootstrap-autonomous-testnet.py` shows the
construction). The practical path for a Windows tester today is to send the
`script` hex from step 3 to the operator together with the amount you want to
stake (e.g. `50000` tSEQ, comfortably above the 40,000-tSEQ minimum): the
operator funds the script from your tSEQ with their tooling and returns the
**registration txid**. Keep the staker WIF from step 1 - you'll put it in the
config next.

Wait for that txid to confirm (the live committee mines it within a block or
two):

```
elements-cli.exe -rpcwallet=main gettransaction <txid>
elements-cli.exe getstakerinfo
```

When your staker's pubkey appears in `getstakerinfo`, you're registered. Now
**enable producing**: stop the node, add these two lines to the `[test]` section
of `elements.conf`, and restart:

```ini
posproducer=1
posproducerkey=<your-staker-WIF-from-step-1>
```

```
elements-cli.exe stop
elementsd.exe -daemon
```

Your node now signs and produces whenever the VRF sortition selects you. Confirm
participation over a few minutes:

```
elements-cli.exe getposschedule
```

Look for your pubkey in the schedule/committee. The more you stake, the more
often you're selected (it's proportional to your share of total stake).

**To stop staking later (unbond):** after the ~15-day lock has matured, spend the
staking output back to a normal address. There's no separate ceremony - that
spend *is* the unbonding. Until you do, it keeps staking.

---

## 6. Create your own assets

Issue an asset (here 1,000,000 units, with a reissuance token so you can mint
more later; `false` = unblinded, which is the testnet default):

```
elements-cli.exe -rpcwallet=main issueasset 1000000 1 false
```

It returns an `asset` id (hex) and a `token` id (the reissuance token). Save the
`asset` id. Confirm it after a block:

```
elements-cli.exe -rpcwallet=main getbalance "*"
elements-cli.exe -rpcwallet=main listissuances
```

Mint more of an existing asset (using the reissuance token you hold):

```
elements-cli.exe -rpcwallet=main reissueasset <asset-id> 500000
```

Send some of your asset to any address:

```
elements-cli.exe -rpcwallet=main -named sendtoaddress address=<dest> amount=10 assetlabel=<asset-id>
```

Destroy (burn) units provably:

```
elements-cli.exe -rpcwallet=main destroyamount <asset-id> 100
```

---

## 7. Run asset / fee tests yourself

Sequentia's distinctive feature is the **open fee market** - a transaction can
pay its fee in *any* asset a block producer prices, not just SEQ. Here's how to
exercise it.

**7.1 Multi-asset transaction.** Send two different assets in one transaction:

```
set A=<asset-id-1>
set B=<asset-id-2>
set D1=<dest-addr-1>
set D2=<dest-addr-2>
elements-cli.exe -rpcwallet=main -named sendmany ^
  amounts="{\"%D1%\":5,\"%D2%\":7}" ^
  output_assets="{\"%D1%\":\"%A%\",\"%D2%\":\"%B%\"}"
```

**7.2 Pay a fee in a different asset.** For a producer to mine a fee paid in your
asset, the producer must *price* that asset. On your own node you set the price
(this controls what *you* accept when you produce a block):

```
elements-cli.exe setfeeexchangerates "{\"<asset-id>\":100000000}"
elements-cli.exe getfeeexchangerates
```

A rate of `100000000` means "1 unit of this asset = 1 SEQ of reference fee
value." A rate of `0` means "refuse this asset for fees." Then a fee-in-asset tx
you build will be mineable by any producer that also prices it (ask the operator
to price your test asset across the committee if you want it mined quickly):

```
elements-cli.exe -rpcwallet=main -named sendtoaddress ^
  address=<dest> amount=5 assetlabel=<asset-id> fee_asset_label=<asset-id>
```

**7.3 Replace-by-fee (RBF), including a fee-asset swap.** Send a replaceable
transaction, then bump it - optionally switching the fee asset:

```
elements-cli.exe -rpcwallet=main -named sendtoaddress ^
  address=<dest> amount=2 replaceable=true fee_rate=1
elements-cli.exe -rpcwallet=main -named bumpfee txid=<txid> ^
  options="{\"fee_rate\":50}"
```

The original is evicted and the replacement (higher fee) takes its place.

**7.4 Confidential (blinded) amounts.** Although the testnet defaults to
transparent amounts, you can still send a *blinded* output by sending to the
confidential form of an address:

```
set ADDR=<a getnewaddress address>
elements-cli.exe -rpcwallet=main getaddressinfo %ADDR%
```

Take the `confidential` field from that output and send to it; the amount on
that output is then hidden (a `valuecommitment` instead of an explicit value,
visible via `getrawtransaction <txid> 1`), while your wallet still tracks it.

**7.5 Watch a Bitcoin reorg flow through (optional).** Because every block is
anchored to Bitcoin testnet4, when testnet4 reorgs, Sequentia follows. Watch the
tip and the anchor over time:

```
elements-cli.exe getblockcount
elements-cli.exe getanchorstatus
elements-cli.exe getcheckpointinfo
```

If testnet4 reorgs a referenced block, you'll see the Sequentia tip roll back and
re-extend, with finality holding modulo the Bitcoin reorg - exactly as designed.

---

## 8. Day-to-day operations & troubleshooting

| Symptom | Check / fix |
|---|---|
| `getconnectioncount` is 0 | the `addnode` host/port (item 2) is wrong or not reachable; confirm with the operator; check Windows Firewall isn't blocking `elementsd.exe`. |
| Stuck in `initialblockdownload` | give it time on first sync; confirm you have a peer (§3) and the testnet4 RPC works (`getanchorstatus`). |
| `getanchorstatus` not `"ok"` | your `mainchainrpc*` settings are wrong/unreachable - fix them in `elements.conf` and restart. |
| Startup error "no valid response was received from the mainchain daemon" | the testnet4 RPC endpoint in `mainchainrpc*` is down or refusing the connection. If it points at your own Bitcoin Core, test it with the `curl.exe` command in §2b - if that fails too, it's the wrong-datadir pitfall (§2b step 2) or a missing `server=1`. |
| Peer connected but `blocks`/`headers` stay at 0 | your own Bitcoin testnet4 node is still in initial block download, so anchors can't be verified yet (`anchor-unknown` in the log). Wait until Bitcoin Core's `getblockchaininfo` shows `"initialblockdownload": false` (§2b step 5). |
| Peer connected, still 0 blocks, and the log (with `-debug=validation`) shows `block-proof-invalid, proof of work failed` / `Misbehaving: ... invalid header received` | your `elements.conf` is missing the consensus params from §2 (`pospubliccommittee=1`, `poscommitteesize=250` under `[test]`). Without them the node expects a different committee-certificate form and rejects every network block as invalid. Nothing is logged at the default level, which makes this the most silent failure in this table. Add the two lines and restart. |
| `getstakescript` says the lock "is below the chain's minimum" | raise the `csv_seconds` argument to at least `1296000` (~15 days). |
| Your staker never appears in `getposschedule` | confirm the registration tx is mined (`getstakerinfo`), that `posproducer=1` and `posproducerkey` are set, and the node has been restarted. |
| A fee-in-asset tx won't confirm | no producer prices that asset; ask the operator to price it across the committee. |
| Node won't start after edits | run `elementsd.exe` (without `-daemon`) to see the error in the console. |

**Useful introspection RPCs:** `getblockchaininfo`, `getposschedule`,
`getstakerinfo`, `getanchorstatus`, `getcheckpointinfo`, `getbalance "*"`,
`listunspent`, `listissuances`, `getfeeexchangerates`.

Full design references: proof-of-stake `doc/sequentia/04-proof-of-stake.md`,
fee market `02-open-fee-market.md`, anchoring `03-bitcoin-anchoring.md`,
operating guide `05-operating-sequentia.md`.
