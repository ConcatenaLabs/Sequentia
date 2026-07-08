#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""OpenAMP Tier B containment covenant (design doc §6).

L_cov is one tapscript leaf that makes it a CONSENSUS rule, carried by the
UTXO, that every output holding asset A in the spending transaction is itself
an enclave output (or an OP_RETURN burn, if the asset permits). A fee output
in A (empty scriptPubKey) can therefore never exist: Rule 1 by consensus.

Recursion without a quine: L_cov is byte-identical for all holders of A (it
embeds only asset-wide constants). Per-holder identity is a sibling
COMMITMENT leaf C(K) = OP_RETURN <K>, which is unspendable and exists only to
make each holder's address unique. Enclave tree = {L_cov, C(K)}, internal key
NUMS (no key-path spend). L_cov proves its own hash by checking
TapTweak(NUMS, TapBranch(h_cov, TapLeaf(C(K_user)))) equals THIS input's own
scriptPubKey via OP_TWEAKVERIFY; since C(K) can never collide with L_cov,
that forces h_cov = H(L_cov). It reuses the proven h_cov to validate every
asset-A output identically.

Witness stack (top -> bottom), all built by cov_witness():
    h_cov
    slot0: key0, swap0, parity0
    ... up to MAX_OUTPUTS slots ...
    Ku_root                 # K_user copy consumed by the self-check
    self_swap
    self_parity
    Ku_sig                  # K_user copy kept for the user CHECKSIG
    user_sig
    policy_sig
"""

import hashlib

from test_framework.script import (
    CScript, LEAF_VERSION_TAPSCRIPT, taproot_construct,
    OP_1, OP_1NEGATE, OP_2DROP, OP_CAT, OP_CHECKSIG, OP_CHECKSIGVERIFY, OP_DROP,
    OP_DUP, OP_ELSE, OP_ENDIF, OP_EQUAL, OP_EQUALVERIFY, OP_FROMALTSTACK, OP_IF,
    OP_INSPECTINPUTSCRIPTPUBKEY, OP_INSPECTNUMOUTPUTS, OP_INSPECTOUTPUTASSET,
    OP_INSPECTOUTPUTSCRIPTPUBKEY, OP_LESSTHAN, OP_PUSHCURRENTINPUTINDEX, OP_RETURN,
    OP_ROT, OP_SHA256, OP_SWAP, OP_TOALTSTACK, OP_TWEAKVERIFY, OP_VERIFY,
)
from test_framework.key import compute_xonly_pubkey, tweak_add_pubkey

NUMS = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")


def _sha(b):
    return hashlib.sha256(b).digest()


D_LEAF = _sha(b"TapLeaf/elements")
D_BRANCH = _sha(b"TapBranch/elements")
D_TWEAK = _sha(b"TapTweak/elements")

# C(K) = OP_RETURN <32-byte K>  ==  0x6a 0x20 K   (34 bytes)
# TapLeaf preimage tail = leafver(0xc4) || compactsize(34)=0x22 || 6a 20 || K
C_PREFIX = bytes([LEAF_VERSION_TAPSCRIPT, 0x22, 0x6a, 0x20])
SHA_OP_RETURN = _sha(bytes([0x6a]))


def commitment_leaf(k):
    return CScript(b"\x6a\x20" + k)


def _tagged(d, msg):
    return _sha(d + d + msg)


def leaf_hash_commit(k):
    return _tagged(D_LEAF, C_PREFIX + k)


# --- script emission helpers (operate at stack top) -------------------------

def _emit_tagged(d):
    # msg on top -> tagged hash on top
    return [d, OP_SWAP, OP_CAT, d, OP_SWAP, OP_CAT, OP_SHA256]


def _emit_enclave_root():
    # consumes [.., swap, key] (key on top); peeks h_cov from alt-top;
    # leaves [.., root]. swap==0 => tagged(leaf||h_cov); swap==1 => tagged(h_cov||leaf).
    ops = [C_PREFIX, OP_SWAP, OP_CAT]
    ops += _emit_tagged(D_LEAF)                       # [.., swap, leafhash]
    ops += [OP_FROMALTSTACK, OP_DUP, OP_TOALTSTACK]   # [.., swap, leafhash, h_cov]
    ops += [OP_ROT]                                   # [.., leafhash, h_cov, swap]
    ops += [OP_IF, OP_SWAP, OP_ENDIF]                 # swap? [.., h_cov, leafhash] : [.., leafhash, h_cov]
    ops += [OP_CAT]
    ops += _emit_tagged(D_BRANCH)                     # [.., root]
    return ops


def _emit_tweak_from_root():
    # root on top -> taptweak scalar on top
    return [NUMS, OP_SWAP, OP_CAT] + _emit_tagged(D_TWEAK)


def build_lcov(policy_x, asset_internal, max_outputs=4, burn_allowed=True):
    """The containment covenant leaf for asset A (internal-order 32-byte id).

    Authorization is a 2-of-2 split across the script so each signature check
    runs on a clean stack: the policy signature is verified FIRST (embedded
    key, top of witness), and the user signature LAST as the final op (its
    boolean is the sole result). The containment checks sit between them.
    """
    s = []
    # policy signature first, on a clean stack (policy_sig at witness top)
    s += [policy_x, OP_CHECKSIGVERIFY]
    # park h_cov on alt (peeked throughout, dropped implicitly at end)
    s += [OP_TOALTSTACK]

    # per-output containment loop
    for i in range(max_outputs):
        s += [i, OP_INSPECTNUMOUTPUTS, OP_LESSTHAN]          # i < N ?
        s += [OP_IF]
        #   output i exists: require explicit, and if asset A, enclave/burn
        s += [i, OP_INSPECTOUTPUTASSET]                      # [.., key,swap,parity, asset, prefix]
        s += [OP_1, OP_EQUALVERIFY]                          # prefix==01 (explicit)
        s += [asset_internal, OP_EQUAL]                      # asset==A ?
        s += [OP_IF]
        #     asset A: [.., key, swap, parity]
        s += _emit_enclave_root()                            # consumes key,swap -> [.., parity, root]
        s += _emit_tweak_from_root()                         # [.., parity, tweak]
        s += [i, OP_INSPECTOUTPUTSCRIPTPUBKEY]               # [.., parity, tweak, spk, ver]
        s += [OP_DUP, OP_1, OP_EQUAL, OP_IF]
        #       witness v1 enclave: [.., parity, tweak, program, ver]
        s += [OP_DROP]                                       # [.., parity, tweak, program]
        s += [OP_ROT]                                        # [.., tweak, program, parity]
        s += [OP_SWAP, OP_CAT]                               # [.., tweak, parity||program=Q33]
        s += [OP_SWAP, NUMS, OP_TWEAKVERIFY]                 # verify enclave, []
        s += [OP_ELSE]
        if burn_allowed:
            #     OP_RETURN burn: [.., parity, tweak, spkhash, ver]
            s += [OP_1NEGATE, OP_EQUALVERIFY]                # ver==-1
            s += [SHA_OP_RETURN, OP_EQUALVERIFY]             # spk==sha256(OP_RETURN)
            s += [OP_2DROP]                                  # drop parity, tweak
        else:
            s += [OP_RETURN]                                 # asset A must be an enclave: fail
        s += [OP_ENDIF]
        s += [OP_ELSE]
        #     asset != A: no constraint, drop key,swap,parity
        s += [OP_2DROP, OP_DROP]
        s += [OP_ENDIF]
        s += [OP_ELSE]
        #   i >= N: unused slot, drop key,swap,parity
        s += [OP_2DROP, OP_DROP]
        s += [OP_ENDIF]

    # self-check: prove h_cov and bind K_user to this input's own scriptPubKey
    # stack now (top->): Ku_root, self_swap, self_parity, Ku_sig, user_sig, policy_sig
    s += _emit_enclave_root()                                # consumes Ku_root, self_swap -> [.., self_parity, root]
    s += _emit_tweak_from_root()                             # [.., self_parity, tweak]
    s += [OP_PUSHCURRENTINPUTINDEX, OP_INSPECTINPUTSCRIPTPUBKEY]  # [.., self_parity, tweak, program, ver]
    s += [OP_1, OP_EQUALVERIFY]                              # ver==1 -> [.., self_parity, tweak, program]
    s += [OP_ROT]                                            # [.., tweak, program, self_parity]
    s += [OP_SWAP, OP_CAT]                                   # [.., tweak, self_parity||program=Q33]
    s += [OP_SWAP, NUMS, OP_TWEAKVERIFY]                     # verify self, []

    # user signature LAST, as the final op: its boolean is the sole result.
    # stack (top->): Ku_sig, user_sig
    s += [OP_CHECKSIG]                                       # user_sig under Ku_sig
    return CScript(s)


def enclave_taptree(user_x, policy_x, asset_internal, max_outputs=4, burn_allowed=True):
    """Tier B enclave tree {L_cov, C(user_x)} with NUMS internal key."""
    lcov = build_lcov(policy_x, asset_internal, max_outputs, burn_allowed)
    commit = commitment_leaf(user_x)
    tap = taproot_construct(NUMS, [("cov", lcov), ("commit", commit)])
    return tap, lcov


def _enclave_root(user_x, lcov):
    h_cov = _tagged(D_LEAF, _leaf_preimage(lcov))
    lh = leaf_hash_commit(user_x)
    a, b = sorted([h_cov, lh])
    root = _tagged(D_BRANCH, a + b)
    return h_cov, lh, root


def _leaf_preimage(script):
    b = bytes(script)
    # compactsize length prefix
    if len(b) < 253:
        ln = bytes([len(b)])
    elif len(b) < 0x10000:
        ln = b"\xfd" + len(b).to_bytes(2, "little")
    else:
        ln = b"\xfe" + len(b).to_bytes(4, "little")
    return bytes([LEAF_VERSION_TAPSCRIPT]) + ln + b


def swap_and_parity(user_x, lcov):
    """The witness swap bit and OP_TWEAKVERIFY parity byte for an enclave."""
    h_cov, lh, root = _enclave_root(user_x, lcov)
    swap = b"" if lh < h_cov else b"\x01"   # script baseline (swap==0) = leaf||h_cov
    _, negated = tweak_add_pubkey(NUMS, _tagged(D_TWEAK, NUMS + root))
    parity = b"\x03" if negated else b"\x02"
    return h_cov, swap, parity


def cov_witness(tx, spent, input_index, lcov, tap, user_x, sig_user, sig_policy,
                out_keys, genesis_hash, max_outputs=4):
    """Build the full witness stack for spending a Tier B enclave input.

    out_keys: list matching tx.vout; for each output that carries asset A and
    is an enclave, the holder x-only key; for burns/other outputs, None (a
    dummy key is supplied and ignored by the covenant).
    """
    h_cov, self_swap, self_parity = swap_and_parity(user_x, lcov)

    # Per-output slots. For an asset-A enclave output, supply the real holder
    # key + its swap/parity; otherwise a dummy triple the covenant ignores.
    dummy = b"\x11" * 32
    slots = []
    for i in range(max_outputs):
        key = out_keys[i] if i < len(out_keys) else None
        if key is not None:
            _, sw, pa = swap_and_parity(key, lcov)
            slots.append((key, sw, pa))
        else:
            slots.append((dummy, b"", b"\x02"))

    # Assemble the witness bottom->top (the interpreter pops from the top).
    # Consumption order from top: policy_sig (checked first), h_cov, then
    # slot0..slot{M-1} each (key, swap, parity), then self-check (Ku_root,
    # self_swap, self_parity), then Ku_sig, then user_sig (final CHECKSIG).
    stack = [sig_user, user_x, self_parity, self_swap, user_x]
    for (k, sw, pa) in reversed(slots):
        stack += [pa, sw, k]   # within a slot, key is popped first => topmost
    stack += [h_cov, sig_policy]
    return stack
