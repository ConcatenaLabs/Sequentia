// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <supervision_submit.h>

#include <logging.h>
#include <primitives/block.h>
#include <supervision.h>
#include <tinyformat.h>

bool IsSupervisionSubmission(const CTransaction& tx)
{
    for (const CTxOut& out : tx.vout) {
        if (ParseSupervisionRecordScript(out.scriptPubKey)) return true;
    }
    return false;
}

bool SupervisionSubmissionQueue::Add(const CTransactionRef& tx, int height, std::string& err)
{
    LOCK(m_mutex);
    if (m_queue.size() >= MAX_ENTRIES) {
        err = strprintf("submission queue is full (%u entries)", (unsigned)MAX_ENTRIES);
        return false;
    }
    for (const Entry& entry : m_queue) {
        if (entry.tx->GetHash() == tx->GetHash()) {
            err = "already submitted";
            return false;
        }
    }
    m_queue.push_back(Entry{tx, height});
    LogPrintf("Supervision: accepted private submission %s at height %d\n",
              tx->GetHash().ToString(), height);
    return true;
}

std::vector<CTransactionRef> SupervisionSubmissionQueue::Entries() const
{
    LOCK(m_mutex);
    std::vector<CTransactionRef> out;
    out.reserve(m_queue.size());
    for (const Entry& entry : m_queue) out.push_back(entry.tx);
    return out;
}

int SupervisionSubmissionQueue::HeightOf(const uint256& txid) const
{
    LOCK(m_mutex);
    for (const Entry& entry : m_queue) {
        if (entry.tx->GetHash() == txid) return entry.height;
    }
    return -1;
}

void SupervisionSubmissionQueue::Update(const CBlock& block, int height)
{
    LOCK(m_mutex);
    if (m_queue.empty()) return;

    std::set<uint256> mined;
    for (const CTransactionRef& tx : block.vtx) mined.insert(tx->GetHash());

    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (mined.count(it->tx->GetHash())) {
            LogPrintf("Supervision: private submission %s confirmed\n",
                      it->tx->GetHash().ToString());
            it = m_queue.erase(it);
        } else if (height - it->height >= EXPIRY_BLOCKS) {
            // Said out loud rather than dropped quietly: an issuer whose freeze
            // never landed needs to know, and the silent case is exactly the
            // one where a compliance obligation is quietly not being met.
            LogPrintf("Supervision: private submission %s expired unmined after %d blocks; " /* Continued */
                      "resubmit, and to more producers\n",
                      it->tx->GetHash().ToString(), EXPIRY_BLOCKS);
            it = m_queue.erase(it);
        } else {
            ++it;
        }
    }
}
