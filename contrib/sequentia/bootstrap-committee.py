#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bootstrap a small Proof-of-Stake committee on a Sequentia testnet.

A bundled Sequentia chain (-chain=test / -chain=sequentia) seeds exactly ONE
staker in genesis (the founder). Normal block certification needs a strict
majority of the committee to countersign, so a lone founder can only produce
blocks under the escaping-stall rule (one per few Bitcoin blocks). To run a
real committee at full speed you need enough on-chain stakers to reach quorum.

This tool does that for a single operator on a *fresh* testnet:
  1. generates N new staker keys,
  2. spends the founder's genesis coins into N CSV-locked staking outputs
     (each output registers that key as an on-chain staker),
  3. mines ONE escaping-stall block to confirm the registrations, and
  4. mines a first full-committee block to prove the quorum is reachable.

The new staker keys (WIFs) are printed at the end; keep them — you pass them
to `generateposblock` to produce blocks from then on.

Requirements:
  * Run it from the repository root (it imports the test framework's Elements
    transaction classes to build the staking-output transaction).
  * The node must be running -chain=test with -validateanchor=1 and its
    -mainchainrpc* parent connection configured, and be at block height 0 (a
    fresh chain). Wipe ~/.elements/testnet3 and restart if you already produced
    blocks. (getanchorstatus reads "not_validated" on a fresh chain -- that is
    expected; the genesis tip has no anchor yet, and block 1 creates the first
    one. The parent connection is verified at startup, so a node that finished
    loading is ready.)
  * The node must honor the chosen committee size: start it with
    -poscommitteesize=<members+1> (e.g. 3 for the default 2 members).

Example:
  ./src/elementsd -chain=test -daemon            # with -poscommitteesize=3 in conf
  python3 contrib/sequentia/bootstrap-committee.py \
      --founder-wif cURsyjY6KwZM9pBk7rfWwdDzYS1R4w85M2pPzh5RySfGpA8n9LB4
"""

import argparse
import json
import os
import subprocess
import sys
from decimal import Decimal

# Make the functional-test framework importable (Elements tx serialization).
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO_ROOT, "test", "functional"))

from test_framework.key import ECKey                       # noqa: E402
from test_framework.address import byte_to_base58           # noqa: E402
from test_framework.messages import (                       # noqa: E402
    COutPoint, CTransaction, CTxIn, CTxOut, CTxOutAsset,
)
from test_framework.script import CScript                   # noqa: E402

DEFAULT_FEE = "0.001"  # SEQ; flat fee for the single small funding transaction

COIN = 100_000_000
TESTNET_WIF_PREFIX = 239  # 0xEF, the -chain=test secret-key prefix


class CliError(Exception):
    pass


class Cli:
    """Minimal JSON-RPC client that shells out to elements-cli."""

    def __init__(self, cli_cmd):
        self.base = cli_cmd.split()

    def __call__(self, method, *args):
        cmd = list(self.base) + [method]
        for a in args:
            cmd.append(a if isinstance(a, str) else json.dumps(a))
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            raise CliError("%s failed: %s" % (method, (p.stderr or p.stdout).strip()))
        out = p.stdout.strip()
        if not out:
            return None
        try:
            return json.loads(out, parse_float=Decimal)
        except json.JSONDecodeError:
            return out  # plain string result (txid / blockhash / hex)


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b"\x01", TESTNET_WIF_PREFIX)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


def to_atoms(value):
    """Exact SEQ -> atoms (value parsed as Decimal to avoid float error)."""
    return int((Decimal(value) * COIN).to_integral_value())


def find_funding_output(cli):
    """Return the founder's spendable genesis P2WPKH output."""
    genesis = cli("getblock", cli("getblockhash", 0), 2)
    for tx in genesis["tx"]:
        for v in tx["vout"]:
            spk = v["scriptPubKey"]
            if spk.get("type") == "witness_v0_keyhash" and Decimal(v["value"]) > 0:
                if cli("gettxout", tx["txid"], v["n"]) is not None:
                    return {
                        "txid": tx["txid"], "vout": v["n"],
                        "value": Decimal(v["value"]),
                        "asset": v["asset"],
                        "spk": spk["hex"],
                    }
    raise CliError("no spendable founder output (witness_v0_keyhash) in genesis")


def asset_bytes_for(cli, asset_hex, sample_script):
    """Determine the serialization byte order for the policy asset by a
    decode round-trip, so outputs carry the exact policy asset."""
    for candidate in (bytes.fromhex(asset_hex), bytes.fromhex(asset_hex)[::-1]):
        a = CTxOutAsset()
        a.setToAsset(candidate)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vout = [CTxOut(COIN, sample_script, nAsset=a)]
        decoded = cli("decoderawtransaction", tx.serialize().hex())
        if decoded["vout"][0].get("asset") == asset_hex:
            return candidate
    raise CliError("could not match policy asset byte order via decoderawtransaction")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", default="./src/elements-cli -chain=test",
                    help="elements-cli invocation (default: %(default)s)")
    ap.add_argument("--founder-wif", required=True, help="genesis founder WIF")
    ap.add_argument("--members", type=int, default=2,
                    help="number of NEW stakers to create (default: 2)")
    ap.add_argument("--amount", default="1000000",
                    help="SEQ to lock per new staker (default: 1000000; keep all "
                         "stakers equal so VRF sortition always selects them)")
    ap.add_argument("--csv-seconds", type=int, default=1_300_000,
                    help="unbonding CSV lock per staking output, seconds "
                         "(default: 1300000, ~15 days; must be >= chain minimum)")
    ap.add_argument("--fee", default=DEFAULT_FEE,
                    help="flat fee in SEQ for the funding transaction (default: %(default)s)")
    args = ap.parse_args()

    cli = Cli(args.cli)

    # --- Preflight -----------------------------------------------------------
    info = cli("getblockchaininfo")
    if info.get("chain") != "test":
        raise CliError("expected -chain=test, got chain=%r" % info.get("chain"))
    height = cli("getblockcount")
    if height != 0:
        raise CliError(
            "chain is at height %d, not 0. The registration block must be the "
            "first block (an escaping-stall block). Stop the node, delete "
            "~/.elements/testnet3, restart, and re-run." % height)
    # NOTE: getanchorstatus reports "not_validated" on a fresh chain because the
    # genesis tip carries no anchor -- the FIRST block creates the first anchor.
    # So we do NOT wait for anchorstatus=="ok" here (that would never happen at
    # height 0). The mainchain connection was already verified at startup: a node
    # started with -validateanchor will not finish loading unless MainchainRPCCheck
    # passed. We just confirm anchoring is configured; if the parent connection is
    # actually down, the block-1 generateposblock below fails with a clear error.
    anchor = cli("getanchorstatus")
    if not anchor.get("validateanchor"):
        raise CliError("this node is not validating a Bitcoin anchor "
                       "(validateanchor is false). Set validateanchor=1 and "
                       "configure the -mainchainrpc* connection, then restart.")

    members = [make_staker() for _ in range(args.members)]
    committee_size = args.members + 1  # founder + new stakers
    quorum = committee_size // 2 + 1
    stake_atoms = to_atoms(args.amount)

    print("Bootstrapping a %d-member committee (quorum %d): founder + %d new stakers, "
          "%s SEQ each.\n" % (committee_size, quorum, args.members, args.amount))

    # --- Build the funding transaction ---------------------------------------
    # One input (the founder's genesis P2WPKH), one bare staking-script output
    # per new staker, change back to the founder, and an explicit fee. Signed
    # with the founder WIF directly (signrawtransactionwithkey) — no wallet, so
    # nothing depends on a rescan finding the genesis output.
    fund = find_funding_output(cli)
    asset_raw = asset_bytes_for(cli, fund["asset"], CScript([0x51]))
    asset = CTxOutAsset(); asset.setToAsset(asset_raw)

    in_atoms = to_atoms(fund["value"])
    fee_atoms = to_atoms(args.fee)
    change = in_atoms - args.members * stake_atoms - fee_atoms
    if change < 0:
        raise CliError("founder output (%s SEQ) too small to fund %d x %s SEQ + fee"
                       % (fund["value"], args.members, args.amount))

    tx = CTransaction()
    tx.nVersion = 2
    tx.vin = [CTxIn(COutPoint(int(fund["txid"], 16), fund["vout"]))]
    for wif, pub in members:
        s = cli("getstakescript", pub, None, args.csv_seconds)  # csv_seconds
        tx.vout.append(CTxOut(stake_atoms, bytes.fromhex(s["script"]), nAsset=asset))
    tx.vout.append(CTxOut(change, bytes.fromhex(fund["spk"]), nAsset=asset))  # change to founder
    tx.vout.append(CTxOut(fee_atoms, b"", nAsset=asset))                      # explicit fee

    amount_commit = "01" + in_atoms.to_bytes(8, "big").hex()
    prevtxs = [{"txid": fund["txid"], "vout": fund["vout"],
                "scriptPubKey": fund["spk"], "amountcommitment": amount_commit}]
    signed = cli("signrawtransactionwithkey", tx.serialize().hex(),
                 [args.founder_wif], prevtxs)
    if not signed.get("complete"):
        raise CliError("signing incomplete: %s" % signed.get("errors"))
    txid = cli("sendrawtransaction", signed["hex"])
    print("Funding transaction broadcast: %s" % txid)

    # --- Block 1: escaping-stall block confirming the registrations ----------
    # This is the first block to carry a Bitcoin anchor; it requires a live
    # mainchain connection to fetch the parent tip.
    try:
        b1 = cli("generateposblock", args.founder_wif)
    except CliError as e:
        raise CliError("could not produce block 1 (%s).\nThis usually means the "
                       "Bitcoin parent connection is down -- check that your "
                       "mainchain RPC / proxy is running and reachable." % e)
    print("Block 1 (escaping-stall, founder only): %s" % b1["hash"])

    a = cli("getanchorstatus")
    if a.get("anchorstatus") != "ok":
        print("WARNING: tip anchor status is %r after block 1 (expected \"ok\")."
              % a.get("anchorstatus"))

    reg = cli("getstakerinfo")
    missing = [pub for _, pub in members if pub not in reg]
    if missing:
        raise CliError("stakers not registered after block 1: %s" % missing)
    print("Registered stakers: %d (founder + %d new), all present.\n"
          % (len(reg), args.members))

    # --- Block 2: first full-committee block ---------------------------------
    member_wifs = [wif for wif, _ in members]
    b2 = cli("generateposblock", args.founder_wif, member_wifs)
    print("Block 2 (full committee): %s  countersignatures=%d"
          % (b2["hash"], b2["countersignatures"]))
    if b2["countersignatures"] < quorum:
        raise CliError("block 2 below quorum (%d < %d)"
                       % (b2["countersignatures"], quorum))

    print("\n=== Committee is live. Save these staker WIFs. ===")
    print("Founder (leader) WIF: %s" % args.founder_wif)
    for i, (wif, pub) in enumerate(members, 1):
        print("Member %d WIF: %s  (pubkey %s)" % (i, wif, pub))
    wifs_json = json.dumps(member_wifs)
    print("\nProduce further blocks at full speed with:")
    print("  %s generateposblock \"%s\" '%s'" % (args.cli, args.founder_wif, wifs_json))


if __name__ == "__main__":
    try:
        main()
    except CliError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        sys.exit(1)
