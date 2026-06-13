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
// Two ways to produce a signature:
//  - MuSigSign runs the full two-round protocol locally in one call, for a
//    producer that holds all the signing keys (generateposblock).
//  - The MuSigSession* functions below split the rounds across separately
//    hosted members, none of which shares its key (the musig* RPCs +
//    getposblocktemplate/submitposblock; doc/sequentia/07 §6).

#ifndef BITCOIN_MUSIG_H
#define BITCOIN_MUSIG_H

#include <key.h>
#include <pubkey.h>
#include <span.h>

#include <optional>
#include <string>
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

// --- Distributed (multi-host) signing (doc/sequentia/07-vrf.md §6) ---
//
// The local MuSigSign above needs every signer's key on one host (the block
// producer). For a real decentralized committee each member runs on its own
// host and contributes without revealing its key, via BIP327's two MuSig2
// rounds. BIP327's secret nonce is deliberately non-serialisable (so it can
// never be persisted and accidentally reused — reuse leaks the key), so each
// member's node keeps the live secret nonce in memory between rounds, in a
// session store, and consumes it exactly once.

/** Round 1 on a member's host: generate this member's public nonce for signing
 *  `msg32` under the aggregate of `pubkeys`, and stash the matching secret
 *  nonce under `session_id` for round 2. `session_id` must be fresh (an
 *  existing id is an error, to prevent secret-nonce reuse). Returns the
 *  66-byte public nonce to broadcast to the other members. */
std::optional<std::vector<unsigned char>> MuSigSessionNonce(
    const std::string& session_id, const CKey& key,
    const std::vector<CPubKey>& pubkeys, Span<const unsigned char> msg32,
    std::string& error);

/** Round 2 on a member's host: given every member's round-1 public nonce
 *  (`pubnonces`, order-independent), produce this member's 32-byte partial
 *  signature and consume the stored secret nonce (the session is erased, so a
 *  second call with the same id fails — single-use). The `pubkeys` and `msg32`
 *  must match round 1. */
std::optional<std::vector<unsigned char>> MuSigSessionPartialSign(
    const std::string& session_id, const CKey& key,
    const std::vector<CPubKey>& pubkeys,
    const std::vector<std::vector<unsigned char>>& pubnonces,
    Span<const unsigned char> msg32, std::string& error);

/** Aggregate the members' partial signatures into the final 64-byte BIP340
 *  signature (public; no secret material). `pubnonces` and `partials` are the
 *  round-1 and round-2 contributions; `pubkeys`/`msg32` define the context. */
std::optional<std::vector<unsigned char>> MuSigAggregatePartials(
    const std::vector<CPubKey>& pubkeys,
    const std::vector<std::vector<unsigned char>>& pubnonces,
    const std::vector<std::vector<unsigned char>>& partials,
    Span<const unsigned char> msg32);

/** Discard a pending round-1 session without signing (cleanup on abort). No-op
 *  if the id is unknown. */
void MuSigSessionAbort(const std::string& session_id);

#endif // BITCOIN_MUSIG_H
