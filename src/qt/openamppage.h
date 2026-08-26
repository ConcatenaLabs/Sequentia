// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OPENAMPPAGE_H
#define BITCOIN_QT_OPENAMPPAGE_H

#include <QString>
#include <QWidget>

#include <univalue.h>

class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
QT_END_NAMESPACE

/**
 * Sequentia "OpenAMP" page: be a holder of issuer-governed restricted assets.
 *
 * OpenAMP (doc/sequentia/openamp-design.md) is an issuer's policy server. A
 * restricted asset lives in a 2-of-2 taproot "enclave" -- the holder's key and
 * the issuer's policy key -- so every transfer needs the issuer to co-sign, which
 * is how a transfer restriction is enforced with no consensus rule behind it.
 * None of that server is part of this node: it belongs to whoever issued the
 * asset, and a holder is only ever its counterparty.
 *
 * Being that counterparty takes three things, and this page is all three:
 *
 *  - an ENCLAVE KEY, which is an ordinary key of this wallet;
 *  - an ACCOUNT ID (AID), which the issuer and any platform built on them (SeqPal
 *    among them) identify the holder by, and which is a hash of the key set --
 *    so it is derived here rather than issued to us; and
 *  - a SIGNATURE over each sighash the server hands back for a transfer.
 *
 * Until this page a Core wallet could produce none of them, and restricted assets
 * were reachable only from the web and mobile wallets.
 *
 * WHY THERE IS NO "CONNECT TO SERVER" BUTTON. It would not work. Qt's network
 * stack is built in depends/ with no TLS backend at all (-no-openssl, and the
 * same for the platform ones), so a released sequentia-qt cannot open an https
 * connection, and every policy server worth talking to is https. Rather than
 * re-add OpenSSL to the Qt build for one page -- which is a supply-chain decision
 * about the whole GUI, not a detail of this feature -- the page does the half
 * that is genuinely local and hands the other half to whatever already speaks
 * https: the issuer's own web flow, a platform like SeqPal, or curl.
 *
 * That split is not a workaround so much as the shape an offline signer already
 * has, and it keeps a useful property: the request the server answers is public
 * data, while the key never leaves the wallet. Signing goes through
 * signopenamptransfer, which recomputes every sighash from the transaction it is
 * given and refuses anything that does not match what the server asked for, so a
 * signature made here can authorise nothing but that transaction.
 */
class OpenAmpPage : public QWidget
{
    Q_OBJECT

public:
    explicit OpenAmpPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);

    void setModel(WalletModel* model);

private Q_SLOTS:
    void onNewEnclaveKey();
    void onCopyKey();
    void onCopyAid();
    void onDeriveEnclave();
    void onCopyAddress();
    void onSign();
    void onCopySignatures();

private:
    void setupUi(const PlatformStyle* platformStyle);
    void setStatus(const QString& msg, bool error = false);
    void loadSettings();
    void saveSettings();
    void refreshAccount();

    UniValue callWalletRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    UniValue callNodeRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    std::string walletUri() const;

    //! The x-only public key of a fresh address from this wallet. Same route the
    //! Assets page uses for supervision keys: an ordinary wallet address, so the
    //! wallet can sign for it and a rescan recovers it.
    bool freshEnclaveKey(QString& out, QString& error);

    WalletModel* m_wallet_model{nullptr};

    QString m_enclave_key;
    QString m_aid;

    QLabel* m_key_label{nullptr};
    QPushButton* m_new_key_button{nullptr};
    QPushButton* m_copy_key{nullptr};
    QLabel* m_aid_label{nullptr};
    QPushButton* m_copy_aid{nullptr};

    QLineEdit* m_policy_key{nullptr};
    QLineEdit* m_issuer_key{nullptr};
    QPushButton* m_derive_button{nullptr};
    QLabel* m_address_label{nullptr};
    QPushButton* m_copy_address{nullptr};
    QString m_transfer_leaf;
    QString m_transfer_control;

    QPlainTextEdit* m_sign_input{nullptr};
    QPushButton* m_sign_button{nullptr};
    QPlainTextEdit* m_sign_output{nullptr};
    QPushButton* m_copy_signatures{nullptr};

    QLabel* m_status{nullptr};
};

#endif // BITCOIN_QT_OPENAMPPAGE_H
