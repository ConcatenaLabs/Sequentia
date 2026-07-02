# Data note: committee-size distribution under threshold sortition

Empirical follow-up to the honest-split memo, quantifying how often the disjoint-quorum window opens.
Measured directly from the sortition code (`PosVrfSlot`, pos.cpp:324-335), not a model.

## Method

Exact transcription of `PosVrfSlot` (floor-divide beta by weight, take the top 64 bits, scale by total
weight, shift down 64, cap at 1<<20); a staker is in the committee for a slot iff that slot value is
below the committee size. 100 equal-weight stakers, VRF betas drawn as uniform 256-bit values
(independent per staker per slot), 100,000 slots. The measured per-staker membership rate came out at
0.3002 against the expected 0.30, which confirms the sortition is unbiased and the transcription
faithful.

## Result: committee 30, quorum 16, 100 stakers (your proposed config)

| metric | value |
| --- | --- |
| per-staker P(in committee) | 0.3002 (expected 0.30) |
| eligible committee size | mean 30.0, std 4.6, range [12, 51] |
| P(size <= 31), safe: the two quorums must intersect | 63.1% |
| P(size >= 32), double-cert window open: two disjoint 16-quorums fit | 36.9% |
| P(size >= 33) | 29.2% |

Quorum intersection is guaranteed only while 2*quorum > eligible, i.e. eligible <= 31. That holds for
63.1% of slots; the other 36.9% are the vulnerable regime, where the eligible set is large enough that
two disjoint 16-member quorums can co-exist. The distribution is a clean Binomial(100, 0.3), peaked at
29 to 31. So on a 100-staker testbed with committee 30 the window is open in more than a third of all
slots, which is frequent enough to observe directly.

## Cross-checks

- Current testnet (committee 100, 100 stakers): eligible is ALWAYS exactly 100 (std 0), window
  probability 0%. When the staker pool equals the committee target, threshold sortition admits every
  staker every slot, so there is no size variance and the split cannot occur. This is precisely why
  the issue is invisible on the live chain today.
- Mainnet analog (committee 100, 1000 stakers): mean 100, std 9.4, range [62, 141],
  P(size >= 102) = 43%. This confirms the residual is real and common at genuine decentralization, and
  matches the rough 40% estimate from the code sweep.

## Reading the 36.9%

It is how often the window is OPEN (eligible >= 32), not the realized split rate. A realized
double-certification additionally requires a network partition that splits the eligible members into
two disjoint groups of at least 16, each backing a different block. So 36.9% bounds the per-slot
opportunity; the sandbox cluster (100 stakers, committee 30, with an induced partition) is what would
demonstrate an actual split.

## The fix removes the window entirely

Deriving the quorum from the ACTUAL eligible count for the slot makes 2*quorum > eligible hold by
construction (P(window) -> 0). Alternatively, capping membership to a deterministic top-`committee_size`
by VRF rank makes eligible <= committee_size always. Either restores quorum intersection, so honest
double-certification becomes impossible again.

## Practical note

This cannot ride the live testnet: committee size and quorum are validated per block against the
current global value (validation.cpp:2254/2279/2310/3165), so switching the running chain to 30/16
would re-derive committee membership for the existing ~17.6k blocks under the new threshold and reject
them, making the chain un-syncable. The clean paths are a separate sandbox cluster with
`-poscommitteesize 30` (quorum 16 follows automatically) for observation, and folding 30/16 into the
planned re-genesis.
