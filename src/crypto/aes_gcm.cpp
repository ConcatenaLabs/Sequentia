// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/aes_gcm.h>

#include <crypto/aes.h>
#include <crypto/common.h>
#include <support/cleanse.h>

#include <cstring>

namespace {

using Block = unsigned char[16];

//! Multiply X by Y in GF(2^128), GCM's reversed bit convention: bit 0 of byte 0
//! is the most significant coefficient, and the reduction polynomial is
//! x^128 + x^7 + x^2 + x + 1, applied as 0xe1 into the top byte on a shift-out.
//!
//! The straightforward bit-at-a-time algorithm. Not fast, and it does not need
//! to be: the courier seals messages of a few hundred bytes, a handful of times
//! per conversion. A table-driven version would be harder to check against the
//! vectors, which is the only property this code is bought for.
void GHashMul(unsigned char x[16], const unsigned char y[16])
{
    unsigned char z[16] = {0};
    unsigned char v[16];
    memcpy(v, x, 16);

    for (int i = 0; i < 128; ++i) {
        const int byte = i >> 3;
        const int bit = 7 - (i & 7);
        if ((y[byte] >> bit) & 1) {
            for (int k = 0; k < 16; ++k) z[k] ^= v[k];
        }
        // v >>= 1 across the whole 128-bit block, reducing when a 1 falls off.
        const bool lsb = v[15] & 1;
        for (int k = 15; k > 0; --k) v[k] = (unsigned char)((v[k] >> 1) | ((v[k - 1] & 1) << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(x, z, 16);
}

//! GHASH over a padded byte string, folded into `y`.
void GHashUpdate(unsigned char y[16], const unsigned char h[16],
                 const unsigned char* data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        unsigned char block[16] = {0};
        const size_t n = (len - off) < 16 ? (len - off) : 16;
        memcpy(block, data + off, n);
        for (int k = 0; k < 16; ++k) y[k] ^= block[k];
        GHashMul(y, h);
        off += n;
    }
}

void Inc32(unsigned char ctr[16])
{
    for (int i = 15; i >= 12; --i) {
        if (++ctr[i] != 0) break;
    }
}

//! The GCM tag over `aad` and `ciphertext`, given the hash subkey and J0.
void ComputeTag(const AES256Encrypt& aes, const unsigned char h[16],
                const unsigned char j0[16],
                const std::vector<unsigned char>& aad,
                const unsigned char* ct, size_t ct_len,
                unsigned char tag[16])
{
    unsigned char y[16] = {0};
    GHashUpdate(y, h, aad.data(), aad.size());
    GHashUpdate(y, h, ct, ct_len);

    // The length block: bit lengths of A and C, each 64-bit big-endian.
    unsigned char lens[16] = {0};
    const uint64_t abits = (uint64_t)aad.size() * 8;
    const uint64_t cbits = (uint64_t)ct_len * 8;
    for (int i = 0; i < 8; ++i) {
        lens[7 - i] = (unsigned char)((abits >> (8 * i)) & 0xff);
        lens[15 - i] = (unsigned char)((cbits >> (8 * i)) & 0xff);
    }
    for (int k = 0; k < 16; ++k) y[k] ^= lens[k];
    GHashMul(y, h);

    unsigned char s[16];
    aes.Encrypt(s, j0);
    for (int k = 0; k < 16; ++k) tag[k] = (unsigned char)(y[k] ^ s[k]);
}

//! GCTR: XOR the data with the AES keystream starting at inc32(J0).
void GCtr(const AES256Encrypt& aes, const unsigned char j0[16],
          const unsigned char* in, size_t len, unsigned char* out)
{
    unsigned char ctr[16];
    memcpy(ctr, j0, 16);
    size_t off = 0;
    while (off < len) {
        Inc32(ctr);
        unsigned char ks[16];
        aes.Encrypt(ks, ctr);
        const size_t n = (len - off) < 16 ? (len - off) : 16;
        for (size_t k = 0; k < n; ++k) out[off + k] = (unsigned char)(in[off + k] ^ ks[k]);
        memory_cleanse(ks, sizeof(ks));
        off += n;
    }
    memory_cleanse(ctr, sizeof(ctr));
}

//! Constant-time equality. A tag check that leaks where it failed is a tag
//! check an attacker can walk one byte at a time.
bool ConstantTimeEqual(const unsigned char* a, const unsigned char* b, size_t len)
{
    unsigned char diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

void DeriveSubkeys(const AES256Encrypt& aes, const unsigned char nonce[AES_GCM_NONCE_SIZE],
                   unsigned char h[16], unsigned char j0[16])
{
    const unsigned char zero[16] = {0};
    aes.Encrypt(h, zero);
    memset(j0, 0, 16);
    memcpy(j0, nonce, AES_GCM_NONCE_SIZE);
    j0[15] = 1;   // 96-bit nonce: J0 = IV || 0^31 || 1
}

} // namespace

std::vector<unsigned char> AES256GCMEncrypt(const unsigned char key[32],
                                            const unsigned char nonce[AES_GCM_NONCE_SIZE],
                                            const std::vector<unsigned char>& plaintext,
                                            const std::vector<unsigned char>& aad)
{
    const AES256Encrypt aes(key);
    unsigned char h[16], j0[16];
    DeriveSubkeys(aes, nonce, h, j0);

    std::vector<unsigned char> out(plaintext.size() + AES_GCM_TAG_SIZE);
    if (!plaintext.empty()) GCtr(aes, j0, plaintext.data(), plaintext.size(), out.data());

    unsigned char tag[16];
    ComputeTag(aes, h, j0, aad, out.data(), plaintext.size(), tag);
    memcpy(out.data() + plaintext.size(), tag, AES_GCM_TAG_SIZE);

    memory_cleanse(h, sizeof(h));
    memory_cleanse(j0, sizeof(j0));
    return out;
}

bool AES256GCMDecrypt(const unsigned char key[32],
                      const unsigned char nonce[AES_GCM_NONCE_SIZE],
                      const std::vector<unsigned char>& ciphertext_and_tag,
                      const std::vector<unsigned char>& aad,
                      std::vector<unsigned char>& out)
{
    if (ciphertext_and_tag.size() < AES_GCM_TAG_SIZE) return false;
    const size_t ct_len = ciphertext_and_tag.size() - AES_GCM_TAG_SIZE;

    const AES256Encrypt aes(key);
    unsigned char h[16], j0[16];
    DeriveSubkeys(aes, nonce, h, j0);

    unsigned char tag[16];
    ComputeTag(aes, h, j0, aad, ciphertext_and_tag.data(), ct_len, tag);
    if (!ConstantTimeEqual(tag, ciphertext_and_tag.data() + ct_len, AES_GCM_TAG_SIZE)) {
        memory_cleanse(h, sizeof(h));
        memory_cleanse(j0, sizeof(j0));
        return false;
    }

    // Only now is there a plaintext worth producing.
    std::vector<unsigned char> plain(ct_len);
    if (ct_len) GCtr(aes, j0, ciphertext_and_tag.data(), ct_len, plain.data());
    out = std::move(plain);

    memory_cleanse(h, sizeof(h));
    memory_cleanse(j0, sizeof(j0));
    return true;
}
