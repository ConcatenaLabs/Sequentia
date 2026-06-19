// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: unit tests for compact committee proposals (BIP152-style), the
// bandwidth-efficient form of a `posproposal` — header + coinbase + the other
// transactions' ids, reconstructed from the receiver's mempool.

#include <consensus/merkle.h>
#include <pos_producer.h>
#include <streams.h>
#include <txmempool.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pos_compact_tests, TestingSetup)

// A block shaped like an unsigned committee proposal: a coinbase plus `n_txs`
// further transactions. The proof and PoW are irrelevant to compaction (only the
// header and the merkle root matter), so they are left default.
static CBlock BuildProposalBlock(int n_txs)
{
    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].scriptSig.resize(10);
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 42;
    block.vtx.push_back(MakeTransactionRef(coinbase));

    for (int i = 0; i < n_txs; ++i) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout.hash = InsecureRand256();
        tx.vin[0].prevout.n = i;
        tx.vout.resize(1);
        tx.vout[0].nValue = 100 + i;
        block.vtx.push_back(MakeTransactionRef(tx));
    }

    bool mutated = false;
    block.hashMerkleRoot = BlockMerkleRoot(block, &mutated);
    BOOST_REQUIRE(!mutated);
    return block;
}

BOOST_AUTO_TEST_CASE(compact_proposal_roundtrip)
{
    CBlock block = BuildProposalBlock(3);
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;
    {
        LOCK2(cs_main, pool.cs);
        for (size_t i = 1; i < block.vtx.size(); ++i) pool.addUnchecked(entry.FromTx(block.vtx[i]));
    }

    // The compact carries the coinbase in full and the other three by id.
    PosCompactProposal compact = MakePosCompactProposal(block);
    BOOST_CHECK(compact.coinbase && compact.coinbase->GetHash() == block.vtx[0]->GetHash());
    BOOST_CHECK_EQUAL(compact.txids.size(), 3U);

    // Wire serialization round-trips.
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << compact;
    PosCompactProposal compact2;
    ss >> compact2;
    BOOST_CHECK(compact2.header.GetHash() == block.GetHash());
    BOOST_CHECK_EQUAL(compact2.txids.size(), 3U);

    // Reconstruction from a mempool that has every transaction yields the block.
    std::shared_ptr<CBlock> rebuilt = ReconstructPosProposal(compact2, pool);
    BOOST_REQUIRE(rebuilt != nullptr);
    BOOST_CHECK(rebuilt->GetHash() == block.GetHash());
    BOOST_CHECK_EQUAL(rebuilt->vtx.size(), block.vtx.size());
    BOOST_CHECK(rebuilt->hashMerkleRoot == block.hashMerkleRoot);
}

BOOST_AUTO_TEST_CASE(compact_proposal_missing_tx_fails)
{
    CBlock block = BuildProposalBlock(3);
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;
    {
        LOCK2(cs_main, pool.cs);
        // Add only two of the three non-coinbase transactions.
        pool.addUnchecked(entry.FromTx(block.vtx[1]));
        pool.addUnchecked(entry.FromTx(block.vtx[2]));
    }

    PosCompactProposal compact = MakePosCompactProposal(block);
    // A missing transaction must make reconstruction fail (caller fetches the full
    // block via getposproposal), never silently produce a wrong block.
    BOOST_CHECK(ReconstructPosProposal(compact, pool) == nullptr);
}

BOOST_AUTO_TEST_CASE(compact_proposal_bad_merkle_fails)
{
    CBlock block = BuildProposalBlock(2);
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;
    {
        LOCK2(cs_main, pool.cs);
        for (size_t i = 1; i < block.vtx.size(); ++i) pool.addUnchecked(entry.FromTx(block.vtx[i]));
    }

    PosCompactProposal compact = MakePosCompactProposal(block);
    // Tamper the committed merkle root: reconstruction must reject it even though
    // every referenced transaction is available, so a forged compact cannot smuggle
    // a different transaction set under a valid header.
    compact.header.hashMerkleRoot = InsecureRand256();
    BOOST_CHECK(ReconstructPosProposal(compact, pool) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
