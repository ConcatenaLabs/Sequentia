// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/rpc/util.h>

#include <assetsdir.h>
#include <rpc/util.h>
#include <util/translation.h>
#include <util/url.h>
#include <wallet/context.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {
static const std::string WALLET_ENDPOINT_BASE = "/wallet/";
const std::string HELP_REQUIRING_PASSPHRASE{"\nRequires wallet passphrase to be set with walletpassphrase call if wallet is encrypted.\n"};

bool GetAvoidReuseFlag(const CWallet& wallet, const UniValue& param) {
    bool can_avoid_reuse = wallet.IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE);
    bool avoid_reuse = param.isNull() ? can_avoid_reuse : param.get_bool();

    if (avoid_reuse && !can_avoid_reuse) {
        throw JSONRPCError(RPC_WALLET_ERROR, "wallet does not have the \"avoid reuse\" feature enabled");
    }

    return avoid_reuse;
}

/** Used by RPC commands that have an include_watchonly parameter.
 *  We default to true for watchonly wallets if include_watchonly isn't
 *  explicitly set.
 */
bool ParseIncludeWatchonly(const UniValue& include_watchonly, const CWallet& wallet)
{
    if (include_watchonly.isNull()) {
        // if include_watchonly isn't explicitly set, then check if we have a watchonly wallet
        return wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    }

    // otherwise return whatever include_watchonly was set to
    return include_watchonly.get_bool();
}

bool GetWalletNameFromJSONRPCRequest(const JSONRPCRequest& request, std::string& wallet_name)
{
    if (URL_DECODE && request.URI.substr(0, WALLET_ENDPOINT_BASE.size()) == WALLET_ENDPOINT_BASE) {
        // wallet endpoint was used
        wallet_name = URL_DECODE(request.URI.substr(WALLET_ENDPOINT_BASE.size()));
        return true;
    }
    return false;
}

std::shared_ptr<CWallet> GetWalletForJSONRPCRequest(const JSONRPCRequest& request)
{
    CHECK_NONFATAL(request.mode == JSONRPCRequest::EXECUTE);
    WalletContext& context = EnsureWalletContext(request.context);

    std::string wallet_name;
    if (GetWalletNameFromJSONRPCRequest(request, wallet_name)) {
        const std::shared_ptr<CWallet> pwallet = GetWallet(context, wallet_name);
        if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
        return pwallet;
    }

    std::vector<std::shared_ptr<CWallet>> wallets = GetWallets(context);
    if (wallets.size() == 1) {
        return wallets[0];
    }

    if (wallets.empty()) {
        throw JSONRPCError(
            RPC_WALLET_NOT_FOUND, "No wallet is loaded. Load a wallet using loadwallet or create a new one with createwallet. (Note: A default wallet is no longer automatically created)");
    }
    throw JSONRPCError(RPC_WALLET_NOT_SPECIFIED,
        "Wallet file not specified (must request wallet RPC through /wallet/<filename> uri-path).");
}

void EnsureWalletIsUnlocked(const CWallet& wallet)
{
    if (wallet.IsLocked()) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }
}

WalletContext& EnsureWalletContext(const std::any& context)
{
    auto wallet_context = util::AnyPtr<WalletContext>(context);
    if (!wallet_context) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Wallet context not found");
    }
    return *wallet_context;
}

// also_create should only be set to true only when the RPC is expected to add things to a blank wallet and make it no longer blank
LegacyScriptPubKeyMan& EnsureLegacyScriptPubKeyMan(CWallet& wallet, bool also_create)
{
    LegacyScriptPubKeyMan* spk_man = wallet.GetLegacyScriptPubKeyMan();
    if (!spk_man && also_create) {
        spk_man = wallet.GetOrCreateLegacyScriptPubKeyMan();
    }
    if (!spk_man) {
        throw JSONRPCError(RPC_WALLET_ERROR, "This type of wallet does not support this command");
    }
    return *spk_man;
}

const LegacyScriptPubKeyMan& EnsureConstLegacyScriptPubKeyMan(const CWallet& wallet)
{
    const LegacyScriptPubKeyMan* spk_man = wallet.GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        throw JSONRPCError(RPC_WALLET_ERROR, "This type of wallet does not support this command");
    }
    return *spk_man;
}

std::string LabelFromValue(const UniValue& value)
{
    std::string label = value.get_str();
    if (label == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, "Invalid label name");
    return label;
}

void HandleWalletError(const std::shared_ptr<CWallet> wallet, DatabaseStatus& status, bilingual_str& error)
{
    if (!wallet) {
        // Map bad format to not found, since bad format is returned when the
        // wallet directory exists, but doesn't contain a data file.
        RPCErrorCode code = RPC_WALLET_ERROR;
        switch (status) {
            case DatabaseStatus::FAILED_NOT_FOUND:
            case DatabaseStatus::FAILED_BAD_FORMAT:
                code = RPC_WALLET_NOT_FOUND;
                break;
            case DatabaseStatus::FAILED_ALREADY_LOADED:
                code = RPC_WALLET_ALREADY_LOADED;
                break;
            case DatabaseStatus::FAILED_ALREADY_EXISTS:
                code = RPC_WALLET_ALREADY_EXISTS;
                break;
            case DatabaseStatus::FAILED_INVALID_BACKUP_FILE:
                code = RPC_INVALID_PARAMETER;
                break;
            default: // RPC_WALLET_ERROR is returned for all other cases.
                break;
        }
        throw JSONRPCError(code, error.original);
    }
}

/**
 * SEQUENTIA -- the fee asset of an RPC-built transaction. See wallet/rpc/util.h
 * for the rule; these three functions are its only implementation, so no RPC can
 * quietly adopt a different one.
 */
std::optional<CAsset> ParseFeeAssetArg(const UniValue& arg)
{
    if (arg.isNull() || !arg.isStr() || arg.get_str().empty()) return std::nullopt;
    const std::string str = arg.get_str();
    const CAsset asset = GetAssetFromString(str);
    if (asset.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown label and invalid asset hex for fee: %s", str));
    }
    return asset;
}

/** The single asset a set of fee-asset statements agrees on, or nothing. */
static std::optional<CAsset> AgreedAsset(const std::vector<CAsset>& assets, const std::string& disagreement)
{
    std::optional<CAsset> agreed;
    for (const CAsset& asset : assets) {
        if (agreed && *agreed != asset) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "%s (%s and %s): a transaction pays its fee in exactly one asset.",
                disagreement, agreed->GetHex(), asset.GetHex()));
        }
        agreed = asset;
    }
    return agreed;
}

std::optional<FeeAssetDetermination> DeterminedByFeeOutputs(const std::vector<CAsset>& fee_output_assets)
{
    const std::optional<CAsset> asset = AgreedAsset(
        fee_output_assets, "The transaction carries fee outputs of different assets");
    if (!asset) return std::nullopt;
    return FeeAssetDetermination{*asset, strprintf(
        "the transaction carries a fee output denominated in %s, which names the asset the fee is paid in",
        asset->GetHex())};
}

std::optional<FeeAssetDetermination> DeterminedBySubtractFee(const std::vector<CAsset>& subtract_from_assets)
{
    const std::optional<CAsset> asset = AgreedAsset(
        subtract_from_assets, "Cannot subtract the fee from outputs of different assets");
    if (!asset) return std::nullopt;
    return FeeAssetDetermination{*asset, strprintf(
        "the fee is subtracted from an output of asset %s, so it comes out of that output's amount and can only be denominated in it",
        asset->GetHex())};
}

std::optional<FeeAssetDetermination> CombineFeeAssetDeterminations(const std::optional<FeeAssetDetermination>& a,
                                                                   const std::optional<FeeAssetDetermination>& b)
{
    if (!a) return b;
    if (!b) return a;
    // Both hold, which is fine when they agree -- a raw transaction whose fee
    // output is GOLD and whose fee is subtracted from a GOLD output states the
    // same thing twice. When they disagree neither can be satisfied.
    if (a->asset != b->asset) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
            "The transaction determines the fee asset in two ways that disagree: %s; and %s. Both cannot hold.",
            a->because, b->because));
    }
    return a;
}

CAsset ResolveFeeAsset(const std::optional<CAsset>& explicit_fee_asset,
                       const std::optional<FeeAssetDetermination>& determined,
                       const std::string& parameter_name)
{
    if (determined) {
        // The transaction already states the fee asset, so there is nothing left
        // to name. An explicit fee asset here would look like a choice and would
        // not be one: change the transaction and the "chosen" fee asset changes
        // with it. Refused whether or not it agrees, and identically for every
        // asset, so no asset is privileged by which side would win.
        if (explicit_fee_asset) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "The transaction already determines the fee asset (%s): %s. That is not a choice, so %s must be omitted.",
                determined->asset.GetHex(), determined->because, parameter_name));
        }
        return determined->asset;
    }
    if (explicit_fee_asset) return *explicit_fee_asset;

    // Nothing in this transaction determines the fee asset, so it has to be
    // named. Falling back to ::policyAsset here would answer "SEQ" every time a
    // caller stays silent, which is precisely the privilege the open fee market
    // exists to abolish.
    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
        "Nothing in this transaction determines the fee asset, so it must be named: pass %s. This chain has an open fee market -- a fee may be paid in any asset this node accepts, and no asset, the policy asset included, is the default (getfeeexchangerates lists what this node accepts).",
        parameter_name));
}
} // namespace wallet
