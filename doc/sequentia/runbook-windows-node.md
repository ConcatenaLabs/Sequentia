# Runbook — running a Sequentia testnet node on Windows (stake, create assets, run asset tests)

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
**four things**. Keep them handy — you'll paste them into the config in §2.

| # | Item | Looks like | Used for |
|---|------|-----------|----------|
| 1 | The installer | `SequentiaTestnetSetup-x.y.z.exe` (or a ZIP of the binaries) | installing the node |
| 2 | A Sequentia peer to connect to | `addnode` = `host:port`, e.g. `seqtest.example.com:18444` | finding the testnet |
| 3 | A Bitcoin testnet4 RPC endpoint | host / port / user / password | validating the Bitcoin anchors and producing blocks |
| 4 | Your tSEQ | sent to an address you generate in §4, **after** your node is synced | staking, fees, asset issuance |

Items 2 and 3 are the only network-specific values; everything else is baked
into the installer's default config. The operator sends your tSEQ (item 4) once
you confirm your node is synced (end of §3).

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

Paste in the following, then **fill the three `<...>` values** from §A and
**choose your own `rpcpassword`**:

```ini
chain=test

[test]
# --- consensus params: these MUST match the testnet (do not change) ---
poscommitteesize=100
acceptnonstdtxn=1
parentgenesisblockhash=00000000da84f2bafbbc53dee25a72ae507ff4914b867c565be350b0da8bf043

# --- your node's local RPC (used by elements-cli on this machine only) ---
server=1
rpcuser=alberto
rpcpassword=<choose-a-long-random-password>
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=18201

# --- peer-to-peer: connect to the testnet ---
listen=1
discover=0
dnsseed=0
upnp=0
addnode=<SEQ_PEER_HOST:PORT>          # item 2 from §A

# --- Bitcoin testnet4 anchor source (item 3 from §A) ---
con_bitcoin_anchor=1
validateanchor=1
anchorminconf=1
anchorpollinterval=15
mainchainrpchost=<TESTNET4_RPC_HOST>
mainchainrpcport=<TESTNET4_RPC_PORT>
mainchainrpcuser=<TESTNET4_RPC_USER>
mainchainrpcpassword=<TESTNET4_RPC_PASSWORD>
```

Save and close. (Leave the staking lines out for now — you add them in §5,
*after* you have a funded staking output.)

> Why the Bitcoin RPC? Every Sequentia block commits a Bitcoin testnet4 block as
> its anchor; your node checks that anchor against testnet4, and when you stake
> it anchors the blocks *you* produce to fresh testnet4 blocks. Without it the
> node can't validate the tip or produce. See `doc/sequentia/03-bitcoin-anchoring.md`.

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

`getanchorstatus` should read `"anchorstatus": "ok"` — that confirms your
testnet4 RPC is reachable and the tip's anchor checks out. **Tell the operator
once you're synced; they'll send your tSEQ.**

Stop the node any time with `elements-cli.exe stop`; restart with
`elementsd.exe -daemon`.

---

## 4. Create a wallet and receive your tSEQ

Create a wallet (use a legacy wallet — it makes the staking key step in §5
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

You should see a `"bitcoin"` balance (that's the policy asset, the Sequence token (SEQ) — it carries
the Elements default label "bitcoin"). You can also see individual coins with
`elements-cli.exe -rpcwallet=main listunspent`.

---

## 5. Become a staker

Staking is **opt-in**: holding tSEQ does nothing on its own. You must lock some
tSEQ into a **staking output**, after which its weight counts continuously (for
as long as the output stays unspent — indefinitely; the lock is only the
*unbonding delay* before you could withdraw). See
`doc/sequentia/04-proof-of-stake.md` §2 and `05-operating-sequentia.md` §8.

A staking output pays to a special script (`getstakescript`) that an ordinary
`sendtoaddress` can't target, so use the provided helper, which builds, funds,
signs and broadcasts it for you and prints the staker key:

```
register-stake.exe --rpcport 18201 --rpcuser alberto --rpcpassword <yours> ^
                   --wallet main --amount 50000
```

(`50000` tSEQ is well above the 40,000-SEQ minimum stake; adjust as you like.)
It prints two things:

- the **staker WIF** (private key) — you'll put this in the config next, and
- the **registration txid**.

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
posproducerkey=<your-staker-WIF-from-register-stake>
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
staking output back to a normal address. There's no separate ceremony — that
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

Sequentia's distinctive feature is the **open fee market** — a transaction can
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
transaction, then bump it — optionally switching the fee asset:

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
re-extend, with finality holding modulo the Bitcoin reorg — exactly as designed.

---

## 8. Day-to-day operations & troubleshooting

| Symptom | Check / fix |
|---|---|
| `getconnectioncount` is 0 | the `addnode` host/port (item 2) is wrong or not reachable; confirm with the operator; check Windows Firewall isn't blocking `elementsd.exe`. |
| Stuck in `initialblockdownload` | give it time on first sync; confirm you have a peer (§3) and the testnet4 RPC works (`getanchorstatus`). |
| `getanchorstatus` not `"ok"` | your `mainchainrpc*` settings (item 3) are wrong/unreachable — fix them in `elements.conf` and restart. |
| `register-stake` says "below the chain's minimum" | raise `--amount` above 40,000, or pass `--csv-seconds 1296000`. |
| Your staker never appears in `getposschedule` | confirm the registration tx is mined (`getstakerinfo`), that `posproducer=1` and `posproducerkey` are set, and the node has been restarted. |
| A fee-in-asset tx won't confirm | no producer prices that asset; ask the operator to price it across the committee. |
| Node won't start after edits | run `elementsd.exe` (without `-daemon`) to see the error in the console. |

**Useful introspection RPCs:** `getblockchaininfo`, `getposschedule`,
`getstakerinfo`, `getanchorstatus`, `getcheckpointinfo`, `getbalance "*"`,
`listunspent`, `listissuances`, `getfeeexchangerates`.

Full design references: proof-of-stake `doc/sequentia/04-proof-of-stake.md`,
fee market `02-open-fee-market.md`, anchoring `03-bitcoin-anchoring.md`,
operating guide `05-operating-sequentia.md`.
