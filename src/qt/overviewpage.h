// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OVERVIEWPAGE_H
#define BITCOIN_QT_OVERVIEWPAGE_H

#include <interfaces/wallet.h>

#include <QWidget>
#include <memory>

class ClientModel;
class TransactionFilterProxy;
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
    void showOutOfSyncWarning(bool fShow);

public Q_SLOTS:
    void setBalance(const interfaces::WalletBalances& balances);
    void setPrivacy(bool privacy);

Q_SIGNALS:
    void transactionClicked(const QModelIndex &index);
    void outOfSyncWarningClicked();

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

    // Sequentia network-status panel (Bitcoin anchor + staking / producer)
    QTimer *m_seq_status_timer{nullptr};
    QLabel *m_anchor_label{nullptr};
    QLabel *m_staking_label{nullptr};
    QLabel *m_finality_label{nullptr};
    QLabel *m_btc_label{nullptr};
    bool m_btc_scan_inflight{false};      // guards re-entry of the slow parent-chain scan
    unsigned m_btc_refresh_tick{0};       // throttles the periodic dual-balance refresh

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
};

#endif // BITCOIN_QT_OVERVIEWPAGE_H
