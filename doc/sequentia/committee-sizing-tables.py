#!/usr/bin/env python3
"""
Committee-sizing tables for Alberto (2026-07-03, v2).

Pure-Python, EXACT tails (no scipy/numpy). Everything the memo
alberto-reply-2026-07-03-committee-sizing.md cites comes from here.

Models (committee drawn WITHOUT replacement from the staker population, i.e.
the fixed-size public-schedule committee that enforces the (quorum-1)*2 cap):

 (1) REPRESENTATIVENESS. SD of a group's share of the committee when the group
     is a fraction p of the population: hypergeometric sampling variance with
     the finite-population correction. Reported in percentage points at p=1/3
     (the EU/US/Asia example); the worst-case p=0.5 region is a x1.061 factor.

 (2) STALL (sleepy). Sleepy-in-committee ~ Hypergeometric(N, S=round(s*N), n).
     A height stalls when awake members < quorum, i.e. sleepy >= n - Q + 1,
     Q = n//2 + 1. The seed is fixed per height (PosSeedForChild), so a stall
     lasts until the escaping-stall valve (3 Bitcoin blocks, ~30 min).

 (3) CAPTURE (malicious). Coalition members in committee ~ Hypergeometric with
     m = coalition's share of stake. Capture = coalition >= Q: it alone can
     certify (and equivocate) at that height. Veto = coalition >= n - Q + 1,
     numerically the sleepy tables read at s = m.

 (4) THRESHOLD-SORTITION comparison: under the CURRENT private-VRF threshold
     rule the committee size itself is Binomial, so awake-eligible ~
     Binomial(pool, (target/pool)*(1-s)) while quorum stays fixed at
     PosQuorum(target): both size- and sleep-variance count against quorum.

 (5) PRIVATE-SORTITION MARGIN: the committee size tau needed so that, keeping
     membership private (variable size), P(eligible >= 2Q) is negligible AND
     the chain still lives at 35% sleepy: the Algorand-style regime.

Run: python3 committee-sizing-tables.py
"""
import math

SAMPLES = [20, 30, 50, 80, 100, 250, 500]   # committee sizes (rows, Alberto's list)
POPS    = [100, 500, 1000, 10000, 50000]     # staker populations (columns)
BLOCKS_PER_DAY = 2880                        # 30 s slots (g_pos_slot_interval)

def logcomb(n, k):
    if k < 0 or k > n:
        return float('-inf')
    return math.lgamma(n + 1) - math.lgamma(k + 1) - math.lgamma(n - k + 1)

def quorum(n):
    return n // 2 + 1                        # cap formula: n <= (Q-1)*2

# ---------- exact hypergeometric upper tail: P(X >= k), X~Hyp(N, S, n) ----------
def hyper_pmf(k, N, S, n):
    return math.exp(logcomb(S, k) + logcomb(N - S, n - k) - logcomb(N, n))

def hyper_tail_ge(kmin, N, S, n):
    p = 0.0
    for k in range(max(kmin, 0), min(n, S) + 1):
        p += hyper_pmf(k, N, S, n)
    return p

# ---------- exact binomial tails ----------
def binom_pmf(k, n, p):
    if p <= 0.0: return 1.0 if k == 0 else 0.0
    if p >= 1.0: return 1.0 if k == n else 0.0
    return math.exp(logcomb(n, k) + k * math.log(p) + (n - k) * math.log(1 - p))

def binom_tail_ge(kmin, n, p):
    mean = n * p; sd = math.sqrt(max(n * p * (1 - p), 1.0))
    lo = max(kmin, 0); hi = min(n, int(mean + 20 * sd) + 2)
    s = 0.0
    for k in range(lo, hi + 1):
        s += binom_pmf(k, n, p)
    # if the window clipped real mass (kmin far above mean+20sd), s is ~0 anyway
    return s

def binom_tail_le(kmax, n, p):
    mean = n * p; sd = math.sqrt(max(n * p * (1 - p), 1.0))
    lo = max(0, int(mean - 20 * sd) - 2); hi = min(kmax, n)
    if hi < lo: return 0.0
    s = 0.0
    for k in range(lo, hi + 1):
        s += binom_pmf(k, n, p)
    return s

# ---------- (1) representativeness ----------
def sd_share_pp(n, N, p=1.0 / 3.0):
    if n > N: return None
    if n == N: return 0.0
    fpc = (N - n) / (N - 1)
    return math.sqrt(p * (1 - p) / n * fpc) * 100.0

# ---------- (2) sleepy stall ----------
def stall_prob(n, N, s):
    if n > N: return None
    return hyper_tail_ge(n - quorum(n) + 1, N, round(s * N), n)

# ---------- (3) malicious capture ----------
def capture_prob(n, N, m):
    if n > N: return None
    return hyper_tail_ge(quorum(n), N, round(m * N), n)

def fmt_p(p):
    if p is None:      return "  n/a"
    if p == 0.0:       return "    0"
    if p >= 0.0995:    return f"{p*100:5.1f}%"
    if p >= 0.001:     return f"{p*100:5.2f}%"
    if p >= 1e-6:      return f"{p*100:.1e}%".replace('e-0', 'e-')
    return f"{p:.0e}"

def fmt_time(p):
    """expected time to first event at BLOCKS_PER_DAY heights/day"""
    if p is None or p <= 0: return "never"
    days = 1.0 / (p * BLOCKS_PER_DAY)
    if days < 1/24/60: return "minutes"
    if days < 1:   return f"{days*24:.1f} h"
    if days < 365: return f"{days:.1f} d"
    if days < 365000: return f"{days/365.25:.1f} yr"
    return f"{days/365.25:.0e} yr"

def print_table(title, cellfn, cellfmt, cols=POPS, colhdr=None):
    print(f"\n### {title}")
    hdr = "committee\\ |" + "".join(f"{(colhdr[i] if colhdr else c):>10}" for i, c in enumerate(cols))
    print(hdr); print("-" * len(hdr))
    for n in SAMPLES:
        print(f"{n:>10} |" + "".join(f"{cellfmt(cellfn(n, c)):>10}" for c in cols))

# ============ Table 1: representativeness ============
print("=" * 74)
print("TABLE 1 - representativeness: SD of a 33% group's committee share (pp)")
print("  x1.061 for the worst-case p=0.5 region. 95% band ~ +/-1.96*SD.")
print_table("SD (percentage points), p=1/3", sd_share_pp,
            lambda v: "n/a" if v is None else f"{v:.2f}")

print("\n95% range of a 33.3% region's committee share (population 10,000):")
for n in [30, 50, 80, 100, 150, 200, 250, 500]:
    sd = sd_share_pp(n, 10000)
    lo, hi = 100/3 - 1.96*sd, 100/3 + 1.96*sd
    print(f"  committee {n:>3}: 33.3% +/- {1.96*sd:4.1f}pp  ->  [{lo:4.1f}%, {hi:4.1f}%]")

# ============ Tables 2-4: sleepy stall ============
for s in (0.20, 0.35, 0.45):
    print("=" * 74)
    print(f"TABLE - P(height stalls) with {int(s*100)}% of stakers sleepy")
    print("  stall = fewer than quorum awake; the height's committee is fixed")
    print("  (seed = f(parent anchor, height)), so a stall lasts until the")
    print("  escaping-stall valve: 3 Bitcoin blocks, ~30 minutes.")
    print_table(f"P(stall), sleepy={int(s*100)}%",
                lambda n, N, s=s: stall_prob(n, N, s), fmt_p)

print("\nExpected stall frequency (population 10,000; 2880 heights/day):")
for n in [50, 100, 150, 200, 250]:
    row = f"  committee {n:>3}: "
    row += "  ".join(f"s={int(s*100)}%: {fmt_time(stall_prob(n, 10000, s)):>8}"
                     for s in (0.20, 0.30, 0.35, 0.40, 0.45))
    print(row)

# ============ Table 5: malicious capture ============
MS = [0.20, 0.25, 0.30, 1/3, 0.40, 0.45]
print("=" * 74)
print("TABLE 5 - P(a coalition holding m of the stake captures >= quorum of the")
print("committee) per height, population 50,000 (smaller populations are strictly")
print("harder for the attacker). Capture = the coalition alone certifies, and can")
print("certify TWO blocks at that height (malicious members equivocate freely).")
print(f"\n### P(capture); columns = coalition stake share")
hdr = "committee\\ |" + "".join(f"{f'{m*100:.0f}%':>10}" for m in MS)
print(hdr); print("-" * len(hdr))
for n in sorted(SAMPLES + [150, 200]):
    print(f"{n:>10} |" + "".join(f"{fmt_p(capture_prob(n, 50000, m)):>10}" for m in MS))

print("\nExpected time to the first capture opportunity (2880 heights/day):")
for n in [100, 150, 200, 250, 500]:
    row = f"  committee {n:>3}: "
    row += "  ".join(f"m={f'{m*100:.0f}%':>4}: {fmt_time(capture_prob(n, 50000, m)):>8}"
                     for m in [0.25, 0.30, 1/3, 0.40, 0.45])
    print(row)

# tolerance scan: largest coalition with capture rarer than once per decade/century
print("\nLargest coalition stake share tolerated (capture rarer than the target):")
for n in [100, 128, 150, 200, 250, 500]:
    tol = {}
    for label, per_slot in (("once/decade", 1/(BLOCKS_PER_DAY*3652.5)),
                            ("once/century", 1/(BLOCKS_PER_DAY*36525))):
        m, best = 0.01, 0.0
        while m < 0.50:
            p = capture_prob(n, 50000, m)
            if p is not None and p > per_slot: break
            best = m
            m += 0.0025
        tol[label] = best
    print(f"  committee {n:>3}: {tol['once/decade']*100:4.1f}% (decade)   {tol['once/century']*100:4.1f}% (century)")

# ============ (4) current threshold sortition vs the cap ============
print("=" * 74)
print("CURRENT THRESHOLD SORTITION vs THE CAP (35% sleepy, target 100, Q=51)")
print("  threshold rule: committee size itself varies (Binomial), quorum fixed;")
print("  stall = awake-eligible < 51, awake-eligible ~ Binomial(pool, (100/pool)*0.65)")
for pool in (500, 1000, 10000):
    p_thresh = binom_tail_le(50, pool, (100.0/pool) * 0.65)
    p_cap = stall_prob(100, pool, 0.35)
    print(f"  pool {pool:>6}: threshold {fmt_p(p_thresh)} ({fmt_time(p_thresh)})   "
          f"vs cap {fmt_p(p_cap)} ({fmt_time(p_cap)})   ratio ~{p_thresh/p_cap:.0f}x")

# ============ demonstration: a 2/3 quorum cannot live with sleepy nodes ============
print("=" * 74)
print("WHY THE QUORUM MUST BE A BARE MAJORITY (committee 100, population 50,000)")
for Q, name in ((51, "majority (51)"), (67, "2/3 (67)")):
    for s in (0.20, 0.35):
        p = hyper_tail_ge(100 - Q + 1, 50000, round(s*50000), 100)
        print(f"  quorum {name:>14}, sleepy {int(s*100)}%: P(stall) = {fmt_p(p):>8}  ({fmt_time(p)})")

# ============ (5) private sortition margin (Algorand regime) ============
print("=" * 74)
print("KEEPING PRIVATE SORTITION INSTEAD: committee size tau needed so that")
print("P(eligible >= 2Q) <= 1e-9 (disjoint quorums cryptographically negligible)")
print("AND the chain lives at 35% sleepy. Eligible ~ Binomial(50000, tau/50000).")
for tau in (100, 500, 1000, 2000, 3000):
    # smallest Q with P(eligible >= 2Q) <= 1e-9
    Q = tau // 2 + 1
    while binom_tail_ge(2 * Q, 50000, tau / 50000.0) > 1e-9:
        Q += 1
    p_stall = binom_tail_le(Q - 1, 50000, (tau / 50000.0) * 0.65)
    print(f"  tau {tau:>5}: safe quorum Q = {Q:>5} ({100.0*Q/tau:5.1f}% of tau)   "
          f"P(stall @35% sleepy) = {fmt_p(p_stall):>8} ({fmt_time(p_stall)})")

# ============ recommendation scan ============
print("=" * 74)
print("RECOMMENDATION SCAN (population 50,000; FPC ~ 1; 2880 heights/day)")
print("  size  quorum  SD(33%)pp  stall@35%    stall@45%   capture@25%  capture@33%  capture@40%")
for n in [20, 30, 50, 80, 100, 128, 150, 200, 250, 500]:
    print(f"  {n:>4}  {quorum(n):>5}   {sd_share_pp(n, 50000):>7.2f}"
          f"  {fmt_p(stall_prob(n, 50000, 0.35)):>9}  {fmt_p(stall_prob(n, 50000, 0.45)):>9}"
          f"   {fmt_p(capture_prob(n, 50000, 0.25)):>9}   {fmt_p(capture_prob(n, 50000, 1/3)):>9}"
          f"   {fmt_p(capture_prob(n, 50000, 0.40)):>9}")
