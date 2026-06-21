// Copyright (c) 2024 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STAKINGPAGE_H
#define BITCOIN_QT_STAKINGPAGE_H

#include <QWidget>

class ClientModel;
class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QSpinBox;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

/** SEQUENTIA: Proof-of-Stake monitoring + staking-output helper page.
 *
 *  Read-only status is derived from the node's PoS RPCs (getposschedule,
 *  getstakerinfo); the stake-script helper wraps getstakescript so a user can
 *  obtain the canonical staking output to fund (and the unbonding lock it
 *  enforces). All RPC calls go through clientModel->node().executeRpc(). */
class StakingPage : public QWidget
{
    Q_OBJECT

public:
    explicit StakingPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);
    ~StakingPage();

    void setClientModel(ClientModel* clientModel);
    void setWalletModel(WalletModel* walletModel);

public Q_SLOTS:
    void refresh();

private Q_SLOTS:
    void generateStakeScript();
    void copyScript();

private:
    const PlatformStyle* m_platform_style;
    ClientModel* m_client_model{nullptr};
    WalletModel* m_wallet_model{nullptr};
    QTimer* m_timer{nullptr};

    // status
    QLabel* m_status{nullptr};
    QLabel* m_height{nullptr};
    QLabel* m_stakers{nullptr};
    QLabel* m_total_weight{nullptr};
    QLabel* m_committee{nullptr};
    QLabel* m_slot{nullptr};
    QTableWidget* m_table{nullptr};

    // stake-script helper
    QLineEdit* m_pubkey_in{nullptr};
    QSpinBox* m_csv_blocks{nullptr};
    QPushButton* m_generate{nullptr};
    QPlainTextEdit* m_script_out{nullptr};
    QLabel* m_script_info{nullptr};
    QPushButton* m_copy{nullptr};

    void setStatusMessage(const QString& msg, bool error);
};

#endif // BITCOIN_QT_STAKINGPAGE_H
