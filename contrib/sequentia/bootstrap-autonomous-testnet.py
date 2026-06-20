#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bring up an N-node autonomous Sequentia committee the way MAINNET actually
launches: from a single genesis founder, through escaping-stall solo-mining, to
a self-running quorum — anchored to a real Bitcoin parent.

Unlike run-local-testnet.py (which pre-seeds all N stakers in genesis as a
shortcut), this reproduces the real bootstrap economics:

  1. Genesis seeds exactly ONE staker — the founder — via -con_genesis_stake,
     plus a spendable -initialfreecoins output. No -staker config.
  2. The founder is below quorum, so it can only produce ESCAPING-STALL blocks
     (sub-quorum, leader-only), and only once the Bitcoin anchor has advanced
     >= POS_ESCAPING_STALL_ANCHOR_GAP (3) since the previous block.
  3. The founder spends its initialfreecoins output into one CSV-locked staking
     output per new staker (getstakescript) — funding+registering them on-chain —
     and solo-mines ONE escaping-stall block (generateposblock) to confirm them.
  4. The N-1 new staker nodes come online (one key each, -posproducer -posbls).
     Now a quorum of registered stakers exists, so the committee certifies full
     blocks autonomously over gossip — the founder stops solo-mining.

Anchoring (the parent "Bitcoin"): --local-parent spawns+mines a throwaway parent
(self-contained smoke test, advances on demand so escaping stall triggers fast);
or point at an external node with --parent-rpc*/--parent-genesis (Bitcoin testnet
via a local proxy: genesis 000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943).
Against a real parent, step 3 waits for Bitcoin to advance 3 blocks — that wait
is the escaping-stall rule doing its job, exactly as on mainnet.

  --stop --basedir <dir>   tears a running network down.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from decimal import Decimal

# Reuse the daemon/config plumbing from the sibling launcher, and the test
# framework's tx + key helpers (same pattern as bootstrap-committee.py).
HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO_ROOT, "test", "functional"))
import importlib.util
_spec = importlib.util.spec_from_file_location("rlt", os.path.join(HERE, "run-local-testnet.py"))
rlt = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(rlt)
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut, CTxOutAsset  # noqa: E402
from test_framework.script import CScript                                                # noqa: E402

CHAIN = rlt.CHAIN
COIN = 100_000_000
BITCOIN_TESTNET3_GENESIS = rlt.BITCOIN_TESTNET3_GENESIS
cli, start_daemon, wait_rpc, write_conf, make_staker, _read_pid = (
    rlt.cli, rlt.start_daemon, rlt.wait_rpc, rlt.write_conf, rlt.make_staker, rlt._read_pid)


def jcli(*a, **k):
    out, rc = cli(*a, **k)
    return (json.loads(out) if (rc == 0 and out) else None), rc


def asset_bytes_for(ec, d, port, asset_hex):
    """Policy-asset serialization byte order, via a decode round-trip."""
    for cand in (bytes.fromhex(asset_hex), bytes.fromhex(asset_hex)[::-1]):
        a = CTxOutAsset(); a.setToAsset(cand)
        tx = CTransaction(); tx.nVersion = 2
        tx.vout = [CTxOut(COIN, CScript([0x51]), nAsset=a)]
        dec, _ = jcli(ec, d, port, ["decoderawtransaction", tx.serialize().hex()])
        if dec and dec["vout"][0].get("asset") == asset_hex:
            return cand
    raise RuntimeError("could not match policy asset byte order")


def find_optrue_output(ec, d, port):
    """The spendable -initialfreecoins genesis output (script OP_TRUE = '51').
    Polls briefly: right after startup the genesis UTXO may not be queryable yet."""
    for _ in range(20):
        g0, _ = cli(ec, d, port, ["getblockhash", "0"])
        gb, _ = jcli(ec, d, port, ["getblock", g0, "2"])
        if gb:
            for tx in gb["tx"]:
                for v in tx["vout"]:
                    spk = v["scriptPubKey"]
                    if spk.get("hex") == "51" and Decimal(str(v["value"])) > 0:
                        utxo, _ = cli(ec, d, port, ["gettxout", tx["txid"], str(v["n"])], check=False)
                        if utxo:
                            return {"txid": tx["txid"], "vout": v["n"],
                                    "value": Decimal(str(v["value"])), "asset": v["asset"]}
        time.sleep(1)
    raise RuntimeError("no spendable OP_TRUE initialfreecoins output in genesis")


def to_atoms(v):
    return int((Decimal(v) * COIN).to_integral_value())


def stop_all(base, elementsd):
    rlt.stop_all(base, elementsd)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nodes", type=int, default=100, help="committee size N = founder + (N-1) registered (default 100)")
    ap.add_argument("--slot", type=int, default=10)
    ap.add_argument("--basedir", default=os.path.abspath("./seq-bootstrap"))
    ap.add_argument("--bindir", default=os.path.join(REPO_ROOT, "src"))
    ap.add_argument("--p2p-base", type=int, default=18000)
    ap.add_argument("--rpc-base", type=int, default=18200)
    ap.add_argument("--rpcuser", default="seq")
    ap.add_argument("--rpcpassword", default="seq")
    ap.add_argument("--stake-seq", default="1", help="SEQ locked per staker (default 1)")
    ap.add_argument("--csv-seconds", type=int, default=1_300_000, help="unbonding CSV lock per staking output")
    ap.add_argument("--posunbonding", type=int, default=10, help="min unbonding period (blocks)")
    ap.add_argument("--posminstake", type=int, default=0, help="min stake atoms (0 = no floor)")
    ap.add_argument("--local-parent", action="store_true")
    ap.add_argument("--parent-rpchost", default="127.0.0.1")
    ap.add_argument("--parent-rpcport", type=int, default=0)
    ap.add_argument("--parent-rpcuser", default="")
    ap.add_argument("--parent-rpcpassword", default="")
    ap.add_argument("--parent-genesis", default="")
    ap.add_argument("--anchorpoll", type=int, default=0)
    ap.add_argument("--run-seconds", type=int, default=0)
    ap.add_argument("--stop", action="store_true")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(line_buffering=True)   # live progress when piped to a log
    except Exception:
        pass

    elementsd = os.path.join(args.bindir, "elementsd")
    ec = os.path.join(args.bindir, "elements-cli")
    for b in (elementsd, ec):
        if not os.path.exists(b):
            sys.exit("missing binary: %s" % b)
    if args.stop:
        stop_all(args.basedir, elementsd); return

    N = args.nodes
    base = args.basedir
    if os.path.exists(base):
        sys.exit("basedir %s exists; --stop or remove it first" % base)
    os.makedirs(base)
    pids = {}
    rb, pb = args.rpc_base, args.p2p_base
    poll = args.anchorpoll or max(2, args.slot // 2)
    stake_atoms = to_atoms(args.stake_seq)

    # ---- Parent ("Bitcoin") --------------------------------------------------
    parent_miner = None     # (datadir, rpcport, addr) for local; None for external
    def advance_parent(blocks):
        if parent_miner:
            pdir, prpc, addr = parent_miner
            cli(ec, pdir, prpc, ["generatetoaddress", str(blocks), addr],
                args.rpcuser, args.rpcpassword, check=False, timeout=120)

    def parent_tip():
        if parent_miner:
            pdir, prpc, _ = parent_miner
            out, _ = cli(ec, pdir, prpc, ["getblockcount"], args.rpcuser, args.rpcpassword, check=False)
            return int(out) if out.isdigit() else -1
        return None  # external: we don't poll it here

    if args.local_parent:
        pdir = os.path.join(base, "parent")
        os.makedirs(os.path.join(pdir, CHAIN))
        p_rpc = rb - 2
        write_conf(os.path.join(pdir, "elements.conf"), [
            "chain=%s" % CHAIN, "[%s]" % CHAIN, "server=1",
            "rpcuser=%s" % args.rpcuser, "rpcpassword=%s" % args.rpcpassword,
            "port=%d" % (pb - 2), "rpcport=%d" % p_rpc,
            "bind=127.0.0.1", "rpcbind=127.0.0.1", "rpcallowip=127.0.0.1",
            "listen=1", "discover=0", "dnsseed=0", "validatepegin=0", "initialfreecoins=0",
            # Serve many committee nodes' anchor checks without 503 ("work queue
            # exceeded"): a real Bitcoin parent / the caching proxy does this too.
            "rpcthreads=32", "rpcworkqueue=4096",
            "con_blocksubsidy=5000000000", "anyonecanspendaremine=1", "signblockscript=51",
            "fallbackfee=0.0001",
        ])
        print("Starting local parent (throwaway Bitcoin stand-in)...")
        start_daemon(elementsd, pdir); wait_rpc(ec, pdir, p_rpc, "parent")
        cli(ec, pdir, p_rpc, ["-named", "createwallet", "wallet_name=w", "descriptors=true"], args.rpcuser, args.rpcpassword)
        addr, _ = cli(ec, pdir, p_rpc, ["getnewaddress"], args.rpcuser, args.rpcpassword)
        cli(ec, pdir, p_rpc, ["generatetoaddress", "25", addr], args.rpcuser, args.rpcpassword, timeout=120)
        parent_genesis, _ = cli(ec, pdir, p_rpc, ["getblockhash", "0"], args.rpcuser, args.rpcpassword)
        parent_miner = (pdir, p_rpc, addr)
        pids["parent"] = {"datadir": pdir, "rpcport": p_rpc, "pid": _read_pid(pdir)}
        anchor = ["con_bitcoin_anchor=1", "validateanchor=1", "anchorminconf=1",
                  "anchorpollinterval=%d" % poll,
                  "mainchainrpchost=127.0.0.1", "mainchainrpcport=%d" % p_rpc,
                  "mainchainrpcuser=%s" % args.rpcuser, "mainchainrpcpassword=%s" % args.rpcpassword,
                  "parentgenesisblockhash=%s" % parent_genesis]
    else:
        if not (args.parent_rpcport and args.parent_genesis):
            sys.exit("external anchor needs --parent-rpcport and --parent-genesis (or --local-parent)")
        parent_genesis = args.parent_genesis
        anchor = ["con_bitcoin_anchor=1", "validateanchor=1", "anchorminconf=1",
                  "anchorpollinterval=%d" % poll,
                  "mainchainrpchost=%s" % args.parent_rpchost, "mainchainrpcport=%d" % args.parent_rpcport,
                  "mainchainrpcuser=%s" % args.parent_rpcuser, "mainchainrpcpassword=%s" % args.parent_rpcpassword,
                  "parentgenesisblockhash=%s" % parent_genesis]
        print("Anchoring to external parent %s:%d" % (args.parent_rpchost, args.parent_rpcport))

    # ---- Keys + shared consensus block (founder genesis stake, NO -staker) ----
    founder_wif, founder_pub = make_staker()
    members = [make_staker() for _ in range(N - 1)]           # the rest, registered on-chain
    json.dump({"founder": {"wif": founder_wif, "pub": founder_pub},
               "members": [{"wif": w, "pub": p} for w, p in members]},
              open(os.path.join(base, "committee_keys.json"), "w"), indent=2)
    fund_total = (N + 4) * stake_atoms                        # founder change + fee headroom
    # Stake locks must meet the chain's unbonding minimum (posunbonding * slot
    # seconds). Bump --csv-seconds to clear it, and lock the founder's GENESIS
    # stake with the same BIP68 *time-based* sequence (a height-based count fails
    # once posunbonding exceeds it).
    min_csv = max(1, args.posunbonding * args.slot)
    if args.csv_seconds < min_csv:
        args.csv_seconds = min_csv + 512
        print("  (raised --csv-seconds to %d to clear posunbonding*slot)" % args.csv_seconds)
    genesis_csv_seq = (1 << 22) | ((args.csv_seconds + 511) // 512)   # BIP68 time-based
    consensus = [
        "con_pos=1", "posvrf=1", "posbls=1",
        "poscommitteesize=%d" % N, "posslotinterval=%d" % args.slot,
        "posunbonding=%d" % args.posunbonding, "posminstake=%d" % args.posminstake,
        # The BLS committee certificate costs ~257 bytes per signing member; size
        # the cap for the whole committee signing (quorum..N) plus headroom, or a
        # 100-member cert (~13-26 KB) is rejected as block-proof-invalid.
        "con_max_block_sig_size=%d" % (280 * N + 2000),
        "signblockscript=51",
        "con_blocksubsidy=5000000000", "anyonecanspendaremine=1", "validatepegin=0",
        # The CSV staking outputs are non-standard; accept them into the mempool
        # so the founder can mine the registration tx (relay policy, not consensus).
        "acceptnonstdtxn=1",
        "con_bitcoin_anchor=1",
        "con_genesis_stake=%s:%d:%d" % (founder_pub, stake_atoms, genesis_csv_seq),
        "con_connect_genesis_outputs=1", "initialfreecoins=%d" % fund_total,
    ]

    def p2p(i): return pb + i
    hubs = list(range(min(4, N)))
    def peers_for(i):
        s = set()
        if N > 1: s.add((i + 1) % N); s.add((i - 1) % N)
        s.update(h for h in hubs if h != i)
        if i not in hubs and hubs: s.add(hubs[i % len(hubs)])
        return sorted(s)

    def node_conf(i, wif):
        local = ["server=1", "rpcuser=%s" % args.rpcuser, "rpcpassword=%s" % args.rpcpassword,
                 "port=%d" % p2p(i), "rpcport=%d" % (rb + i),
                 "bind=127.0.0.1", "rpcbind=127.0.0.1", "rpcallowip=127.0.0.1",
                 "listen=1", "discover=0", "dnsseed=0", "upnp=0",
                 "maxconnections=64", "posproducer=1", "posproducerkey=%s" % wif] \
                + anchor + ["addnode=127.0.0.1:%d" % p2p(j) for j in peers_for(i)]
        d = os.path.join(base, "node%03d" % i)
        os.makedirs(os.path.join(d, CHAIN))
        write_conf(os.path.join(d, "elements.conf"),
                   ["chain=%s" % CHAIN, "[%s]" % CHAIN] + consensus + [""] + local)
        return d

    # ---- Phase 1: start the founder ALONE ------------------------------------
    print("=== Phase 1: founder genesis (node000) ===")
    fdir = node_conf(0, founder_wif)
    start_daemon(elementsd, fdir); wait_rpc(ec, fdir, rb, "founder")
    pids["node000"] = {"datadir": fdir, "rpcport": rb, "pid": _read_pid(fdir)}
    json.dump(pids, open(os.path.join(base, "pids.json"), "w"))
    reg, _ = jcli(ec, fdir, rb, ["getstakerinfo"])
    print("  founder registered from genesis stake: %s" % (founder_pub in (reg or {})))
    print("  committee size N=%d, quorum=%d, slot=%ds" % (N, N // 2 + 1, args.slot))

    # ---- Phase 2: founder funds + registers the other N-1 stakers ------------
    print("=== Phase 2: fund + register %d stakers from initialfreecoins ===" % (N - 1))
    fund = find_optrue_output(ec, fdir, rb)
    asset_raw = asset_bytes_for(ec, fdir, rb, fund["asset"])
    asset = CTxOutAsset(); asset.setToAsset(asset_raw)
    in_atoms = to_atoms(fund["value"]); fee_atoms = to_atoms("0.001")
    change = in_atoms - (N - 1) * stake_atoms - fee_atoms
    if change < 0:
        sys.exit("initialfreecoins (%s) too small for %d stakers" % (fund["value"], N - 1))
    tx = CTransaction(); tx.nVersion = 2
    tx.vin = [CTxIn(COutPoint(int(fund["txid"], 16), fund["vout"]))]   # OP_TRUE input: empty scriptSig
    for _, pub in members:
        ss, _ = jcli(ec, fdir, rb, ["getstakescript", pub, "null", str(args.csv_seconds)])
        tx.vout.append(CTxOut(stake_atoms, bytes.fromhex(ss["script"]), nAsset=asset))
    tx.vout.append(CTxOut(change, CScript([0x51]), nAsset=asset))      # change back to OP_TRUE
    tx.vout.append(CTxOut(fee_atoms, b"", nAsset=asset))               # explicit fee
    _cmd = [ec, "-datadir=%s" % fdir, "-chain=%s" % CHAIN, "-rpcconnect=127.0.0.1",
            "-rpcport=%d" % rb, "-rpcuser=%s" % args.rpcuser, "-rpcpassword=%s" % args.rpcpassword,
            "-rpcwait", "sendrawtransaction", tx.serialize().hex()]
    _r = subprocess.run(_cmd, capture_output=True, text=True)
    txid, rc = _r.stdout.strip(), _r.returncode
    if rc != 0:
        stop_all(base, elementsd)
        sys.exit("registration tx rejected: %s" % ((_r.stderr or _r.stdout).strip()))
    print("  registration tx broadcast: %s" % txid)

    # ---- Phase 3: solo-mine ONE escaping-stall block to confirm them ---------
    print("=== Phase 3: escaping-stall block (founder solo-mines the registrations) ===")
    if parent_miner:
        advance_parent(4)                                   # local: trigger the >=3 anchor gap now
    else:
        base_tip = None
        a0, _ = jcli(ec, fdir, rb, ["getanchorstatus"])
        start_anchor = (a0 or {}).get("anchorheight", 0)
        print("  waiting for the Bitcoin parent to advance >= 3 blocks (escaping-stall rule)...")
    b1 = None
    deadline = time.time() + (args.run_seconds or 7200)
    while time.time() < deadline:
        out, rc = cli(ec, fdir, rb, ["generateposblock", founder_wif, "[]"], check=False, timeout=60)
        if rc == 0:
            b1 = json.loads(out); break
        time.sleep(5 if parent_miner else 20)
    if not b1:
        stop_all(base, elementsd); sys.exit("could not produce the escaping-stall block (parent reachable / advanced?)")
    print("  block 1: height=%d countersignatures=%d (sub-quorum, founder only)" %
          (b1["height"], b1["countersignatures"]))
    reg, _ = jcli(ec, fdir, rb, ["getstakerinfo"])
    missing = [p for _, p in members if p not in (reg or {})]
    if missing:
        stop_all(base, elementsd); sys.exit("stakers not registered after block 1: %d missing" % len(missing))
    print("  registry now holds %d stakers (founder + %d). Quorum is reachable." % (len(reg), N - 1))

    # ---- Phase 4: bring the committee online -> autonomous full blocks -------
    print("=== Phase 4: starting %d staker nodes; committee goes autonomous ===" % (N - 1))
    for i in range(1, N):
        d = node_conf(i, members[i - 1][0])
        start_daemon(elementsd, d)
        pids["node%03d" % i] = {"datadir": d, "rpcport": rb + i, "pid": _read_pid(d)}
        if (i) % 10 == 0: print("  started %d/%d" % (i, N - 1))
    json.dump(pids, open(os.path.join(base, "pids.json"), "w"))
    for i in range(1, N):
        wait_rpc(ec, pids["node%03d" % i]["datadir"], rb + i, "node%d" % i)
    print("  all %d nodes online; watching for full-committee certification (height >= 2)\n" % N)

    stop_at = time.time() + args.run_seconds if args.run_seconds else None
    try:
        last = -1
        while True:
            if parent_miner: advance_parent(1)
            time.sleep(max(3, args.slot))
            hs = []
            for i in range(N):
                out, rc = cli(ec, pids["node%03d" % i]["datadir"], rb + i, ["getblockcount"], check=False, timeout=15)
                hs.append(int(out) if rc == 0 and out.isdigit() else -1)
            lo, hi = min(hs), max(hs)
            if hi != last:
                cs = ""
                if hi >= 2:
                    bh, _ = cli(ec, fdir, rb, ["getblockhash", str(hi)], check=False)
                    blk, r2 = jcli(ec, fdir, rb, ["getblock", bh], check=False) if bh else (None, 1)
                    # countersignature count isn't in getblock; report agreement + anchor instead
                a, _ = jcli(ec, fdir, rb, ["getanchorstatus"], check=False)
                forked = len({h for h in hs if h >= 0}) > 1 and lo >= 1 and _hash_fork(ec, pids, N, rb, lo)
                print("  height min=%d max=%d  fork=%s  anchor=%s@btc%s" %
                      (lo, hi, "YES" if forked else "no",
                       (a or {}).get("anchorstatus"), (a or {}).get("anchorheight")))
                last = hi
            if stop_at and time.time() >= stop_at:
                print("\n--run-seconds elapsed; stopping."); break
    except KeyboardInterrupt:
        print("\nCtrl-C — stopping...")
    finally:
        stop_all(base, elementsd)


def _hash_fork(ec, pids, N, rb, common):
    if common < 1: return False
    ref = None
    for i in range(N):
        out, rc = cli(ec, pids["node%03d" % i]["datadir"], rb + i, ["getblockhash", str(common)], check=False, timeout=15)
        if rc != 0: continue
        if ref is None: ref = out
        elif out != ref: return True
    return False


if __name__ == "__main__":
    main()
