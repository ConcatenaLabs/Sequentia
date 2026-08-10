#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Generate the Sequentia price-server config for the testnet demo: the whole
committee (+ gateway + explorer) fed from the local market-data API, with the
asset universe discovered from the Asset Registry.

Two things this script deliberately does NOT do.

It does not carry its own copy of the config schema. It SEEDS from
contrib/price-server/config.example.json, the schema of record that ships with
the server, and overrides only the parts that are specific to the box. An earlier
version kept its own idea of the schema and drifted: it still emitted the
PRE-REGISTRY shape (a hand-written `assets` list, per-asset `sources`,
`min_sources`), none of which the current server reads, so running it replaced a
working config with one the server could not act on. Seeding from the example
makes that class of drift impossible: whatever the server documents as its
config IS what comes out of here.

It does not overwrite an existing config unless asked with --force. A config file
is not only generated, it is also EDITED, by hand and by the server's own config
UI, which persists poll interval, admission thresholds, manual prices, UI access
and the admin password hash back to the same path. Clobbering that on a rerun
would silently drop the admin password and every tuned threshold, on a live
sidecar. Regenerating is therefore an explicit act.

Run on the box where the nodes live:
    gen-price-config.py --out /root/price-demo/config.json [--force]
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# The schema of record, in repo layout first, then beside this script (how it is
# deployed on the box).
EXAMPLE_CANDIDATES = (
    os.path.join(HERE, os.pardir, "price-server", "config.example.json"),
    os.path.join(HERE, "config.example.json"),
)
DEFAULT_OUT = "/root/price-demo/config.json"
DEFAULT_SOURCE_URL = "http://127.0.0.1:8088/prices"
DEFAULT_REGISTRY_URL = "http://159.195.15.140/registry/index.minimal.json"
# The committee's RPC ports are 18200 + node index; the gateway and the explorer
# node read their credentials from their own elements.conf.
COMMITTEE_PORT_BASE = 18200
# Where the committee node directories live, so the fed set can be DISCOVERED
# rather than hand-listed (see discover_committee).
COMMITTEE_ROOT = "/root/seq-testnet"
EXTRA_NODES = (("/root/seq-testnet/node-gw/elements.conf", 18443),
               ("/root/sequentia/explorer-node/elements.conf", 18401))


def creds(path):
    u = p = None
    try:
        for line in open(path):
            line = line.strip()
            if line.startswith("rpcuser="):
                u = line.split("=", 1)[1]
            elif line.startswith("rpcpassword="):
                p = line.split("=", 1)[1]
    except OSError:
        pass
    return u, p


def find_example(explicit=None):
    tried = (explicit,) if explicit else EXAMPLE_CANDIDATES
    for path in tried:
        if path and os.path.exists(path):
            return os.path.normpath(path)
    raise SystemExit(
        "cannot find config.example.json (looked in: %s).\nIt is the schema this "
        "script fills in, so it is required rather than duplicated here. Pass "
        "--example PATH, or copy contrib/price-server/config.example.json next to "
        "this script." % ", ".join(os.path.normpath(p) for p in tried))


def discover_committee(root=COMMITTEE_ROOT):
    """The committee node indices that actually EXIST on this box.

    ⚠ This is the fix for a real outage. The deployed config once enumerated RPC
    ports 18200-18209 BY HAND and stopped at ten, so when the committee grew to
    twenty, node010-019 were never fed: they held only the policy asset and
    refused every issued asset. Half the committee would neither relay nor mine
    an any-asset-fee transaction. There was no consensus risk — consensus never
    consults the rate table, so those nodes accepted the BLOCK they had refused
    to relay — but it looked exactly like unexplained network slowness.

    A hand-written list drifts the moment the committee changes, and nothing
    tells you it has. So the list is DISCOVERED from the node directories rather
    than declared: add a node, rerun, and it is fed. Returns sorted indices, or
    an empty list when the root is not present (a laptop run), where the caller
    falls back to an explicit count.
    """
    try:
        names = os.listdir(root)
    except OSError:
        return []
    out = []
    for name in names:
        if not name.startswith("node"):
            continue
        suffix = name[len("node"):]
        if suffix.isdigit() and os.path.isdir(os.path.join(root, name)):
            out.append(int(suffix))
    return sorted(out)


def node_rpcs(committee=None, root=COMMITTEE_ROOT):
    """Every node the sidecar must feed: the committee, plus the gateway and the
    explorer node.

    This list has to stay in step with the set that BROADCASTS and MINES, because
    an asset priced on some of them and not others is offered to users and then
    refused. Discovery is preferred for exactly that reason; `committee` is only
    the fallback for generating a config off-box.
    """
    indices = discover_committee(root)
    if not indices:
        if committee is None:
            raise SystemExit(
                "no committee nodes found under %s and no --committee count given.\n"
                "Run this on the box where the nodes live, or pass --committee N to "
                "generate a config for a committee of N nodes at %d+i."
                % (root, COMMITTEE_PORT_BASE))
        indices = list(range(committee))
    nodes = [{"host": "127.0.0.1", "port": COMMITTEE_PORT_BASE + i,
              "user": "seq", "password": "seq", "timeout": 15}
             for i in indices]
    for path, port in EXTRA_NODES:
        u, p = creds(path)
        if u:
            nodes.append({"host": "127.0.0.1", "port": port, "user": u, "password": p, "timeout": 15})
    return nodes


def build_config(example, *, source_url, registry_url, committee):
    """The example config with the demo's deployment specifics applied.

    Everything not named here (admission rules, manual prices, feed aliases, the
    reference-unit factor, the UI block) comes from the example as shipped, so a
    key added to the server's schema arrives here without this script changing.
    """
    cfg = json.loads(json.dumps(example))  # never mutate the caller's example
    cfg["_comment"] = ("Sequentia testnet demo, generated by contrib/sequentia/"
                       "gen-price-config.py from contrib/price-server/config.example.json. "
                       "Assets are DISCOVERED from the registry; there is no asset list to "
                       "maintain here. Safe to edit by hand or from the config UI: the "
                       "generator refuses to overwrite this file without --force.")
    cfg["poll_interval_secs"] = 30
    cfg["source_timeout"] = 10
    cfg["source_name"] = "sequentia-testnet-demo"
    cfg["source"] = dict(cfg.get("source", {}), url=source_url,
                         quote_currency="USD", format="sequentia")
    cfg["registry_url"] = registry_url
    # The reference unit is a CONVERSION FACTOR: how many of the feed's numeraire
    # units (USD here) make one reference unit. 1.0 is the identity, so the demo
    # publishes the feed's own numeraire. It is never a token. Pinning one asset
    # at 1e8 would make every other rate a quote against that asset's fortunes, a
    # privileged anchor, while in Sequentia the Sequence token (ticker SEQ) has
    # equal standing with every issued asset and no asset is the unit of account.
    # A unit that happens to equal one token today is expressed as that token's
    # price in the feed's numeraire, and floats again the moment that price moves.
    cfg["api_units_per_reference_unit"] = 1.0
    # Admission rules: mcap/volume floors plus a single-poll jump guard. NOT
    # max_volatility, deliberately: tSEQ is a bounded random walk in the demo feed
    # and a volatility gate would drop it. min_sources and max_source_spread are
    # NOT set, because the server reads one market source and ignores both keys.
    cfg["default_thresholds"] = {"require": "all",
                                 "min_market_cap": 50000000,
                                 "min_volume_24h": 1000000,
                                 "max_change_factor": 5.0}
    cfg["node_rpcs"] = node_rpcs(committee)
    cfg.pop("node_rpc", None)
    return cfg


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=DEFAULT_OUT, help="config path to write (default: %s)" % DEFAULT_OUT)
    ap.add_argument("--example", help="path to config.example.json (the schema this fills in)")
    ap.add_argument("--source-url", default=DEFAULT_SOURCE_URL,
                    help="market-data API, Sequentia format (default: %s)" % DEFAULT_SOURCE_URL)
    ap.add_argument("--registry-url", default=DEFAULT_REGISTRY_URL,
                    help="asset registry the asset universe is discovered from")
    ap.add_argument("--committee", type=int, default=None,
                    help="FALLBACK committee size when the node directories are not present "
                         "(off-box runs). On the box the set is discovered from %s, so adding a "
                         "node and rerunning is enough." % COMMITTEE_ROOT)
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing config (it may carry UI edits and the admin password hash)")
    args = ap.parse_args(argv)

    if os.path.exists(args.out) and not args.force:
        sys.stderr.write(
            "refusing to overwrite the existing config at %s.\nIt may carry hand edits and "
            "everything the server's config UI persists there, including the admin password "
            "hash, the admission thresholds and any manual prices. Re-run with --force if you "
            "really mean to replace it, or pass --out to write elsewhere.\n" % args.out)
        return 1

    example = find_example(args.example)
    with open(example) as f:
        cfg = build_config(json.load(f), source_url=args.source_url,
                           registry_url=args.registry_url, committee=args.committee)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w") as f:
        f.write(json.dumps(cfg, indent=2) + "\n")
    print("wrote %s from %s: %d nodes, assets discovered from %s"
          % (args.out, example, len(cfg["node_rpcs"]), cfg["registry_url"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
