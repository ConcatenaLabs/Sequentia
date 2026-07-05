# Committee parameters, locked for re-genesis (confirmed 2026-07-04)

The committee-design decisions are settled. This is the authoritative record; the
values are also baked into the SEQUENTIA chain params (src/chainparams.cpp) and
the flag-gated implementation on this branch.

## The decisions

| decision | value | why / reference |
| --- | --- | --- |
| Committee selection | Public fixed-size (Option A), `-pospubliccommittee=1` | The (quorum-1)x2 cap enforced by construction; two disjoint quorums impossible. Spec: committee-public-selection-impl-spec.md. |
| Committee cap | 250 (quorum 126) | Smallest size clearing the classical 1/3 Byzantine bound with margin; certificate is size-independent after the bitfield form, so 250 costs no more than 100 per block. alberto-reply-2026-07-03-committee-sizing. |
| Election seed | Bitcoin anchor (option 1A), unchanged | An external, memoryless randomness beacon. A buried anchor (1B) and a VRF-chain seed (Option B) are PERMANENTLY REJECTED. alberto-reply-2026-07-04c (1B is a no-op) and alberto-reply-2026-07-04d (Option B verdict). |
| Certificate | Bitfield (registry-held BLS keys) | ~300 bytes regardless of committee size (a K=4 block measured at 665 bytes). |

## What is live vs. re-genesis

The live (pre-re-genesis) chain is untouched: the SEQUENTIA params default to
committee 100 with the public committee OFF, so this binary is byte-identical to
the current one on the running chain. The cap is arg-overridable, so turning on
the new committee is a launch-config choice at re-genesis, not a further code
edit. This binary must NOT replace the running chain's binary before re-genesis
(it would not change behavior without the flags, but the coupling is deliberate:
the flags flip only with the new genesis).

## Re-genesis launch flags (in addition to the existing genesis/asset setup)

    -con_pos=1 -posvrf=1 -posbls=1 \
    -pospubliccommittee=1 \
    -poscommitteesize=250

Everything else (slot interval 30 s, min stake 40,000 SEQ, unbonding, the
anchor-driven seed, the escaping-stall valve, the tiebreak) is unchanged from the
current SEQUENTIA params. Runtime stakers register their committee BLS key with
`getblsregistration` + the `blspubkey`/`pop` args on `registerstake` /
`getstakescript`; genesis/config stakers carry it in the `-staker` spec
(`<pub>:<weight>:<blspub>:<pop>`).

## Still open (not blocking the parameters)

- Merge of the implementation (PR #3) into the working branch, then fold into the
  re-genesis commit that mints the new genesis.
- The K=250 throttled-hardware / injected-latency sandbox measurement (a manual
  environmental check, not a code test) as the final confirmation before launch.
