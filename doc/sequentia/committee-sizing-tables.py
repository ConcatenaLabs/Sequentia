#!/usr/bin/env python3
"""
Committee-sizing tables for Alberto (2026-07-03).

Pure-Python, EXACT hypergeometric (no scipy/numpy dependency). Answers the two
questions in Alberto's 2026-07-03 reply:

 (1) REPRESENTATIVENESS. If a geographic (or any) group is a fraction p of the
     staker population N, how far does that group's SHARE of a committee of size n
     typically land from p? This is the standard deviation of the sample proportion
     under sampling WITHOUT replacement (hypergeometric), with the finite-population
     correction (FPC). Reported in percentage points for p = 1/3 (Alberto's
     EU/US/Asia example). The SD scales as sqrt(p(1-p)); the worst-case single
     region p = 0.5 is a x1.061 multiplier.

 (2) STALL. A slot stalls (cannot certify a block) when fewer than `quorum`
     committee members are awake, i.e. when the number of SLEEPY members is at least
     n - quorum + 1. Under Alberto's cap committee = (quorum-1)*2, quorum = n//2 + 1,
     so a slot stalls iff at least ceil(n/2) of the committee is asleep (a majority
     of the committee asleep). Sleepy-in-committee ~ Hypergeometric(N, S=round(s*N),
     n); we compute the exact upper tail.

Run: python3 committee-sizing-tables.py
"""
import math

SAMPLES = [20, 30, 50, 80, 100, 250, 500]   # committee sizes (rows)
POPS    = [100, 500, 1000, 10000, 50000]     # staker populations (columns)

def logcomb(n, k):
    if k < 0 or k > n:
        return float('-inf')
    return math.lgamma(n + 1) - math.lgamma(k + 1) - math.lgamma(n - k + 1)

def quorum(n):
    return n // 2 + 1                          # Alberto's cap: committee = (quorum-1)*2

# ---- (1) representativeness: SD of the sample proportion, hypergeometric FPC ----
def sd_share_pp(n, N, p=1.0 / 3.0):
    if n > N:
        return None                            # cannot sample more than the population
    if n == N:
        return 0.0                             # committee == whole population, no sampling error
    fpc = (N - n) / (N - 1)                     # finite population correction
    var = p * (1 - p) / n * fpc
    return math.sqrt(var) * 100.0              # percentage points

# ---- (2) stall probability: P(X >= k_stall), X ~ Hypergeometric(N, S, n) ----
def hyper_pmf(k, N, S, n):
    return math.exp(logcomb(S, k) + logcomb(N - S, n - k) - logcomb(N, n))

def stall_prob(n, N, s):
    if n > N:
        return None
    S = round(s * N)                           # sleepy stakers in the population
    k_stall = n - quorum(n) + 1                # sleepy-in-committee that just denies quorum
    p = 0.0
    for k in range(k_stall, min(n, S) + 1):
        p += hyper_pmf(k, N, S, n)
    return p

def fmt_p(p):
    if p is None:      return "  n/a"
    if p == 0.0:       return "    0"
    if p >= 0.0995:    return f"{p*100:5.1f}%"
    if p >= 0.001:     return f"{p*100:5.2f}%"
    if p >= 1e-6:      return f"{p*100:.1e}%".replace('e-0', 'e-')
    return f"{p:.0e}"

def print_table(title, cellfn, cellfmt):
    print(f"\n### {title}")
    hdr = "sample\\pop |" + "".join(f"{N:>10}" for N in POPS)
    print(hdr); print("-" * len(hdr))
    for n in SAMPLES:
        row = f"{n:>9} |" + "".join(f"{cellfmt(cellfn(n, N)):>10}" for N in POPS)
        print(row)

# Table 1: representativeness
print("=" * 72)
print("TABLE 1 - representativeness: SD of a 33% group's committee share (pp)")
print("  x1.061 for the worst-case p=0.5 region. 95% band ~ +/-1.96*SD.")
print_table("SD (percentage points), p=1/3", sd_share_pp,
            lambda v: "n/a" if v is None else f"{v:.2f}")

# Tables 2-4: stall probability at 20/35/45% sleepy
for s in (0.20, 0.35, 0.45):
    print("=" * 72)
    print(f"TABLE - P(slot stall) with {int(s*100)}% of stakers sleepy")
    print("  stall = fewer than quorum awake = majority of committee asleep")
    print_table(f"P(stall), sleepy={int(s*100)}%",
                lambda n, N, s=s: stall_prob(n, N, s), fmt_p)

# Recommendation scan (large population => FPC ~ 1)
print("\n" + "=" * 72)
print("RECOMMENDATION SCAN (population = 50000, i.e. FPC ~ 1)")
print("  size  quorum   SD(33%)pp   stall@20%   stall@35%   stall@45%")
N = 50000
for n in [20, 30, 50, 64, 80, 100, 128, 160, 250, 500]:
    row = f"  {n:>4}  {quorum(n):>5}   {sd_share_pp(n, N):>8.2f}    "
    row += "  ".join(fmt_p(stall_prob(n, N, s)) for s in (0.20, 0.35, 0.45))
    print(row)
