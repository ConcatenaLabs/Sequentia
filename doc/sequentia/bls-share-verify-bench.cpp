// Build (from the repo root; mirrors src/bls.cpp call-for-call):
//   gcc -O2 -fPIC -c src/blst/src/server.c -o /tmp/server.o -Isrc/blst/src
//   gcc -O2 -fPIC -c src/blst/build/assembly.S -o /tmp/assembly.o -Isrc/blst/build
//   ar rcs /tmp/libblst.a /tmp/server.o /tmp/assembly.o
//   g++ -O2 -std=c++17 -Isrc/blst/bindings doc/sequentia/bls-share-verify-bench.cpp /tmp/libblst.a -o /tmp/bls_bench
// Measured 2026-07-04 (dev laptop, single thread; and under systemd CPUQuota=25% as a weak-hardware proxy):
//   per-share PoP+sig (todays OnShare):  2.27 ms  -> K=250 sequential 569 ms   | quarter-core: 8.57 ms -> 2144 ms
//   per-share sig-only (PoP at reg.):    1.12 ms  -> K=250 sequential 280 ms   | quarter-core: 4.50 ms -> 1125 ms
//   K=250 batched (agg + 1 verify):      34.9 ms                                | quarter-core: 139 ms
// Micro-benchmark mirroring Sequentia's committee-share verification paths.
// Replicates src/bls.cpp exactly (IETF min-pk POP ciphersuite, blst.hpp calls):
//   per-share path (OnShare, pos_producer.cpp:1227-1228):
//       BlsVerifyPossession(pk, pop) + BlsVerify(pk, block_hash, share)
//   batched path (Option A, registry keys, one certificate check):
//       aggregate K sigs (P2 add) + aggregate K pks (P1 add) + ONE core_verify
#include <blst.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using blst::BLST_SUCCESS;
static const std::string SIG_DST = "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";
static const std::string POP_DST = "BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main() {
    const int K = 250;
    unsigned char msg[32];
    for (int i = 0; i < 32; ++i) msg[i] = (unsigned char)(i * 7 + 1);

    std::vector<blst::SecretKey> sks(K);
    std::vector<std::vector<unsigned char>> pks(K), sigs(K), pops(K);
    for (int i = 0; i < K; ++i) {
        unsigned char ikm[32];
        for (int j = 0; j < 32; ++j) ikm[j] = (unsigned char)(i + j + 3);
        sks[i].keygen(ikm, 32);
        pks[i].resize(48); sigs[i].resize(96); pops[i].resize(96);
        blst::P1(sks[i]).compress(pks[i].data());
        blst::P2 sig; sig.hash_to(msg, 32, SIG_DST)->sign_with(sks[i]);
        sig.compress(sigs[i].data());
        blst::P2 pop; pop.hash_to(pks[i].data(), 48, POP_DST)->sign_with(sks[i]);
        pop.compress(pops[i].data());
    }

    // -- per-share verify (one member's share, as OnShare does today) --
    // warm up, then time many iterations of (PoP verify + share verify).
    auto per_share = [&](int i) {
        blst::P1_Affine pkaff(pks[i].data());
        blst::P2_Affine popaff(pops[i].data());
        if (popaff.core_verify(pkaff, true, pks[i].data(), 48, POP_DST) != BLST_SUCCESS) return false;
        blst::P2_Affine sigaff(sigs[i].data());
        return sigaff.core_verify(pkaff, true, msg, 32, SIG_DST) == BLST_SUCCESS;
    };
    for (int i = 0; i < 10; ++i) per_share(i % K); // warm-up
    const int ITER = 200;
    auto t0 = clk::now();
    int ok = 0;
    for (int i = 0; i < ITER; ++i) ok += per_share(i % K);
    auto t1 = clk::now();
    double per_share_ms = ms(t0, t1) / ITER;
    printf("per-share (PoP verify + sig verify, today's OnShare): %.2f ms  [ok=%d/%d]\n",
           per_share_ms, ok, ITER);
    printf("  x100 shares sequential: %.0f ms\n", per_share_ms * 100);
    printf("  x250 shares sequential: %.0f ms\n", per_share_ms * 250);

    // sig-verify only (no PoP), i.e. per-share cost once PoP moves to registration
    auto sig_only = [&](int i) {
        blst::P1_Affine pkaff(pks[i].data());
        blst::P2_Affine sigaff(sigs[i].data());
        return sigaff.core_verify(pkaff, true, msg, 32, SIG_DST) == BLST_SUCCESS;
    };
    t0 = clk::now();
    ok = 0;
    for (int i = 0; i < ITER; ++i) ok += sig_only(i % K);
    t1 = clk::now();
    double sig_only_ms = ms(t0, t1) / ITER;
    printf("per-share (sig verify only, PoP at registration):      %.2f ms\n", sig_only_ms);
    printf("  x250 shares sequential: %.0f ms\n", sig_only_ms * 250);

    // -- batched certificate check (Option A): aggregate K sigs + K pks, ONE verify --
    const int BITER = 20;
    t0 = clk::now();
    ok = 0;
    for (int it = 0; it < BITER; ++it) {
        blst::P2 aggsig(sigs[0].data());
        for (int i = 1; i < K; ++i) aggsig.aggregate(blst::P2_Affine(sigs[i].data()));
        unsigned char aggsig_c[96];
        aggsig.compress(aggsig_c);
        blst::P1 aggpk(pks[0].data());
        for (int i = 1; i < K; ++i) aggpk.aggregate(blst::P1_Affine(pks[i].data()));
        blst::P2_Affine sigaff(aggsig_c);
        ok += (sigaff.core_verify(aggpk.to_affine(), true, msg, 32, SIG_DST) == BLST_SUCCESS);
    }
    t1 = clk::now();
    printf("K=250 batched (aggregate 250 sigs + 250 pks + 1 verify): %.1f ms  [ok=%d/%d]\n",
           ms(t0, t1) / BITER, ok, BITER);
    return 0;
}
