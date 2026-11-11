// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: the producer's certified-sibling guard (AnchorCertifiedSiblingPending)
// costs the size of the anchor watcher's recovery set, not of the block index.
//
// The guard runs on every producer poll. A root can stay in the recovery set
// long after its recovery window has closed (a branch the watcher un-failed
// that then lost fork choice never reconnects, so nothing removes it), and
// answering "no" for such a root by walking the whole block index under
// cs_main costs every poll a full pass. These tests hold the guard to two
// things: it returns exactly what that walk returns, in every state, and it
// does not walk when the answer can be read off the roots.

#include <anchor.h>
#include <arith_uint256.h>
#include <chain.h>
#include <pos.h>
#include <primitives/block.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <optional>
#include <set>
#include <vector>

namespace {

using StaleSet = std::set<std::pair<uint32_t, uint256>>;

//! The guard with a full walk of the index in place of the reading of the
//! roots, and its snapshot taken as arguments: the reference
//! every state is checked against.
std::optional<uint256> ReferenceSiblingPending(ChainstateManager& chainman, const std::set<uint256>& invalidated,
                                               const StaleSet& stale, const uint256& tip_hash, int child_height)
{
    if (invalidated.empty()) return std::nullopt;
    LOCK(cs_main);
    std::vector<const CBlockIndex*> roots;
    for (const uint256& hash : invalidated) {
        const CBlockIndex* p = chainman.m_blockman.LookupBlockIndex(hash);
        if (!p || p->nHeight > child_height) continue;
        const bool failed = p->nStatus & BLOCK_FAILED_MASK;
        if (failed && !(p->nStatus & BLOCK_FAILED_ANCHOR)) continue;
        if (stale.count({p->m_anchor_height, p->m_anchor_hash})) continue;
        roots.push_back(p);
    }
    if (roots.empty()) return std::nullopt;
    const CBlockIndex* target = nullptr;
    for (const auto& [hash, p] : chainman.m_blockman.m_block_index) {
        if (p->nHeight != child_height || !p->pprev || p->pprev->GetBlockHash() != tip_hash) continue;
        for (const CBlockIndex* root : roots) {
            if (p->GetAncestor(root->nHeight) == root) { target = p; break; }
        }
        if (target) break;
    }
    if (!target) return std::nullopt;
    bool guarded = target->PosCarriesQuorum();
    if (!guarded) {
        for (const auto& [hash, p] : chainman.m_blockman.m_block_index) {
            if (p->nHeight <= child_height || !p->PosCarriesQuorum()) continue;
            if (p->GetAncestor(child_height) == target) { guarded = true; break; }
        }
    }
    if (!guarded) return std::nullopt;
    return target->GetBlockHash();
}

//! A block tree of its own inside the test chainman's index, removed again on
//! destruction. Hashes are a counter in a range no real block reaches.
class FakeTree
{
public:
    explicit FakeTree(ChainstateManager& chainman) : m_chainman(chainman) {}
    ~FakeTree()
    {
        LOCK(cs_main);
        for (CBlockIndex* p : m_blocks) {
            m_chainman.m_blockman.m_block_index.erase(p->GetBlockHash());
            delete p;
        }
    }

    CBlockIndex* Add(CBlockIndex* parent) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        const uint256 hash = ArithToUint256(arith_uint256(++m_counter) << 192);
        CBlockIndex* p = new CBlockIndex();
        auto [it, inserted] = m_chainman.m_blockman.m_block_index.emplace(hash, p);
        assert(inserted);
        p->phashBlock = &it->first;
        p->pprev = parent;
        p->nHeight = parent ? parent->nHeight + 1 : 0;
        p->m_anchor_height = (uint32_t)p->nHeight;
        p->m_anchor_hash = hash;
        p->BuildSkip();
        m_blocks.push_back(p);
        return p;
    }

    CBlockIndex* Chain(CBlockIndex* from, int length) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        for (int i = 0; i < length; ++i) from = Add(from);
        return from;
    }

    const std::vector<CBlockIndex*>& Blocks() const { return m_blocks; }

private:
    ChainstateManager& m_chainman;
    std::vector<CBlockIndex*> m_blocks;
    uint64_t m_counter{0};
};

void Certify(CBlockIndex* p, bool certified)
{
    p->m_pos_cert_known = certified;
    p->m_pos_certified = certified;
}

//! Turns on the three switches the guard requires, for one test.
struct GuardSwitches {
    bool pos{g_con_pos}, anchor{g_con_bitcoin_anchor}, validate{g_validate_anchor};
    GuardSwitches() { g_con_pos = g_con_bitcoin_anchor = g_validate_anchor = true; }
    ~GuardSwitches()
    {
        g_con_pos = pos;
        g_con_bitcoin_anchor = anchor;
        g_validate_anchor = validate;
        SetAnchorRecoveryStateForTesting({}, {});
    }
};

//! The guard's answer, checked against the reference before it is returned.
std::optional<uint256> Pending(ChainstateManager& chainman, const std::set<uint256>& invalidated,
                               const StaleSet& stale, const CBlockIndex* tip, int child_height)
{
    SetAnchorRecoveryStateForTesting(invalidated, stale);
    const uint256 tip_hash = tip->GetBlockHash();
    const auto got = AnchorCertifiedSiblingPending(chainman, tip_hash, child_height);
    const auto want = ReferenceSiblingPending(chainman, invalidated, stale, tip_hash, child_height);
    BOOST_CHECK(got == want);
    return got;
}

template <typename F>
double MicrosPerCall(int calls, F f)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; ++i) f();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(end - start).count() / calls;
}

}  // namespace

BOOST_FIXTURE_TEST_SUITE(anchor_sibling_tests, TestingSetup)

// The states the guard exists for, each named, against the reference.
BOOST_AUTO_TEST_CASE(recovery_windows_and_steady_state)
{
    GuardSwitches on;
    ChainstateManager& chainman = *m_node.chainman;
    FakeTree tree(chainman);
    CBlockIndex *tip, *orphan, *dead, *mid;
    {
        LOCK(cs_main);
        CBlockIndex* genesis = tree.Add(nullptr);
        mid = tree.Chain(genesis, 50);
        tip = tree.Chain(mid, 50);                       // height 100
        dead = tree.Chain(mid, 3);                       // a lost branch at 51..53
        orphan = tree.Add(tip);                          // height 101, child of the tip
    }
    const int child = tip->nHeight + 1;

    // Awaiting the verdict: the orphaned, certified child of the tip is held.
    orphan->nStatus = BLOCK_FAILED_VALID | BLOCK_FAILED_ANCHOR;
    Certify(orphan, true);
    BOOST_CHECK(Pending(chainman, {orphan->GetBlockHash()}, {}, tip, child) == orphan->GetBlockHash());

    // Confirmed off the parent chain: a genuine departure, never held.
    BOOST_CHECK(!Pending(chainman, {orphan->GetBlockHash()},
                         {{orphan->m_anchor_height, orphan->m_anchor_hash}}, tip, child));

    // Failed for another reason (no provenance marker): never held.
    orphan->nStatus = BLOCK_FAILED_VALID;
    BOOST_CHECK(!Pending(chainman, {orphan->GetBlockHash()}, {}, tip, child));

    // Sub-quorum child with a certified descendant: held for the descendant.
    orphan->nStatus = BLOCK_FAILED_VALID | BLOCK_FAILED_ANCHOR;
    Certify(orphan, false);
    BOOST_CHECK(!Pending(chainman, {orphan->GetBlockHash()}, {}, tip, child));
    CBlockIndex* above;
    {
        LOCK(cs_main);
        above = tree.Chain(orphan, 2);
    }
    Certify(above, true);
    BOOST_CHECK(Pending(chainman, {orphan->GetBlockHash()}, {}, tip, child) == orphan->GetBlockHash());

    // Reconnecting: the root is already connected below the tip, the next
    // branch block is a clean child of the tip and still held.
    orphan->nStatus = 0;
    Certify(orphan, true);
    BOOST_CHECK(Pending(chainman, {mid->GetBlockHash()}, {}, tip, child) == orphan->GetBlockHash());

    // Steady state: the only root sits on a lost branch below the tip. It is
    // un-failed and its anchor is canonical, so it passes every filter, yet
    // no child of the tip can descend from it.
    dead->nStatus = 0;
    Certify(dead, true);
    BOOST_CHECK(!Pending(chainman, {dead->GetBlockHash()}, {}, tip, child));

    // A child height that is not the tip's next, and a tip outside the index.
    BOOST_CHECK(!Pending(chainman, {orphan->GetBlockHash()}, {}, tip, child + 1));
    SetAnchorRecoveryStateForTesting({orphan->GetBlockHash()}, {});
    BOOST_CHECK(!AnchorCertifiedSiblingPending(chainman, ArithToUint256(arith_uint256(7)), child));
}

// Random trees, random statuses, random sets and stale caches, random tips:
// the guard and the reference never disagree.
BOOST_AUTO_TEST_CASE(matches_reference_in_random_states)
{
    GuardSwitches on;
    ChainstateManager& chainman = *m_node.chainman;
    FakeTree tree(chainman);
    {
        LOCK(cs_main);
        CBlockIndex* genesis = tree.Add(nullptr);
        tree.Chain(genesis, 60);
        // Branches off random blocks, including off other branches and off
        // the tips (siblings at one height).
        for (int i = 0; i < 80; ++i) {
            const auto& blocks = tree.Blocks();
            CBlockIndex* from = blocks[InsecureRandRange(blocks.size())];
            tree.Chain(from, 1 + InsecureRandRange(6));
        }
    }
    const std::vector<CBlockIndex*> blocks = tree.Blocks();
    int held = 0, roots_seen = 0;
    for (int round = 0; round < 3000; ++round) {
        std::set<uint256> invalidated;
        StaleSet stale;
        for (CBlockIndex* p : blocks) {
            switch (InsecureRandRange(4)) {
            case 0: p->nStatus = BLOCK_FAILED_VALID | BLOCK_FAILED_ANCHOR; break;
            case 1: p->nStatus = BLOCK_FAILED_VALID; break;
            case 2: p->nStatus = BLOCK_FAILED_CHILD; break;
            default: p->nStatus = 0;
            }
            Certify(p, InsecureRandRange(3) == 0);
            if (InsecureRandRange(4) == 0) stale.emplace(p->m_anchor_height, p->m_anchor_hash);
        }
        const int set_size = InsecureRandRange(6);
        for (int i = 0; i < set_size; ++i) invalidated.insert(blocks[InsecureRandRange(blocks.size())]->GetBlockHash());
        if (InsecureRandBool()) invalidated.insert(InsecureRand256());  // gone from the index
        roots_seen += !invalidated.empty();
        const CBlockIndex* tip = blocks[InsecureRandRange(blocks.size())];
        const int child = InsecureRandRange(8) == 0 ? tip->nHeight + (int)InsecureRandRange(3) : tip->nHeight + 1;
        held += Pending(chainman, invalidated, stale, tip, child).has_value();
    }
    // The rounds must reach both answers, or the comparison proves little.
    BOOST_TEST_MESSAGE("random states: " << held << " held of 3000 (" << roots_seen << " with a non-empty set)");
    BOOST_CHECK(held > 0);
    BOOST_CHECK(held < 3000);
}

// A lone root on a lost branch, as on a long-running producer: the answer is
// "not held", and getting it must not cost a walk of the index. Measured
// against the reference, which does walk it.
BOOST_AUTO_TEST_CASE(steady_state_root_does_not_walk_the_index)
{
    GuardSwitches on;
    ChainstateManager& chainman = *m_node.chainman;
    for (const int length : {2000, 20000, 100000}) {
        FakeTree tree(chainman);
        CBlockIndex *tip, *dead;
        {
            LOCK(cs_main);
            CBlockIndex* genesis = tree.Add(nullptr);
            CBlockIndex* fork = tree.Chain(genesis, length / 2);
            tip = tree.Chain(fork, length / 2);
            dead = tree.Chain(fork, 2);
        }
        Certify(dead, true);
        const std::set<uint256> invalidated{dead->GetBlockHash()};
        SetAnchorRecoveryStateForTesting(invalidated, {});
        const uint256 tip_hash = tip->GetBlockHash();
        const int child = tip->nHeight + 1;
        BOOST_CHECK(!AnchorCertifiedSiblingPending(chainman, tip_hash, child));
        BOOST_CHECK(!ReferenceSiblingPending(chainman, invalidated, {}, tip_hash, child));
        // Best of three, so one scheduler hiccup does not decide the ratio.
        double guard = 1e18, reference = 1e18;
        for (int i = 0; i < 3; ++i) {
            guard = std::min(guard, MicrosPerCall(200, [&] { AnchorCertifiedSiblingPending(chainman, tip_hash, child); }));
            reference = std::min(reference, MicrosPerCall(20, [&] { ReferenceSiblingPending(chainman, invalidated, {}, tip_hash, child); }));
        }
        BOOST_TEST_MESSAGE("index of " << length << " blocks: guard " << guard << " us/call, full walk " << reference << " us/call");
        // The walk grows with the index and the guard does not; at this size
        // the gap is three orders of magnitude, so a factor of 20 is a margin
        // that only a walk can fail.
        if (length == 100000) BOOST_CHECK_MESSAGE(guard * 20 < reference,
                                                  "the guard costs " << guard << " us against the walk's " << reference);
    }
}

BOOST_AUTO_TEST_SUITE_END()
