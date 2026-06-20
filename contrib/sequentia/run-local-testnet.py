#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Stand up a local, Bitcoin-anchored, autonomous Proof-of-Stake Sequentia
network of N real elementsd daemons — the same consensus path a mainnet
operator runs, only with its own genesis committee and pointed at a parent
Bitcoin you choose (real testnet via a proxy, or a throwaway local parent).

What this is (vs. the functional-test demo). The python functional harness in
test/functional/feature_pos_100_node_network.py also starts real daemons, but on
regtest with anchoring OFF and instant slots, orchestrated by the test
framework. THIS script runs standalone daemons with:

  * Bitcoin anchoring ON (-con_bitcoin_anchor=1 -validateanchor=1): every block
    references a parent-chain block; block 1 needs the parent reachable once.
  * a realistic slot interval (default 10s),
  * the autonomous gossip producer (-posproducer -posbls=1): no coordinator —
    each node runs the round-robin re-vote and BLS-aggregates the committee
    certificate itself,
  * a custom-chain genesis whose committee is the N -staker= keys (one per node),

i.e. the faithful mainnet configuration, on one machine.

PARENT (Bitcoin) — two ways:

  --local-parent
      Spawn a throwaway elementsregtest daemon as the "Bitcoin" parent and mine
      it automatically. Self-contained; good for a first run / CI smoke. NOT a
      real anchor to Bitcoin.

  --parent-rpchost H --parent-rpcport P --parent-rpcuser U --parent-rpcpassword W
  --parent-genesis HASH
      Point every node at an external Bitcoin parent. For Bitcoin testnet via a
      local TLS-terminating proxy (the mainchain RPC client speaks plain HTTP +
      Basic auth), set these to the proxy and use the testnet3 genesis hash
      000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943.

Stop a running network with:  run-local-testnet.py --stop --basedir <dir>
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time

# The genesis-staker keys are committee keys; we generate them with the same
# ECKey/WIF helpers the bundled bootstrap tool uses.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO_ROOT, "test", "functional"))
from test_framework.key import ECKey                     # noqa: E402
from test_framework.address import byte_to_base58         # noqa: E402

# Well-known Bitcoin TESTNET3 genesis (for --parent-genesis when anchoring to it).
BITCOIN_TESTNET3_GENESIS = "000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943"
CHAIN = "elementsregtest"   # the Elements custom-chain path that reads -staker / -con_*


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b"\x01", 239)     # 239 = test/regtest WIF prefix
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


def cli(binary, datadir, port, args, user="seq", pw="seq", timeout=60, check=True):
    cmd = [binary, "-datadir=%s" % datadir, "-chain=%s" % CHAIN,
           "-rpcconnect=127.0.0.1", "-rpcport=%d" % port,
           "-rpcuser=%s" % user, "-rpcpassword=%s" % pw, "-rpcwait"] + args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        # -rpcwait blocks until the RPC is ready; a not-yet-ready daemon hits this.
        # Return a soft failure so pollers (wait_rpc) retry instead of the whole
        # run crashing on one slow daemon during a large startup.
        if check:
            raise RuntimeError("cli %s timed out after %ss" % (args, timeout))
        return "", -1
    if check and r.returncode != 0:
        raise RuntimeError("cli %s failed: %s" % (args, r.stderr.strip()))
    return r.stdout.strip(), r.returncode


def wait_rpc(cli_bin, datadir, port, label, deadline_s=300):
    # Poll with a SHORT per-call timeout (fast retry) and a generous overall
    # deadline: starting ~100 daemons against one parent makes RPC warmup slow,
    # but it does come up. One slow node must not abort the run.
    end = time.time() + deadline_s
    while time.time() < end:
        out, rc = cli(cli_bin, datadir, port, ["getblockcount"], check=False, timeout=5)
        if rc == 0 and out.isdigit():
            return
        time.sleep(1)
    raise RuntimeError("%s RPC never came up on port %d" % (label, port))


def write_conf(path, lines):
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def start_daemon(elementsd, datadir):
    subprocess.run([elementsd, "-datadir=%s" % datadir, "-daemon"],
                   capture_output=True, text=True, check=True)


def read_pids(base):
    pf = os.path.join(base, "pids.json")
    return json.load(open(pf)) if os.path.exists(pf) else {}


def stop_all(base, elementsd):
    pids = read_pids(base)
    for name, info in pids.items():
        try:
            cli(elementsd.replace("elementsd", "elements-cli"), info["datadir"],
                info["rpcport"], ["stop"], check=False, timeout=20)
        except Exception:
            pass
    time.sleep(3)
    for name, info in pids.items():
        try:
            os.kill(info["pid"], signal.SIGTERM)
        except Exception:
            pass
    print("Stopped %d daemon(s)." % len(pids))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nodes", type=int, default=100, help="committee size = node count (default 100)")
    ap.add_argument("--slot", type=int, default=10, help="seconds per slot (default 10)")
    ap.add_argument("--basedir", default=os.path.abspath("./seq-testnet"))
    ap.add_argument("--bindir", default=os.path.join(REPO_ROOT, "src"))
    ap.add_argument("--p2p-base", type=int, default=18000)
    ap.add_argument("--rpc-base", type=int, default=18200)
    ap.add_argument("--rpcuser", default="seq")
    ap.add_argument("--rpcpassword", default="seq")
    # Parent / anchor
    ap.add_argument("--local-parent", action="store_true",
                    help="spawn+mine a throwaway local parent (NOT a real Bitcoin anchor)")
    ap.add_argument("--parent-rpchost", default="127.0.0.1")
    ap.add_argument("--parent-rpcport", type=int, default=0)
    ap.add_argument("--parent-rpcuser", default="")
    ap.add_argument("--parent-rpcpassword", default="")
    ap.add_argument("--parent-genesis", default="",
                    help="parent genesis block hash (Bitcoin testnet3: %s)" % BITCOIN_TESTNET3_GENESIS)
    ap.add_argument("--anchorpoll", type=int, default=0,
                    help="seconds between parent-chain polls per node (0 = slot/2). Raise it "
                         "for a big committee against a shared/public parent to avoid rate limits.")
    ap.add_argument("--no-anchor", action="store_true", help="disable anchoring entirely (not mainnet-faithful)")
    ap.add_argument("--stop", action="store_true", help="stop a network previously started in --basedir")
    ap.add_argument("--run-seconds", type=int, default=0, help="auto-stop after N seconds (0 = run until Ctrl-C)")
    args = ap.parse_args()

    elementsd = os.path.join(args.bindir, "elementsd")
    elementscli = os.path.join(args.bindir, "elements-cli")
    for b in (elementsd, elementscli):
        if not os.path.exists(b):
            sys.exit("missing binary: %s (build first, or pass --bindir)" % b)

    if args.stop:
        stop_all(args.basedir, elementsd)
        return

    N = args.nodes
    base = args.basedir
    if os.path.exists(base):
        sys.exit("basedir %s already exists; remove it or use --stop first" % base)
    os.makedirs(base)
    pids = {}

    # ---- Parent (Bitcoin) ----------------------------------------------------
    parent_miner = None
    if args.no_anchor:
        anchor_lines, parent_genesis = [], ""
        p_host = p_port = p_user = p_pw = None
        print("Anchoring DISABLED (--no-anchor).")
    elif args.local_parent:
        pdir = os.path.join(base, "parent")
        os.makedirs(os.path.join(pdir, CHAIN))
        p_port, p_rpc = args.p2p_base - 2, args.rpc_base - 2
        write_conf(os.path.join(pdir, "elements.conf"), [
            "chain=%s" % CHAIN, "server=1",
            "rpcuser=%s" % args.rpcuser, "rpcpassword=%s" % args.rpcpassword,
            "[%s]" % CHAIN,
            "port=%d" % p_port, "rpcport=%d" % p_rpc,
            "bind=127.0.0.1", "rpcbind=127.0.0.1", "rpcallowip=127.0.0.1",
            "listen=1", "discover=0", "dnsseed=0",
            "validatepegin=0", "initialfreecoins=0",
            "con_blocksubsidy=5000000000", "anyonecanspendaremine=1", "signblockscript=51",
            "fallbackfee=0.0001",
        ])
        print("Starting local parent (throwaway Bitcoin stand-in)...")
        start_daemon(elementsd, pdir)
        wait_rpc(elementscli, pdir, p_rpc, "parent")
        cli(elementscli, pdir, p_rpc, ["-named", "createwallet", "wallet_name=w", "descriptors=true"],
            args.rpcuser, args.rpcpassword)
        addr, _ = cli(elementscli, pdir, p_rpc, ["getnewaddress"], args.rpcuser, args.rpcpassword)
        cli(elementscli, pdir, p_rpc, ["generatetoaddress", "20", addr], args.rpcuser, args.rpcpassword, timeout=120)
        parent_genesis, _ = cli(elementscli, pdir, p_rpc, ["getblockhash", "0"], args.rpcuser, args.rpcpassword)
        p_host, p_port_rpc, p_user, p_pw = "127.0.0.1", p_rpc, args.rpcuser, args.rpcpassword
        pids["parent"] = {"datadir": pdir, "rpcport": p_rpc, "pid": _read_pid(pdir)}
        parent_miner = (pdir, p_rpc, addr)
        anchor_lines = [
            "con_bitcoin_anchor=1", "validateanchor=1", "anchorminconf=1",
            "anchorpollinterval=%d" % (args.anchorpoll or max(2, args.slot // 2)),
            "mainchainrpchost=127.0.0.1", "mainchainrpcport=%d" % p_port_rpc,
            "mainchainrpcuser=%s" % p_user, "mainchainrpcpassword=%s" % p_pw,
            "parentgenesisblockhash=%s" % parent_genesis,
        ]
    else:
        if not (args.parent_rpcport and args.parent_genesis):
            sys.exit("external anchor needs --parent-rpcport and --parent-genesis "
                     "(or use --local-parent, or --no-anchor)")
        parent_genesis = args.parent_genesis
        anchor_lines = [
            "con_bitcoin_anchor=1", "validateanchor=1", "anchorminconf=1",
            "anchorpollinterval=%d" % (args.anchorpoll or max(2, args.slot // 2)),
            "mainchainrpchost=%s" % args.parent_rpchost,
            "mainchainrpcport=%d" % args.parent_rpcport,
            "mainchainrpcuser=%s" % args.parent_rpcuser,
            "mainchainrpcpassword=%s" % args.parent_rpcpassword,
            "parentgenesisblockhash=%s" % parent_genesis,
        ]
        print("Anchoring to external parent %s:%d (genesis %s...)" %
              (args.parent_rpchost, args.parent_rpcport, parent_genesis[:16]))

    # ---- Committee keys + shared consensus block -----------------------------
    stakers = [make_staker() for _ in range(N)]
    json.dump([{"wif": w, "pub": p} for w, p in stakers],
              open(os.path.join(base, "committee_keys.json"), "w"), indent=2)
    consensus = [
        "con_pos=1", "posvrf=1", "posbls=1",
        "poscommitteesize=%d" % N, "posslotinterval=%d" % args.slot,
        # BLS cert ~257 B/signing member; size for the whole committee + headroom
        # (a fixed 8000 rejects a 100-member cert as block-proof-invalid).
        "con_max_block_sig_size=%d" % (280 * N + 2000), "signblockscript=51",
        "con_blocksubsidy=5000000000", "anyonecanspendaremine=1", "validatepegin=0",
    ] + (["con_bitcoin_anchor=1"] if not args.no_anchor else []) \
      + ["staker=%s:1" % pub for _, pub in stakers]

    # Low-diameter mesh: ring + a 4-node hub backbone (same shape as the demo).
    def p2p(i): return args.p2p_base + i
    hubs = list(range(min(4, N)))

    print("Writing %d node configs + starting daemons..." % N)
    for i in range(N):
        d = os.path.join(base, "node%03d" % i)
        os.makedirs(os.path.join(d, CHAIN))
        peers = set()
        if N > 1:
            peers.add((i + 1) % N)
            peers.add((i - 1) % N)
        peers.update(h for h in hubs if h != i)
        if i not in hubs and hubs:
            peers.add(hubs[i % len(hubs)])
        local = [
            "server=1",
            "rpcuser=%s" % args.rpcuser, "rpcpassword=%s" % args.rpcpassword,
            "port=%d" % p2p(i), "rpcport=%d" % (args.rpc_base + i),
            "bind=127.0.0.1", "rpcbind=127.0.0.1", "rpcallowip=127.0.0.1",
            "listen=1", "discover=0", "dnsseed=0", "upnp=0",
            "maxconnections=%d" % (len(peers) + 16),
            "posproducer=1", "posproducerkey=%s" % stakers[i][0],
        ] + anchor_lines + ["addnode=127.0.0.1:%d" % p2p(j) for j in sorted(peers)]
        # elements.conf: chain selector, then everything under the [chain] section —
        # consensus block (shared verbatim) first, node-local after.
        write_conf(os.path.join(d, "elements.conf"),
                   ["chain=%s" % CHAIN, "[%s]" % CHAIN] + consensus + [""] + local)
        start_daemon(elementsd, d)
        pids["node%03d" % i] = {"datadir": d, "rpcport": args.rpc_base + i, "pid": _read_pid(d)}
        if (i + 1) % 10 == 0:
            print("  started %d/%d" % (i + 1, N))
    json.dump(pids, open(os.path.join(base, "pids.json"), "w"))

    # ---- Wait for RPC + genesis agreement ------------------------------------
    print("Waiting for all %d nodes' RPC..." % N)
    for i in range(N):
        wait_rpc(elementscli, pids["node%03d" % i]["datadir"], args.rpc_base + i, "node%d" % i)
    g0, _ = cli(elementscli, pids["node000"]["datadir"], args.rpc_base, ["getblockhash", "0"])
    bad = []
    for i in range(1, N):
        gi, _ = cli(elementscli, pids["node%03d" % i]["datadir"], args.rpc_base + i, ["getblockhash", "0"])
        if gi != g0:
            bad.append(i)
    if bad:
        print("!! genesis MISMATCH on nodes %s — consensus configs differ" % bad)
    else:
        print("Genesis hash identical on all %d nodes: %s" % (N, g0))
    print("Quorum = %d of %d. Slot = %ds. Anchor = %s." %
          (N // 2 + 1, N, args.slot, "off" if args.no_anchor else "on"))

    # ---- Monitor (and keep the local parent advancing) -----------------------
    def mine_parent():
        if parent_miner:
            pdir, prpc, addr = parent_miner
            cli(elementscli, pdir, prpc, ["generatetoaddress", "1", addr],
                args.rpcuser, args.rpcpassword, check=False, timeout=60)

    stop_at = time.time() + args.run_seconds if args.run_seconds else None
    print("\nNetwork live. Heights below (min should track max = no fork). Ctrl-C to stop.\n")
    try:
        last = -1
        while True:
            mine_parent()
            time.sleep(max(2, args.slot))
            hs = []
            for i in range(N):
                out, rc = cli(elementscli, pids["node%03d" % i]["datadir"], args.rpc_base + i,
                              ["getblockcount"], check=False, timeout=15)
                hs.append(int(out) if rc == 0 and out.isdigit() else -1)
            lo, hi = min(hs), max(hs)
            if hi != last:
                ahint = ""
                if not args.no_anchor:
                    a, rc = cli(elementscli, pids["node000"]["datadir"], args.rpc_base,
                                ["getanchorstatus"], check=False, timeout=15)
                    if rc == 0:
                        try:
                            j = json.loads(a)
                            ahint = " anchor=%s@btc%s" % (j.get("anchorstatus"), j.get("anchorheight"))
                        except Exception: ahint = ""
                print("  height min=%d max=%d  (fork=%s)%s" %
                      (lo, hi, "YES" if len({h for h in hs if h >= 0}) > 1 and lo >= 1 and _forked(elementscli, pids, args, hs) else "no", ahint))
                last = hi
            if stop_at and time.time() >= stop_at:
                print("\n--run-seconds elapsed; stopping.")
                break
    except KeyboardInterrupt:
        print("\nCtrl-C — stopping network...")
    finally:
        stop_all(base, elementsd)


def _forked(cli_bin, pids, args, hs):
    """True iff two nodes disagree on the hash at the common height (real fork)."""
    common = min(h for h in hs if h >= 0)
    if common < 1:
        return False
    ref = None
    for name, info in pids.items():
        if name == "parent":
            continue
        out, rc = cli(cli_bin, info["datadir"], info["rpcport"], ["getblockhash", str(common)], check=False, timeout=15)
        if rc != 0:
            continue
        if ref is None:
            ref = out
        elif out != ref:
            return True
    return False


def _read_pid(datadir):
    pidf = os.path.join(datadir, CHAIN, "elementsd.pid")
    for _ in range(50):
        if os.path.exists(pidf):
            try:
                return int(open(pidf).read().strip())
            except Exception:
                pass
        time.sleep(0.1)
    return 0


if __name__ == "__main__":
    main()
