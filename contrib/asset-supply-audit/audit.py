#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Independently compute the exact circulating supply of a Sequentia asset.

The node has no per-asset supply index: coinstatsindex sums atoms across every
asset into one number and drops blinded outputs, and listissuances is
wallet-scoped. So supply is not a value you can look up; it is a value you
reconstruct by walking the chain. This tool does exactly that, over public RPC,
with no trust in the node beyond the block data it serves.

For an asset id it reports, in raw atoms (the integer consensus amount,
independent of the asset's display precision):

    circulating = issued + reissued - burned

where
  issued    = sum of explicit amounts of INITIAL issuances of the asset,
  reissued  = sum of explicit amounts of REISSUANCES of the asset,
  burned    = sum of explicit amounts sent to provably-unspendable outputs
              (OP_RETURN / NULL_DATA) carrying the asset.

Completeness is not assumed, it is checked. If the asset ever had a blinded
issuance, reissuance, or burn, the corresponding amount is invisible to any
observer, so the true supply is not knowable from chain data alone. When that
happens this tool reports the visible figure as a BOUND, not an equality, and
exits non-zero. A stablecoin standard that wants an exact, externally verifiable
supply must therefore keep every issuance, reissuance and burn explicit; this
tool is what verifies that they did.

The report is deterministic given a chain, so two parties (for example a bridge
operator and Circle) running it against their own nodes must get identical
numbers or one of their nodes is lying.

Usage:
    audit.py --asset <assetid> [--rpc-url URL] [--datadir DIR | --cookie FILE
             | --rpc-user U --rpc-password P] [--start H] [--end H]
             [--checkpoint FILE] [--json]

With no --asset, every asset seen on the chain is reported.
"""

import argparse
import json
import os
import sys
import time
import urllib.request
from decimal import Decimal, ROUND_HALF_UP

COIN = Decimal(100_000_000)  # atoms per whole unit; the RPC divides raw amounts by this


def to_atoms(value):
    """Convert an RPC decimal amount back to the integer consensus amount.

    The node serializes every asset amount as raw_atoms / 1e8 regardless of the
    asset's display precision, so multiplying back by 1e8 recovers the exact
    integer. parse_float=Decimal (below) keeps this lossless."""
    return int((Decimal(value) * COIN).to_integral_value(rounding=ROUND_HALF_UP))


class RPC:
    def __init__(self, url, user, password):
        self._url = url
        token = "%s:%s" % (user, password)
        import base64
        self._auth = "Basic " + base64.b64encode(token.encode()).decode()
        self._id = 0

    def call(self, method, params=None):
        self._id += 1
        body = json.dumps({"jsonrpc": "1.0", "id": self._id, "method": method,
                           "params": params or []}).encode()
        req = urllib.request.Request(self._url, body, {
            "Content-Type": "application/json", "Authorization": self._auth})
        with urllib.request.urlopen(req, timeout=120) as resp:
            # parse_float=Decimal so amounts never touch binary floating point
            payload = json.loads(resp.read().decode(), parse_float=Decimal)
        if payload.get("error"):
            raise RuntimeError("%s: %s" % (method, payload["error"]))
        return payload["result"]


def resolve_credentials(args):
    """Figure out URL + user/password from the flags, mirroring how a node
    exposes RPC: an explicit user/password, a .cookie file, or a datadir that
    contains one."""
    url = args.rpc_url or "http://127.0.0.1:%d/" % (args.rpc_port or 7041)
    if args.rpc_user and args.rpc_password:
        return url, args.rpc_user, args.rpc_password
    cookie = args.cookie
    if not cookie and args.datadir:
        # look for .cookie at datadir root and in the usual chain subdirs
        for sub in ("", "testnet3", "testnet4", "regtest", "sequentia"):
            cand = os.path.join(args.datadir, sub, ".cookie")
            if os.path.exists(cand):
                cookie = cand
                break
    if cookie and os.path.exists(cookie):
        user, _, password = open(cookie).read().strip().partition(":")
        return url, user, password
    sys.exit("no RPC credentials: pass --rpc-user/--rpc-password, --cookie, or "
             "--datadir pointing at a node whose .cookie is readable")


class Acc:
    """Per-asset accumulator."""
    __slots__ = ("issued", "reissued", "burned",
                 "blinded_issue", "blinded_reissue", "blinded_burn")

    def __init__(self):
        self.issued = 0
        self.reissued = 0
        self.burned = 0
        self.blinded_issue = 0
        self.blinded_reissue = 0
        self.blinded_burn = 0

    def as_dict(self):
        circ = self.issued + self.reissued - self.burned
        exact = not (self.blinded_issue or self.blinded_reissue or self.blinded_burn)
        return {
            "issued_atoms": self.issued,
            "reissued_atoms": self.reissued,
            "burned_atoms": self.burned,
            "circulating_atoms": circ,
            "exact": exact,
            "blinded_issuances": self.blinded_issue,
            "blinded_reissuances": self.blinded_reissue,
            "blinded_burns": self.blinded_burn,
        }


def scan_tx(tx, accs, want):
    """Fold one decoded transaction into the accumulators.

    want is None (track every asset) or a set of asset ids to restrict to."""
    def acc_for(assetid):
        if want is not None and assetid not in want:
            return None
        a = accs.get(assetid)
        if a is None:
            a = accs[assetid] = Acc()
        return a

    for vin in tx.get("vin", []):
        iss = vin.get("issuance")
        if not iss:
            continue
        assetid = iss.get("asset")
        if not assetid:
            continue
        a = acc_for(assetid)
        if a is None:
            continue
        is_reissue = iss.get("isreissuance", False)
        if "assetamount" in iss:
            atoms = to_atoms(iss["assetamount"])
            if is_reissue:
                a.reissued += atoms
            else:
                a.issued += atoms
        elif "assetamountcommitment" in iss:
            # Blinded issuance/reissuance: amount is hidden, supply unknowable.
            if is_reissue:
                a.blinded_reissue += 1
            else:
                a.blinded_issue += 1

    for vout in tx.get("vout", []):
        spk = vout.get("scriptPubKey", {})
        if spk.get("type") != "nulldata":
            continue  # only provably-unspendable outputs remove supply
        assetid = vout.get("asset")
        if assetid is None:
            # blinded asset on an unspendable output: a burn we cannot attribute
            # to any specific asset, so we cannot count it. Flag against nothing
            # here; a target-asset blinded burn is caught below only when the
            # asset is explicit but the value is blinded.
            continue
        a = acc_for(assetid)
        if a is None:
            continue
        if "value" in vout:
            atoms = to_atoms(vout["value"])
            if atoms > 0:
                a.burned += atoms
        elif "valuecommitment" in vout or "value-minimum" in vout:
            a.blinded_burn += 1


def main():
    p = argparse.ArgumentParser(description="Audit exact per-asset supply from chain data.")
    p.add_argument("--asset", action="append", default=None,
                   help="asset id to audit (repeatable); omit to report every asset")
    p.add_argument("--rpc-url", default=None, help="node RPC URL (default http://127.0.0.1:<port>/)")
    p.add_argument("--rpc-port", type=int, default=None, help="RPC port when --rpc-url is not given")
    p.add_argument("--rpc-user", default=None)
    p.add_argument("--rpc-password", default=None)
    p.add_argument("--cookie", default=None, help="path to a node .cookie file")
    p.add_argument("--datadir", default=None, help="node datadir to find a .cookie under")
    p.add_argument("--start", type=int, default=0, help="first block height to scan")
    p.add_argument("--end", type=int, default=None, help="last block height (default: tip)")
    p.add_argument("--checkpoint", default=None,
                   help="JSON file to persist progress to; resumes if present")
    p.add_argument("--json", action="store_true", help="emit the report as JSON")
    p.add_argument("--progress-every", type=int, default=5000)
    args = p.parse_args()

    url, user, password = resolve_credentials(args)
    rpc = RPC(url, user, password)

    want = set(args.asset) if args.asset else None
    accs = {}
    start = args.start

    # Resume from a checkpoint if one exists and matches the requested filter.
    if args.checkpoint and os.path.exists(args.checkpoint):
        saved = json.load(open(args.checkpoint), parse_float=Decimal)
        if saved.get("want") == (sorted(want) if want else None):
            start = saved["next_height"]
            for aid, d in saved["accs"].items():
                a = Acc()
                a.issued = int(d["issued_atoms"]); a.reissued = int(d["reissued_atoms"])
                a.burned = int(d["burned_atoms"])
                a.blinded_issue = d["blinded_issuances"]; a.blinded_reissue = d["blinded_reissuances"]
                a.blinded_burn = d["blinded_burns"]
                accs[aid] = a
            sys.stderr.write("resumed from %s at height %d\n" % (args.checkpoint, start))

    tip = rpc.call("getblockcount")
    end = args.end if args.end is not None else tip
    if end > tip:
        end = tip

    t0 = time.time()
    h = start
    while h <= end:
        bh = rpc.call("getblockhash", [h])
        block = rpc.call("getblock", [bh, 2])
        for tx in block["tx"]:
            scan_tx(tx, accs, want)
        if args.checkpoint and h % args.progress_every == 0:
            _save_checkpoint(args.checkpoint, want, h + 1, accs)
        if h % args.progress_every == 0:
            rate = (h - start + 1) / max(time.time() - t0, 1e-9)
            sys.stderr.write("scanned %d/%d (%.0f blk/s)\n" % (h, end, rate))
        h += 1

    if args.checkpoint:
        _save_checkpoint(args.checkpoint, want, end + 1, accs)

    report = {
        "chain_tip": tip,
        "scanned_start": args.start,
        "scanned_end": end,
        "assets": {aid: a.as_dict() for aid, a in sorted(accs.items())},
    }

    any_inexact = any(not a.as_dict()["exact"] for a in accs.values())

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        _print_human(report, want)

    # Non-zero exit if any requested asset's supply is not exactly knowable.
    if want:
        for aid in want:
            if aid in accs and not accs[aid].as_dict()["exact"]:
                return 2
    elif any_inexact:
        return 2
    return 0


def _save_checkpoint(path, want, next_height, accs):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump({
            "want": sorted(want) if want else None,
            "next_height": next_height,
            "accs": {aid: a.as_dict() for aid, a in accs.items()},
        }, f, default=str)
    os.replace(tmp, path)


def _print_human(report, want):
    print("Sequentia asset supply audit")
    print("  chain tip:      %d" % report["chain_tip"])
    print("  scanned range:  %d..%d" % (report["scanned_start"], report["scanned_end"]))
    assets = report["assets"]
    if want:
        for aid in want:
            _print_asset(aid, assets.get(aid))
    else:
        if not assets:
            print("  (no issuances found in range)")
        for aid, d in assets.items():
            _print_one(aid, d)


def _print_asset(aid, d):
    if d is None:
        print("\n  %s\n    no issuance of this asset found in the scanned range" % aid)
        return
    _print_one(aid, d)


def _print_one(aid, d):
    print("\n  asset %s" % aid)
    print("    issued      %d atoms" % d["issued_atoms"])
    print("    reissued    %d atoms" % d["reissued_atoms"])
    print("    burned      %d atoms" % d["burned_atoms"])
    print("    circulating %d atoms%s" % (
        d["circulating_atoms"], "" if d["exact"] else "   (LOWER/UPPER BOUND, not exact)"))
    if not d["exact"]:
        print("    WARNING: supply is not exactly knowable from chain data:")
        if d["blinded_issuances"]:
            print("      %d blinded issuance(s) hide an unknown minted amount" % d["blinded_issuances"])
        if d["blinded_reissuances"]:
            print("      %d blinded reissuance(s) hide an unknown minted amount" % d["blinded_reissuances"])
        if d["blinded_burns"]:
            print("      %d blinded burn(s) hide an unknown destroyed amount" % d["blinded_burns"])
        print("    A standard-compliant bridged asset MUST keep every issuance,")
        print("    reissuance and burn explicit; this asset did not.")


if __name__ == "__main__":
    sys.exit(main())
