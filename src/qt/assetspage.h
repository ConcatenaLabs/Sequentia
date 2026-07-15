// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_ASSETSPAGE_H
#define BITCOIN_QT_ASSETSPAGE_H

#include <QWidget>

#include <univalue.h>

class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QTableWidget;
class QLineEdit;
class QCheckBox;
class QLabel;
class QPushButton;
class QShowEvent;
QT_END_NAMESPACE

/**
 * Sequentia "Assets" page: issue new assets, reissue (mint more) existing ones,
 * list your issuances, and see your per-asset balances. Sending an asset is done
 * from the asset-aware Send page. All actions go through the node's wallet RPCs
 * via interfaces::Node::executeRpc.
 */
class AssetsPage : public QWidget
{
    Q_OBJECT

public:
    explicit AssetsPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);

    void setModel(WalletModel* model);

public Q_SLOTS:
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onIssue();
    void onReissue();

private:
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QTableWidget* m_balances{nullptr};
    QLabel* m_balances_empty{nullptr};
    QTableWidget* m_issuances{nullptr};

    QLineEdit* m_issue_amount{nullptr};
    QLineEdit* m_issue_tokens{nullptr};
    QCheckBox* m_issue_blind{nullptr};
    QPushButton* m_issue_button{nullptr};
    QLabel* m_issue_result{nullptr};

    QLineEdit* m_reissue_asset{nullptr};
    QLineEdit* m_reissue_amount{nullptr};
    QPushButton* m_reissue_button{nullptr};

    QLabel* m_status{nullptr};

    //! Run a wallet RPC; returns the result, sets ok=false and a message on error.
    UniValue callWalletRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    std::string walletUri() const;
    void setStatus(const QString& msg, bool error = false);
};

#endif // BITCOIN_QT_ASSETSPAGE_H
