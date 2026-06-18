// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vrf.h>

#include <crypto/sha256.h>
#include <random.h>
#include <span.h>
#include <support/cleanse.h>

#include <secp256k1.h>
#include <secp256k1_ecdh.h>

#include <cstring>
#include <mutex>

namespace {

// ECVRF-SECP256K1-SHA256-TAI, structured per RFC 9381 (ECVRF). secp256k1 is not
// one of the RFC's registered ciphersuites, so following §5.5's convention for
// an *experimental* suite we use suite octet 0xFF; the rest of the construction
// (encode_to_curve TAI, challenge generation with the truncated 16-byte
// challenge, proof_to_hash) follows the RFC's framing and domain separators.
const unsigned char SUITE = 0xFF; // RFC 9381 §5.5: 0xFF = experimental use

// RFC 9381 domain-separator "front" octets, each paired with a 0x00 "back"
// octet, hashed as: suite || front || <points/strings> || back.
const unsigned char H2C_FRONT = 0x01;       // §5.4.1.1 encode_to_curve (TAI)
const unsigned char CHALLENGE_FRONT = 0x02; // §5.4.3  challenge_generation
const unsigned char OUTPUT_FRONT = 0x03;    // §5.2    proof_to_hash
const unsigned char BACK = 0x00;
// Nonce derivation is a private, unverifiable step (the RFC's P256-TAI suite
// uses an RFC 6979 HMAC_DRBG); we derive it deterministically from (sk, H) with
// SHA-256 under a private domain octet. This is the one documented deviation
// from the RFC framing and is interop-irrelevant: nonce secrecy/determinism is
// all that the proof's soundness needs (see doc/sequentia/04-proof-of-stake.md §2).
const unsigned char NONCE_FRONT = 0x81;

// Length of the truncated Fiat-Shamir challenge c (RFC 9381 §5.5: cLen = 16 for
// the 128-bit-security suites; c is a 16-byte big-endian integer, always < n).
const size_t C_LEN = 16;

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
        const bool ok = secp256k1_context_randomize(c, seed);
        assert(ok);
        memory_cleanse(seed, 32);
        return c;
    }();
    return ctx;
}

//! Wipe a memory region at scope exit, so secret scalars never outlive their
//! use in stack remnants (cf. src/key.cpp).
class ScopedCleanse
{
    void* m_ptr;
    size_t m_len;

public:
    ScopedCleanse(void* ptr, size_t len) : m_ptr(ptr), m_len(len) {}
    ~ScopedCleanse() { memory_cleanse(m_ptr, m_len); }
    ScopedCleanse(const ScopedCleanse&) = delete;
    ScopedCleanse& operator=(const ScopedCleanse&) = delete;
};

//! secp256k1_ecdh hash callback that returns the raw uncompressed point
//! instead of hashing it, exposing ecdh's constant-time multiplication.
extern "C" int EcdhRawPoint(unsigned char* output, const unsigned char* x32,
                            const unsigned char* y32, void*)
{
    output[0] = 0x04;
    std::memcpy(output + 1, x32, 32);
    std::memcpy(output + 33, y32, 32);
    return 1;
}

//! out = scalar * point, in constant time with respect to `scalar`.
//! secp256k1_ec_pubkey_tweak_mul is variable-time (its tweak is presumed
//! public), which would leak our secret scalars (the staking key, the proof
//! nonce) through timing side channels on this hot, repeated path; ECDH is the
//! library's public route to the constant-time multiplier.
bool MulConstTime(const secp256k1_pubkey& point, const unsigned char scalar[32],
                  secp256k1_pubkey& out)
{
    unsigned char raw[65];
    if (!secp256k1_ecdh(Ctx(), raw, &point, scalar, EcdhRawPoint, nullptr)) return false;
    return secp256k1_ec_pubkey_parse(Ctx(), &out, raw, 65);
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

//! RFC 9381 §5.4.1.1 ECVRF_encode_to_curve_try_and_increment: bind `alpha` to
//! the public key (the encode_to_curve_salt) and hash to a candidate point,
//! incrementing a counter until one lands on the curve. Per the RFC's TAI
//! suites, arbitrary_string_to_point tries the single 0x02 (even-y) prefix:
//!   hash = SHA256(suite || 0x01 || PK || alpha || ctr || 0x00)
//!   H    = string_to_point(0x02 || hash)
bool HashToCurve(const std::vector<unsigned char>& pubkey_ser, Span<const unsigned char> alpha,
                 secp256k1_pubkey& out)
{
    const unsigned char suite = SUITE;
    const unsigned char front = H2C_FRONT;
    const unsigned char back = BACK;
    for (int ctr = 0; ctr < 256; ++ctr) {
        unsigned char ctrb = (unsigned char)ctr;
        uint256 x = Sha256Cat({{&suite, 1}, {&front, 1},
                               {pubkey_ser.data(), pubkey_ser.size()},
                               {alpha.data(), (size_t)alpha.size()},
                               {&ctrb, 1}, {&back, 1}});
        unsigned char candidate[33];
        candidate[0] = 0x02;
        std::memcpy(candidate + 1, x.begin(), 32);
        if (secp256k1_ec_pubkey_parse(Ctx(), &out, candidate, 33)) {
            return true;
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

//! RFC 9381 §5.4.3 ECVRF_challenge_generation:
//!   c_string  = SHA256(suite || 0x02 || Y || H || Gamma || U || V || 0x00)
//!   c         = string_to_int( c_string[0 .. cLen-1] )   (cLen = 16)
//! The 16-byte truncated challenge is returned as a full 32-byte big-endian
//! scalar (zero-padded high), so it is always nonzero (negligibly) and < n.
void ChallengeScalar(const unsigned char Y[33], const unsigned char H[33],
                     const unsigned char Gamma[33], const unsigned char U[33],
                     const unsigned char V[33], unsigned char c[32])
{
    const unsigned char suite = SUITE;
    const unsigned char front = CHALLENGE_FRONT;
    const unsigned char back = BACK;
    uint256 h = Sha256Cat({{&suite, 1}, {&front, 1}, {Y, 33}, {H, 33},
                           {Gamma, 33}, {U, 33}, {V, 33}, {&back, 1}});
    std::memset(c, 0, 32);
    std::memcpy(c + (32 - C_LEN), h.begin(), C_LEN); // low 16 bytes = truncated c
}

} // namespace

std::optional<std::vector<unsigned char>> VrfProve(const CKey& key, Span<const unsigned char> alpha)
{
    if (!key.IsValid()) return std::nullopt;
    CPubKey cpub = key.GetPubKey();
    // Only compressed keys: ChallengeScalar binds a 33-byte point_to_string(Y)
    // per RFC 9381 §5.4.3, and stakers are identified by their exact
    // serialized bytes. (VrfVerify enforces the same, so prove/verify agree.)
    if (!cpub.IsCompressed()) return std::nullopt;
    std::vector<unsigned char> pub_ser(cpub.begin(), cpub.end());

    unsigned char sk[32];
    std::memcpy(sk, key.begin(), 32);
    ScopedCleanse cleanse_sk(sk, 32);

    // H = hash_to_curve(alpha, Y)
    secp256k1_pubkey H_point;
    if (!HashToCurve(pub_ser, alpha, H_point)) return std::nullopt;
    unsigned char H_ser[33];
    if (!SerializePoint(H_point, H_ser)) return std::nullopt;

    // Gamma = sk * H (constant-time: sk is the long-term staking secret)
    secp256k1_pubkey Gamma;
    if (!MulConstTime(H_point, sk, Gamma)) return std::nullopt;
    unsigned char Gamma_ser[33];
    if (!SerializePoint(Gamma, Gamma_ser)) return std::nullopt;

    // Deterministic nonce k = HashToScalar(SUITE || NONCE_FRONT || sk || H)
    // (private/unverifiable step; see the NONCE_FRONT note above).
    const unsigned char suite = SUITE;
    const unsigned char ndom = NONCE_FRONT;
    uint256 k_hash = Sha256Cat({{&suite, 1}, {&ndom, 1}, {sk, 32}, {H_ser, 33}});
    ScopedCleanse cleanse_k_hash(k_hash.begin(), 32);
    unsigned char k[32];
    ScopedCleanse cleanse_k(k, 32);
    if (!HashToScalar(k_hash, k)) return std::nullopt;

    // U = k*G
    secp256k1_pubkey U;
    if (!secp256k1_ec_pubkey_create(Ctx(), &U, k)) return std::nullopt;
    unsigned char U_ser[33];
    if (!SerializePoint(U, U_ser)) return std::nullopt;

    // V = k*H (constant-time: k is secret — its leak recovers sk from s)
    secp256k1_pubkey V;
    if (!MulConstTime(H_point, k, V)) return std::nullopt;
    unsigned char V_ser[33];
    if (!SerializePoint(V, V_ser)) return std::nullopt;

    // c = challenge(Y, H, Gamma, U, V); c[32] is the truncated 16-byte value
    // sitting in its low bytes.
    unsigned char c[32];
    ChallengeScalar(pub_ser.data(), H_ser, Gamma_ser, U_ser, V_ser, c);

    // s = k + c*sk  (mod n)
    unsigned char s[32];
    std::memcpy(s, c, 32);
    if (!secp256k1_ec_seckey_tweak_mul(Ctx(), s, sk)) return std::nullopt; // s = c*sk
    if (!secp256k1_ec_seckey_tweak_add(Ctx(), s, k)) return std::nullopt;  // s = c*sk + k

    // pi = point_to_string(Gamma) || int_to_string(c, cLen) || int_to_string(s, qLen)
    //    = Gamma(33) || c(16) || s(32)   (RFC 9381 §5.5)
    std::vector<unsigned char> proof;
    proof.reserve(VRF_PROOF_SIZE);
    proof.insert(proof.end(), Gamma_ser, Gamma_ser + 33);
    proof.insert(proof.end(), c + (32 - C_LEN), c + 32); // 16-byte truncated c
    proof.insert(proof.end(), s, s + 32);
    return proof;
}

std::optional<uint256> VrfProofToHash(Span<const unsigned char> proof)
{
    if ((size_t)proof.size() != VRF_PROOF_SIZE) return std::nullopt;
    // RFC 9381 §5.2 proof_to_hash:
    //   beta = SHA256(suite || 0x03 || point_to_string(cofactor*Gamma) || 0x00)
    // secp256k1 has cofactor 1, so cofactor*Gamma = Gamma.
    const unsigned char suite = SUITE;
    const unsigned char front = OUTPUT_FRONT;
    const unsigned char back = BACK;
    // Re-serialize Gamma in canonical form to bind beta to the point, not the
    // (potentially non-canonical) input bytes.
    secp256k1_pubkey Gamma;
    if (!secp256k1_ec_pubkey_parse(Ctx(), &Gamma, proof.data(), 33)) return std::nullopt;
    unsigned char Gamma_ser[33];
    if (!SerializePoint(Gamma, Gamma_ser)) return std::nullopt;
    return Sha256Cat({{&suite, 1}, {&front, 1}, {Gamma_ser, 33}, {&back, 1}});
}

bool VrfVerify(const CPubKey& pubkey, Span<const unsigned char> alpha,
               Span<const unsigned char> proof, uint256& output)
{
    if (!pubkey.IsValid()) return false;
    if (!pubkey.IsCompressed()) return false; // must mirror VrfProve
    if ((size_t)proof.size() != VRF_PROOF_SIZE) return false;

    // pi = Gamma(33) || c(16) || s(32)  (RFC 9381 §5.5)
    const unsigned char* Gamma_in = proof.data();
    const unsigned char* c_in = proof.data() + 33;
    const unsigned char* s_in = proof.data() + 33 + C_LEN;

    // Parse Y, Gamma; load c (16-byte truncated → low bytes of a 32-byte
    // scalar) and validate s as a scalar.
    secp256k1_pubkey Y;
    if (!secp256k1_ec_pubkey_parse(Ctx(), &Y, pubkey.data(), pubkey.size())) return false;
    secp256k1_pubkey Gamma;
    if (!secp256k1_ec_pubkey_parse(Ctx(), &Gamma, Gamma_in, 33)) return false;
    unsigned char c[32];
    std::memset(c, 0, 32);
    std::memcpy(c + (32 - C_LEN), c_in, C_LEN);
    if (!secp256k1_ec_seckey_verify(Ctx(), c)) return false; // c != 0
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

    // c' = challenge(Y, H, Gamma, U, V); accept iff c' == c.
    unsigned char c_check[32];
    ChallengeScalar(pub_ser.data(), H_ser, Gamma_ser, U_ser, V_ser, c_check);
    if (std::memcmp(c_check, c, 32) != 0) return false;

    auto beta = VrfProofToHash(proof);
    if (!beta) return false;
    output = *beta;
    return true;
}
