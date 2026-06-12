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
// This is an ECVRF-style construction (RFC 9381 shape: hash-to-curve via
// try-and-increment, gamma = sk*H, Fiat-Shamir proof). It is NOT yet validated
// against RFC 9381 test vectors and uses secp256k1 (not a standardized RFC 9381
// ciphersuite), so it should be treated as a proof-of-concept primitive, not a
// drop-in standards-compliant VRF.

#ifndef BITCOIN_VRF_H
#define BITCOIN_VRF_H

#include <key.h>
#include <pubkey.h>
#include <uint256.h>

#include <optional>
#include <vector>

/** Serialized VRF proof: gamma (33-byte compressed point) || c (32) || s (32). */
static const size_t VRF_PROOF_SIZE = 33 + 32 + 32;

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
