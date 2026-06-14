// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_AMOUNT_H
#define BITCOIN_CONSENSUS_AMOUNT_H

#include <cstdint>

/** Amount in satoshis (Can be negative) */
typedef int64_t CAmount;

/** The amount of satoshis in one BTC. */
static constexpr CAmount COIN = 100000000;

/** No amount larger than this (in satoshi) is valid.
 *
 * The exact value of MAX_MONEY is consensus-critical (it bounds per-output and
 * per-transaction value sums; an overflow bug here could let coins be created
 * from thin air). It is **per-chain**: each chain sets it in its CChainParams
 * constructor before any consensus check runs (like the other consensus
 * globals), and it never changes thereafter, so it is effectively constant per
 * run while letting different networks use different caps.
 *
 * SEQUENTIA: the Sequentia chains set this to the 400,000,000 SEQ hard cap (at
 * 8 decimals = 4e16 atoms), so the full pre-mined supply can be issued and
 * circulated; it stays far below int64's ~9.2e18 ceiling and real sums are
 * bounded by the supply, so there is no overflow exposure. The inherited
 * Bitcoin/Elements chains keep Bitcoin's 21,000,000-coin cap (2.1e15). Default
 * (defined in tx_check.cpp) is the Bitcoin cap. */
extern CAmount MAX_MONEY;
inline bool MoneyRange(const CAmount& nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }

#endif // BITCOIN_CONSENSUS_AMOUNT_H
