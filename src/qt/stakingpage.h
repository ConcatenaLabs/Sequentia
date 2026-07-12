// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STAKINGPAGE_H
#define BITCOIN_QT_STAKINGPAGE_H

#include <QWidget>

#include <univalue.h>

class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QTableWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QShowEvent;
QT_END_NAMESPACE

/**
 * Sequentia "Staking" page: one-click staking. Locks SEQ into a staking output
 * (via the registerstake wallet RPC) and shows the committee/registry status and
 * how to enable block production. All actions go through the node RPCs.
 */
class StakingPage : public QWidget
{
    Q_OBJECT

public:
    explicit StakingPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);
    void setModel(WalletModel* model);

public Q_SLOTS:
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onStake();
    void onEnableProduction();
    void onRefreshClicked();

private:
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QLabel* m_producer_status{nullptr};
    QPushButton* m_enable_button{nullptr};
    QLabel* m_summary{nullptr};
    QTableWidget* m_stakers{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QLineEdit* m_stake_amount{nullptr};
    QPushButton* m_stake_button{nullptr};
    QLabel* m_result{nullptr};
    QLabel* m_status{nullptr};

    //! Run an RPC (wallet=true uses the /wallet/<name> endpoint; false the node endpoint).
    UniValue callRpc(const std::string& method, const UniValue& params, bool& ok, QString& error, bool wallet = true);
    std::string walletUri() const;
    void setStatus(const QString& msg, bool error = false);
    //! Enable autonomous block production at runtime for the given staking WIF(s)
    //! (via startposproducer). No restart. Returns true if the node is now producing.
    bool enableProduction(const QStringList& wifs, QString& err);
    //! Export WIFs for every registered stake this wallet controls (best-effort;
    //! legacy wallets only, like dumpprivkey).
    QStringList walletStakingWifs();
};

#endif // BITCOIN_QT_STAKINGPAGE_H
