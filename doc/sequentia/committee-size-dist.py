import random, statistics
MASK64 = (1<<64)-1
POS_VRF_MAX_SLOT = 1<<20

# Exact transcription of PosVrfSlot (pos.cpp:324-335): all exact integer ops.
def pos_vrf_slot(beta, weight, total_weight):
    if weight == 0 or total_weight == 0: return POS_VRF_MAX_SLOT
    q = beta // weight                       # arith_uint256 /=
    slot_a = (q >> 192) * total_weight       # (q>>192) * total
    slot_a >>= 64                            # >>= 64
    slot = slot_a & MASK64                    # GetLow64
    return min(slot, POS_VRF_MAX_SLOT)

def measure(committee_size, n_stakers, slots, weight=40000*10**8, seed=12345):
    rnd = random.Random(seed)
    total = n_stakers * weight
    quorum = committee_size//2 + 1
    sizes = []
    member_hits = 0
    getrb = rnd.getrandbits
    for _ in range(slots):
        c = 0
        for _s in range(n_stakers):
            beta = getrb(256)
            if pos_vrf_slot(beta, weight, total) < committee_size:
                c += 1
        sizes.append(c)
        member_hits += c
    n = len(sizes)
    mean = member_hits / (n*n_stakers)          # per-staker P(member)
    csz_mean = statistics.fmean(sizes)
    csz_sd = statistics.pstdev(sizes)
    twoQ = 2*quorum
    p_window = sum(1 for s in sizes if s >= twoQ)/n     # two disjoint quorums fit
    p_ge = lambda k: sum(1 for s in sizes if s >= k)/n
    p_le = lambda k: sum(1 for s in sizes if s <= k)/n
    from collections import Counter
    hist = Counter(sizes)
    return dict(committee_size=committee_size, n_stakers=n_stakers, slots=n, quorum=quorum,
                twoQ=twoQ, per_staker_p=mean, size_mean=csz_mean, size_sd=csz_sd,
                size_min=min(sizes), size_max=max(sizes),
                p_safe=p_le(twoQ-1), p_window=p_window, p_ge_twoQ1=p_ge(twoQ+1), hist=hist)

def report(r):
    print(f"\n=== committee_size={r['committee_size']} (quorum={r['quorum']}, 2*quorum={r['twoQ']}), "
          f"{r['n_stakers']} equal-weight stakers, {r['slots']} slots ===")
    print(f"per-staker P(in committee) = {r['per_staker_p']:.4f}  (expected ~{r['committee_size']/r['n_stakers']:.2f})")
    print(f"eligible-committee size: mean={r['size_mean']:.2f} std={r['size_sd']:.2f} "
          f"min={r['size_min']} max={r['size_max']}")
    print(f"P(size <= {r['twoQ']-1}) SAFE (2Q > size, quorums must intersect) = {r['p_safe']:.4f}")
    print(f"P(size >= {r['twoQ']}) DOUBLE-CERT WINDOW OPEN (two disjoint {r['quorum']}-quorums fit) = {r['p_window']:.4f}")
    print(f"P(size >= {r['twoQ']+1}) = {r['p_ge_twoQ1']:.4f}")
    lo=min(r['hist']); hi=max(r['hist'])
    print("histogram (size: pct):")
    line=[]
    for s in range(lo, hi+1):
        pct = 100*r['hist'].get(s,0)/r['slots']
        if pct>=0.05: line.append(f"{s}:{pct:4.1f}")
    print("  "+"  ".join(line))

print("Empirical committee-size distribution from the EXACT PosVrfSlot (pos.cpp:324-335).")
print("Betas sampled as uniform 256-bit VRF outputs (independent per staker per slot).")
report(measure(30, 100, 100000))     # Alberto's proposed config
report(measure(100, 100, 20000))     # current testnet (pool == target)
report(measure(100, 1000, 20000))    # mainnet analog (pool >> target): the original scenario-2
