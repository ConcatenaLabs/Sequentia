// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PARENTCHAINTXMODEL_H
#define BITCOIN_QT_PARENTCHAINTXMODEL_H

#include <qt/guiutil.h>

#include <QAbstractTableModel>
#include <QDateTime>
#include <QList>

class WalletModel;

/** The wallet's native-Bitcoin history as table rows shaped exactly like
    TransactionTableModel's, so the two concatenate into one chronological
    list: Bitcoin is another row, never another tab. Receives arrive with the
    parent-chain scan the Overview already runs; sends are the wallet's own
    recorded parent-chain sends, fetched over RPC when the scan lands. */
class ParentChainTxModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit ParentChainTxModel(QObject* parent = nullptr);

    void setWalletModel(WalletModel* model);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

public Q_SLOTS:
    //! Fed by OverviewPage's parent-chain scan; fetches the recorded sends and rebuilds.
    void refresh(const QList<GUIUtil::ParentChainUtxo>& utxos, int parent_height);

private:
    struct Row {
        bool send{false};
        QString txid;
        QString address;
        qint64 amount{0};       // satoshi, negative for sends
        QString fee;            // formatted BTC string, sends only
        qint64 time{0};
        int confirmations{0};   // -1 unknown
    };

    void rebuild(const QList<GUIUtil::ParentChainUtxo>& utxos, const QList<Row>& sends);

    WalletModel* m_wallet_model{nullptr};
    QList<Row> m_rows;
};

#endif // BITCOIN_QT_PARENTCHAINTXMODEL_H
