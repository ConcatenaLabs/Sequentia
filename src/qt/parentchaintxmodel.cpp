// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/parentchaintxmodel.h>

#include <interfaces/node.h>
#include <qt/bitcoinunits.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>
#include <rpc/util.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <QPointer>
#include <QSet>
#include <QThread>

#include <thread>

ParentChainTxModel::ParentChainTxModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void ParentChainTxModel::setWalletModel(WalletModel* model)
{
    m_wallet_model = model;
}

int ParentChainTxModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_rows.size();
}

int ParentChainTxModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return TransactionTableModel::Value + 1; // the same seven columns the wallet rows have
}

void ParentChainTxModel::refresh(const QList<GUIUtil::ParentChainUtxo>& utxos, int parent_height)
{
    Q_UNUSED(parent_height);
    if (!m_wallet_model) return;

    // The scan already ran (that is what delivered `utxos`); the only thing to fetch is
    // the wallet's recorded parent-chain sends. Off the GUI thread, like the scan itself.
    interfaces::Node* node = &m_wallet_model->node();
    const std::string uri = "/wallet/" + m_wallet_model->getWalletName().toStdString();
    QPointer<ParentChainTxModel> self(this);
    std::thread([self, node, uri, utxos]() {
        QList<Row> sends;
        try {
            UniValue params(UniValue::VARR);
            params.push_back(false); // scan=false: recorded sends only, no second scantxoutset
            UniValue r = node->executeRpc("listbtctransactions", params, uri);
            if (r.isArray()) {
                for (size_t i = 0; i < r.size(); ++i) {
                    const UniValue& o = r[i];
                    if (!o.isObject() || !o.exists("category") || o["category"].getValStr() != "send") continue;
                    Row row;
                    row.send = true;
                    if (o.exists("txid")) row.txid = QString::fromStdString(o["txid"].getValStr());
                    if (o.exists("address")) row.address = QString::fromStdString(o["address"].getValStr());
                    if (o.exists("btc")) {
                        int64_t parsed = 0;
                        if (ParseFixedPoint(o["btc"].getValStr(), 8, &parsed)) row.amount = -parsed;
                    }
                    if (o.exists("fee")) row.fee = QString::fromStdString(o["fee"].getValStr());
                    if (o.exists("time") && o["time"].isNum()) row.time = o["time"].get_int64();
                    row.confirmations = (o.exists("confirmations") && o["confirmations"].isNum()) ? o["confirmations"].get_int() : -1;
                    sends.append(row);
                }
            }
        } catch (...) {
            // No sends readable is an empty list, not an error state worth surfacing here.
        }
        QMetaObject::invokeMethod(qApp, [self, utxos, sends]() {
            if (self) self->rebuild(utxos, sends);
        });
    }).detach();
}

void ParentChainTxModel::rebuild(const QList<GUIUtil::ParentChainUtxo>& utxos, const QList<Row>& sends)
{
    QSet<QString> send_txids;
    for (const Row& s : sends) send_txids.insert(s.txid);

    QList<Row> rows;
    for (const GUIUtil::ParentChainUtxo& u : utxos) {
        // A send's change is part of the send, not a receive of its own.
        if (send_txids.contains(u.txid)) continue;
        Row row;
        row.send = false;
        row.txid = u.txid;
        row.address = u.address;
        int64_t parsed = 0;
        if (ParseFixedPoint(u.amount.toStdString(), 8, &parsed)) row.amount = parsed;
        row.time = u.time;
        row.confirmations = u.confirmations;
        rows.append(row);
    }
    rows.append(sends);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.time > b.time; });

    beginResetModel();
    m_rows = rows;
    endResetModel();
}

QVariant ParentChainTxModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return QVariant();
    const Row& r = m_rows.at(index.row());
    const QDateTime when = QDateTime::fromSecsSinceEpoch(r.time > 0 ? r.time : 0);

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case TransactionTableModel::Status: return QString();
        case TransactionTableModel::Watchonly: return QString();
        case TransactionTableModel::Date: return r.time > 0 ? GUIUtil::dateTimeStr(when) : QString();
        case TransactionTableModel::Type: return r.send ? tr("Sent to") : tr("Received with");
        case TransactionTableModel::ToAddress: return r.address;
        case TransactionTableModel::Amount:
            return QString(BitcoinUnits::format(BitcoinUnits::BTC, r.amount, false, BitcoinUnits::SeparatorStyle::ALWAYS)
                   + QStringLiteral(" ") + GUIUtil::parentBtcTicker());
        case TransactionTableModel::Value:
            return GUIUtil::formatReferenceApproxByLabel(QStringLiteral("BTC"),
                       std::abs(r.amount) / double(COIN) * (r.amount < 0 ? -1.0 : 1.0), GUIUtil::referenceCurrency());
        }
        return QVariant();
    case Qt::EditRole:
        // The proxies sort on EditRole; the date column must sort as a moment, the
        // amount as a number, everything else as its display text.
        switch (index.column()) {
        case TransactionTableModel::Date: return when;
        case TransactionTableModel::Amount: return qint64(r.amount);
        default: return data(index, Qt::DisplayRole);
        }
    case Qt::TextAlignmentRole:
        if (index.column() == TransactionTableModel::Amount || index.column() == TransactionTableModel::Value) {
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        }
        return QVariant();
    case Qt::ToolTipRole:
        return tr("%1 on the Bitcoin parent chain\nTransaction id: %2\nConfirmations: %3")
            .arg(r.send ? tr("Sent") : tr("Received"), r.txid,
                 r.confirmations >= 0 ? QString::number(r.confirmations) : tr("unknown"));
    case TransactionTableModel::TypeRole:
        return r.send ? int(TransactionRecord::SendToAddress) : int(TransactionRecord::RecvWithAddress);
    case TransactionTableModel::DateRole:
        return when;
    case TransactionTableModel::WatchonlyRole:
        return false;
    case TransactionTableModel::LongDescriptionRole:
        return data(index, Qt::ToolTipRole);
    case TransactionTableModel::AddressRole:
        return r.address;
    case TransactionTableModel::LabelRole:
        return QString();
    case TransactionTableModel::AmountRole:
        return qint64(r.amount);
    case TransactionTableModel::TxHashRole:
        return r.txid;
    case TransactionTableModel::ConfirmedRole:
        return r.confirmations > 0;
    case TransactionTableModel::FormattedAmountRole:
        return BitcoinUnits::format(BitcoinUnits::BTC, r.amount, false, BitcoinUnits::SeparatorStyle::ALWAYS);
    case TransactionTableModel::StatusRole:
        return r.confirmations > 0 ? int(TransactionStatus::Confirmed) : int(TransactionStatus::Unconfirmed);
    case TransactionTableModel::AssetRole:
        return GUIUtil::parentBtcTicker();
    case TransactionTableModel::ParentChainRole:
        return true;
    }
    return QVariant();
}

QVariant ParentChainTxModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    Q_UNUSED(section); Q_UNUSED(orientation); Q_UNUSED(role);
    return QVariant(); // the wallet model's headers lead; concatenation keeps the first source's
}
