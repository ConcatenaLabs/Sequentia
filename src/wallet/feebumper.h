// Copyright (c) 2017-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_FEEBUMPER_H
#define BITCOIN_WALLET_FEEBUMPER_H

#include <primitives/transaction.h>
// BumpRefusedReason returns a bilingual_str by value, so the complete type is
// needed here rather than the forward declaration this header used to carry.
#include <util/translation.h>

class uint256;
enum class FeeEstimateMode;

namespace wallet {
class CCoinControl;
class CWallet;
class CWalletTx;

namespace feebumper {

enum class Result
{
    OK,
    INVALID_ADDRESS_OR_KEY,
    INVALID_REQUEST,
    INVALID_PARAMETER,
    WALLET_ERROR,
    MISC_ERROR,
};

//! Return whether transaction can be bumped.
bool TransactionCanBeBumped(const CWallet& wallet, const uint256& txid);

//! SEQUENTIA: why a fee bump is refused, decided before the user is asked to
//! choose a fee. Empty when nothing refuses it.
//!
//! A caller that only greys out a menu entry leaves the user with a function
//! that has silently disappeared, which on a wallet holding confidential coins
//! is every fee bump it will ever offer. The reason is worth carrying.
bilingual_str BumpRefusedReason(const CWallet& wallet, const uint256& txid);

//! SEQUENTIA: whether this transaction can be REPLACED by one with different
//! outputs. The same preconditions as a bump, minus the requirement that the
//! original be unblinded: a replacement is built from scratch and may be
//! confidential, whereas a bump rewrites the original's amounts in the clear and
//! therefore cannot be applied to a confidential transaction.
bool TransactionCanBeReplaced(const CWallet& wallet, const uint256& txid);

//! Create bumpfee transaction based on feerate estimates.
Result CreateRateBumpTransaction(CWallet& wallet,
    const uint256& txid,
    const CCoinControl& coin_control,
    std::vector<bilingual_str>& errors,
    CAmount& old_fee,
    CAmount& new_fee,
    CMutableTransaction& mtx);

//! Sign the new transaction,
//! @return false if the tx couldn't be found or if it was
//! impossible to create the signature(s)
bool SignTransaction(CWallet& wallet, CMutableTransaction& mtx);

//! Commit the bumpfee transaction.
//! @return success in case of CWallet::CommitTransaction was successful,
//! but sets errors if the tx could not be added to the mempool (will try later)
//! or if the old transaction could not be marked as replaced.
Result CommitTransaction(CWallet& wallet,
    const uint256& txid,
    CMutableTransaction&& mtx,
    std::vector<bilingual_str>& errors,
    uint256& bumped_txid);

} // namespace feebumper
} // namespace wallet

#endif // BITCOIN_WALLET_FEEBUMPER_H
