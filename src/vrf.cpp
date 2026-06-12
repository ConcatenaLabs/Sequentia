// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vrf.h>

#include <crypto/sha256.h>
#include <random.h>
#include <span.h>

#include <secp256k1.h>

#include <cstring>
#include <mutex>

namespace {

// Domain-separation tags (the "suite" byte distinguishes hash uses, RFC 9381 §5).
const unsigned char SUITE = 0xFE; // non-standard: marks this secp256k1 PoC suite
const unsigned char H2C_DOMAIN = 0x01;     // hash-to-curve
const unsigned char NONCE_DOMAIN = 0x02;   // nonce derivation
const unsigned char CHALLENGE_DOMAIN = 0x03; // challenge derivation
const unsigned char OUTPUT_DOMAIN = 0x04;  // proof-to-hash

const secp256k1_context* Ctx()
{
    // A full context is required: pubkey creation (sk*G) needs the
    // ecmult-gen tables that the static context lacks. Built once, randomized
    // for side-channel hardening, leaked intentionally (process-lifetime).
    static secp256k1_context* ctx = []() {
        secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        assert(c != nullptr);
        unsigned char seed[32];
        GetRandBytes(seed, 32);
        assert(secp256k1_context_randomize(c, seed));
        return c;
    }();
    return ctx;
}

//! SHA256 over a sequence of byte spans.
uint256 Sha256Cat(std::initializer_list<Span<const unsigned char>> parts)
{
    CSHA256 sha;
    for (const auto& p : parts) {
        sha.Write(p.data(), p.size());
    }
    uint256 out;
    sha.Finalize(out.begin());
    return out;
}

//! Hash `alpha` (bound to the public key) to a curve point via try-and-
//! increment: hash to a candidate x-coordinate and attempt to lift it to a
//! point, incrementing a counter until one lands on the curve.
bool HashToCurve(const std::vector<unsigned char>& pubkey_ser, Span<const unsigned char> alpha,
                 secp256k1_pubkey& out)
{
    const unsigned char suite = SUITE;
    const unsigned char domain = H2C_DOMAIN;
    for (int ctr = 0; ctr < 256; ++ctr) {
        unsigned char ctrb = (unsigned char)ctr;
        uint256 x = Sha256Cat({{&suite, 1}, {&domain, 1},
                               {pubkey_ser.data(), pubkey_ser.size()},
                               {alpha.data(), (size_t)alpha.size()},
                               {&ctrb, 1}});
        // Try both possible y parities for this x.
        for (unsigned char prefix : {0x02, 0x03}) {
            unsigned char candidate[33];
            candidate[0] = prefix;
            std::memcpy(candidate + 1, x.begin(), 32);
            if (secp256k1_ec_pubkey_parse(Ctx(), &out, candidate, 33)) {
                return true;
            }
        }
    }
    return false;
}

bool SerializePoint(const secp256k1_pubkey& point, unsigned char out[33])
{
    size_t len = 33;
    return secp256k1_ec_pubkey_serialize(Ctx(), out, &len, &point, SECP256K1_EC_COMPRESSED) && len == 33;
}

//! Reduce a 32-byte hash to a valid (nonzero, < n) scalar. The probability of a
//! hash exceeding n is ~2^-128, handled by re-hashing with a counter byte.
bool HashToScalar(uint256 h, unsigned char out[32])
{
    for (int ctr = 0; ctr < 256; ++ctr) {
        std::memcpy(out, h.begin(), 32);
        if (secp256k1_ec_seckey_verify(Ctx(), out)) {
            return true;
        }
        unsigned char ctrb = (unsigned char)ctr;
        h = Sha256Cat({{h.begin(), 32}, {&ctrb, 1}});
    }
    return false;
}

//! c = H(SUITE || CHALLENGE || H || Gamma || U || V) reduced to a scalar.
bool ChallengeScalar(const unsigned char H[33], const unsigned char Gamma[33],
                     const unsigned char U[33], const unsigned char V[33],
                     unsigned char c[32])
{
    const unsigned char suite = SUITE;
    const unsigned char domain = CHALLENGE_DOMAIN;
    uint256 h = Sha256Cat({{&suite, 1}, {&domain, 1}, {H, 33}, {Gamma, 33}, {U, 33}, {V, 33}});
    return HashToScalar(h, c);
}

} // namespace

std::optional<std::vector<unsigned char>> VrfProve(const CKey& key, Span<const unsigned char> alpha)
{
    if (!key.IsValid()) return std::nullopt;
    CPubKey cpub = key.GetPubKey();
    std::vector<unsigned char> pub_ser(cpub.begin(), cpub.end());

    unsigned char sk[32];
    std::memcpy(sk, key.begin(), 32);

    // H = hash_to_curve(alpha, Y)
    secp256k1_pubkey H_point;
    if (!HashToCurve(pub_ser, alpha, H_point)) return std::nullopt;
    unsigned char H_ser[33];
    if (!SerializePoint(H_point, H_ser)) return std::nullopt;

    // Gamma = sk * H
    secp256k1_pubkey Gamma = H_point;
    if (!secp256k1_ec_pubkey_tweak_mul(Ctx(), &Gamma, sk)) return std::nullopt;
    unsigned char Gamma_ser[33];
    if (!SerializePoint(Gamma, Gamma_ser)) return std::nullopt;

    // Deterministic nonce k = HashToScalar(SUITE || NONCE || sk || H)
    const unsigned char suite = SUITE;
    const unsigned char ndom = NONCE_DOMAIN;
    uint256 k_hash = Sha256Cat({{&suite, 1}, {&ndom, 1}, {sk, 32}, {H_ser, 33}});
    unsigned char k[32];
    if (!HashToScalar(k_hash, k)) return std::nullopt;

    // U = k*G
    secp256k1_pubkey U;
    if (!secp256k1_ec_pubkey_create(Ctx(), &U, k)) return std::nullopt;
    unsigned char U_ser[33];
    if (!SerializePoint(U, U_ser)) return std::nullopt;

    // V = k*H
    secp256k1_pubkey V = H_point;
    if (!secp256k1_ec_pubkey_tweak_mul(Ctx(), &V, k)) return std::nullopt;
    unsigned char V_ser[33];
    if (!SerializePoint(V, V_ser)) return std::nullopt;

    // c = challenge(H, Gamma, U, V)
    unsigned char c[32];
    if (!ChallengeScalar(H_ser, Gamma_ser, U_ser, V_ser, c)) return std::nullopt;

    // s = k + c*sk  (mod n)
    unsigned char s[32];
    std::memcpy(s, c, 32);
    if (!secp256k1_ec_seckey_tweak_mul(Ctx(), s, sk)) return std::nullopt; // s = c*sk
    if (!secp256k1_ec_seckey_tweak_add(Ctx(), s, k)) return std::nullopt;  // s = c*sk + k

    std::vector<unsigned char> proof;
    proof.reserve(VRF_PROOF_SIZE);
    proof.insert(proof.end(), Gamma_ser, Gamma_ser + 33);
    proof.insert(proof.end(), c, c + 32);
    proof.insert(proof.end(), s, s + 32);
    return proof;
}

std::optional<uint256> VrfProofToHash(Span<const unsigned char> proof)
{
    if ((size_t)proof.size() != VRF_PROOF_SIZE) return std::nullopt;
    // beta = SHA256(SUITE || OUTPUT || Gamma)
    const unsigned char suite = SUITE;
    const unsigned char odom = OUTPUT_DOMAIN;
    // Re-serialize Gamma in canonical form to bind beta to the point, not the
    // (potentially non-canonical) input bytes.
    secp256k1_pubkey Gamma;
    if (!secp256k1_ec_pubkey_parse(Ctx(), &Gamma, proof.data(), 33)) return std::nullopt;
    unsigned char Gamma_ser[33];
    if (!SerializePoint(Gamma, Gamma_ser)) return std::nullopt;
    return Sha256Cat({{&suite, 1}, {&odom, 1}, {Gamma_ser, 33}});
}

bool VrfVerify(const CPubKey& pubkey, Span<const unsigned char> alpha,
               Span<const unsigned char> proof, uint256& output)
{
    if (!pubkey.IsValid()) return false;
    if ((size_t)proof.size() != VRF_PROOF_SIZE) return false;

    const unsigned char* Gamma_in = proof.data();
    const unsigned char* c_in = proof.data() + 33;
    const unsigned char* s_in = proof.data() + 65;

    // Parse Y, Gamma; validate c and s as scalars.
    secp256k1_pubkey Y;
    if (!secp256k1_ec_pubkey_parse(Ctx(), &Y, pubkey.data(), pubkey.size())) return false;
    secp256k1_pubkey Gamma;
    if (!secp256k1_ec_pubkey_parse(Ctx(), &Gamma, Gamma_in, 33)) return false;
    unsigned char c[32];
    std::memcpy(c, c_in, 32);
    if (!secp256k1_ec_seckey_verify(Ctx(), c)) return false;
    unsigned char s[32];
    std::memcpy(s, s_in, 32);
    if (!secp256k1_ec_seckey_verify(Ctx(), s)) return false;

    // H = hash_to_curve(alpha, Y)
    std::vector<unsigned char> pub_ser(pubkey.begin(), pubkey.end());
    secp256k1_pubkey H_point;
    if (!HashToCurve(pub_ser, alpha, H_point)) return false;
    unsigned char H_ser[33];
    if (!SerializePoint(H_point, H_ser)) return false;
    unsigned char Gamma_ser[33];
    if (!SerializePoint(Gamma, Gamma_ser)) return false;

    // U = s*G - c*Y
    secp256k1_pubkey sG;
    if (!secp256k1_ec_pubkey_create(Ctx(), &sG, s)) return false;
    secp256k1_pubkey cY = Y;
    if (!secp256k1_ec_pubkey_tweak_mul(Ctx(), &cY, c)) return false;
    if (!secp256k1_ec_pubkey_negate(Ctx(), &cY)) return false; // -c*Y
    const secp256k1_pubkey* u_terms[2] = {&sG, &cY};
    secp256k1_pubkey U;
    if (!secp256k1_ec_pubkey_combine(Ctx(), &U, u_terms, 2)) return false;
    unsigned char U_ser[33];
    if (!SerializePoint(U, U_ser)) return false;

    // V = s*H - c*Gamma
    secp256k1_pubkey sH = H_point;
    if (!secp256k1_ec_pubkey_tweak_mul(Ctx(), &sH, s)) return false;
    secp256k1_pubkey cG = Gamma;
    if (!secp256k1_ec_pubkey_tweak_mul(Ctx(), &cG, c)) return false;
    if (!secp256k1_ec_pubkey_negate(Ctx(), &cG)) return false; // -c*Gamma
    const secp256k1_pubkey* v_terms[2] = {&sH, &cG};
    secp256k1_pubkey V;
    if (!secp256k1_ec_pubkey_combine(Ctx(), &V, v_terms, 2)) return false;
    unsigned char V_ser[33];
    if (!SerializePoint(V, V_ser)) return false;

    // c' = challenge(H, Gamma, U, V); accept iff c' == c.
    unsigned char c_check[32];
    if (!ChallengeScalar(H_ser, Gamma_ser, U_ser, V_ser, c_check)) return false;
    if (std::memcmp(c_check, c, 32) != 0) return false;

    auto beta = VrfProofToHash(proof);
    if (!beta) return false;
    output = *beta;
    return true;
}
