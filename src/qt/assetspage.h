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
class QSpinBox;
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
    void onSaveProofFile();

private:
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QTableWidget* m_balances{nullptr};
    QLabel* m_balances_empty{nullptr};
    QTableWidget* m_issuances{nullptr};

    QLineEdit* m_issue_name{nullptr};
    QLineEdit* m_issue_ticker{nullptr};
    QLineEdit* m_issue_domain{nullptr};
    QSpinBox* m_issue_precision{nullptr};
    QLineEdit* m_issue_amount{nullptr};
    QLineEdit* m_issue_tokens{nullptr};
    QCheckBox* m_issue_blind{nullptr};
    QPushButton* m_issue_button{nullptr};
    QLabel* m_issue_result{nullptr};

    QLineEdit* m_reissue_asset{nullptr};
    QLineEdit* m_reissue_amount{nullptr};
    QPushButton* m_reissue_button{nullptr};

    QLabel* m_status{nullptr};

    //! The proof the last issuance needs published, kept so it can be saved to a
    //! file. Empty until an asset is issued in this session.
    QString m_proof_domain;
    QString m_proof_asset;
    QString m_proof_line;
    QPushButton* m_proof_save_button{nullptr};
    QLabel* m_proof_explainer{nullptr};

    //! Run a wallet RPC; returns the result, sets ok=false and a message on error.
    UniValue callWalletRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    std::string walletUri() const;
    void setStatus(const QString& msg, bool error = false);
    //! Ask the user to confirm what issuance permanently commits to; false to abort.
    bool confirmIssuance(const QString& name, const QString& ticker, const QString& domain);
    //! Warn if the issuer domain does not resolve, since a typo cannot be undone.
    bool domainResolves(const QString& domain) const;
};

#endif // BITCOIN_QT_ASSETSPAGE_H
