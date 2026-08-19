#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""Funded resting loan offers: the lender's principal IS the offer.

A signed offer is a promise. A lender who posts one has to be online to co-sign
when a borrower turns up, can post the same principal to ten venues at once, and
can walk away between the quote and the signature. A FUNDED offer is a coin: the
lender locks the principal in a covenant whose only non-refund exit hands it to
anyone who, in the same transaction, locks the agreed collateral in a correctly
formed loan vault. The lender can then be offline, cannot oversell, and cannot
change the terms -- because the terms are inside the address, exactly as they
are for the vault itself.

The hard part, and how it is solved
-----------------------------------
The offer must satisfy itself that output `2k` really is a Pignus vault on the
agreed terms. Every term is known when the offer is written EXCEPT one: the
borrower's key, which only exists when someone takes it. So the covenant has to
compute a taproot address from a witness-supplied key, in script.

That is possible -- `OP_SHA256INITIALIZE/UPDATE/FINALIZE` hash arbitrary-length
data, and `OP_TWEAKVERIFY` checks `Q = P + t*G` -- but only if the tree is
shallow enough to rebuild. A BIP341 TapBranch sorts its two children
lexicographically, and tapscript has no lexicographic comparison, so a
multi-leaf tree cannot be recomputed honestly. Letting the witness supply the
branch ORDER instead is not a shortcut, it is a hole: a taker who supplies the
wrong order computes a root for which no valid control block exists, funds a
vault nobody can ever spend, and walks off with the principal while the lender
is left with a dead output.

So an offer-originated vault uses a SINGLE leaf holding all four exits behind a
selector. One leaf means the Merkle root IS the leaf hash: no branch, no sort,
no hole. The four exit bodies are the same code the four-leaf vault uses --
imported, not re-written -- so the two formats cannot drift apart in what they
enforce.

What it costs: a single-leaf vault reveals the whole ~1 kB script on every exit
instead of a 192-byte REPAY leaf and a 97-byte control block. That is the price
of the lender being able to go offline, it is paid by whoever exits the loan,
and it is stated here so nobody discovers it in a fee estimate.

What the offer enforces, and what it deliberately does not
----------------------------------------------------------
The covenant input at index `k` uses output `2k` for the vault and output `2k+1`
for its own remainder, the same input-bound map the vault uses, so an offer
input and a vault input in one transaction can never point at the same output.

It requires: output `2k` carries at least `collateral` of the explicit
collateral asset at the address reconstructed from the witness key; and the
amount drawn from the offer is EXACTLY `principal`, with any remainder re-paid
to the same offer covenant so the rest stays available for the next borrower.

It does NOT require the principal to be paid to the borrower. The taker builds
the transaction and is the borrower; a taker who fails to pay themselves has
only robbed themselves, and every output the LENDER depends on is already
pinned. Enforcing it would be script spent protecting someone from themselves.

A taker who supplies a key nobody controls locks their own collateral in a vault
whose surplus goes nowhere -- but the lender is untouched, because REPAY,
LIQUIDATE and DEFAULT are all permissionless and all pay the lender at a pinned
script. The taker's mistake stays the taker's.

One constraint this places on a taker, worth stating because it is easy to trip
over: when the WHOLE offer is drawn there is no remainder, and output `2k+1`
must then carry something that is not the debt asset. The covenant reads asset D
at `2k+1` as a claim that the remainder rests there, and will demand it be paid
back to the offer. Putting the borrower's own drawn principal at that index is
the obvious mistake; the collateral change belongs there instead.

Divisibility is by repeated taking, not by partial fill: each take draws exactly
one `principal` and the remainder re-rests. A partial take would be a different
loan size, hence a different debt and a different collateral requirement, hence
a different vault address -- which would mean computing the vault's constants
inside the script from the amount taken. Fixed lots with self-replication give a
lender the same divisibility for none of that complexity.
"""

import hashlib

from test_framework.script import (
    CScript, taproot_construct, TaggedHash,
    OP_0, OP_1, OP_2, OP_CAT, OP_CHECKLOCKTIMEVERIFY, OP_CHECKSIG, OP_DROP,
    OP_DUP, OP_ELSE, OP_ENDIF, OP_EQUAL, OP_EQUALVERIFY, OP_IF, OP_LESSTHAN,
    OP_NIP, OP_OVER, OP_ROT, OP_SWAP, OP_VERIFY,
    OP_SHA256INITIALIZE, OP_SHA256UPDATE, OP_SHA256FINALIZE, OP_TWEAKVERIFY,
    OP_INSPECTINPUTVALUE, OP_INSPECTINPUTSCRIPTPUBKEY, OP_PUSHCURRENTINPUTINDEX,
    OP_INSPECTOUTPUTASSET, OP_INSPECTOUTPUTVALUE, OP_INSPECTOUTPUTSCRIPTPUBKEY,
    OP_INSPECTNUMOUTPUTS,
    OP_SUB64, OP_GREATERTHANOREQUAL64,
)

import pignus_covenant as pig
from pignus_covenant import NUMS, le8, _CREDIT_IDX, _RETURN_IDX

LEAF_VERSION = 0xc4

# A value that cannot occur as a real x-only key inside a script we build, used
# to locate where the borrower key sits so the offer can splice a witness-
# supplied one in at the same places. Splitting on a sentinel keeps the two
# implementations -- the leaf and the offer that hashes it -- in lockstep by
# construction: there is no hand-maintained list of offsets to get wrong.
_SENTINEL = bytes.fromhex("ba5eba11" * 8)


def _compact_size(n):
    if n < 0xfd:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + n.to_bytes(2, "little")
    if n <= 0xffffffff:
        return b"\xfe" + n.to_bytes(4, "little")
    return b"\xff" + n.to_bytes(8, "little")


def _tag_prefix(tag):
    """The doubled tag hash BIP340 tagged hashing starts from."""
    h = hashlib.sha256(tag.encode()).digest()
    return h + h


# --------------------------------------------------------- the single leaf

def offer_vault_leaf(*, asset_c, asset_d, debt, lender_prog, borrower_prog,
                     lender_x, feed_id, strike, maturity, recover_after,
                     not_before, oracle_x=None, oracles=None,
                     oracle_threshold=None, bonus_num=105, bonus_den=100,
                     price_scale=pig.PRICE_SCALE, max_price=None):
    """All four exits in ONE leaf, chosen by a selector at the top of the witness.

    The bodies are the four-leaf vault's own, so an offer-originated loan
    enforces exactly what a directly-originated one enforces.

        selector 0 -> REPAY      1 -> LIQUIDATE      2 -> DEFAULT      else RECOVER
    """
    keys, threshold = pig._resolve_oracles(oracle_x, oracles, oracle_threshold)
    gross = pig.gross_owed(debt, bonus_num, bonus_den)
    bound = max_price if max_price is not None else strike
    assert gross * price_scale + bound < (1 << 63), (
        "loan too large for 64-bit seizure arithmetic "
        f"(gross={gross}, scale={price_scale}, bound={bound})")

    repay = pig._repay_body(asset_c, asset_d, debt, lender_prog, borrower_prog)
    liq = pig._seizure_body(asset_c, asset_d, debt, lender_prog, borrower_prog,
                            feed_id, keys, threshold, not_before, strike,
                            gross, price_scale, None)
    dflt = pig._seizure_body(asset_c, asset_d, debt, lender_prog, borrower_prog,
                             feed_id, keys, threshold, not_before, None,
                             gross, price_scale, maturity)
    recover = [recover_after, OP_CHECKLOCKTIMEVERIFY, OP_DROP, lender_x, OP_CHECKSIG]

    s = []
    s += [OP_DUP, OP_0, OP_EQUAL, OP_IF, OP_DROP] + repay + [OP_ELSE]
    s += [OP_DUP, OP_1, OP_EQUAL, OP_IF, OP_DROP] + liq + [OP_ELSE]
    s += [OP_DUP, OP_2, OP_EQUAL, OP_IF, OP_DROP] + dflt + [OP_ELSE]
    s += [OP_DROP] + recover
    s += [OP_ENDIF, OP_ENDIF, OP_ENDIF]
    return CScript(s)


def offer_vault_taptree(*, internal_key=NUMS, **kw):
    """P2TR(NUMS, one leaf). The Merkle root is the leaf hash, which is the whole
    reason this format exists."""
    leaf = offer_vault_leaf(**kw)
    tap = taproot_construct(internal_key, [("vault", leaf)])
    return tap, leaf


def offer_vault_chunks(**kw):
    """The single leaf split at every point the borrower key appears.

    Returns `chunks` with `len(chunks) == n_inserts + 1`, so the leaf is
    `chunks[0] + X + chunks[1] + X + ... + chunks[-1]`. Built by compiling the
    leaf with a sentinel key and splitting on it, so the split can never
    disagree with the leaf the vault actually uses.
    """
    kw = dict(kw)
    kw["borrower_prog"] = _SENTINEL
    blob = bytes(offer_vault_leaf(**kw))
    chunks = blob.split(_SENTINEL)
    assert len(chunks) >= 2, "borrower key does not appear in the leaf"
    return chunks


# ------------------------------------------------------------- the offer

def _hash_leaf_with_key(chunks, leaf_len):
    """Script that consumes nothing but a borrower key sitting one below the top
    of the stack, and leaves the TapLeaf hash.

    Entered with [.., X] and returns with [.., X, leafhash]: the key is left in
    place because the caller decides when it is no longer needed.
    """
    preimage_head = (_tag_prefix("TapLeaf/elements")
                     + bytes([LEAF_VERSION]) + _compact_size(leaf_len))
    s = []
    first = preimage_head + chunks[0]
    # OP_SHA256INITIALIZE takes one element, and a stack element is capped at
    # MAX_SCRIPT_ELEMENT_SIZE, so long constant runs are fed in pieces. The
    # split point is arbitrary -- SHA-256 does not care -- but the cap is not.
    head, rest = first[:500], first[500:]
    s += [head, OP_SHA256INITIALIZE]
    for i in range(0, len(rest), 500):
        s += [rest[i:i + 500], OP_SHA256UPDATE]
    for idx, chunk in enumerate(chunks[1:], start=1):
        # the key goes in ahead of every chunk after the first
        s += [OP_OVER, OP_SHA256UPDATE]
        last = idx == len(chunks) - 1
        pieces = [chunk[i:i + 500] for i in range(0, len(chunk), 500)] or [b""]
        for j, piece in enumerate(pieces):
            final = last and j == len(pieces) - 1
            s += [piece, OP_SHA256FINALIZE if final else OP_SHA256UPDATE]
    return s


def build_take_leaf(*, asset_c, asset_d, principal, collateral, vault_kwargs):
    """The offer's permissionless TAKE leaf.

    Witness: `[parity, X]` with the borrower key on top, where `parity` is the
    0x02/0x03 prefix byte of the vault's taproot output key. The parity cannot be
    read from a scriptPubKey, so the taker supplies it; supplying the wrong one
    simply fails OP_TWEAKVERIFY.
    """
    chunks = offer_vault_chunks(**vault_kwargs)
    leaf_len = sum(len(c) for c in chunks) + 32 * (len(chunks) - 1)

    s = []
    # --- rebuild the vault address from the witness key -------------------
    s += _hash_leaf_with_key(chunks, leaf_len)      # [parity, X, root]
    s += [OP_NIP]                                   # [parity, root]  (X is done)
    s += [_tag_prefix("TapTweak/elements") + NUMS, OP_SHA256INITIALIZE]
    s += [OP_SWAP, OP_SHA256FINALIZE]               # [parity, tweak]
    s += _CREDIT_IDX + [OP_INSPECTOUTPUTSCRIPTPUBKEY, OP_1, OP_EQUALVERIFY]
    #                                                 [parity, tweak, prog]
    s += [OP_ROT, OP_SWAP, OP_CAT]                  # [tweak, parity||prog]
    s += [OP_SWAP, NUMS, OP_TWEAKVERIFY]            # verified; []
    # --- output 2k really is that vault, and it holds the collateral ------
    s += _CREDIT_IDX + [OP_INSPECTOUTPUTASSET, OP_1, OP_EQUALVERIFY,
                        asset_c, OP_EQUALVERIFY]
    s += _CREDIT_IDX + [OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY,
                        le8(collateral), OP_GREATERTHANOREQUAL64, OP_VERIFY]
    # --- exactly one principal is drawn; the rest re-rests ----------------
    s += [OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTVALUE, OP_1, OP_EQUALVERIFY]
    s += _RETURN_IDX + [OP_INSPECTNUMOUTPUTS, OP_LESSTHAN]
    s += [OP_IF]
    s += _RETURN_IDX + [OP_INSPECTOUTPUTASSET, OP_1, OP_EQUALVERIFY,
                        asset_d, OP_EQUAL]
    s += [OP_IF]
    s += _RETURN_IDX + [OP_INSPECTOUTPUTSCRIPTPUBKEY]
    s += [OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTSCRIPTPUBKEY]
    s += [OP_ROT, OP_EQUALVERIFY, OP_EQUALVERIFY]   # remainder re-rests here
    s += _RETURN_IDX + [OP_INSPECTOUTPUTVALUE, OP_1, OP_EQUALVERIFY]
    s += [OP_ELSE, le8(0), OP_ENDIF]
    s += [OP_ELSE, le8(0), OP_ENDIF]
    s += [OP_SUB64, OP_VERIFY]                      # [taken]
    s += [le8(principal), OP_EQUAL]                 # taken == principal
    return CScript(s)


def build_offer_refund_leaf(expiry_locktime, lender_x):
    """The lender withdraws an untaken offer after its expiry."""
    assert len(lender_x) == 32
    return CScript([expiry_locktime, OP_CHECKLOCKTIMEVERIFY, OP_DROP,
                    lender_x, OP_CHECKSIG])


def offer_taptree(*, asset_c, asset_d, principal, collateral, vault_kwargs,
                  expiry_locktime, lender_x, internal_key=NUMS):
    take = build_take_leaf(asset_c=asset_c, asset_d=asset_d,
                           principal=principal, collateral=collateral,
                           vault_kwargs=vault_kwargs)
    refund = build_offer_refund_leaf(expiry_locktime, lender_x)
    tap = taproot_construct(internal_key, [("take", take), ("refund", refund)])
    return tap, {"take": take, "refund": refund}


def control_block(tap, leaf_name):
    leaf = tap.leaves[leaf_name]
    return bytes([leaf.version + tap.negflag]) + tap.internal_pubkey + leaf.merklebranch


def take_witness(tap, leaves, borrower_prog, vault_tap):
    """TAKE witness: the parity byte of the vault's output key, then the borrower
    key. `vault_tap` is the taproot info for the vault being created, whose
    `negflag` carries the parity the covenant cannot read from the output."""
    parity = bytes([0x03 if vault_tap.negflag else 0x02])
    return [parity, borrower_prog, bytes(leaves["take"]),
            control_block(tap, "take")]


def offer_refund_witness(tap, leaves, sig_lender):
    return [sig_lender, bytes(leaves["refund"]), control_block(tap, "refund")]


# --------------------------------------------------- vault spend witnesses

_SELECTOR = {"repay": b"", "liquidate": bytes([1]), "default": bytes([2]),
             "recover": bytes([3])}


def vault_witness(vault_tap, leaf, exit_name, data=()):
    """Spend an offer-originated (single-leaf) vault.

    `data` is whatever that exit needs, in the same order the four-leaf vault
    wants it; the selector is pushed last so the script reads it first.
    """
    return list(data) + [_SELECTOR[exit_name], bytes(leaf),
                         bytes([0xc4 + vault_tap.negflag]) + vault_tap.internal_pubkey]
