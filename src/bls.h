// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: BLS12-381 signatures (the "min-pk" scheme: 48-byte public keys in
// G1, 96-byte signatures in G2) with proof-of-possession, for the autonomous
// Proof-of-Stake committee (doc/sequentia/proposals/autonomous-committee.md §7).
//
// Unlike the interactive, n-of-n MuSig2 used by the coordinator path, BLS
// aggregation is NON-INTERACTIVE: each committee member signs the block's
// signhash independently and broadcasts a single share; ANY node then
// aggregates whatever subset of shares it has into one constant-size signature,
// verifiable against exactly the pubkeys of the members who signed. That is
// what lets a gossip committee assemble a certificate from "whichever members
// respond" with no second round and no pre-committed signer set.
//
// All keys/signatures cross this interface as their canonical compressed byte
// encodings; blst is an internal detail of bls.cpp. The IETF "POP" ciphersuite
// (BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_) is used; rogue-key attacks are
// defended by requiring a proof-of-possession for every registered pubkey.

#ifndef BITCOIN_BLS_H
#define BITCOIN_BLS_H

#include <span.h>

#include <optional>
#include <vector>

/** Canonical compressed encodings. */
static const size_t BLS_SK_SIZE = 32;   //!< secret key material (IKM)
static const size_t BLS_PK_SIZE = 48;   //!< compressed G1 public key
static const size_t BLS_SIG_SIZE = 96;  //!< compressed G2 signature

/** Derive the 48-byte compressed public key for the secret key material
 *  `sk` (32 bytes, used as IKM via the EIP-2333 keygen). nullopt on failure. */
std::optional<std::vector<unsigned char>> BlsDerivePubKey(Span<const unsigned char> sk);

/** Sign the 32-byte message `msg32` with secret key material `sk`, returning the
 *  96-byte signature. nullopt on failure. */
std::optional<std::vector<unsigned char>> BlsSign(Span<const unsigned char> sk,
                                                  Span<const unsigned char> msg32);

/** Verify a 96-byte signature `sig` over `msg32` under the 48-byte public key
 *  `pk`. False on a bad encoding or an invalid signature. */
bool BlsVerify(Span<const unsigned char> pk, Span<const unsigned char> msg32,
               Span<const unsigned char> sig);

/** Aggregate public keys into one 48-byte aggregate key. The aggregate of the
 *  members' keys is what a block's BLS challenge commits to; a 96-byte aggregate
 *  signature then verifies against it (BlsVerify) exactly when every member
 *  signed. Order-independent. nullopt if the set is empty or any encoding is
 *  invalid. */
std::optional<std::vector<unsigned char>> BlsAggregatePublicKeys(
    const std::vector<std::vector<unsigned char>>& pks);

/** Aggregate signatures (each over the *same* message) into one 96-byte
 *  signature. The inputs are individual 96-byte signatures in any order; the
 *  result verifies with BlsFastAggregateVerify against the signers' pubkeys.
 *  nullopt if the set is empty or any encoding is invalid. */
std::optional<std::vector<unsigned char>> BlsAggregate(
    const std::vector<std::vector<unsigned char>>& sigs);

/** Verify a 96-byte aggregate signature `aggsig` over the single message
 *  `msg32` signed by all of `pks` (each 48 bytes). This is the fast-aggregate
 *  check: it holds iff every listed key signed exactly `msg32`. False on an
 *  empty set, a bad encoding, or an invalid aggregate. */
bool BlsFastAggregateVerify(const std::vector<std::vector<unsigned char>>& pks,
                            Span<const unsigned char> msg32,
                            Span<const unsigned char> aggsig);

/** Produce a 96-byte proof-of-possession for `sk`: a signature over the key's
 *  own public key under the POP ciphersuite, binding the key holder to the
 *  pubkey and closing the BLS rogue-key attack. nullopt on failure. */
std::optional<std::vector<unsigned char>> BlsProvePossession(Span<const unsigned char> sk);

/** Verify a 96-byte proof-of-possession `pop` for the 48-byte public key `pk`. */
bool BlsVerifyPossession(Span<const unsigned char> pk, Span<const unsigned char> pop);

#endif // BITCOIN_BLS_H
