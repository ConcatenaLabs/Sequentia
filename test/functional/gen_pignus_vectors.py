#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""Emit golden vectors for the Pignus loan-vault covenant.

The vault covenant is proven here, against a real node, by
feature_pignus_vault.py. Anything else that has to build the same address -- the
platform daemon, a wallet, a browser -- is a second implementation, and a second
implementation that drifts by one byte produces a DIFFERENT taproot address and
silently sends collateral somewhere nobody can spend it.

So the proven builder emits vectors and every other implementation pins itself
to them. Run:

    PYTHONPATH=test/functional python3 test/functional/gen_pignus_vectors.py

Vectors cover the leaf scripts, the taproot output key, the control blocks and
the attestation message, across parameter sets chosen to exercise the encodings
that actually differ between implementations: small and large integers, the
CScript minimal-push boundary at 0x4c, and a locktime above the 500000000
time/height split.
"""

import json
import sys

import pignus_covenant as pig


def vault_case(name, **kw):
    tap, leaves = pig.vault_taptree(**kw)
    return {
        "name": name,
        "params": {k: ([b.hex() for b in v] if isinstance(v, (list, tuple))
                        else v.hex() if isinstance(v, bytes) else v)
                   for k, v in kw.items()},
        "leaves": {n: bytes(s).hex() for n, s in leaves.items()},
        "scriptPubKey": bytes(tap.scriptPubKey).hex(),
        "output_key": tap.output_pubkey.hex(),
        "negflag": tap.negflag,
        "control_blocks": {n: pig.control_block(tap, n).hex() for n in leaves},
    }


def main():
    COIN = 100_000_000
    base = dict(
        asset_c=bytes.fromhex("aa" * 32),
        asset_d=bytes.fromhex("bb" * 32),
        lender_prog=bytes.fromhex("cc" * 32),
        borrower_prog=bytes.fromhex("dd" * 32),
        lender_x=bytes.fromhex("ee" * 32),
        feed_id=bytes.fromhex("11" * 32),
        oracle_x=bytes.fromhex("22" * 32),
    )

    cases = [
        vault_case("typical", **base, debt=1500 * COIN,
                   strike=180 * pig.PRICE_SCALE, maturity=504, recover_after=604,
                   not_before=1_700_000_000, max_price=1_000_000 * pig.PRICE_SCALE),
        # Small operands: exercises OP_1..OP_16 and short minimal pushes.
        vault_case("small", **base, debt=1,
                   strike=2, maturity=1, recover_after=2,
                   not_before=0, max_price=3),
        # A time-based locktime (above the 500000000 height/time split) and a
        # debt in the top decade of what price_scale=1e5 permits (the ceiling is
        # ~878,000 units at 8dp; see the 64-bit bound in pignus-design.md).
        vault_case("timelock_and_large_debt", **base, debt=500_000 * COIN,
                   strike=1_000 * pig.PRICE_SCALE,
                   maturity=1_800_000_000, recover_after=1_802_592_000,
                   not_before=1_700_000_000, max_price=2_000 * pig.PRICE_SCALE),
        # A threshold oracle set: a different leaf shape entirely, so an
        # implementation that only handles the single-key form fails here rather
        # than silently deriving a wrong address for a 2-of-3 vault.
        vault_case("oracle_set_2_of_3",
                   **{k: v for k, v in base.items() if k != "oracle_x"},
                   oracles=[bytes.fromhex(h * 32) for h in ("22", "33", "44")],
                   oracle_threshold=2,
                   debt=1500 * COIN, strike=180 * pig.PRICE_SCALE,
                   maturity=504, recover_after=604, not_before=1_700_000_000,
                   max_price=1_000_000 * pig.PRICE_SCALE),
        # A 3-of-3 set, so the threshold is pinned as well as the key list.
        vault_case("oracle_set_3_of_3",
                   **{k: v for k, v in base.items() if k != "oracle_x"},
                   oracles=[bytes.fromhex(h * 32) for h in ("22", "33", "44")],
                   oracle_threshold=3,
                   debt=1500 * COIN, strike=180 * pig.PRICE_SCALE,
                   maturity=504, recover_after=604, not_before=1_700_000_000,
                   max_price=1_000_000 * pig.PRICE_SCALE),
        # SEGWIT V0 payouts. The browser wallet extension is a wpkhSlip77
        # wallet and can only receive at v0, so this shape is not exotic -- it
        # is the one every browser-originated loan uses, and an implementation
        # that assumes taproot payouts fails here rather than in production.
        vault_case("v0_payouts",
                   **{k: v for k, v in base.items()
                      if k not in ("lender_prog", "borrower_prog")},
                   lender_prog=bytes.fromhex("cc" * 20),
                   borrower_prog=bytes.fromhex("dd" * 20),
                   lender_ver=0, borrower_ver=0,
                   debt=1500 * COIN, strike=180 * pig.PRICE_SCALE,
                   maturity=504, recover_after=604, not_before=1_700_000_000,
                   max_price=1_000_000 * pig.PRICE_SCALE),
        # A non-default bonus and price scale.
        vault_case("custom_bonus_and_scale", **base, debt=250 * COIN,
                   strike=42 * 1000, maturity=1000, recover_after=1100,
                   not_before=1_700_000_000, bonus_num=110, bonus_den=100,
                   price_scale=1000, max_price=1_000_000),
    ]

    attestations = [
        {"feed_id": ("11" * 32), "timestamp": ts, "price": p,
         "message": pig.attestation_message(bytes.fromhex("11" * 32), ts, p).hex()}
        for ts, p in [(1_800_000_000, 170 * pig.PRICE_SCALE), (0, 1),
                      (2**63 - 1, 2**63 - 1)]
    ]

    seizures = [
        {"debt": d, "price": p, "bonus_num": bn, "bonus_den": bd,
         "price_scale": ps,
         "gross": pig.gross_owed(d, bn, bd),
         "seize": pig.seizure_atoms(d, p, bn, bd, ps)}
        for d, p, bn, bd, ps in [
            (1500 * COIN, 170 * pig.PRICE_SCALE, 105, 100, pig.PRICE_SCALE),
            (1500 * COIN, 400 * pig.PRICE_SCALE, 105, 100, pig.PRICE_SCALE),
            (1, 1, 105, 100, 1),                      # ceilings on both divisions
            (250 * COIN, 42 * 1000, 110, 100, 1000),
        ]
    ]

    out = {
        "_comment": "Generated by test/functional/gen_pignus_vectors.py from the "
                    "covenant proven by feature_pignus_vault.py. Do not hand-edit.",
        "nums": pig.NUMS.hex(),
        "leaf_version": 196,
        "price_scale_default": pig.PRICE_SCALE,
        "vaults": cases,
        "attestations": attestations,
        "seizures": seizures,
    }
    json.dump(out, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
