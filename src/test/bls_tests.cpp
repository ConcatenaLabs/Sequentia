// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

#ifndef WIN32
#include <sys/mman.h>
#endif

BOOST_FIXTURE_TEST_SUITE(bls_tests, BasicTestingSetup)

static std::vector<unsigned char> Seed(unsigned char b)
{
    return std::vector<unsigned char>(BLS_SK_SIZE, b);
}
static std::vector<unsigned char> Msg(unsigned char b)
{
    return std::vector<unsigned char>(32, b);
}

BOOST_AUTO_TEST_CASE(bls_sign_verify_roundtrip)
{
    const auto sk = Seed(1);
    auto pk = BlsDerivePubKey(sk);
    BOOST_REQUIRE(pk.has_value());
    BOOST_CHECK_EQUAL(pk->size(), BLS_PK_SIZE);

    const auto msg = Msg(0x11);
    auto sig = BlsSign(sk, msg);
    BOOST_REQUIRE(sig.has_value());
    BOOST_CHECK_EQUAL(sig->size(), BLS_SIG_SIZE);

    BOOST_CHECK(BlsVerify(*pk, msg, *sig));
    BOOST_CHECK(!BlsVerify(*pk, Msg(0x12), *sig));        // wrong message
    BOOST_CHECK(!BlsVerify(*BlsDerivePubKey(Seed(2)), msg, *sig)); // wrong key

    // Signing is deterministic (same key + message -> same signature).
    BOOST_CHECK(*BlsSign(sk, msg) == *sig);
}

BOOST_AUTO_TEST_CASE(bls_fast_aggregate_verify)
{
    const size_t n = 5;
    const auto msg = Msg(0x42);
    std::vector<std::vector<unsigned char>> pks, sigs;
    for (size_t i = 0; i < n; ++i) {
        const auto sk = Seed((unsigned char)(i + 1));
        pks.push_back(*BlsDerivePubKey(sk));
        sigs.push_back(*BlsSign(sk, msg));
    }

    // Full aggregate verifies against all pubkeys.
    auto agg = BlsAggregate(sigs);
    BOOST_REQUIRE(agg.has_value());
    BOOST_CHECK_EQUAL(agg->size(), BLS_SIG_SIZE);
    BOOST_CHECK(BlsFastAggregateVerify(pks, msg, *agg));

    // The gossip property: an aggregate of an arbitrary SUBSET of shares
    // verifies against exactly that subset's pubkeys — no pre-committed signer
    // set, "collect whoever responds".
    std::vector<std::vector<unsigned char>> sub_sigs{sigs[0], sigs[2], sigs[4]};
    std::vector<std::vector<unsigned char>> sub_pks{pks[0], pks[2], pks[4]};
    auto sub_agg = BlsAggregate(sub_sigs);
    BOOST_REQUIRE(sub_agg.has_value());
    BOOST_CHECK(BlsFastAggregateVerify(sub_pks, msg, *sub_agg));

    // ...and that subset aggregate does NOT verify against the full key set,
    // nor against the subset for a different message.
    BOOST_CHECK(!BlsFastAggregateVerify(pks, msg, *sub_agg));
    BOOST_CHECK(!BlsFastAggregateVerify(sub_pks, Msg(0x43), *sub_agg));

    // Degenerate inputs are rejected, not crashed.
    BOOST_CHECK(!BlsAggregate({}).has_value());
    BOOST_CHECK(!BlsFastAggregateVerify({}, msg, *agg));
}

BOOST_AUTO_TEST_CASE(bls_proof_of_possession)
{
    const auto sk = Seed(7);
    const auto pk = *BlsDerivePubKey(sk);

    auto pop = BlsProvePossession(sk);
    BOOST_REQUIRE(pop.has_value());
    BOOST_CHECK_EQUAL(pop->size(), BLS_SIG_SIZE);
    BOOST_CHECK(BlsVerifyPossession(pk, *pop));

    // A PoP for one key does not validate another.
    BOOST_CHECK(!BlsVerifyPossession(*BlsDerivePubKey(Seed(8)), *pop));

    // A PoP is domain-separated from an ordinary signature: a signature over the
    // pubkey under the signing ciphersuite is not a valid PoP.
    auto plain = BlsSign(sk, std::vector<unsigned char>(pk.begin(), pk.begin() + 32));
    BOOST_REQUIRE(plain.has_value());
    BOOST_CHECK(!BlsVerifyPossession(pk, *plain));
}

BOOST_AUTO_TEST_CASE(bls_bad_encodings)
{
    const auto sk = Seed(3);
    const auto pk = *BlsDerivePubKey(sk);
    const auto msg = Msg(0x55);
    const auto sig = *BlsSign(sk, msg);

    // Wrong lengths are rejected.
    BOOST_CHECK(!BlsVerify(std::vector<unsigned char>(10), msg, sig));
    BOOST_CHECK(!BlsVerify(pk, msg, std::vector<unsigned char>(10)));
    BOOST_CHECK(!BlsSign(sk, std::vector<unsigned char>(31)).has_value());
    BOOST_CHECK(!BlsDerivePubKey(std::vector<unsigned char>(16)).has_value());

    // Right length but invalid points are rejected, not crashed.
    BOOST_CHECK(!BlsVerify(std::vector<unsigned char>(BLS_PK_SIZE, 0xff), msg, sig));
    BOOST_CHECK(!BlsVerify(pk, msg, std::vector<unsigned char>(BLS_SIG_SIZE, 0xff)));
}

#ifndef WIN32
//! A copy of `in` placed flush against an unreadable guard page, so that reading
//! even one byte past its end faults instead of silently hitting heap slack.
class GuardedBuffer
{
    static constexpr size_t PAGE{4096};
    unsigned char* m_base{nullptr};
    size_t m_size;

public:
    explicit GuardedBuffer(const std::vector<unsigned char>& in) : m_size(in.size())
    {
        void* base = mmap(nullptr, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        BOOST_REQUIRE(base != MAP_FAILED);
        m_base = static_cast<unsigned char*>(base);
        BOOST_REQUIRE(mprotect(m_base + PAGE, PAGE, PROT_NONE) == 0);
        std::copy(in.begin(), in.end(), data());
    }
    ~GuardedBuffer() { munmap(m_base, 2 * PAGE); }
    unsigned char* data() const { return m_base + PAGE - m_size; }
    Span<const unsigned char> span() const { return {data(), m_size}; }
};

// Audit D6: blst picks the point encoding from the FIRST byte of its input. A
// first byte without the compression flag (0x80) means "uncompressed", which is
// 96 bytes for G1 and 192 for G2 — twice what every BLS buffer here holds. The
// wrappers must reject such input by its length, never read past the buffer.
BOOST_AUTO_TEST_CASE(bls_uncompressed_flag_never_reads_past_buffer)
{
    const auto sk = Seed(4);
    const auto pk = *BlsDerivePubKey(sk);
    const auto msg = Msg(0x66);
    const auto sig = *BlsSign(sk, msg);
    const auto pop = *BlsProvePossession(sk);

    // 0x00: uncompressed; 0x40: uncompressed infinity (also a full-width read).
    for (const unsigned char first : {0x00, 0x01, 0x40}) {
        std::vector<unsigned char> bad_pk(BLS_PK_SIZE, 0);
        std::vector<unsigned char> bad_sig(BLS_SIG_SIZE, 0);
        bad_pk[0] = first;
        bad_sig[0] = first;
        const GuardedBuffer gpk(bad_pk), gsig(bad_sig);

        BOOST_CHECK(!BlsVerify(gpk.span(), msg, sig));
        BOOST_CHECK(!BlsVerify(pk, msg, gsig.span()));
        BOOST_CHECK(!BlsVerifyPossession(gpk.span(), pop));
        BOOST_CHECK(!BlsVerifyPossession(pk, gsig.span()));
        BOOST_CHECK(!BlsFastAggregateVerify({pk}, msg, gsig.span()));

        // The aggregators take owned vectors; heap slack usually hides an
        // overread there, so this checks only that they reject the input.
        BOOST_CHECK(!BlsFastAggregateVerify({bad_pk}, msg, sig));
        BOOST_CHECK(!BlsFastAggregateVerify({pk, bad_pk}, msg, sig));
        BOOST_CHECK(!BlsAggregatePublicKeys({bad_pk}).has_value());
        BOOST_CHECK(!BlsAggregatePublicKeys({pk, bad_pk}).has_value());
        BOOST_CHECK(!BlsAggregate({bad_sig}).has_value());
        BOOST_CHECK(!BlsAggregate({sig, bad_sig}).has_value());
    }
}
#endif // WIN32

BOOST_AUTO_TEST_SUITE_END()
