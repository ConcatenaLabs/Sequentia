#!/usr/bin/env python3
"""Does burying the committee seed (spec DECISION 1B) reduce anchor-grinding? No.

Model (exactly the shipped public committee): committee(H) = the top-K stakers by
H(seed(H) || pubkey), i.e. a uniform K-subset of the pool for a given seed; a
coalition of stake fraction m captures H iff it holds >= quorum of the K seats.
Coalition-in-committee ~ Hypergeometric(N, round(m*N), K). quorum = K//2+1+(K&1).

The grind: the producer of the block that SUPPLIES the seed's anchor chooses which
recent Bitcoin block to anchor to (monotonic-advance rule => a handful of choices,
k), and under a PUBLIC committee it can compute committee(H) for each and pick the
anchor that best seats its coalition -> best of k. It only gets this when it leads
that one seed-supplying block (prob m).

seed(H) = H(anchor(H-D), H). D = 1 (current, 1A) or 2 (buried, 1B). The producer of
block H-D chooses the anchor; the registry is public and ~static over D blocks, so
it evaluates committee(H) the same way for any D. The capture probability below has
NO D term: burying only changes WHICH producer holds the (identical) lever.
"""
import math
BLOCKS_PER_DAY = 2880

def logcomb(n, k):
    if k < 0 or k > n: return float('-inf')
    return math.lgamma(n+1) - math.lgamma(k+1) - math.lgamma(n-k+1)

def quorum(K):  # public-committee quorum (odd-K bump), as shipped (PosPublicQuorum)
    return K // 2 + 1 + (K & 1)

def base_capture(N, K, m):
    """q = P(coalition >= quorum in ONE committee) = hypergeometric upper tail."""
    S = round(m * N); Q = quorum(K)
    return sum(math.exp(logcomb(S, k) + logcomb(N-S, K-k) - logcomb(N, K))
               for k in range(Q, min(K, S) + 1))

def per_height_capture(N, K, m, k):
    """The attacker grinds (best of k) only on blocks it leads (prob m); otherwise
       it faces one honest-chosen seed. Independent of the burial depth D."""
    q = base_capture(N, K, m)
    grind = 1.0 - (1.0 - q) ** k          # best-of-k selection
    return m * grind + (1.0 - m) * q

def when(p):
    if p <= 0: return "never"
    yrs = 1.0 / (p * BLOCKS_PER_DAY * 365.25)
    if yrs < 1/365.25: return f"{yrs*365.25*24:.1f} h"
    if yrs < 1: return f"{yrs*365.25:.0f} d"
    if yrs < 1e6: return f"{yrs:,.0f} yr"
    return f"{yrs:.0e} yr"

N = 50000
print("Capture per height, and expected time to the first captured height.")
print("k = number of anchor choices the seed-supplying leader can grind over")
print("(k=1 is BOTH the no-grind case AND the buried-seed 1B case: identical).\n")
for cap in (100, 250):
    print(f"=== committee cap {cap} (quorum {quorum(cap)}) ===")
    print(f"  {'coalition':>9} | {'base (k=1)':>22} | {'grind k=2':>22} | {'grind k=4':>22} | {'grind k=8':>22}")
    for m in (0.25, 1/3, 0.40):
        cells = []
        for k in (1, 2, 4, 8):
            p = per_height_capture(N, cap, m, k)
            cells.append(f"{p:8.1e} ({when(p):>9})")
        print(f"  {m*100:8.0f}% | " + " | ".join(cells))
    print()

print("Burial-depth independence (committee cap 250, coalition 33%, k=4):")
print("  the capture formula has no D; these are byte-identical by construction:")
p = per_height_capture(N, 250, 1/3, 4)
for D in (1, 2, 3, 5):
    print(f"    seed from anchor(H-{D}):  P(capture)/height = {p:.3e}  ({when(p)})")
