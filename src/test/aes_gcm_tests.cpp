// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// AES-256-GCM, against the NIST GCM validation vectors.
//
// This is new cryptographic code in a node, which is a thing that should have
// to justify itself. Its justification is that the SeqDEX courier seals its
// end-to-end payloads this way and a node that cannot open that envelope cannot
// take a cross-chain offer -- and its warrant is these vectors, which are
// published, adversarially chosen, and not ours.

#include <crypto/aes_gcm.h>

#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(aes_gcm_tests, BasicTestingSetup)

namespace {
void CheckVector(const std::string& key_hex, const std::string& nonce_hex,
                 const std::string& pt_hex, const std::string& aad_hex,
                 const std::string& ct_hex, const std::string& tag_hex)
{
    const auto key = ParseHex(key_hex);
    const auto nonce = ParseHex(nonce_hex);
    const auto pt = ParseHex(pt_hex);
    const auto aad = ParseHex(aad_hex);
    BOOST_REQUIRE_EQUAL(key.size(), 32U);
    BOOST_REQUIRE_EQUAL(nonce.size(), AES_GCM_NONCE_SIZE);

    const auto sealed = AES256GCMEncrypt(key.data(), nonce.data(), pt, aad);
    BOOST_REQUIRE_EQUAL(sealed.size(), pt.size() + AES_GCM_TAG_SIZE);
    BOOST_CHECK_EQUAL(HexStr(std::vector<unsigned char>(sealed.begin(), sealed.begin() + pt.size())), ct_hex);
    BOOST_CHECK_EQUAL(HexStr(std::vector<unsigned char>(sealed.begin() + pt.size(), sealed.end())), tag_hex);

    std::vector<unsigned char> opened;
    BOOST_CHECK(AES256GCMDecrypt(key.data(), nonce.data(), sealed, aad, opened));
    BOOST_CHECK_EQUAL(HexStr(opened), pt_hex);
}
} // namespace

// NIST SP 800-38D / CAVP gcmEncryptExtIV256, the 96-bit-IV cases.
BOOST_AUTO_TEST_CASE(nist_vector_empty_plaintext)
{
    CheckVector(
        "0000000000000000000000000000000000000000000000000000000000000000",
        "000000000000000000000000",
        "", "",
        "",
        "530f8afbc74536b9a963b4f1c4cb738b");
}

BOOST_AUTO_TEST_CASE(nist_vector_one_block)
{
    CheckVector(
        "0000000000000000000000000000000000000000000000000000000000000000",
        "000000000000000000000000",
        "00000000000000000000000000000000", "",
        "cea7403d4d606b6e074ec5d3baf39d18",
        "d0d1c8a799996bf0265b98b5d48ab919");
}

BOOST_AUTO_TEST_CASE(nist_vector_multi_block_no_aad)
{
    CheckVector(
        "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
        "cafebabefacedbaddecaf888",
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c9595680953"
        "2fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
        "",
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3d"
        "a7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
        "b094dac5d93471bdec1a502270e3cc6c");
}

BOOST_AUTO_TEST_CASE(nist_vector_with_aad)
{
    // The AAD case matters even though the courier passes none: getting the
    // length block wrong is invisible without it.
    CheckVector(
        "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
        "cafebabefacedbaddecaf888",
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c9595680953"
        "2fcf0e2449a6b525b16aedf5aa0de657ba637b39",
        "feedfacedeadbeeffeedfacedeadbeefabaddad2",
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3d"
        "a7b08b1056828838c5f61e6393ba7a0abcc9f662",
        "76fc6ece0f4e1768cddf8853bb2d551b");
}

BOOST_AUTO_TEST_CASE(a_tampered_tag_is_refused)
{
    const auto key = ParseHex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const auto nonce = ParseHex("cafebabefacedbaddecaf888");
    const std::vector<unsigned char> pt = ParseHex("00112233445566778899aabbccddeeff");

    auto sealed = AES256GCMEncrypt(key.data(), nonce.data(), pt, {});
    std::vector<unsigned char> opened;
    BOOST_REQUIRE(AES256GCMDecrypt(key.data(), nonce.data(), sealed, {}, opened));

    // Flip one bit of the tag: the message must not open. A GCM failure means
    // the bytes were not written by someone holding the key, and there is
    // nothing in them worth keeping.
    sealed.back() ^= 0x01;
    std::vector<unsigned char> ruined;
    BOOST_CHECK(!AES256GCMDecrypt(key.data(), nonce.data(), sealed, {}, ruined));
    BOOST_CHECK(ruined.empty());

    // ...and so must a flipped bit of the ciphertext.
    sealed.back() ^= 0x01;
    sealed[0] ^= 0x80;
    BOOST_CHECK(!AES256GCMDecrypt(key.data(), nonce.data(), sealed, {}, ruined));

    // A truncated message is refused rather than read past.
    BOOST_CHECK(!AES256GCMDecrypt(key.data(), nonce.data(), {0x00}, {}, ruined));
    BOOST_CHECK(!AES256GCMDecrypt(key.data(), nonce.data(), {}, {}, ruined));
}

BOOST_AUTO_TEST_CASE(aad_is_authenticated_not_merely_carried)
{
    const auto key = ParseHex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const auto nonce = ParseHex("cafebabefacedbaddecaf888");
    const std::vector<unsigned char> pt = ParseHex("00112233445566778899aabbccddeeff");
    const std::vector<unsigned char> aad = ParseHex("feedfacedeadbeef");

    const auto sealed = AES256GCMEncrypt(key.data(), nonce.data(), pt, aad);
    std::vector<unsigned char> opened;
    BOOST_CHECK(AES256GCMDecrypt(key.data(), nonce.data(), sealed, aad, opened));
    BOOST_CHECK_EQUAL(HexStr(opened), HexStr(pt));
    // Opening under DIFFERENT associated data must fail.
    BOOST_CHECK(!AES256GCMDecrypt(key.data(), nonce.data(), sealed, ParseHex("feedfacedeadbeee"), opened));
    BOOST_CHECK(!AES256GCMDecrypt(key.data(), nonce.data(), sealed, {}, opened));
}

BOOST_AUTO_TEST_SUITE_END()
