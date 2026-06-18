#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Cross-chain HTLC atomic swap demo: native BTC <-> a Sequentia-issued asset.

Self-contained: it launches two temporary local Elements chains -- one standing
in for Bitcoin ("BTC"), one for Sequentia ("SEQ", with an issued asset) -- runs
a complete hash-time-locked atomic swap, then demonstrates the timeout-refund
safety path, and finally tears everything down. Run it and watch:

    python3 contrib/sequentia/swap-demo.py

The swap itself is the standard HTLC construction (a hashlock for the happy path,
a CLTV timelock for the refund), so it is script-identical to a real BTC<->SEQ
swap. What makes it *real-time* on Sequentia is NOT shown here but in
test/functional/feature_anchor_swap_consistency.py: because a Sequentia block is
final iff the Bitcoin block it anchors survives, if the BTC leg is reorged the
SEQ leg reorgs *with it* -- so the swap needs no extra reorg-protection timelock
beyond Bitcoin's own confirmation wait. See doc/sequentia/03-bitcoin-anchoring.md.

Roles: Alice holds BTC and wants the asset; Bob holds the asset and wants BTC.
A single secret preimage makes the swap atomic -- redeeming one leg reveals the
secret that unlocks the other.
"""

import hashlib
import json
import os
import signal
import subprocess
import sys
import time
from decimal import Decimal

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "test", "functional"))
from test_framework.key import ECKey                                         # noqa: E402
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut, CTxOutAsset  # noqa: E402
from test_framework.script import (CScript, LegacySignatureHash, SIGHASH_ALL,  # noqa: E402
    OP_IF, OP_ELSE, OP_ENDIF, OP_SHA256, OP_EQUALVERIFY, OP_CHECKSIG,
    OP_CHECKLOCKTIMEVERIFY, OP_DROP)

COIN = 100_000_000
ELD = os.path.join(REPO, "src", "elementsd")
ELC = os.path.join(REPO, "src", "elements-cli")


def common_args(chain, datadir, port):
    return [ELD, "-chain=" + chain, "-datadir=" + datadir, "-server",
            "-rpcport=%d" % port, "-rpcuser=s", "-rpcpassword=s", "-listen=0",
            "-con_default_blinded_addresses=0", "-signblockscript=51",
            "-con_blocksubsidy=5000000000", "-initialfreecoins=0",
            "-validatepegin=0", "-fallbackfee=0.0001", "-acceptnonstdtxn=1",
            "-txindex=1", "-con_any_asset_fees=1", "-printtoconsole=0"]


def cli_base(chain, datadir, port):
    return [ELC, "-chain=" + chain, "-datadir=" + datadir,
            "-rpcport=%d" % port, "-rpcuser=s", "-rpcpassword=s"]


def cli(base, *a):
    cmd = base + [x if isinstance(x, str) else json.dumps(x) for x in a]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError("%s: %s" % (a[0], (p.stderr or p.stdout).strip()))
    out = p.stdout.strip()
    try:
        return json.loads(out, parse_float=Decimal)
    except json.JSONDecodeError:
        return out


def w(base, name):
    return base + ["-rpcwallet=" + name]


def asset_out(asset_hex):
    a = CTxOutAsset(); a.setToAsset(bytes.fromhex(asset_hex)[::-1]); return a


def newkey():
    k = ECKey(); k.generate(compressed=True)
    return k, k.get_pubkey().get_bytes()


def htlc_script(H, recipient_pub, locktime, refund_pub):
    """Standard HTLC: redeem with the preimage of H, or refund after locktime."""
    return CScript([OP_IF,
                        OP_SHA256, H, OP_EQUALVERIFY, recipient_pub, OP_CHECKSIG,
                    OP_ELSE,
                        locktime, OP_CHECKLOCKTIMEVERIFY, OP_DROP, refund_pub, OP_CHECKSIG,
                    OP_ENDIF])


def lock_htlc(base, wallet, script, amount, assetlabel=None):
    """Pay `amount` to the HTLC (P2SH); return its outpoint + amount + asset."""
    p2sh = cli(base, "decodescript", script.hex())["p2sh"]
    args = ["-named", "sendtoaddress", "address=%s" % p2sh, "amount=%s" % amount]
    if assetlabel:
        args.append("assetlabel=%s" % assetlabel)
    txid = cli(w(base, wallet), *args)
    spk = cli(base, "validateaddress", p2sh)["scriptPubKey"]
    for v in cli(base, "getrawtransaction", txid, True)["vout"]:
        if v["scriptPubKey"]["hex"] == spk:
            return txid, v["n"], int((Decimal(v["value"]) * COIN).to_integral_value()), v.get("asset")
    raise RuntimeError("HTLC output not found in funding tx")


def spend_htlc(script, txid, vout, in_atoms, asset_hex, dest_spk, fee_atoms,
               key, secret=None, locktime=None):
    """Build+sign a spend of the HTLC. Redeem path if `secret` given, else refund
    path (needs locktime, sets nLockTime + a non-final sequence)."""
    tx = CTransaction(); tx.nVersion = 2
    seq = 0xffffffff if secret is not None else 0xfffffffe
    tx.vin = [CTxIn(COutPoint(int(txid, 16), vout), nSequence=seq)]
    if secret is None:
        tx.nLockTime = locktime
    a = asset_out(asset_hex)
    tx.vout = [CTxOut(in_atoms - fee_atoms, dest_spk, nAsset=a),
               CTxOut(fee_atoms, b"", nAsset=a)]
    sighash, err = LegacySignatureHash(script, tx, 0, SIGHASH_ALL)
    assert err is None, err
    sig = key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])
    if secret is not None:
        tx.vin[0].scriptSig = CScript([sig, secret, 1, bytes(script)])   # IF branch
    else:
        tx.vin[0].scriptSig = CScript([sig, 0, bytes(script)])           # ELSE branch
    return tx.serialize().hex()


def main():
    tmp = "/tmp/seq-swap-demo"
    subprocess.run(["rm", "-rf", tmp])
    btc_dir, seq_dir = os.path.join(tmp, "btc"), os.path.join(tmp, "seq")
    os.makedirs(btc_dir); os.makedirs(seq_dir)
    BTC = cli_base("btcdemo", btc_dir, 19990)
    SEQ = cli_base("seqdemo", seq_dir, 19991)
    procs = []

    def mine(base, n=1):
        cli(w(base, "w"), "generatetoaddress", n, cli(w(base, "w"), "getnewaddress"))

    try:
        for chain, d, port in (("btcdemo", btc_dir, 19990), ("seqdemo", seq_dir, 19991)):
            procs.append(subprocess.Popen(common_args(chain, d, port),
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        for base, label in ((BTC, "BTC"), (SEQ, "SEQ")):
            for _ in range(60):
                try:
                    cli(base, "getblockcount"); break
                except RuntimeError:
                    time.sleep(1)
            cli(base, "-named", "createwallet", "wallet_name=w", "descriptors=true")
            mine(base, 110)
        print("Two chains up:  BTC (native coin) and SEQ (issued asset).\n")

        asset = cli(w(SEQ, "w"), "issueasset", "100000", "0")["asset"]
        mine(SEQ)
        cli(SEQ, "setfeeexchangerates", {asset: 100000000})   # asset can pay its own fee
        btc_asset = cli(BTC, "getsidechaininfo")["pegged_asset"]
        print("Asset issued on SEQ: %s\n" % asset)

        alice_k, alice_pub = newkey()   # Alice: has BTC, wants the asset
        bob_k, bob_pub = newkey()       # Bob:   has the asset, wants BTC
        secret = os.urandom(32)
        H = hashlib.sha256(secret).digest()
        print("Alice picks a secret; both legs are locked to H = sha256(secret) = %s\n" % H.hex())

        # --- Lock both legs (Bob's leg only after he's seen the BTC leg on-chain) ---
        btc_lt = cli(BTC, "getblockcount") + 100     # Alice's refund timeout (longer)
        seq_lt = cli(SEQ, "getblockcount") + 50       # Bob's refund timeout (shorter)
        btc_s = htlc_script(H, bob_pub, btc_lt, alice_pub)
        bt, bv, bamt, _ = lock_htlc(BTC, "w", btc_s, "10")
        mine(BTC)
        print("1. Alice LOCKS 10 BTC in an HTLC (Bob redeems with the secret).")

        seq_s = htlc_script(H, alice_pub, seq_lt, bob_pub)
        st, sv, samt, sas = lock_htlc(SEQ, "w", seq_s, "1000", assetlabel=asset)
        mine(SEQ)
        print("2. Bob LOCKS 1000 asset in an HTLC (Alice redeems with the secret).")

        # --- Execute ---
        alice_dest = bytes.fromhex(cli(SEQ, "validateaddress", cli(w(SEQ, "w"), "getnewaddress"))["scriptPubKey"])
        seq_redeem = spend_htlc(seq_s, st, sv, samt, sas, alice_dest, 100000, alice_k, secret=secret)
        rt = cli(SEQ, "sendrawtransaction", seq_redeem); mine(SEQ)
        print("3. Alice REDEEMS the asset by revealing the secret  (tx %s)." % rt[:12])

        asm = cli(SEQ, "getrawtransaction", rt, True)["vin"][0]["scriptSig"]["asm"]
        found = secret.hex() in asm
        print("4. Bob reads the secret off Alice's redeem witness on SEQ:  %s" % ("found" if found else "NOT FOUND"))

        bob_dest = bytes.fromhex(cli(BTC, "validateaddress", cli(w(BTC, "w"), "getnewaddress"))["scriptPubKey"])
        btc_redeem = spend_htlc(btc_s, bt, bv, bamt, btc_asset, bob_dest, 100000, bob_k, secret=secret)
        rt2 = cli(BTC, "sendrawtransaction", btc_redeem); mine(BTC)
        print("5. Bob REDEEMS the 10 BTC with that secret  (tx %s).\n" % rt2[:12])
        print("   => SWAP COMPLETE, atomically: Alice has the asset, Bob has the BTC.\n")

        # --- Safety path: if a counterparty stalls, the locker refunds after timeout ---
        print("Refund safety (shown on a fresh SEQ-side HTLC):")
        rk, rpub = newkey()
        lt = cli(SEQ, "getblockcount") + 5
        rs = htlc_script(hashlib.sha256(b"unused").digest(), rpub, lt, rpub)
        ft, fv, famt, fas = lock_htlc(SEQ, "w", rs, "500", assetlabel=asset); mine(SEQ)
        dest = bytes.fromhex(cli(SEQ, "validateaddress", cli(w(SEQ, "w"), "getnewaddress"))["scriptPubKey"])
        early = spend_htlc(rs, ft, fv, famt, fas, dest, 100000, rk, locktime=lt)
        try:
            cli(SEQ, "sendrawtransaction", early); print("   ! early refund accepted (unexpected)")
        except RuntimeError as e:
            print("   - refund BEFORE timeout correctly rejected (%s)." % str(e).split(":")[-1].strip()[:32])
        mine(SEQ, 6)
        late = spend_htlc(rs, ft, fv, famt, fas, dest, 100000, rk, locktime=cli(SEQ, "getblockcount"))
        cli(SEQ, "sendrawtransaction", late); mine(SEQ)
        print("   - refund AFTER timeout accepted: the locker reclaims their funds.\n")
        print("Done. (For the 'real-time' anchoring property -- BTC reorg reverts the")
        print("SEQ leg with it -- run: test/functional/feature_anchor_swap_consistency.py)")
    finally:
        for p in procs:
            try:
                p.send_signal(signal.SIGTERM)
            except Exception:
                pass
        for p in procs:
            try:
                p.wait(timeout=20)
            except Exception:
                p.kill()
        subprocess.run(["rm", "-rf", tmp])


if __name__ == "__main__":
    main()
