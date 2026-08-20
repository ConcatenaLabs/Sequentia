// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TRANSACTIONVIEW_H
#define BITCOIN_QT_TRANSACTIONVIEW_H

#include <qt/guiutil.h>

#include <uint256.h>

#include <vector>

#include <QWidget>
#include <QKeyEvent>

class PlatformStyle;
class TransactionFilterProxy;
class ParentChainTxModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QComboBox;
class QDateTimeEdit;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QTableWidget;
class QTreeView;
QT_END_NAMESPACE

/** Widget showing the transaction list for a wallet, including a filter row.
    Using the filter row, the user can view or export a subset of the transactions.
  */
class TransactionView : public QWidget
{
    Q_OBJECT

public:
    explicit TransactionView(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~TransactionView();

    void setModel(WalletModel *model);

    // Date ranges for filter
    enum DateEnum
    {
        All,
        Today,
        ThisWeek,
        ThisMonth,
        LastMonth,
        ThisYear,
        Range
    };

    enum ColumnWidths {
        //! Wide enough for the "Status" header text, not just the state icon.
        STATUS_COLUMN_WIDTH = 60,
        WATCHONLY_COLUMN_WIDTH = 23,
        //! Wide enough for the whole "m/d/yy hh:mm" stamp. At 120 the time
        //! elided to "8/19/26 23..." on every row, which is precisely the part
        //! that shows the list is in chronological order.
        DATE_COLUMN_WIDTH = 150,
        TYPE_COLUMN_WIDTH = 113,
        ADDRESS_COLUMN_WIDTH = 200,
        AMOUNT_MINIMUM_COLUMN_WIDTH = 120,
        VALUE_COLUMN_WIDTH = 120,
        MINIMUM_COLUMN_WIDTH = 23
    };

protected:
    void changeEvent(QEvent* e) override;

private:
    //! Restore or lay out the column header. Must run with a model attached.
    void setupHeader();

    WalletModel *model{nullptr};
    TransactionFilterProxy *transactionProxyModel{nullptr};
    QTreeView *transactionView{nullptr};

    QComboBox *dateWidget;
    QComboBox *typeWidget;
    QComboBox *assetWidget;
    QComboBox *watchOnlyWidget;
    QLineEdit *search_widget;
    QLineEdit *amountWidget;

    QMenu *contextMenu;

    QFrame *dateRangeWidget;
    QDateTimeEdit *dateFrom;
    QDateTimeEdit *dateTo;
    QAction *abandonAction{nullptr};
    QAction *bumpFeeAction{nullptr};
    QAction *speedUpAction{nullptr};
    QAction *replaceAction{nullptr};
    QAction *copyAddressAction{nullptr};
    QAction *copyLabelAction{nullptr};

    QWidget *createDateRangeWidget();

    /** Re-read the assets present in the wallet and rebuild the token filter
     *  drop-down from them. */
    void updateAssetFilterChoices();
    /** Refill the token filter's items from m_assets_present, preserving the
     *  current selection (carried by asset id) where possible. */
    void rebuildAssetFilterItems();
    /** Re-resolve the token filter's labels against the asset registry, which
     *  can name an asset long after its transactions were loaded. Rebuilds only
     *  when something a user would read has actually changed. */
    void refreshAssetFilterNames();

    /** Assets present in the wallet, in drop-down order. Held as assets rather
     *  than as names so the filter keeps working across a registry update. */
    std::vector<CAsset> m_assets_present;

    /** SEQUENTIA: the Bitcoin (parent-chain) section under the transaction list — a
     *  title plus a small table of the unspent outputs the periodic scan found at this
     *  wallet's addresses. These cannot ride inside the Sequentia transaction model:
     *  the wallet has no parent-chain history (the scan sees the UTXO set, so spent
     *  outputs vanish), and their confirmations are Bitcoin's, not Sequentia states.
     *  Hidden entirely while there is nothing to show. */
    ParentChainTxModel* m_parent_chain_model{nullptr};

    bool eventFilter(QObject *obj, QEvent *event) override;

    const PlatformStyle* m_platform_style;

private Q_SLOTS:
    void contextualMenu(const QPoint &);
    void dateRangeChanged();
    void showDetails();
    void copyAddress();
    void editLabel();
    void copyLabel();
    void copyAmount();
    void copyTxID();
    void copyTxHex();
    void copyTxPlainText();
    void openThirdPartyTxUrl(QString url);
    void updateWatchOnlyColumn(bool fHaveWatchOnly);
    void abandonTx();
    void bumpFee(bool checked);
    void speedUp(bool checked);
    void replace(bool checked);

Q_SIGNALS:
    void doubleClicked(const QModelIndex&);

    /**  Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);

    void bumpedFee(const uint256& txid);

public Q_SLOTS:
    /** SEQUENTIA: refresh the Bitcoin (parent-chain) section from a finished scan
     *  (relayed from the Overview page, which owns the scan cycle). */
    //! The shared native-Bitcoin rows (owned by the Overview, which runs the scan);
    //! must be set before setModel so the merged history can be assembled.
    void setParentChainModel(ParentChainTxModel* model);
    void chooseDate(int idx);
    void chooseType(int idx);
    void chooseAsset(int idx);
    void chooseWatchonly(int idx);
    void changedAmount();
    void changedSearch();
    void exportClicked();
    void focusTransaction(const QModelIndex&);
    void focusTransaction(const uint256& txid);
};

#endif // BITCOIN_QT_TRANSACTIONVIEW_H
