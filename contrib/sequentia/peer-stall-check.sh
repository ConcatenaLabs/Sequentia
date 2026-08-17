#!/usr/bin/env bash
# Report peers that are connected but not following the chain.
#
# This exists because a peer running an older binary of the SAME reported
# version is invisible in getpeerinfo: it keeps a healthy connection, answers
# pings, and reports the version string of the build it was compiled from, while
# silently refusing every block that uses a consensus rule it does not have. The
# supervised-asset zero-supply relaxation forked a peer exactly this way on
# 2026-08-16 and it sat unnoticed for a day.
#
# The signature is behavioural, not cosmetic:
#
#   synced_headers frozen (typically one block below the first block that uses
#   the new rule) while bytessent to that peer keeps climbing, because we keep
#   offering a chain it keeps rejecting.
#
# Run it after every consensus cutover, and on a timer. Exit status is 1 when
# any peer is stalled, so cron mail or a monitor can key off it.
#
# Usage:
#   peer-stall-check.sh [--lag N] [--cli "<cli invocation>"]
#
#   --lag N   headers this far below our tip counts as stalled (default 6)
#   --cli     how to invoke the node's CLI, quoted (default: sequentia-cli)
#
# Examples:
#   contrib/sequentia/peer-stall-check.sh
#   contrib/sequentia/peer-stall-check.sh --lag 2 \
#     --cli "src/sequentia-cli -chain=test -rpcport=18200 -rpcuser=u -rpcpassword=p"

set -euo pipefail

LAG=6
CLI="sequentia-cli"

while [ $# -gt 0 ]; do
  case "$1" in
    --lag) LAG="$2"; shift 2 ;;
    --cli) CLI="$2"; shift 2 ;;
    -h|--help) sed -n '1,30p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

tip=$($CLI getblockcount)
peers=$($CLI getpeerinfo)

printf 'tip %s, stall threshold %s block(s) behind\n\n' "$tip" "$LAG"

echo "$peers" | TIP="$tip" LAG="$LAG" python3 -c '
import json, os, sys

tip = int(os.environ["TIP"])
lag = int(os.environ["LAG"])
peers = json.load(sys.stdin)

stalled = []
for p in peers:
    addr = p.get("addr", "?")
    # Loopback peers are our own committee nodes; a real fork shows up on them
    # too, so they are reported like any other peer.
    headers = p.get("synced_headers", -1)
    blocks = p.get("synced_blocks", -1)
    behind = tip - headers if headers >= 0 else None
    bad = headers < 0 or (behind is not None and behind > lag)
    row = {
        "addr": addr,
        "subver": p.get("subver", "?"),
        "synced_headers": headers,
        "synced_blocks": blocks,
        "behind": behind,
        "bytessent": p.get("bytessent", 0),
        "bytesrecv": p.get("bytesrecv", 0),
        "inbound": p.get("inbound"),
    }
    if bad:
        stalled.append(row)

if not stalled:
    print("all %d peer(s) are following the chain" % len(peers))
    sys.exit(0)

print("STALLED PEERS (%d of %d):" % (len(stalled), len(peers)))
for r in stalled:
    behind = "unknown" if r["behind"] is None else "%d behind" % r["behind"]
    # A large sent/received ratio is the tell that we are repeatedly offering a
    # chain the peer refuses, rather than the peer simply being quiet.
    ratio = (r["bytessent"] / r["bytesrecv"]) if r["bytesrecv"] else float("inf")
    print("  %-24s %s" % (r["addr"], r["subver"]))
    print("      headers %s (%s), blocks %s, sent/recv %.1f"
          % (r["synced_headers"], behind, r["synced_blocks"], ratio))
print()
print("A peer frozen just below a known consensus-change height is running a")
print("binary without that rule, whatever version string it reports. Have its")
print("operator rebuild from master and restart; it will then accept the chain.")
sys.exit(1)
'
