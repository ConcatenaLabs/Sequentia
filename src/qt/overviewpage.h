// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OVERVIEWPAGE_H
#define BITCOIN_QT_OVERVIEWPAGE_H

#include <interfaces/wallet.h>
#include <qt/guiutil.h>

#include <QList>
#include <QWidget>
#include <memory>

class ClientModel;
class TransactionFilterProxy;
class ParentChainTxModel;
class TxViewDelegate;
class PlatformStyle;
class WalletModel;

namespace Ui {
    class OverviewPage;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
class QTimer;
class QLabel;
class QPushButton;
class QTableWidget;
QT_END_NAMESPACE

/** Overview ("home") page widget */
class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~OverviewPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    //! The native-Bitcoin history rows this page's parent-chain scan maintains;
    //! shared with the Transactions tab so both views merge the same stream.
    ParentChainTxModel* parentChainTxModel() const { return m_parent_tx_model; }
    void showOutOfSyncWarning(bool fShow);

public Q_SLOTS:
    void setBalance(const interfaces::WalletBalances& balances);
    void setPrivacy(bool privacy);

private:
    // Fill the per-asset balances table from the current balances, in the chosen unit and
    // reference currency. Handles native-first ordering, watch-only rows, empty/optional
    // columns and privacy masking.
    void populateAssetTable(const interfaces::WalletBalances& balances, int unit, const QString& refCur);

    // A cheap fingerprint of what the table currently shows (per-asset display name + reference
    // value + currency + privacy). Used to rebuild only when late-arriving registry labels or
    // price updates would actually change the rendered content — avoids periodic flicker.
    QString assetTableSignature(const interfaces::WalletBalances& balances, const QString& refCur) const;

Q_SIGNALS:
    void transactionClicked(const QModelIndex &index);
    void outOfSyncWarningClicked();
    // Sequentia: a fresh parent-chain scan finished; carries the unspent Bitcoin outputs
    // paying this wallet's addresses (empty when unreachable). The Transactions tab
    // listens to show them, so the scan runs once for the whole wallet view.
    void btcUtxosChanged(const QList<GUIUtil::ParentChainUtxo>& utxos, int parentHeight);

protected:
    void changeEvent(QEvent* e) override;

private:
    Ui::OverviewPage *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;
    interfaces::WalletBalances m_balances;
    bool m_privacy{false};

    // Sequentia: the wallet's whole worth in the chosen reference currency, shown
    // above the per-asset rows (see setBalance). Shrinks to carry the empty-wallet
    // line, so the headline size is kept rather than re-derived from the font.
    QLabel *m_total_value{nullptr};
    qreal m_headline_point_size{0};
    QString m_asset_sig; // last rendered table fingerprint (see assetTableSignature)

    // Sequentia: the per-asset balances table (Asset | Available | Pending | Immature |
    // Value), replacing the old ambiguous amount+id label grid. Built in the constructor,
    // repopulated by setBalance/populateAssetTable. Pending/Immature columns hide when empty.
    QTableWidget *m_asset_table{nullptr};

    // Sequentia network-status panel (Bitcoin anchor + staking / producer)
    QTimer *m_seq_status_timer{nullptr};
    QLabel *m_anchor_label{nullptr};
    QLabel *m_staking_label{nullptr};
    QLabel *m_finality_label{nullptr};
    QLabel *m_btc_label{nullptr};         // scan status/error line; hidden once the balance has a table row
    bool m_btc_scan_inflight{false};      // guards re-entry of the slow parent-chain scan
    unsigned m_btc_refresh_tick{0};       // throttles the periodic dual-balance refresh

    // Last successful parent-chain scan: the tBTC balance rides in the asset table and the
    // headline total, so it is kept here between scans. -1 = no successful scan yet (or the
    // parent became unreachable), which drops the row rather than showing a stale number.
    ParentChainTxModel* m_parent_tx_model{nullptr};
    CAmount m_btc_amount{-1};
    int m_btc_addresses{0};
    int m_btc_parent_height{0};
    QList<GUIUtil::ParentChainUtxo> m_btc_utxos;

    const PlatformStyle* m_platform_style;

    TxViewDelegate *txdelegate;
    std::unique_ptr<TransactionFilterProxy> filter;

private Q_SLOTS:
    void updateDisplayUnit();
    void handleTransactionClicked(const QModelIndex &index);
    void updateAlerts(const QString &warnings);
    void updateWatchOnlyLabels(bool showWatchOnly);
    void setMonospacedFont(bool use_embedded_font);
    void updateSeqStatus();
    void refreshBtcBalance();
    // Lands the worker thread's parent-chain scan on the GUI thread: refreshes the
    // tBTC table row / headline / status line and republishes the utxo list.
    void onBtcScanResult(bool ok, const QString& error_text, CAmount amount, int naddr,
                         int parent_height, const QList<GUIUtil::ParentChainUtxo>& utxos);
};

#endif // BITCOIN_QT_OVERVIEWPAGE_H
