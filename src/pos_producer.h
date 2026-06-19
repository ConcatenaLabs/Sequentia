// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: autonomous Proof-of-Stake block producer.
//
// The committee-certification cryptography and the produce-one-block logic are
// driven today by the generateposblock / getposblocktemplate+submitposblock
// RPCs (an external coordinator calls them each slot). This module adds the
// missing piece for a coordinator-free node: a background thread that, when the
// operator supplies one or more staking keys (-posproducerkey), detects its own
// eligibility each round, waits out the slot clock, and assembles, signs, and
// submits a block on its own — Phase 1 of the autonomous gossip-and-sign
// committee (doc/sequentia/proposals/autonomous-committee.md §12). It covers the
// leader-only / single-host cases; the peer-to-peer committee gossip and BLS
// aggregation are later phases.
//
// ProducePosBlock() is the shared produce-one-block core, factored out of
// generateposblock so the RPC and the thread run identical signing logic.

#ifndef BITCOIN_POS_PRODUCER_H
#define BITCOIN_POS_PRODUCER_H

#include <key.h>
#include <uint256.h>
#include <validationinterface.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CChainParams;
class CTxMemPool;
class ChainstateManager;

/** The outcome of producing one PoS block, for RPC result formatting. */
struct PosProduceResult {
    uint256 hash;
    int height{0};
    size_t rank{0};
    int countersignatures{0};
    bool vrf{false};
    uint256 vrf_output;
    uint64_t vrf_slot{0};
};

/** Failure category for ProducePosBlock, so an RPC caller can map it to the
 *  matching JSON-RPC error code while the node-layer helper stays free of any
 *  RPC dependency. */
enum class PosProduceError {
    NONE,        //!< success
    NOT_STAKER,  //!< the key is not a registered staker for this slot
    INVALID_KEY, //!< a supplied key is invalid
    BAD_PARAM,   //!< a caller parameter is out of range (e.g. too many keys)
    MISC,        //!< a recoverable failure (signing, sub-quorum committee, ...)
    INTERNAL,    //!< an internal error (template assembly, block not accepted)
};

/** Assemble, sign, and submit one PoS block extending the active tip, with
 *  `leader_key` as the elected proposer and `committee_keys` as any additional
 *  committee signing keys held locally (empty for leader-only / committee=1).
 *
 *  This is the shared core of generateposblock and the autonomous producer: it
 *  computes the slot seed, the leader's election rank/VRF sortition, the
 *  committee eligibility proofs, the block template, the leader (and committee)
 *  signatures, verifies the proof, and calls ProcessNewBlock. It does NOT wait
 *  for a slot to open — the caller is responsible for slot timing — but the
 *  block's own nTime is advanced to the proposer's slot so it is consensus-valid.
 *
 *  Returns true with `result` filled on success; false with a human-readable
 *  `error` and an `err_kind` category otherwise. */
bool ProducePosBlock(ChainstateManager& chainman, CTxMemPool& mempool,
                     const CChainParams& chainparams, const CKey& leader_key,
                     const std::vector<CKey>& committee_keys,
                     PosProduceResult& result, std::string& error,
                     PosProduceError& err_kind);

/** Background thread that produces PoS blocks autonomously from a set of staking
 *  keys, with no external coordinator. Reacts to new tips via the validation
 *  interface and runs the local slot clock; on each round it elects the
 *  best-ranked of its keys as leader and produces a block once that leader's
 *  slot opens (never faster than one slot interval since the parent — the
 *  paper's soft lower-bound cadence floor). */
class PosProducer final : public CValidationInterface
{
public:
    PosProducer(ChainstateManager& chainman, CTxMemPool& mempool,
                const CChainParams& chainparams, std::vector<CKey> keys);
    ~PosProducer();

    /** Register for tip notifications and start the worker thread. */
    void Start();
    /** Unregister, stop the worker thread, and join it. Idempotent. */
    void Stop();

protected:
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork,
                         bool fInitialDownload) override;

private:
    void ThreadLoop();
    //! Evaluate eligibility for the current tip and, if a slot is due, produce a
    //! block. Returns how long to sleep (ms) before the next evaluation.
    int64_t Step();

    ChainstateManager& m_chainman;
    CTxMemPool& m_mempool;
    const CChainParams& m_chainparams;
    const std::vector<CKey> m_keys;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop{false};
    bool m_wake{false};
    bool m_running{false};
};

#endif // BITCOIN_POS_PRODUCER_H
