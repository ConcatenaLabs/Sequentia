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
    void onSaveContract();
    void onLoadContract();
    void onOpenDomain();
    void onRegister();

private:
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QTableWidget* m_issuances{nullptr};

    QLineEdit* m_issue_name{nullptr};
    QLineEdit* m_issue_ticker{nullptr};
    QLineEdit* m_issue_domain{nullptr};
    QPushButton* m_issue_domain_open{nullptr};
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

    //! What the last issuance needs published and registered, kept so it can be
    //! saved. Empty until an asset is issued in this session.
    //!
    //! m_proof_contract matters most: the chain keeps only the contract's hash,
    //! and the issuer key came from the wallet, so a contract that is not written
    //! down here is gone -- and with it any chance of ever registering the asset.
    QString m_proof_domain;
    QString m_proof_asset;
    QString m_proof_line;
    QString m_proof_contract;
    QPushButton* m_proof_save_button{nullptr};
    QPushButton* m_contract_save_button{nullptr};
    QLabel* m_proof_explainer{nullptr};

    QLineEdit* m_register_asset{nullptr};
    QPushButton* m_register_button{nullptr};
    QPushButton* m_register_contract_button{nullptr};
    //! A contract loaded back from the file onSaveContract wrote, sent along
    //! with the registration. Registering normally needs no contract - the
    //! wallet kept it at issuance - but that record lives in the issuing
    //! wallet's transaction store, so a wallet restored from seed, or a
    //! different wallet altogether, no longer has it. Empty = let the wallet
    //! use its own record.
    QString m_register_contract;

    //! Run a wallet RPC; returns the result, sets ok=false and a message on error.
    UniValue callWalletRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    std::string walletUri() const;
    void setStatus(const QString& msg, bool error = false);
    //! Ask the user to confirm what issuance permanently commits to; false to abort.
    bool confirmIssuance(const QString& name, const QString& ticker, const QString& domain);
    //! Warn if the issuer domain does not resolve, since a typo cannot be undone.
    bool domainResolves(const QString& domain) const;
    //! The domain as it will be committed: no scheme, no path, lower case.
    QString issuerDomain() const;

    //! Kick a refresh onto the next event-loop turn so the tab switch paints
    //! first, never blocking the switch on the wallet RPCs. When force is false
    //! it also skips a re-run that would just redo the current result (same tip,
    //! refreshed a moment ago), keeping rapid tab-flipping instant.
    void scheduleRefresh(bool force);
    bool m_refresh_pending{false};   //!< a deferred refresh is already queued
    int m_last_refresh_blocks{-1};   //!< tip height at the last completed refresh
    qint64 m_last_refresh_ms{0};     //!< wall-clock ms of the last completed refresh
};

#endif // BITCOIN_QT_ASSETSPAGE_H
