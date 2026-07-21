// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifdef HAVE_CONFIG_H
#include <config/bitcoin-config.h>
#endif

#include <qt/transactiondesc.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/transactionrecord.h>
#include <qt/walletmodel.h>

#include <consensus/consensus.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <policy/policy.h>
#include <util/system.h>
#include <validation.h>
#include <wallet/ismine.h>

#include <stdint.h>
#include <string>

#include <QLatin1String>
#include <QStringList>

using wallet::ISMINE_ALL;
using wallet::ISMINE_SPENDABLE;
using wallet::ISMINE_WATCH_ONLY;
using wallet::isminetype;

namespace {
//! Render every non-zero entry of a per-asset amount map, each at its own
//! asset's precision and name. GUIUtil::formatMultiAssetAmount deliberately
//! hides non-positive entries (right for balances); a transaction detail must
//! keep them, or a movement of any non-policy asset renders as a meaningless
//! policy-asset zero.
QString formatAssetAmountMap(const CAmountMap& map, int unit, bool plussign = false)
{
    QStringList parts;
    for (const auto& entry : map) {
        if (entry.second == 0) continue;
        QString s = GUIUtil::formatAssetAmount(entry.first, entry.second, unit, BitcoinUnits::SeparatorStyle::STANDARD, /*include_asset_name=*/true);
        if (plussign && entry.second > 0) s = "+" + s;
        parts << s.toHtmlEscaped();
    }
    // An all-zero map is just "0": naming any asset here would crown it a
    // default currency, which no asset is.
    if (parts.isEmpty()) return QStringLiteral("0");
    return parts.join(QStringLiteral(", "));
}

bool hasNonZero(const CAmountMap& map)
{
    for (const auto& entry : map) {
        if (entry.second != 0) return true;
    }
    return false;
}

CAmountMap negated(CAmountMap map)
{
    for (auto& entry : map) entry.second = -entry.second;
    return map;
}
} // namespace

QString TransactionDesc::FormatTxStatus(const interfaces::WalletTx& wtx, const interfaces::WalletTxStatus& status, bool inMempool, int numBlocks)
{
    {
        int nDepth = status.depth_in_main_chain;
        if (nDepth < 0) {
            return tr("conflicted with a transaction with %1 confirmations").arg(-nDepth);
        } else if (nDepth == 0) {
            const QString abandoned{status.is_abandoned ? QLatin1String(", ") + tr("abandoned") : QString()};
            return tr("0/unconfirmed, %1").arg(inMempool ? tr("in memory pool") : tr("not in memory pool")) + abandoned;
        } else if (nDepth < 6) {
            return tr("%1/unconfirmed").arg(nDepth);
        } else {
            return tr("%1 confirmations").arg(nDepth);
        }
    }
}

// Takes an encoded PaymentRequest as a string and tries to find the Common Name of the X.509 certificate
// used to sign the PaymentRequest.
bool GetPaymentRequestMerchant(const std::string& pr, QString& merchant)
{
    // Search for the supported pki type strings
    if (pr.find(std::string({0x12, 0x0b}) + "x509+sha256") != std::string::npos || pr.find(std::string({0x12, 0x09}) + "x509+sha1") != std::string::npos) {
        // We want the common name of the Subject of the cert. This should be the second occurrence
        // of the bytes 0x0603550403. The first occurrence of those is the common name of the issuer.
        // After those bytes will be either 0x13 or 0x0C, then length, then either the ascii or utf8
        // string with the common name which is the merchant name
        size_t cn_pos = pr.find({0x06, 0x03, 0x55, 0x04, 0x03});
        if (cn_pos != std::string::npos) {
            cn_pos = pr.find({0x06, 0x03, 0x55, 0x04, 0x03}, cn_pos + 5);
            if (cn_pos != std::string::npos) {
                cn_pos += 5;
                if (pr[cn_pos] == 0x13 || pr[cn_pos] == 0x0c) {
                    cn_pos++; // Consume the type
                    int str_len = pr[cn_pos];
                    cn_pos++; // Consume the string length
                    merchant = QString::fromUtf8(pr.data() + cn_pos, str_len);
                    return true;
                }
            }
        }
    }
    return false;
}

QString TransactionDesc::toHTML(interfaces::Node& node, interfaces::Wallet& wallet, TransactionRecord *rec, int unit)
{
    int numBlocks;
    interfaces::WalletTxStatus status;
    interfaces::WalletOrderForm orderForm;
    bool inMempool;
    interfaces::WalletTx wtx = wallet.getWalletTxDetails(rec->hash, status, orderForm, inMempool, numBlocks);

    QString strHTML;

    strHTML.reserve(4000);
    strHTML += "<html><font face='verdana, arial, helvetica, sans-serif'>";

    int64_t nTime = wtx.time;
    // Amounts are per-asset maps: a transaction can move any mix of assets, and
    // none of them (the policy asset included) is a default to collapse onto.
    const CAmountMap& mapCredit = wtx.credit;
    const CAmountMap& mapDebit = wtx.debit;
    const CAmountMap mapNet = mapCredit - mapDebit;
    // A pure receive: nothing of ours went in, something of ours came out.
    const bool fPureCredit = !hasNonZero(mapDebit) && hasNonZero(mapCredit);

    strHTML += "<b>" + tr("Status") + ":</b> " + FormatTxStatus(wtx, status, inMempool, numBlocks);
    strHTML += "<br>";

    strHTML += "<b>" + tr("Date") + ":</b> " + (nTime ? GUIUtil::dateTimeStr(nTime) : "") + "<br>";

    //
    // From
    //
    if (wtx.is_coinbase)
    {
        strHTML += "<b>" + tr("Source") + ":</b> " + tr("Block reward") + "<br>";
    }
    else if (wtx.value_map.count("from") && !wtx.value_map["from"].empty())
    {
        // Online transaction
        strHTML += "<b>" + tr("From") + ":</b> " + GUIUtil::HtmlEscape(wtx.value_map["from"]) + "<br>";
    }
    else
    {
        // Offline transaction
        if (fPureCredit)
        {
            // Credit
            CTxDestination address = DecodeDestination(rec->address);
            if (IsValidDestination(address)) {
                std::string name;
                isminetype ismine;
                if (wallet.getAddress(address, &name, &ismine, /* purpose= */ nullptr))
                {
                    strHTML += "<b>" + tr("From") + ":</b> " + tr("unknown") + "<br>";
                    strHTML += "<b>" + tr("To") + ":</b> ";
                    strHTML += GUIUtil::HtmlEscape(rec->address);
                    QString addressOwned = ismine == ISMINE_SPENDABLE ? tr("own address") : tr("watch-only");
                    if (!name.empty())
                        strHTML += " (" + addressOwned + ", " + tr("label") + ": " + GUIUtil::HtmlEscape(name) + ")";
                    else
                        strHTML += " (" + addressOwned + ")";
                    strHTML += "<br>";
                }
            }
        }
    }

    //
    // To
    //
    if (wtx.value_map.count("to") && !wtx.value_map["to"].empty())
    {
        // Online transaction
        std::string strAddress = wtx.value_map["to"];
        strHTML += "<b>" + tr("To") + ":</b> ";
        CTxDestination dest = DecodeDestination(strAddress);
        std::string name;
        if (wallet.getAddress(
                dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
            strHTML += GUIUtil::HtmlEscape(name) + " ";
        strHTML += GUIUtil::HtmlEscape(strAddress) + "<br>";
    }

    //
    // Amount
    //
    if (wtx.is_coinbase && !hasNonZero(mapCredit))
    {
        //
        // Coinbase
        //
        CAmountMap mapUnmatured;
        for (size_t nOut = 0; nOut < wtx.tx->vout.size(); nOut++)
            mapUnmatured = mapUnmatured + wallet.getCredit(*(wtx.tx), nOut, ISMINE_ALL);
        strHTML += "<b>" + tr("Credit") + ":</b> ";
        if (status.is_in_main_chain)
            strHTML += formatAssetAmountMap(mapUnmatured, unit) + " (" + tr("matures in %n more block(s)", "", status.blocks_to_maturity) + ")";
        else
            strHTML += "(" + tr("not accepted") + ")";
        strHTML += "<br>";
    }
    else if (fPureCredit)
    {
        //
        // Credit
        //
        strHTML += "<b>" + tr("Credit") + ":</b> " + formatAssetAmountMap(mapNet, unit) + "<br>";
    }
    else
    {
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txout_is_mine)
        {
            if(fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMe)
        {
            if(fAllFromMe & ISMINE_WATCH_ONLY)
                strHTML += "<b>" + tr("From") + ":</b> " + tr("watch-only") + "<br>";

            //
            // Debit
            //
            auto mine = wtx.txout_is_mine.begin();
            for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i)
            {
                const CTxOut& txout = wtx.tx->vout[i];
                // Ignore change
                isminetype toSelf = *(mine++);
                if ((toSelf == ISMINE_SPENDABLE) && (fAllFromMe == ISMINE_SPENDABLE))
                    continue;
                // The explicit fee output has its own "Transaction fee" row below;
                // listing it here as a Debit would double-report it.
                if (txout.IsFee())
                    continue;

                if (!wtx.value_map.count("to") || wtx.value_map["to"].empty())
                {
                    // Offline transaction
                    CTxDestination address;
                    if (ExtractDestination(txout.scriptPubKey, address))
                    {
                        strHTML += "<b>" + tr("To") + ":</b> ";
                        std::string name;
                        if (wallet.getAddress(
                                address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += GUIUtil::HtmlEscape(EncodeDestination(address));
                        if(toSelf == ISMINE_SPENDABLE)
                            strHTML += " (own address)";
                        else if(toSelf & ISMINE_WATCH_ONLY)
                            strHTML += " (watch-only)";
                        strHTML += "<br>";
                    }
                }

                strHTML += "<b>" + tr("Debit") + ":</b> " + GUIUtil::formatAssetAmount(wtx.txout_assets[i], -wtx.txout_amounts[i], unit, BitcoinUnits::SeparatorStyle::STANDARD, /*include_asset_name=*/true).toHtmlEscaped() + "<br>";
                if(toSelf)
                    strHTML += "<b>" + tr("Credit") + ":</b> " + GUIUtil::formatAssetAmount(wtx.txout_assets[i], wtx.txout_amounts[i], unit, BitcoinUnits::SeparatorStyle::STANDARD, /*include_asset_name=*/true).toHtmlEscaped() + "<br>";
            }

            if (fAllToMe)
            {
                // Payment to self
                const CAmountMap mapValue = mapCredit - wtx.change;
                strHTML += "<b>" + tr("Total debit") + ":</b> " + formatAssetAmountMap(negated(mapValue), unit) + "<br>";
                strHTML += "<b>" + tr("Total credit") + ":</b> " + formatAssetAmountMap(mapValue, unit) + "<br>";
            }

            // Sequentia: the fee may be paid in a non-policy asset; read the tx's actual fee
            // asset instead of assuming the policy asset (which dropped the fee row entirely
            // for any-asset-fee transactions).
            const CAsset feeAsset = wtx.tx->GetFeeAsset(::policyAsset);
            CAmount nTxFee = GetFeeMap(*wtx.tx)[feeAsset];
            if (nTxFee > 0) {
                QString feeStr = (feeAsset == ::policyAsset)
                    ? BitcoinUnits::formatHtmlWithUnit(unit, -nTxFee)
                    : GUIUtil::formatAssetAmount(feeAsset, -nTxFee, unit, BitcoinUnits::SeparatorStyle::STANDARD, true).toHtmlEscaped();
                // SEQUENTIA: append the fee valued in the user's reference currency (display only).
                const QString feeRef = GUIUtil::formatReferenceApprox(feeAsset, nTxFee, QString());
                strHTML += "<b>" + tr("Transaction fee") + ":</b> " + feeStr;
                if (!feeRef.isEmpty()) strHTML += " <span style='color:#888'>" + feeRef.toHtmlEscaped() + "</span>";
                strHTML += "<br>";
            }
        }
        else
        {
            //
            // Mixed debit transaction
            //
            auto mine = wtx.txin_is_mine.begin();
            for (const CTxIn& txin : wtx.tx->vin) {
                if (*(mine++)) {
                    strHTML += "<b>" + tr("Debit") + ":</b> " + formatAssetAmountMap(negated(wallet.getDebit(txin, ISMINE_ALL)), unit) + "<br>";
                }
            }
            mine = wtx.txout_is_mine.begin();
            for (size_t nOut = 0; nOut < wtx.tx->vout.size(); nOut++) {
                if (*(mine++)) {
                    strHTML += "<b>" + tr("Credit") + ":</b> " + formatAssetAmountMap(wallet.getCredit(*(wtx.tx), nOut, ISMINE_ALL), unit) + "<br>";
                }
            }
        }
    }

    strHTML += "<b>" + tr("Net amount") + ":</b> " + formatAssetAmountMap(mapNet, unit, /*plussign=*/true) + "<br>";

    //
    // Message
    //
    if (wtx.value_map.count("message") && !wtx.value_map["message"].empty())
        strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["message"], true) + "<br>";
    if (wtx.value_map.count("comment") && !wtx.value_map["comment"].empty())
        strHTML += "<br><b>" + tr("Comment") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["comment"], true) + "<br>";

    strHTML += "<b>" + tr("Transaction ID") + ":</b> " + rec->getTxHash() + "<br>";
    strHTML += "<b>" + tr("Transaction total size") + ":</b> " + QString::number(wtx.tx->GetTotalSize()) + " bytes<br>";
    strHTML += "<b>" + tr("Transaction virtual size") + ":</b> " + QString::number(GetVirtualTransactionSize(*wtx.tx)) + " bytes<br>";
    strHTML += "<b>" + tr("Output index") + ":</b> " + QString::number(rec->getOutputIndex()) + "<br>";

    // Message from normal bitcoin:URI (bitcoin:123...?message=example)
    for (const std::pair<std::string, std::string>& r : orderForm) {
        if (r.first == "Message")
            strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(r.second, true) + "<br>";

        //
        // PaymentRequest info:
        //
        if (r.first == "PaymentRequest")
        {
            QString merchant;
            if (!GetPaymentRequestMerchant(r.second, merchant)) {
                merchant.clear();
            } else {
                merchant += tr(" (Certificate was not verified)");
            }
            if (!merchant.isNull()) {
                strHTML += "<b>" + tr("Merchant") + ":</b> " + GUIUtil::HtmlEscape(merchant) + "<br>";
            }
        }
    }

    if (wtx.is_coinbase)
    {
        quint32 numBlocksToMaturity = COINBASE_MATURITY +  1;
        strHTML += "<br>" + tr("A block reward must mature %1 blocks before it can be spent. When your node produced this block it was broadcast to the network to be added to the chain. If it fails to get into the chain its state will change to \"not accepted\" and it won't be spendable. This may occasionally happen if another node produces a block within a few seconds of yours.").arg(QString::number(numBlocksToMaturity)) + "<br>";
    }

    //
    // Debug view
    //
    if (node.getLogCategories() != BCLog::DEFAULT_LOG_FLAGS && !g_con_elementsmode)
    {
        strHTML += "<hr><br>" + tr("Debug information") + "<br><br>";
        for (const CTxIn& txin : wtx.tx->vin)
            if(wallet.txinIsMine(txin))
                strHTML += "<b>" + tr("Debit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, -valueFor(wallet.getDebit(txin, ISMINE_ALL), ::policyAsset)) + "<br>";
        for (size_t nOut = 0; nOut < wtx.tx->vout.size(); nOut++) {
            const CTxOut& txout = wtx.tx->vout[nOut];
            if(wallet.txoutIsMine(txout)) {
                strHTML += "<b>" + tr("Credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, valueFor(wallet.getCredit(*(wtx.tx), nOut, ISMINE_ALL), ::policyAsset)) + "<br>";
            }
        }

        strHTML += "<br><b>" + tr("Transaction") + ":</b><br>";
        strHTML += GUIUtil::HtmlEscape(wtx.tx->ToString(), true);

        strHTML += "<br><b>" + tr("Inputs") + ":</b>";
        strHTML += "<ul>";

        for (const CTxIn& txin : wtx.tx->vin)
        {
            COutPoint prevout = txin.prevout;

            Coin prev;
            if(node.getUnspentOutput(prevout, prev))
            {
                {
                    strHTML += "<li>";
                    const CTxOut &vout = prev.out;
                    CTxDestination address;
                    if (ExtractDestination(vout.scriptPubKey, address))
                    {
                        std::string name;
                        if (wallet.getAddress(address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += QString::fromStdString(EncodeDestination(address));
                    }
                    strHTML = strHTML + " " + tr("Amount") + "=" + BitcoinUnits::formatHtmlWithUnit(unit, vout.nValue.GetAmount());
                    strHTML = strHTML + " IsMine=" + (wallet.txoutIsMine(vout) & ISMINE_SPENDABLE ? tr("true") : tr("false")) + "</li>";
                    strHTML = strHTML + " IsWatchOnly=" + (wallet.txoutIsMine(vout) & ISMINE_WATCH_ONLY ? tr("true") : tr("false")) + "</li>";
                }
            }
        }

        strHTML += "</ul>";
    }

    strHTML += "</font></html>";
    return strHTML;
}
