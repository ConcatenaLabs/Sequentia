// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SUPERVISIONPAGE_H
#define BITCOIN_QT_SUPERVISIONPAGE_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include <consensus/amount.h>
#include <univalue.h>

class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
QT_END_NAMESPACE

/**
 * Sequentia "Supervision" page: operate the supervised assets this wallet issues.
 *
 * Issuing one is on the Assets page; everything afterwards is here -- freezing an
 * address, lifting a freeze, pausing the asset and resuming it, and rotating either
 * key. Until now those existed only as sequentia-cli invocations, which for a
 * regulated issuer means the response to a court order was a shell session.
 *
 * Every action follows the three steps documented in doc/sequentia/supervised-assets.md:
 * the node says what must be signed and by which key (getsupervisionrecordhash), the
 * key signs it, and the node assembles the result (buildsupervisionrecord,
 * addsupervisionrecordoutput). The page never sees a private key. Where the keys came
 * out of this wallet at issuance it asks the wallet for the signature; where they live
 * in an HSM or behind a FROST quorum it shows the message to sign and takes the
 * signature back by hand. Both paths produce the identical transaction.
 *
 * The page is hidden unless this wallet has a supervised asset to operate: see
 * hasSupervision().
 */
class SupervisionPage : public QWidget
{
    Q_OBJECT

public:
    explicit SupervisionPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);

    void setModel(WalletModel* model);

    //! Whether this wallet has any supervised asset it can act for. False keeps the
    //! tab out of the sidebar entirely.
    bool hasSupervision() const { return !m_assets.isEmpty(); }

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    //! Emitted when this wallet gains (or loses) a supervised asset to operate, so
    //! the sidebar can show or hide the tab.
    void availabilityChanged(bool available);

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onAssetChanged();
    void onFreeze();
    void onUnfreeze();
    void onPause();
    void onRotate();
    void onCheckTarget();
    void onNewKeyFromWallet();

private:
    //! One supervised asset this wallet can act for, as getsupervisedassets reports
    //! it plus what this wallet can do about it.
    struct Asset {
        QString id;
        QString operational_key;     //!< current, which is what consensus checks
        QString recovery_key;
        bool pause_allowed{false};
        bool paused{false};
        int frozen{0};
        bool wallet_has_operational{false};
        bool wallet_has_recovery{false};
    };

    //! An unspent freeze record this wallet created: the output whose existence IS
    //! the freeze, and whose spending lifts it.
    struct Record {
        QString asset;
        QString target_hash;
        QString txid;
        int vout{0};
    };

    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QVector<Asset> m_assets;
    //! Freeze records found for the selected asset, by target hash.
    QHash<QString, Record> m_records;
    //! Wallet transactions already examined for record outputs. A confirmed
    //! transaction never grows one later, so the scan is done once per session and
    //! only the spent/unspent question is asked again.
    QSet<QString> m_scanned_txids;
    QVector<Record> m_known_records;

    QComboBox* m_asset_selector{nullptr};
    QLabel* m_asset_summary{nullptr};
    QLabel* m_keys_summary{nullptr};

    QTableWidget* m_freezes{nullptr};
    QPushButton* m_unfreeze_button{nullptr};

    QLineEdit* m_freeze_target{nullptr};
    QLabel* m_freeze_check{nullptr};
    QCheckBox* m_freeze_private{nullptr};
    QPushButton* m_freeze_button{nullptr};

    QPushButton* m_pause_button{nullptr};
    QLabel* m_pause_hint{nullptr};

    QComboBox* m_rotate_which{nullptr};
    QLineEdit* m_rotate_new_key{nullptr};
    QPushButton* m_rotate_generate{nullptr};
    QPushButton* m_rotate_button{nullptr};

    QLabel* m_status{nullptr};

    //! Run a wallet RPC; returns the result, sets ok=false and a message on error.
    UniValue callWalletRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    std::string walletUri() const;
    void setStatus(const QString& msg, bool error = false);

    //! Re-read the supervised assets this wallet can act for. Cheap enough to run on
    //! every balance change, because the tab's visibility depends on the answer and
    //! nothing else would notice a freshly issued asset.
    void refreshAssets();
    //! The asset the selector names, or nullptr when there is none.
    const Asset* selectedAsset() const;
    //! Whether this wallet can sign with the given x-only key.
    bool walletHoldsKey(const QString& xonly_hex) const;

    //! An unspent, explicit policy-asset output to build a record transaction on.
    //! Explicit because a record transaction is built and signed raw here: a
    //! confidential input would need blinding this path does not do.
    bool fundingOutpoint(QString& txid, int& vout, CAmount& amount, QString& error);

    //! Ask for the BIP340 signature over `sighash` under `key`. Signs with the wallet
    //! when it holds that key; otherwise shows the message and takes a signature back.
    //! Empty when the user cancels or signing fails.
    QString requestSignature(const QString& sighash, const QString& key, const QString& role,
                             const QString& what);

    //! The whole three-step flow for a record that CREATES something: freeze, pause,
    //! or a rotation. Returns the txid, empty on failure or cancellation.
    QString sendRecord(const QString& kind, const QString& asset, const UniValue& target,
                       const UniValue& old_key, bool submit_privately);

    //! Spend a freeze record, which is how a freeze (or a pause) is lifted.
    QString sendUnfreeze(const QString& asset, const Record& record);

    //! Find this wallet's unspent supervision records for `asset`, so a freeze made
    //! here can be lifted here. A record made by a different wallet cannot be found
    //! this way -- nothing on the chain indexes records by asset -- and the page says
    //! so rather than pretending the freeze does not exist.
    void refreshRecords(const QString& asset);

    void scheduleRefresh();
    bool m_refresh_pending{false};
};

#endif // BITCOIN_QT_SUPERVISIONPAGE_H
