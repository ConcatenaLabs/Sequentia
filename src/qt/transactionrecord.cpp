// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionrecord.h>

#include <chain.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <policy/policy.h>
#include <pos.h>
#include <validation.h>
#include <wallet/ismine.h>

#include <stdint.h>

#include <QDateTime>

using wallet::ISMINE_NO;
using wallet::ISMINE_SPENDABLE;
using wallet::ISMINE_WATCH_ONLY;
using wallet::isminetype;

/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction()
{
    // There are currently no cases where we hide transactions, but
    // we may want to use this in the future for things like RBF.
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const interfaces::WalletTx& wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.time;
    CAmount nCredit = valueFor(wtx.credit, ::policyAsset);
    CAmount nDebit = valueFor(wtx.debit, ::policyAsset);
    CAmount nNet = nCredit - nDebit;
    uint256 hash = wtx.tx->GetHash();
    std::map<std::string, std::string> mapValue = wtx.value_map;

    bool involvesWatchAddress = false;
    isminetype fAllFromMe = ISMINE_SPENDABLE;
    bool any_from_me = false;
    std::set<CAsset> assets_issued_to_me_only;
    if (wtx.is_coinbase) {
        fAllFromMe = ISMINE_NO;
    }
    else
    {
        CAmountMap assets_received_by_me_only;
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++)
        {
            if (wtx.tx->vout[i].IsFee()) {
                continue;
            }
            const CAsset& asset = wtx.txout_assets[i];
            if (assets_received_by_me_only.count(asset) && assets_received_by_me_only.at(asset) < 0) {
                // Already known to be received by not-me
                continue;
            }
            isminetype mine = wtx.txout_address_is_mine[i];
            if (!mine) {
                assets_received_by_me_only[asset] = -1;
            } else {
                assets_received_by_me_only[asset] += wtx.txout_amounts[i];
            }
        }

        any_from_me = false;
        for (size_t i = 0; i < wtx.tx->vin.size(); ++i)
        {
            /* Issuance detection */
            isminetype mine = wtx.txin_is_mine[i];
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllFromMe > mine) fAllFromMe = mine;
            if (mine) any_from_me = true;
            CAmountMap assets;
            assets[wtx.txin_issuance_asset[i]] = wtx.txin_issuance_asset_amount[i];
            assets[wtx.txin_issuance_token[i]] = wtx.txin_issuance_token_amount[i];
            for (const auto& asset : assets) {
                if (!asset.first.IsNull()) {
                    if (assets_received_by_me_only.count(asset.first) == 0) {
                        continue;
                    }
                    if (asset.second == assets_received_by_me_only.at(asset.first)) {
                        // Special case: collapse the chain of issue, send, receive to just an issue
                        assets_issued_to_me_only.insert(asset.first);
                        continue;
                    } else {
                        TransactionRecord sub(hash, nTime);
                        sub.involvesWatchAddress = involvesWatchAddress;
                        sub.asset = asset.first;
                        sub.amount = asset.second;
                        sub.type = TransactionRecord::IssuedAsset;
                        parts.append(sub);
                    }
                }
            }
        }
    }

    if (fAllFromMe || !any_from_me) {
        for(unsigned int i = 0; i < wtx.tx->vout.size(); i++)
        {
            const CTxOut& txout = wtx.tx->vout[i];
            const CAsset& asset = wtx.txout_assets[i];
            if (txout.IsFee()) {
                // explicit fee; ignore
                continue;
            }

            // SEQUENTIA: a proof-of-stake registration funds a canonical staking
            // output (see registerstake / BuildStakeScript). Recognise it so the
            // wallet shows a single "Staking" line for the amount locked, rather
            // than a confusing send/receive pair to an unfamiliar script.
            const bool is_stake = g_con_pos && ParseStakeScript(txout.scriptPubKey).has_value();

            if (fAllFromMe && assets_issued_to_me_only.count(asset) == 0) {
                // Change is only really possible if we're the sender
                // Otherwise, someone just sent bitcoins to a change address, which should be shown

                if (wtx.txout_is_change[i]) {
                    continue;
                }

                //
                // Debit
                //
                TransactionRecord sub(hash, nTime);
                sub.idx = i;
                sub.involvesWatchAddress = involvesWatchAddress;
                sub.amount = -wtx.txout_amounts[i];
                sub.asset = asset;

                if (is_stake)
                {
                    // Staked into a staking output (still yours; time-locked).
                    sub.type = TransactionRecord::Staking;
                }
                else if (!std::get_if<CNoDestination>(&wtx.txout_address[i]))
                {
                    // Sent to Bitcoin Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else
                {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }
                parts.append(sub);
            }

            // A staking output funded by us is already represented by the
            // "Staking" debit above; don't also emit a credit for it (the
            // output is ours but locked), which would net to a puzzling zero.
            if (is_stake && fAllFromMe) {
                continue;
            }

            isminetype mine = wtx.txout_is_mine[i];
            if(mine)
            {
                //
                // Credit
                //

                TransactionRecord sub(hash, nTime);
                sub.idx = i; // vout index
                sub.amount = wtx.txout_amounts[i];
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;
                if (wtx.txout_address_is_mine[i])
                {
                    // Received by Bitcoin Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                    sub.asset = asset;
                }
                else
                {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.address = mapValue["from"];
                    sub.asset = wtx.txout_assets[i];
                }
                if (wtx.is_coinbase)
                {
                    // Generated
                    sub.type = TransactionRecord::Generated;
                    sub.asset = wtx.txout_assets[i];
                }
                if (assets_issued_to_me_only.count(wtx.txout_assets[i])) {
                    sub.type = TransactionRecord::IssuedAsset;
                }

                parts.append(sub);
            }
        }

        if (fAllFromMe) {
            // SEQUENTIA: a fee is what you pay to move a payment, so present it as
            // a sub-entry of that payment rather than a standalone top-level row.
            // Hang the fee(s) under the transaction's primary spend record — the
            // first outgoing payment / stake / issuance, or failing that the first
            // record of this transaction. Only when the transaction has no such
            // record at all (e.g. a pure self-consolidation) does the fee stand on
            // its own, as before.
            int parent_idx = -1;
            for (int p = 0; p < parts.size(); ++p) {
                const TransactionRecord::Type t = parts[p].type;
                if (t == TransactionRecord::SendToAddress || t == TransactionRecord::SendToOther ||
                    t == TransactionRecord::SendToSelf || t == TransactionRecord::Staking ||
                    t == TransactionRecord::IssuedAsset) {
                    parent_idx = p;
                    break;
                }
            }
            if (parent_idx < 0 && !parts.isEmpty()) parent_idx = 0;

            for (const auto& tx_fee : GetFeeMap(*wtx.tx)) {
                if (!tx_fee.second) continue;

                TransactionRecord sub(hash, nTime);
                sub.type = TransactionRecord::Fee;
                sub.asset = tx_fee.first;
                sub.amount = -tx_fee.second;
                sub.involvesWatchAddress = involvesWatchAddress;

                if (parent_idx >= 0) {
                    parts[parent_idx].children.append(sub);
                } else {
                    parts.append(sub);
                }
            }
        }
    }
    else
    {
        //
        // Mixed debit transaction, can't break down payees
        //
        parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet, CAsset()));
        parts.last().involvesWatchAddress = involvesWatchAddress;
    }

    return parts;
}

void TransactionRecord::updateStatus(const interfaces::WalletTxStatus& wtx, const uint256& block_hash, int numBlocks, int64_t block_time)
{
    // Determine transaction status

    // Sort order, unrecorded transactions sort to the top
    int typesort;
    switch (type) {
    case Fee:
        typesort = 0;
        break;
    case IssuedAsset:
        typesort = 1;
        break;
    case SendToAddress:
    case SendToOther:
    case SendToSelf:
    case Staking:
        typesort = 2;
        break;
    case RecvWithAddress:
    case RecvFromOther:
        typesort = 3;
        break;
    default:
        typesort = 10;
    }
    status.sortKey = strprintf("%010d-%01d-%010u-%03d-%d",
        wtx.block_height,
        wtx.is_coinbase ? 1 : 0,
        wtx.time_received,
        idx,
        typesort);
    status.countsForBalance = wtx.is_trusted && !(wtx.blocks_to_maturity > 0);
    status.depth = wtx.depth_in_main_chain;
    status.m_cur_block_hash = block_hash;

    // For generated transactions, determine maturity
    if (type == TransactionRecord::Generated) {
        if (wtx.blocks_to_maturity > 0)
        {
            status.status = TransactionStatus::Immature;

            if (wtx.is_in_main_chain)
            {
                status.matures_in = wtx.blocks_to_maturity;
            }
            else
            {
                status.status = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    else
    {
        if (status.depth < 0)
        {
            status.status = TransactionStatus::Conflicted;
        }
        else if (status.depth == 0)
        {
            status.status = TransactionStatus::Unconfirmed;
            if (wtx.is_abandoned)
                status.status = TransactionStatus::Abandoned;
        }
        else if (status.depth < RecommendedNumConfirmations)
        {
            status.status = TransactionStatus::Confirming;
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    status.needsUpdate = false;
}

bool TransactionRecord::statusUpdateNeeded(const uint256& block_hash) const
{
    assert(!block_hash.IsNull());
    return status.m_cur_block_hash != block_hash || status.needsUpdate;
}

QString TransactionRecord::getTxHash() const
{
    return QString::fromStdString(hash.ToString());
}

int TransactionRecord::getOutputIndex() const
{
    return idx;
}
