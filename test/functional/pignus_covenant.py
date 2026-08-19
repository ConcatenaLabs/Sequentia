#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""Pignus loan-vault covenant: non-custodial collateralised lending on Sequentia.

A borrower locks C units of an explicit collateral asset in ONE taproot UTXO --
"the loan is the coin". The output has internal key = NUMS (no key-path spend)
and a four-leaf tree, every loan term baked in as a compile-time constant and
therefore committed inside the taproot output key. Neither party, nor the
platform, nor the oracle can alter a term after origination; they can only
satisfy one of the four exits.

  REPAY      (permissionless, NO signature, NO oracle) -- anyone may spend the
             vault iff the transaction pays the lender >= `debt` of the debt
             asset at the lender's pinned scriptPubKey AND returns the WHOLE
             collateral to the borrower's pinned scriptPubKey. Needs no witness
             data at all. This is the path that makes an oracle outage
             survivable: a solvent borrower can always exit without anyone's
             cooperation.

  LIQUIDATE  (permissionless, oracle-attested) -- spendable iff a BIP340
             signature by the pinned oracle key over `feed_id || ts || price`
             verifies, `price < strike`, and `ts >= not_before`. The liquidator
             must pay the lender `debt` and return the surplus collateral
             `C - seize` to the borrower, where `seize` is computed ON CHAIN
             from the attested price. The liquidator's profit is exactly the
             baked liquidation bonus -- it cannot be inflated by seizing more.

  DEFAULT    (permissionless, oracle-attested, after `maturity`) -- LIQUIDATE
             without the price test: once the term is up the position is
             callable at any price. Surplus still returns to the borrower.

  RECOVER    <recover_after> CLTV DROP <P_lender> CHECKSIG -- the oracle-liveness
             backstop. If the oracle is dead through the whole grace window the
             lender sweeps the vault. Deliberately blunt, deliberately last, and
             reachable only long after the borrower's oracle-free REPAY window
             has been ignored.

Design points, and why each is the way it is:

  * Explicit-only. Every introspected asset/value prefix must be 0x01. A blinded
    (confidential) output the covenant cannot read is rejected outright. This is
    the normal case on transparent-by-default Sequentia (a confidential loan
    would have to use an interactive tier instead).

  * Input-bound output map (anti-aliasing). The covenant input at consensus index
    k credits the lender at output 2k and returns collateral at output 2k+1.
    Because the index is derived per input from OP_PUSHCURRENTINPUTINDEX, two
    vault inputs can never both point at one shared lender-credit output, so a
    single payment can never settle two loans. Same guarantee the SeqOB FILL leaf
    relies on (seqob_covenant.py).

  * Fixed total repayment. `debt` is principal plus the whole term's interest,
    fixed at origination, so no interest accrual is computed on chain. Term
    loans, not perpetual positions.

  * Surplus is enforced, not promised. Liquidation and default both compute
    `seize` from the attested price with OP_ADD64/OP_SUB64/OP_DIV64 and require
    `C - seize` back to the borrower. A liquidator paying the debt cannot keep
    the whole collateral.

  * The oracle is trusted for ONE number and nothing else. It cannot move funds,
    cannot choose a recipient, cannot trigger a default before `maturity`, and
    cannot stop a repayment. Its only power is to assert a price low enough to
    open the LIQUIDATE leaf.

Overflow bound (documented, asserted by the builder). OP_ADD64 aborts on
signed-64-bit overflow. The only large value formed on chain is
`gross * price_scale + price - 1`, where `gross = ceil(debt * bonus_num /
bonus_den)` is folded at build time. The leaf builders assert
`gross * price_scale + bound < 2**63`, so a vault that would abort mid-spend
cannot be constructed in the first place.

Replay window (documented, real). The covenant can test that an attestation is
NEWER than `not_before`, but nothing in tapscript can test that it is RECENT: a
liquidator who saved a signed attestation from a genuine dip may present it
later, after the price has recovered. The position WAS liquidatable at that
moment, so this is a timing advantage rather than a theft, and the borrower's
cure is the same either way -- repay, or top up before the dip. Narrowing the
window is an oracle-side epoch commitment, specified in
doc/sequentia/pignus-design.md section 5.3.
"""

from test_framework.script import (
    CScript, taproot_construct,
    OP_0, OP_1, OP_1ADD, OP_2DROP, OP_2DUP, OP_ADD, OP_CAT,
    OP_CHECKLOCKTIMEVERIFY, OP_CHECKSIG, OP_CHECKSIGFROMSTACK,
    OP_CHECKSIGFROMSTACKVERIFY, OP_DROP, OP_DUP, OP_ELSE, OP_ENDIF, OP_EQUAL,
    OP_EQUALVERIFY, OP_FROMALTSTACK, OP_GREATERTHANOREQUAL, OP_IF, OP_LESSTHAN,
    OP_NIP, OP_ROT, OP_SWAP, OP_TOALTSTACK, OP_VERIFY,
    OP_INSPECTINPUTVALUE, OP_PUSHCURRENTINPUTINDEX,
    OP_INSPECTOUTPUTASSET, OP_INSPECTOUTPUTVALUE, OP_INSPECTOUTPUTSCRIPTPUBKEY,
    OP_INSPECTNUMOUTPUTS,
    OP_ADD64, OP_SUB64, OP_DIV64, OP_GREATERTHANOREQUAL64, OP_LESSTHAN64,
)

# BIP341 nothing-up-my-sleeve point: taproot internal key with no known discrete
# log, so a NUMS-internal-key output has no key-path spend. Same constant the
# SeqOB covenant pins.
NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")

# Default fixed-point scale for oracle prices. A price is quoted as
#   price = round(debt_atoms_per_collateral_atom * PRICE_SCALE)
# which keeps every realistic pair inside 64 bits with ~1e-5 relative precision.
# See the overflow bound in the module docstring.
PRICE_SCALE = 100_000


def le8(n):
    """A 64-bit little-endian push constant, the on-stack form of OP_*64 operands."""
    assert 0 <= n < (1 << 63), f"le8 operand out of range: {n}"
    return int(n).to_bytes(8, "little")


# Index-map helpers. The covenant input at consensus index k credits the lender
# at output 2k and returns collateral at output 2k+1. Recomputed from
# OP_PUSHCURRENTINPUTINDEX each time, so the leaf carries no per-spend state.
_CREDIT_IDX = [OP_PUSHCURRENTINPUTINDEX, OP_DUP, OP_ADD]           # 2k
_RETURN_IDX = [OP_PUSHCURRENTINPUTINDEX, OP_DUP, OP_ADD, OP_1ADD]  # 2k + 1


def gross_owed(debt, bonus_num, bonus_den):
    """The debt-asset amount a seizure must cover: the debt plus the liquidation
    bonus that pays the liquidator for doing the work. Folded at build time
    because every input is a baked constant."""
    return -(-debt * bonus_num // bonus_den)   # ceil(debt * num / den)


def _require_lender_credit(asset_d, lender_prog, debt):
    """Output 2k pays the lender at least `debt` of the explicit debt asset at
    the pinned scriptPubKey. Leaves the stack as it found it."""
    return (
        _CREDIT_IDX + [OP_INSPECTOUTPUTASSET, OP_1, OP_EQUALVERIFY, asset_d, OP_EQUALVERIFY] +
        _CREDIT_IDX + [OP_INSPECTOUTPUTSCRIPTPUBKEY, OP_1, OP_EQUALVERIFY, lender_prog, OP_EQUALVERIFY] +
        _CREDIT_IDX + [OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY,
                       le8(debt), OP_GREATERTHANOREQUAL64, OP_VERIFY]
    )


def _borrower_return_value(asset_c, borrower_prog):
    """Push the collateral amount returned to the borrower at output 2k+1, or 0
    if no such output exists. Mirrors the SeqOB remainder probe: an output that
    is absent, or carries a different asset, contributes nothing -- so a spender
    who owes a return and omits it fails the comparison that follows."""
    return (
        _RETURN_IDX + [OP_INSPECTNUMOUTPUTS, OP_LESSTHAN] +
        [OP_IF] +
        _RETURN_IDX + [OP_INSPECTOUTPUTASSET, OP_1, OP_EQUALVERIFY, asset_c, OP_EQUAL] +
        [OP_IF] +
        _RETURN_IDX + [OP_INSPECTOUTPUTSCRIPTPUBKEY, OP_1, OP_EQUALVERIFY,
                       borrower_prog, OP_EQUALVERIFY] +
        _RETURN_IDX + [OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY] +
        [OP_ELSE] +
        [le8(0)] +
        [OP_ENDIF] +
        [OP_ELSE] +
        [le8(0)] +
        [OP_ENDIF]
    )


def build_repay_leaf(asset_c, asset_d, debt, lender_prog, borrower_prog):
    """REPAY: permissionless, oracle-free, no witness data.

    Anyone -- the borrower, a friend, a refinancing bot -- may close the loan by
    paying the lender `debt` and returning the WHOLE collateral to the borrower.
    Because both destinations are pinned, a third-party repayer can only make
    the borrower better off.
    """
    assert len(asset_c) == 32 and len(asset_d) == 32
    assert len(lender_prog) == 32 and len(borrower_prog) == 32
    assert debt >= 1

    s = []
    # locked = this covenant input's own value (must be explicit)
    s += [OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTVALUE, OP_1, OP_EQUALVERIFY]  # [C]
    # the lender is made whole
    s += _require_lender_credit(asset_d, lender_prog, debt)                      # [C]
    # the borrower gets ALL of it back: returned >= C
    s += _RETURN_IDX + [OP_INSPECTOUTPUTASSET, OP_1, OP_EQUALVERIFY, asset_c, OP_EQUALVERIFY]
    s += _RETURN_IDX + [OP_INSPECTOUTPUTSCRIPTPUBKEY, OP_1, OP_EQUALVERIFY,
                        borrower_prog, OP_EQUALVERIFY]
    s += _RETURN_IDX + [OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY]             # [C, returned]
    s += [OP_SWAP, OP_GREATERTHANOREQUAL64]                                      # returned >= C
    return CScript(s)


def _seizure_tail(asset_c, asset_d, debt, lender_prog, borrower_prog,
                  gross, price_scale):
    """The shared tail of LIQUIDATE and DEFAULT, entered with [price] on the
    stack: compute the seizure from the attested price, pay the lender, and
    force the surplus back to the borrower.

        seize = ceil(gross * price_scale / price)

    `gross` (debt plus liquidation bonus) is a build-time constant, so the only
    on-chain arithmetic is one add, one subtract and one division.
    """
    s = []
    # num = gross*scale + price - 1, then seize = num / price
    s += [OP_DUP]                                                # [price, price]
    s += [le8(gross * price_scale), OP_ADD64, OP_VERIFY]         # [price, G+price]
    s += [le8(1), OP_SUB64, OP_VERIFY]                           # [price, num]
    s += [OP_SWAP, OP_DIV64, OP_VERIFY, OP_NIP]                  # [seize]
    # required_return = C - seize (may be <= 0 for an underwater vault)
    s += [OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTVALUE, OP_1, OP_EQUALVERIFY]  # [seize, C]
    s += [OP_SWAP, OP_SUB64, OP_VERIFY]                          # [required_return]
    # the lender is made whole
    s += _require_lender_credit(asset_d, lender_prog, debt)      # [required_return]
    # the surplus goes back to the borrower
    s += _borrower_return_value(asset_c, borrower_prog)          # [required_return, returned]
    s += [OP_SWAP, OP_GREATERTHANOREQUAL64]                      # returned >= required_return
    return s


def _oracle_check(feed_id, oracle_x, not_before):
    """Verify the oracle attestation and leave [price] on the stack.

    Witness supplies [sig64, price8, ts8] (ts on top). The message the oracle
    signed is the 48-byte `feed_id || ts || price`, reassembled here with OP_CAT
    so the covenant checks a signature over exactly the numbers it then uses --
    there is no separate, unauthenticated copy of the price anywhere.
    """
    s = []
    s += [OP_DUP, OP_TOALTSTACK]        # alt:[ts]        main:[sig, price, ts]
    s += [OP_SWAP]                      #                 main:[sig, ts, price]
    s += [OP_DUP, OP_TOALTSTACK]        # alt:[ts, price] main:[sig, ts, price]
    s += [OP_CAT]                       #                 main:[sig, ts||price]
    s += [feed_id, OP_SWAP, OP_CAT]     #                 main:[sig, msg]
    s += [oracle_x, OP_CHECKSIGFROMSTACKVERIFY]           # main:[]
    s += [OP_FROMALTSTACK, OP_FROMALTSTACK]               # main:[price, ts]
    # the attestation must post-date origination
    s += [le8(not_before), OP_GREATERTHANOREQUAL64, OP_VERIFY]   # [price]
    return s


def _oracle_slot(feed_id, oracle_x, not_before, strike):
    """One oracle's slot in a threshold set.

    Consumes this slot's witness triple `(price, ts, sig)` (sig on top) and
    leaves NOTHING on the main stack: the slot's effective price and its
    accept/reject flag both go to the alt stack, so the next slot's witness
    triple is on top when it runs. Piling results on the main stack instead
    would bury the witness data that has not been read yet.

    An EMPTY signature is an abstention, and this is a property of the opcode
    rather than a convention: `OP_CHECKSIGFROMSTACK` pushes false for an empty
    signature but ABORTS the script for a non-empty invalid one. So a spender
    can present a real signature or none at all, and cannot present rubbish to
    fill a slot.

    A slot that accepts contributes its price; a slot that abstains contributes
    zero, which can never win the maximum below because a zero price would fail
    the division in the seizure anyway.
    """
    s = [OP_TOALTSTACK]                       # stash sig; [price, ts]
    s += [OP_2DUP, OP_SWAP, OP_CAT]           # [price, ts, ts||price]
    s += [feed_id, OP_SWAP, OP_CAT]           # [price, ts, msg]
    s += [OP_FROMALTSTACK, OP_SWAP]           # [price, ts, sig, msg]
    s += [oracle_x, OP_CHECKSIGFROMSTACK]     # [price, ts, ok]
    s += [OP_IF]
    #   accepted: this oracle's own timestamp and price must each pass
    s += [le8(not_before), OP_GREATERTHANOREQUAL64, OP_VERIFY]   # [price]
    if strike is not None:
        s += [OP_DUP, le8(strike), OP_LESSTHAN64, OP_VERIFY]     # [price]
    s += [OP_TOALTSTACK, OP_1, OP_TOALTSTACK]        # alt += (price, 1)
    s += [OP_ELSE]
    #   abstained
    s += [OP_2DROP, le8(0), OP_TOALTSTACK, OP_0, OP_TOALTSTACK]  # alt += (0, 0)
    s += [OP_ENDIF]
    return s


def _oracle_check_threshold(feed_id, oracle_keys, threshold, not_before, strike):
    """Verify a threshold of INDEPENDENT oracles and leave [price] on the stack.

    Each oracle signs its own `(ts, price)`; they never have to agree on a byte,
    which matters because requiring several independent price sources to produce
    an identical timestamp and an identical price is a coordination protocol, not
    an oracle set. Each accepted price must independently clear the strike, so a
    liquidation needs `threshold` oracles to agree the position is under water.

    The price carried into the seizure is the MAXIMUM of the accepted prices.
    That is the borrower-favourable choice -- a higher price means less
    collateral seized -- and it also removes the incentive to shop: presenting
    extra low attestations cannot drag the price down, because the spender must
    still clear the threshold and the largest of whatever they present is what
    counts. A liquidator's best play is to present exactly the `threshold`
    lowest attestations they hold, which makes the effective price the
    threshold-th lowest of the set: a robust quantile rather than any single
    oracle's number.

    Compared with a FROST/MuSig group key (one key, one signature, no script
    change), this costs script size and buys independence: these oracles never
    run a joint signing protocol, so there is no coordinator to compromise and
    no liveness coupling between them. Both are supported; a vault with a single
    key gets the cheap path in `_oracle_check`.
    """
    n = len(oracle_keys)
    s = []
    for key in oracle_keys:
        s += _oracle_slot(feed_id, key, not_before, strike)
    # alt is [eff_0, c_0, ..., eff_{n-1}, c_{n-1}]; unwind it into a running
    # (count, max) pair.
    s += [OP_FROMALTSTACK, OP_FROMALTSTACK]        # [c_{n-1}, eff_{n-1}]
    for _ in range(n - 1):
        s += [OP_FROMALTSTACK, OP_ROT, OP_ADD, OP_SWAP]   # count += c_i
        s += [OP_FROMALTSTACK]                             # [count, max, eff_i]
        s += [OP_2DUP, OP_GREATERTHANOREQUAL64]            # max >= eff_i ?
        s += [OP_IF, OP_DROP, OP_ELSE, OP_NIP, OP_ENDIF]   # [count, max']
    s += [OP_SWAP, threshold, OP_GREATERTHANOREQUAL, OP_VERIFY]   # [max]
    return s


def _resolve_oracles(oracle_x, oracles, threshold):
    """Normalise the two ways of naming an oracle to (keys, threshold).

    `oracle_x` is the single-key form and stays byte-identical to what shipped;
    `oracles` names a set. Supplying both is a mistake worth refusing rather than
    silently preferring one, because the two produce different addresses.
    """
    if oracles:
        if oracle_x is not None:
            raise ValueError("give oracle_x or oracles, not both: they compile "
                             "to different vaults")
        keys = list(oracles)
        for k in keys:
            if len(k) != 32:
                raise ValueError("each oracle key must be 32 bytes (x-only)")
        if len(set(keys)) != len(keys):
            raise ValueError("duplicate oracle key: one signer would fill two "
                             "slots and the threshold would not mean what it says")
        t = len(keys) if threshold is None else int(threshold)
        if not 1 <= t <= len(keys):
            raise ValueError(f"threshold {t} outside 1..{len(keys)}")
        return keys, t
    if oracle_x is None:
        raise ValueError("a vault needs an oracle: pass oracle_x or oracles")
    if len(oracle_x) != 32:
        raise ValueError("oracle_x must be 32 bytes (x-only)")
    if threshold not in (None, 1):
        raise ValueError("threshold > 1 needs an oracle SET, not one key")
    return [oracle_x], 1


def _oracle_section(feed_id, oracle_keys, threshold, not_before, strike):
    """The single-key fast path, or the threshold path. The single-key script is
    unchanged from what shipped, so vaults already funded keep their addresses."""
    if len(oracle_keys) == 1 and threshold == 1:
        s = _oracle_check(feed_id, oracle_keys[0], not_before)
        if strike is not None:
            s = s + [OP_DUP, le8(strike), OP_LESSTHAN64, OP_VERIFY]
        return s
    return _oracle_check_threshold(feed_id, oracle_keys, threshold,
                                   not_before, strike)


def build_liquidate_leaf(asset_c, asset_d, debt, lender_prog, borrower_prog,
                         feed_id, oracle_x, strike, not_before,
                         bonus_num=105, bonus_den=100, price_scale=PRICE_SCALE,
                         max_price=None, oracles=None, oracle_threshold=None):
    """LIQUIDATE: permissionless seizure while the attested price is under the
    strike. Pays the lender, pays the liquidator the baked bonus, returns the
    rest to the borrower.

    Name ONE oracle with `oracle_x`, or a set with `oracles` plus an
    `oracle_threshold`; see _oracle_check_threshold for what the set buys."""
    assert len(feed_id) == 32
    assert 1 <= strike < (1 << 63)
    assert bonus_num >= bonus_den >= 1
    keys, threshold = _resolve_oracles(oracle_x, oracles, oracle_threshold)
    gross = gross_owed(debt, bonus_num, bonus_den)
    # Overflow bound: the largest value the leaf ever forms is
    # gross*scale + price - 1, and LIQUIDATE only runs for price < strike.
    bound = max_price if max_price is not None else strike
    assert gross * price_scale + bound < (1 << 63), (
        "loan too large for 64-bit seizure arithmetic: reduce debt, or the "
        f"price_scale (gross={gross}, scale={price_scale}, bound={bound})")

    s = _oracle_section(feed_id, keys, threshold, not_before, strike)  # [price]
    s += _seizure_tail(asset_c, asset_d, debt, lender_prog, borrower_prog,
                       gross, price_scale)
    return CScript(s)


def build_default_leaf(asset_c, asset_d, debt, lender_prog, borrower_prog,
                       feed_id, oracle_x, maturity, not_before,
                       bonus_num=105, bonus_den=100, price_scale=PRICE_SCALE,
                       max_price=None, oracles=None, oracle_threshold=None):
    """DEFAULT: LIQUIDATE without the price test, gated on the term being up.

    Permissionless on purpose. At maturity the debt is due at ANY price, so
    anyone may call the loan; the covenant still forces the surplus home, which
    is what makes it safe to let anyone do it."""
    assert len(feed_id) == 32
    keys, threshold = _resolve_oracles(oracle_x, oracles, oracle_threshold)
    gross = gross_owed(debt, bonus_num, bonus_den)
    # DEFAULT has no strike, so the caller must declare the highest price the
    # oracle can ever quote for this feed; the assert then pins the same bound
    # LIQUIDATE gets for free.
    bound = max_price if max_price is not None else (1 << 40)
    assert gross * price_scale + bound < (1 << 63), (
        "loan too large for 64-bit seizure arithmetic at the declared max_price "
        f"(gross={gross}, scale={price_scale}, bound={bound})")

    s = [maturity, OP_CHECKLOCKTIMEVERIFY, OP_DROP]
    s += _oracle_section(feed_id, keys, threshold, not_before, None)  # [price]
    s += _seizure_tail(asset_c, asset_d, debt, lender_prog, borrower_prog,
                       gross, price_scale)
    return CScript(s)


def build_recover_leaf(recover_after, lender_x):
    """RECOVER: the oracle-liveness backstop. Only the lender, only long after
    maturity, and only because a dead oracle must not freeze the collateral for
    ever. A borrower who does not want to reach this leaf has the whole term to
    take the oracle-free REPAY exit."""
    assert len(lender_x) == 32
    return CScript([recover_after, OP_CHECKLOCKTIMEVERIFY, OP_DROP, lender_x, OP_CHECKSIG])


def vault_taptree(*, asset_c, asset_d, debt, lender_prog, borrower_prog,
                  lender_x, feed_id, strike, maturity, recover_after,
                  not_before, oracle_x=None, oracles=None, oracle_threshold=None,
                  bonus_num=105, bonus_den=100,
                  price_scale=PRICE_SCALE, max_price=None, internal_key=NUMS):
    """Build the {REPAY, LIQUIDATE, DEFAULT, RECOVER} taproot vault.

    internal_key defaults to NUMS so there is no key-path spend: the four leaves
    are the ONLY ways out, and a borrower verifying a vault before funding it
    must reject any vault whose internal key is not NUMS.
    """
    assert recover_after > maturity, "RECOVER must sit strictly after maturity"
    repay = build_repay_leaf(asset_c, asset_d, debt, lender_prog, borrower_prog)
    liquidate = build_liquidate_leaf(asset_c, asset_d, debt, lender_prog, borrower_prog,
                                     feed_id, oracle_x, strike, not_before,
                                     bonus_num, bonus_den, price_scale, max_price,
                                     oracles, oracle_threshold)
    default = build_default_leaf(asset_c, asset_d, debt, lender_prog, borrower_prog,
                                 feed_id, oracle_x, maturity, not_before,
                                 bonus_num, bonus_den, price_scale, max_price,
                                 oracles, oracle_threshold)
    recover = build_recover_leaf(recover_after, lender_x)
    tap = taproot_construct(internal_key, [
        ("repay", repay), ("liquidate", liquidate),
        ("default", default), ("recover", recover),
    ])
    return tap, {"repay": repay, "liquidate": liquidate,
                 "default": default, "recover": recover}


def control_block(tap, leaf_name):
    """The taproot control block for a script-path spend of the named leaf."""
    leaf = tap.leaves[leaf_name]
    return bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch


def attestation_message(feed_id, timestamp, price):
    """The exact 48 bytes the oracle signs, and the exact bytes the covenant
    reassembles with OP_CAT. One definition, used by the signer and the spender,
    so the two can never drift apart."""
    assert len(feed_id) == 32
    return feed_id + le8(timestamp) + le8(price)


def seizure_atoms(debt, price, bonus_num=105, bonus_den=100, price_scale=PRICE_SCALE):
    """The collateral the covenant will let a liquidator keep at `price`. The
    off-chain mirror of _seizure_tail, so wallets and the test suite can predict
    a spend without evaluating the script."""
    gross = gross_owed(debt, bonus_num, bonus_den)
    return -(-gross * price_scale // price)   # ceil(gross * scale / price)


def repay_witness(tap, leaves):
    """REPAY reads everything it needs from the transaction: no witness data."""
    return [bytes(leaves["repay"]), control_block(tap, "repay")]


def oracle_witness(tap, leaves, leaf_name, sig, price, timestamp):
    """LIQUIDATE / DEFAULT witness for a SINGLE-oracle vault: [sig, price, ts]
    then leaf and control block. The script pops ts first, so ts is pushed last."""
    assert len(sig) == 64
    return [sig, le8(price), le8(timestamp),
            bytes(leaves[leaf_name]), control_block(tap, leaf_name)]


def threshold_oracle_witness(tap, leaves, leaf_name, slots):
    """LIQUIDATE / DEFAULT witness for a THRESHOLD vault.

    `slots` is one entry per oracle key, in the SAME ORDER the vault was built
    with, each either None (abstain) or a `(sig, price, timestamp)` triple. The
    leaf reads slot 0 first, so slots are pushed in reverse; within a slot the
    order is `price, ts, sig` because the script stashes the signature first.

    An abstaining slot still needs its price and timestamp pushed -- the script
    drops them in the else branch -- and its signature is the empty push, which
    is what makes OP_CHECKSIGFROMSTACK return false instead of aborting.
    """
    w = []
    for slot in reversed(slots):
        if slot is None:
            w += [le8(0), le8(0), b""]
        else:
            sig, price, ts = slot
            assert len(sig) == 64, "a present slot needs a 64-byte signature"
            w += [le8(price), le8(ts), sig]
    return w + [bytes(leaves[leaf_name]), control_block(tap, leaf_name)]


def recover_witness(tap, leaves, sig_lender):
    """RECOVER spends with the lender's schnorr signature over the leaf."""
    return [sig_lender, bytes(leaves["recover"]), control_block(tap, "recover")]
