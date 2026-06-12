// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: MuSig2 (BIP327) signature aggregation over secp256k1.
//
// The proof-of-concept committee certification (doc/sequentia/06 §committee,
// doc/sequentia/07 §6) uses an OP_CHECKMULTISIG challenge, which carries one
// pubkey and one signature *per* committee member and so caps the committee at
// 16. MuSig2 lets a set of signers produce a single 64-byte BIP340 Schnorr
// signature under a single aggregate public key — independent of the set size
// — which is the path to the paper's 100-member committees.
//
// This is the standalone, tested primitive. Because the block producer holds
// all the signing keys (they are passed to generateposblock), MuSig functions
// here run the full two-round protocol locally in one call. Distributed
// signing among separately-hosted committee members is an operational layer on
// top of the same primitive and is out of scope for the PoC.

#ifndef BITCOIN_MUSIG_H
#define BITCOIN_MUSIG_H

#include <key.h>
#include <pubkey.h>
#include <span.h>

#include <optional>
#include <vector>

/** Aggregate a set of public keys into a single 32-byte x-only MuSig2
 *  aggregate key. The inputs are sorted canonically first, so any party
 *  computes the same aggregate from the same *set* of keys. Returns the
 *  32-byte aggregate x-only pubkey, or nullopt on failure. */
std::optional<std::vector<unsigned char>> MuSigAggregatePubkey(const std::vector<CPubKey>& pubkeys);

/** Produce a single MuSig2 (BIP340 Schnorr) signature over msg32 under the
 *  aggregate of `pubkeys`, signed by `keys` (which must be exactly the keys
 *  for `pubkeys` — MuSig2 is an n-of-n scheme, so to realize a q-of-m quorum
 *  the caller aggregates exactly the q signing members). Returns the 64-byte
 *  signature, or nullopt on failure. */
std::optional<std::vector<unsigned char>> MuSigSign(const std::vector<CKey>& keys,
                                                    const std::vector<CPubKey>& pubkeys,
                                                    Span<const unsigned char> msg32);

/** Verify a 64-byte MuSig2 signature over msg32 under the aggregate of
 *  `pubkeys`. (Equivalent to BIP340 Schnorr verification against
 *  MuSigAggregatePubkey(pubkeys).) */
bool MuSigVerify(const std::vector<CPubKey>& pubkeys, Span<const unsigned char> msg32,
                 Span<const unsigned char> sig64);

#endif // BITCOIN_MUSIG_H
