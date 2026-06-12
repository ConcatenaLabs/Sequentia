// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: a verifiable random function (VRF) over secp256k1, for private
// cryptographic sortition in Proof-of-Stake (the paper's principle 3 / section
// iv consensus algorithm). See doc/sequentia/07-vrf.md.
//
// Given a secret key sk (with public key Y = sk*G) and an input alpha, the
// holder of sk can compute:
//   - a 32-byte pseudorandom output  beta = VrfProofToHash(pi)
//   - a proof pi, such that anyone with (Y, alpha, pi) can verify that beta is
//     the unique correct output for (Y, alpha).
//
// Properties (verified in src/test/vrf_tests.cpp):
//   - Uniqueness:     for fixed (sk, alpha) there is exactly one valid beta.
//   - Pseudorandom:   beta is indistinguishable from random without sk.
//   - Verifiability:  VrfVerify accepts iff pi was produced by sk over alpha.
//   - Collusion-free: a staker cannot grind beta without changing sk (its
//                     on-chain staking identity).
//
// This is ECVRF-SECP256K1-SHA256-TAI, structured per RFC 9381: encode_to_curve
// via try-and-increment, gamma = sk*H, the RFC's challenge_generation with the
// truncated 16-byte challenge, and the RFC proof_to_hash. secp256k1 is not one
// of the RFC's registered ciphersuites (those are P-256 and edwards25519), so
// the suite octet uses the RFC's experimental value 0xFF and there are no
// official test vectors to validate against; the deterministic nonce uses
// SHA-256 rather than the RFC 6979 HMAC_DRBG of the P-256 suite. These residual
// deviations are documented in doc/sequentia/07-vrf.md §2.

#ifndef BITCOIN_VRF_H
#define BITCOIN_VRF_H

#include <key.h>
#include <pubkey.h>
#include <uint256.h>

#include <optional>
#include <vector>

/** Serialized VRF proof (RFC 9381 §5.5): gamma (33-byte compressed point) ||
 *  c (16-byte truncated challenge) || s (32-byte scalar). */
static const size_t VRF_PROOF_SIZE = 33 + 16 + 32;

/** Produce a VRF proof for input `alpha` under secret key `key`.
 *  Returns the serialized proof, or nullopt on failure. */
std::optional<std::vector<unsigned char>> VrfProve(const CKey& key, Span<const unsigned char> alpha);

/** Verify a VRF proof `proof` for input `alpha` under public key `pubkey`.
 *  On success, writes the 32-byte VRF output to `output` and returns true. */
bool VrfVerify(const CPubKey& pubkey, Span<const unsigned char> alpha,
               Span<const unsigned char> proof, uint256& output);

/** Derive the 32-byte VRF output (beta) from a valid proof, without verifying
 *  it. Use only on proofs already checked with VrfVerify. */
std::optional<uint256> VrfProofToHash(Span<const unsigned char> proof);

#endif // BITCOIN_VRF_H
