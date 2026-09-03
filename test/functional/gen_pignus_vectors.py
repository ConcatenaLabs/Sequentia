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
import pignus_offer as off


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

    # Funded resting offers, and the single-leaf vault they create. A browser
    # has to derive both to let a lender go offline, so both are pinned here.
    offers = []
    for name, vk, principal, collateral in [
        ("offer_v1", dict(
            asset_c=base["asset_c"], asset_d=base["asset_d"], debt=1500 * COIN,
            lender_prog=base["lender_prog"],
            feed_id=base["feed_id"], oracle_x=base["oracle_x"],
            strike=180 * pig.PRICE_SCALE, maturity=504, recover_after=604,
            not_before=1_700_000_000, max_price=1_000_000 * pig.PRICE_SCALE),
         1450 * COIN, 10 * COIN),
        ("offer_v0_payouts", dict(
            asset_c=base["asset_c"], asset_d=base["asset_d"], debt=1500 * COIN,
            lender_prog=bytes.fromhex("cc" * 20),
            lender_ver=0, borrower_ver=0,
            feed_id=base["feed_id"], oracle_x=base["oracle_x"],
            strike=180 * pig.PRICE_SCALE, maturity=504, recover_after=604,
            not_before=1_700_000_000, max_price=1_000_000 * pig.PRICE_SCALE),
         1450 * COIN, 10 * COIN),
        # An offer whose loans are judged by SEVERAL oracles. The single-leaf
        # vault an offer creates carries the whole m-of-n section, and the CLI
        # and the page both let a lender ask for one -- so without a case here
        # that form of the builder is pinned by nothing, in either language,
        # and a drift moves a live vault address with no test going red.
        ("offer_threshold_2_of_3", dict(
            asset_c=base["asset_c"], asset_d=base["asset_d"], debt=1500 * COIN,
            lender_prog=base["lender_prog"],
            feed_id=base["feed_id"],
            oracles=[bytes.fromhex(h * 32) for h in ("22", "33", "44")],
            oracle_threshold=2,
            strike=180 * pig.PRICE_SCALE, maturity=504, recover_after=604,
            not_before=1_700_000_000, max_price=1_000_000 * pig.PRICE_SCALE),
         1450 * COIN, 10 * COIN),
        # ...and the same with v0 payouts, which is what a browser-originated
        # loan looks like: the two variations are independent.
        ("offer_threshold_v0_payouts", dict(
            asset_c=base["asset_c"], asset_d=base["asset_d"], debt=1500 * COIN,
            lender_prog=bytes.fromhex("cc" * 20),
            lender_ver=0, borrower_ver=0,
            feed_id=base["feed_id"],
            oracles=[bytes.fromhex(h * 32) for h in ("22", "33", "44")],
            oracle_threshold=3,
            strike=180 * pig.PRICE_SCALE, maturity=504, recover_after=604,
            not_before=1_700_000_000, max_price=1_000_000 * pig.PRICE_SCALE),
         1450 * COIN, 10 * COIN),
    ]:
        borrower = (bytes.fromhex("dd" * 20) if vk.get("borrower_ver") == 0
                    else bytes.fromhex("dd" * 32))
        vault_tap, vault_leaf = off.offer_vault_taptree(
            borrower_prog=borrower, **vk)
        tap, leaves = off.offer_taptree(
            asset_c=vk["asset_c"], asset_d=vk["asset_d"], principal=principal,
            collateral=collateral, vault_kwargs=vk,
            expiry_locktime=1000)
        offers.append({
            "name": name,
            "params": {k: (v.hex() if isinstance(v, bytes) else
                           [x.hex() if isinstance(x, bytes) else x for x in v]
                           if isinstance(v, (list, tuple)) else v)
                       for k, v in vk.items()},
            "principal": principal, "collateral": collateral,
            "expiry_locktime": 1000,
            "borrower_prog": borrower.hex(),
            "vault_leaf": bytes(vault_leaf).hex(),
            "vault_scriptPubKey": bytes(vault_tap.scriptPubKey).hex(),
            "vault_negflag": vault_tap.negflag,
            "take_leaf": bytes(leaves["take"]).hex(),
            "refund_leaf": bytes(leaves["refund"]).hex(),
            "scriptPubKey": bytes(tap.scriptPubKey).hex(),
            "control_blocks": {n: off.control_block(tap, n).hex() for n in leaves},
        })

    # --- hashlock sweeps: the cross-chain payments ---------------------------
    # Both legs of a native-Bitcoin loan are paid on Sequentia through one of
    # these: the principal the borrower opens with `w`, and the repayment the
    # lender opens with `t`. A browser has to rebuild both addresses before it
    # commits any Bitcoin, so both shapes are pinned here.
    hashlocks = []
    for name, kw in [
        ("hashlock_v1", dict(
            preimage_hash=bytes.fromhex("77" * 32), asset=base["asset_d"],
            payee_prog=base["borrower_prog"], refund_after=900,
            refund_prog=base["lender_prog"])),
        # What a browser-originated loan actually uses: the wallet extension
        # receives at segwit v0, so the payee program is 20 bytes.
        ("hashlock_v0_payee", dict(
            preimage_hash=bytes.fromhex("88" * 32), asset=base["asset_d"],
            payee_prog=bytes.fromhex("dd" * 20), payee_ver=0,
            refund_after=1_800_000_000, refund_prog=bytes.fromhex("cc" * 20),
            refund_ver=0)),
    ]:
        tap, leaves = pig.hashlock_taptree(**kw)
        hashlocks.append({
            "name": name,
            "params": {k: (v.hex() if isinstance(v, bytes) else v)
                       for k, v in kw.items()},
            "leaves": {n: bytes(sc).hex() for n, sc in leaves.items()},
            "scriptPubKey": bytes(tap.scriptPubKey).hex(),
            "output_key": tap.output_pubkey.hex(),
            "negflag": tap.negflag,
            "control_blocks": {n: pig.control_block(tap, n).hex() for n in leaves},
        })

    # --- repurchases (Tier D) -----------------------------------------------
    # A repurchase is not a loan and does not use vault_taptree: RETURN is
    # build_repay_leaf with the lender payout set to C_U(borrower), FORFEIT is
    # build_recover_leaf with a payout of its own, and the two are composed into
    # a tree of two. The browser has its own implementation of that composition,
    # so it needs its own vectors or it is not pinned to anything.
    repurchases = []
    for name, (borrower_ver, lender_ver) in (
            ("taproot-payouts", (1, 1)), ("segwit-v0-payouts", (0, 0))):
        coll = "11" * 32
        money = "22" * 32
        cu = "33" * 32                       # C_U(borrower), always a P2TR
        bprog = ("44" * 32) if borrower_ver == 1 else ("44" * 20)
        lprog = ("55" * 32) if lender_ver == 1 else ("55" * 20)
        terms = {
            "collateral_asset": coll, "collateral_amount": 100 * 10**8,
            "debt_asset": money, "principal": 700 * 10**8, "debt": 750 * 10**8,
            "collateral_value": 1000 * 10**8,
            "borrower_cu": cu, "borrower_prog": bprog, "lender_prog": lprog,
            "borrower_ver": borrower_ver, "lender_ver": lender_ver,
            "forfeit_after": 900_000,
        }
        ret = pig.build_repay_leaf(
            bytes.fromhex(money)[::-1], bytes.fromhex(coll)[::-1],
            terms["collateral_amount"], bytes.fromhex(cu),
            bytes.fromhex(lprog), 1, lender_ver)
        forfeit = pig.build_recover_leaf(
            terms["forfeit_after"], bytes.fromhex(money)[::-1],
            bytes.fromhex(bprog), borrower_ver)
        tap = pig.taproot_construct(pig.NUMS, [("return", ret),
                                               ("forfeit", forfeit)])
        repurchases.append({
            "name": name,
            "terms": terms,
            "bond": terms["collateral_value"] - terms["debt"],
            "leaves": {"return": bytes(ret).hex(), "forfeit": bytes(forfeit).hex()},
            "script_pubkey": bytes(tap.scriptPubKey).hex(),
            "negflag": tap.negflag,
            "control_blocks": {n: pig.control_block(tap, n).hex()
                               for n in ("return", "forfeit")},
        })

    out = {
        "_comment": "Generated by test/functional/gen_pignus_vectors.py from the "
                    "covenant proven by feature_pignus_vault.py. Do not hand-edit.",
        "nums": pig.NUMS.hex(),
        "leaf_version": 196,
        "price_scale_default": pig.PRICE_SCALE,
        "vaults": cases,
        "attestations": attestations,
        "seizures": seizures,
        "offers": offers,
        "repurchase": repurchases,
        "hashlocks": hashlocks,
    }
    json.dump(out, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
