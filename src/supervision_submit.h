// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SUPERVISION_SUBMIT_H
#define BITCOIN_SUPERVISION_SUBMIT_H

#include <primitives/transaction.h>
#include <sync.h>

#include <string>
#include <vector>

class CBlock;
class CChainState;

/** SEQUENTIA: the issuer-to-producer submission channel for supervision records.
 *
 *  WHY THIS EXISTS. A freeze is systematically front-runnable without it
 *  (Alberto's point 6). The record sits in the public mempool for a block
 *  before it confirms, so a watcher sees the target and moves the funds to a
 *  fresh script before the freeze binds, and every freeze becomes a chase the
 *  issuer loses. The same race exists on Ethereum, where issuers answer it with
 *  private relays; this is the equivalent. Without it the compliance promise
 *  reads well and does not survive contact with anyone paying attention.
 *
 *  WHAT IT IS. A transaction submitted here is validated, held privately, and
 *  included in the next block THIS node produces. It never enters the mempool,
 *  so it is never announced, never relayed, and invisible until it is in a
 *  block. An issuer submits to producers it has arrangements with; the more of
 *  the stake it reaches, the sooner the record lands.
 *
 *  WHAT IT IS NOT. It only accepts transactions carrying supervision records.
 *  That restriction is not decoration: a general private-transaction channel
 *  into block templates is a censorship and ordering lever, and building one by
 *  accident while trying to make freezes work would be a bad trade. The queue
 *  is also not an alternative to consensus -- a submitted transaction is
 *  validated exactly like any other and confers no privilege beyond privacy
 *  before confirmation.
 */
class SupervisionSubmissionQueue
{
private:
    struct Entry {
        CTransactionRef tx;
        //! Height at which it was accepted, for expiry.
        int height{0};
    };

    mutable Mutex m_mutex;
    std::vector<Entry> m_queue GUARDED_BY(m_mutex);

public:
    //! How long an unmined submission is kept. A freeze that has not been mined
    //! in this many blocks is not going to be, and the issuer should resubmit
    //! (probably to more producers) rather than have it linger invisibly.
    static constexpr int EXPIRY_BLOCKS = 20;
    //! A hard cap, because this queue is reachable by anyone with RPC access
    //! and unbounded growth is a memory target.
    static constexpr size_t MAX_ENTRIES = 64;

    static SupervisionSubmissionQueue& GetInstance()
    {
        static SupervisionSubmissionQueue instance;
        return instance;
    }

    void Clear()
    {
        LOCK(m_mutex);
        m_queue.clear();
    }

    //! Accept a validated transaction. The caller has already checked that it
    //! carries a supervision record and that consensus accepts it against the
    //! current tip.
    bool Add(const CTransactionRef& tx, int height, std::string& err);

    //! Everything currently queued, for the block assembler.
    std::vector<CTransactionRef> Entries() const;

    //! Drop what a connected block confirmed, and anything past its expiry.
    void Update(const CBlock& block, int height);

    size_t Size() const
    {
        LOCK(m_mutex);
        return m_queue.size();
    }

    //! Height at which a queued transaction was accepted; -1 if not queued.
    int HeightOf(const uint256& txid) const;
};

/** Whether a transaction is one the submission channel will carry.
 *
 *  Exactly: it must create at least one supervision record. A declaration is
 *  not enough -- issuing an asset is a public act with nothing to front-run --
 *  and neither is an ordinary transaction, however convenient a private path
 *  into a block template would be for its author. */
bool IsSupervisionSubmission(const CTransaction& tx);

#endif // BITCOIN_SUPERVISION_SUBMIT_H
