// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_RPC_UTIL_H
#define BITCOIN_WALLET_RPC_UTIL_H

#include <asset.h>

#include <any>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class JSONRPCRequest;
class UniValue;
struct bilingual_str;

namespace wallet {
class CWallet;
class LegacyScriptPubKeyMan;
enum class DatabaseStatus;
struct WalletContext;

extern const std::string HELP_REQUIRING_PASSPHRASE;

/**
 * Figures out what wallet, if any, to use for a JSONRPCRequest.
 *
 * @param[in] request JSONRPCRequest that wishes to access a wallet
 * @return nullptr if no wallet should be used, or a pointer to the CWallet
 */
std::shared_ptr<CWallet> GetWalletForJSONRPCRequest(const JSONRPCRequest& request);
bool GetWalletNameFromJSONRPCRequest(const JSONRPCRequest& request, std::string& wallet_name);

void EnsureWalletIsUnlocked(const CWallet&);
WalletContext& EnsureWalletContext(const std::any& context);
LegacyScriptPubKeyMan& EnsureLegacyScriptPubKeyMan(CWallet& wallet, bool also_create = false);
const LegacyScriptPubKeyMan& EnsureConstLegacyScriptPubKeyMan(const CWallet& wallet);

bool GetAvoidReuseFlag(const CWallet& wallet, const UniValue& param);
bool ParseIncludeWatchonly(const UniValue& include_watchonly, const CWallet& wallet);
std::string LabelFromValue(const UniValue& value);

void HandleWalletError(const std::shared_ptr<CWallet> wallet, DatabaseStatus& status, bilingual_str& error);

/**
 * SEQUENTIA -- settling the fee asset of an RPC-built transaction.
 *
 * ONE RULE: the fee asset must be named explicitly unless the transaction
 * already determines it.
 *
 * On a chain with the open fee market (con_any_asset_fees) a fee may be paid in
 * any asset this node accepts, and NO asset is preferred. The policy asset in
 * particular is not: outside staking eligibility SEQ has exactly the standing of
 * every issued asset, so settling on it whenever a caller says nothing would make
 * it the network's fee currency by default -- the privilege the design does away
 * with. Nothing is defaulted and nothing is inferred from what the transaction
 * happens to send.
 *
 * A transaction determines its fee asset when the value is already implied by
 * what the caller handed in, in either of two ways:
 *
 *   - it carries an explicit FEE OUTPUT, which names the asset the fee is paid
 *     in; or
 *   - the fee is SUBTRACTED FROM an output, so it is taken out of that output's
 *     amount and can only be denominated in that output's asset.
 *
 * Neither is an exception to the rule and neither is a preference the wallet
 * picked: both are the transaction stating the answer. Where the answer is
 * already stated, an explicit fee asset must NOT be given -- it would be a
 * parameter that looks like a choice and is not one. Where the transaction states
 * it twice, the two statements must agree.
 *
 * The functions below are the whole rule, shared by every RPC that builds a
 * transaction, so it cannot drift between them. None of them branches on
 * ::policyAsset: every asset is treated alike.
 */

/** How a transaction states its fee asset, and the clause explaining it that the
 *  error messages read out. */
struct FeeAssetDetermination {
    CAsset asset;
    std::string because;
};

/** Parse an explicit fee-asset argument. Returns nullopt when the caller gave
 *  none (null, absent, or an empty string); throws when the string names no
 *  asset this node knows. */
std::optional<CAsset> ParseFeeAssetArg(const UniValue& arg);

/** The transaction's explicit fee outputs state the fee asset directly. Returns
 *  nullopt when there are none; throws when they disagree, since a transaction
 *  pays its fee in exactly one asset. */
std::optional<FeeAssetDetermination> DeterminedByFeeOutputs(const std::vector<CAsset>& fee_output_assets);

/** Subtracting the fee from an output states it too: the fee comes out of that
 *  output's amount, so it is denominated in that output's asset. Returns nullopt
 *  when no output subtracts the fee; throws when the subtract-from outputs span
 *  several assets. */
std::optional<FeeAssetDetermination> DeterminedBySubtractFee(const std::vector<CAsset>& subtract_from_assets);

/** Fold the two ways a transaction can state its fee asset into one. Agreement
 *  is fine and expected; disagreement is impossible and throws, naming both. */
std::optional<FeeAssetDetermination> CombineFeeAssetDeterminations(const std::optional<FeeAssetDetermination>& a,
                                                                   const std::optional<FeeAssetDetermination>& b);

/**
 * Settle the fee asset under the single rule above:
 *   - the transaction determines it: use that, and refuse an explicit fee asset
 *     alongside it -- agreeing or not -- since there is nothing there to choose;
 *   - it does not: use the caller's explicit fee asset, verbatim;
 *   - it does not and none was given: throw RPC_INVALID_PARAMETER naming
 *     `parameter_name`.
 */
CAsset ResolveFeeAsset(const std::optional<CAsset>& explicit_fee_asset,
                       const std::optional<FeeAssetDetermination>& determined,
                       const std::string& parameter_name);
} //  namespace wallet

#endif // BITCOIN_WALLET_RPC_UTIL_H
