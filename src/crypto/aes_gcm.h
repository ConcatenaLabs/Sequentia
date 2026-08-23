// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_AES_GCM_H
#define BITCOIN_CRYPTO_AES_GCM_H

#include <cstdint>
#include <vector>

/**
 * AES-256-GCM, with a 96-bit nonce and a 128-bit tag.
 *
 * This exists for exactly one reason: the SeqDEX courier seals its
 * end-to-end payloads with AES-256-GCM (Go's `cipher.NewGCM`, and the browser's
 * WebCrypto `AES-GCM`), and a node that cannot open that envelope cannot take a
 * cross-chain offer. Nothing in consensus, in the P2P protocol, or in the
 * wallet's own storage uses it, and nothing should start: this is a client for
 * somebody else's wire format, not a new format of ours.
 *
 * Built on the tree's existing AES-256 block encryption (ctaes). The only new
 * arithmetic is GHASH -- multiplication in GF(2^128) under GCM's reversed bit
 * convention -- which is pinned against the NIST GCM validation vectors in
 * `aes_gcm_tests.cpp`. New cryptographic code inside a node deserves published
 * test vectors rather than a plausible-looking implementation, and that is what
 * it has.
 */

/** GCM's nonce and tag sizes. Only the 96-bit nonce form is implemented,
 *  because it is the only one the courier uses and the only one whose J0
 *  derivation needs no GHASH of its own. */
static constexpr size_t AES_GCM_NONCE_SIZE = 12;
static constexpr size_t AES_GCM_TAG_SIZE = 16;

/** Seal `plaintext` under `key` and `nonce`.
 *
 *  Returns ciphertext || tag, the layout Go's `gcm.Seal` produces and the
 *  browser expects -- the tag is appended, not carried separately. `aad` may be
 *  empty, which is what the courier uses. */
std::vector<unsigned char> AES256GCMEncrypt(const unsigned char key[32],
                                            const unsigned char nonce[AES_GCM_NONCE_SIZE],
                                            const std::vector<unsigned char>& plaintext,
                                            const std::vector<unsigned char>& aad = {});

/** Open a sealed message.
 *
 *  `ciphertext_and_tag` is exactly what AES256GCMEncrypt produced. Returns false
 *  and leaves `out` untouched when the tag does not verify -- which is the only
 *  answer that matters: a GCM failure means the bytes were not written by
 *  someone holding the key, and there is nothing to salvage from them. The tag
 *  comparison is constant-time. */
bool AES256GCMDecrypt(const unsigned char key[32],
                      const unsigned char nonce[AES_GCM_NONCE_SIZE],
                      const std::vector<unsigned char>& ciphertext_and_tag,
                      const std::vector<unsigned char>& aad,
                      std::vector<unsigned char>& out);

#endif // BITCOIN_CRYPTO_AES_GCM_H
