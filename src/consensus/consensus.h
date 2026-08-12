// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_CONSENSUS_H
#define BITCOIN_CONSENSUS_CONSENSUS_H

#include <stdlib.h>
#include <stdint.h>

/** The maximum allowed size for a serialized block, in bytes (only for buffer size limits) */
static const unsigned int MAX_BLOCK_SERIALIZED_SIZE = 4000000;
/** The maximum allowed weight for a block, see BIP 141 (network rule) */
static const unsigned int MAX_BLOCK_WEIGHT = 4000000;
/** The maximum allowed number of signature check operations in a block (network rule) */
static const int64_t MAX_BLOCK_SIGOPS_COST = 80000;
/** Coinbase transaction outputs can only be spent after this number of new blocks (network rule) */
static const int COINBASE_MATURITY = 100;

/** SEQUENTIA: the coinbase maturity actually in force, and the height it binds
 *  from. Both are mirrored out of Consensus::Params when the chain is selected
 *  (chainparams.cpp), because Consensus::CheckTxInputs takes no params argument
 *  and elements-cli / elements-tx link libbitcoin_common without the node
 *  library -- the same reason g_pos_escape_stall_mtp_height exists.
 *
 *  Why a chain would change it: COINBASE_MATURITY is a number of BLOCKS, so
 *  what it protects drifts with the cadence. Bitcoin's 100 blocks at 600 s is
 *  16 h 40 min; the same 100 blocks on a 60-second chain is 100 minutes, a
 *  tenth of the protection. Sequentia holds the WALL-CLOCK figure equal to
 *  Bitcoin's instead of the block count, which at 60 s means 1,000 blocks.
 *  This matters more here than on Bitcoin: Sequentia has no block subsidy, so
 *  the coinbase carries the producer's fee income rather than new issuance.
 *
 *  0 = use COINBASE_MATURITY (every inherited chain). Raising the maturity
 *  REJECTS MORE, so it needs the height: the running testnet has coinbases
 *  already spent at depths between 100 and 1,000, and applying the new figure
 *  to them would make the chain unsyncable. */
extern int g_coinbase_maturity;
extern int g_coinbase_maturity_height;

/** The coinbase maturity a spend at `spend_height` must satisfy. */
inline int CoinbaseMaturityAt(int spend_height)
{
    return (g_coinbase_maturity > 0 && g_coinbase_maturity_height > 0 &&
            spend_height >= g_coinbase_maturity_height)
               ? g_coinbase_maturity
               : COINBASE_MATURITY;
}

static const int WITNESS_SCALE_FACTOR = 4;

static const size_t MIN_TRANSACTION_WEIGHT = WITNESS_SCALE_FACTOR * 60; // 60 is the lower bound for the size of a valid serialized CTransaction
static const size_t MIN_SERIALIZABLE_TRANSACTION_WEIGHT = WITNESS_SCALE_FACTOR * 10; // 10 is the lower bound for the size of a serialized CTransaction

/** Flags for nSequence and nLockTime locks */
/** Interpret sequence numbers as relative lock-time constraints. */
static constexpr unsigned int LOCKTIME_VERIFY_SEQUENCE = (1 << 0);
/** Use GetMedianTimePast() instead of nTime for end point timestamp. */
static constexpr unsigned int LOCKTIME_MEDIAN_TIME_PAST = (1 << 1);

#endif // BITCOIN_CONSENSUS_CONSENSUS_H
