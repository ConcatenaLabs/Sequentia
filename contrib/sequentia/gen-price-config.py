#!/usr/bin/env python3
# Generates the Sequentia price-server config for the testnet demo: every demo
# asset (incl. tSEQ itself) sourced from the local mock price API, pushed to the
# whole committee (+ gateway + explorer). Run on the box where the nodes live.
import json


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


# The demo's live, spendable test assets (Batch B): re-issued after the reorg
# storm orphaned the first batch, now buried/trusted in livetest. tSEQ is the
# reference (see reference_asset_label below).
ASSETS = [
    ("SEQ",   "c8eccacf0953e1931cd31e434d8319101cc36e6c38b0e2104d8687552fae3e40"),
    ("USDX",  "dc7f45fcfeb17c8ae74e284472d85543395f50e88f4a36cb652e8102703b7027"),
    ("EURX",  "f7a756b4e966623065543e52b754324629295c895046a0916a939898ad373667"),
    ("GOLD",  "c28fc933ce41f7a9188da029c6f7377fc961e2d58588372ef4073438610b9283"),
    ("WBTC",  "3e30ad0ebd13cc7ac1bbd12df1414b213708a6048b745d185fe935d9624024db"),
    ("SILVR", "50a00211d7074d5f857a3dec6cb84a1f3fefb26e56a94a954a299b28ac9f32df"),
    ("OILX",  "f9b069ac00f4dc57381a304704fac93301f90d3d509d207cfbddc8367d4e9cfb"),
]

nodes = [{"host": "127.0.0.1", "port": 18200 + i, "user": "seq", "password": "seq", "timeout": 15}
         for i in range(100)]
for path, port in (("/root/seq-testnet/node-gw/elements.conf", 18443),
                   ("/root/sequentia/explorer-node/elements.conf", 18401)):
    u, p = creds(path)
    if u:
        nodes.append({"host": "127.0.0.1", "port": port, "user": u, "password": p, "timeout": 15})

cfg = {
    "poll_interval_secs": 30,
    "source_timeout": 10,
    "source_name": "mock-price-api",
    # SEQ is the native gas asset, so price the whole table in SEQ: SEQ -> 1e8,
    # every other asset expressed in SEQ units (driven by SEQ's market price).
    "reference_asset_label": "SEQ",
    "node_rpcs": nodes,
    "default_thresholds": {"min_sources": 1, "min_market_cap": 50000000,
                           "min_volume_24h": 1000000, "max_source_spread": 0.05,
                           "max_change_factor": 5.0},
    "assets": [{"label": t, "id": i,
                "sources": [{"kind": "jsonapi", "name": "mock",
                             "url": "http://127.0.0.1:8088/price/%s" % t,
                             "price_path": "price",
                             "market_cap_path": "market_cap",
                             "volume_24h_path": "volume_24h"}]}
               for t, i in ASSETS],
}
open("/root/price-demo/config.json", "w").write(json.dumps(cfg, indent=2))
print("config: %d assets, %d nodes" % (len(ASSETS), len(nodes)))
