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

#include <asset.h>
#include <consensus/amount.h>
#include <univalue.h>

class ClientModel;
class FeeSelectionWidget;
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
    //! Only for the block signal. A fee is judged from the whitelist, the asset
    //! registry and the price feed, none of which announce themselves; a new block
    //! is the one tick that reliably arrives after they have moved.
    void setClientModel(ClientModel* client_model);

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

    //! Which asset a record pays its fee in and at what rate. The same widget the
    //! Send tab uses: an issuer freezing an address is choosing a fee asset under
    //! exactly the rules a payment is, and the two pages must not be able to drift
    //! apart about what those rules are.
    FeeSelectionWidget* m_fee_widget{nullptr};

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

    //! Assemble a record's output script from a signature. The signature is not
    //! verified here or anywhere on the way to the chain, which is what lets the
    //! same call produce the placeholder the funding pass is priced over.
    QString recordScript(const QString& kind, const QString& asset, const UniValue& target,
                         const UniValue& old_key, const QString& signature, QString& error);

    //! The asset a record transaction pays its fee in.
    CAsset selectedFeeAsset() const;

    //! Price a record transaction without making one, so the fee panel can say
    //! what a freeze will cost before the issuer commits to it. The wallet is
    //! asked to fund a placeholder record and the fee it charges is the answer --
    //! its own figure, over its own dummy-signed size estimate. Silent on failure:
    //! an issuer with nothing to pay with should hear about it from the fee
    //! panel's warning, not from a number going blank.
    void priceRecord();

    //! Hand a record transaction to the wallet to fund: it chooses the coins, the
    //! fee and the change, in the fee asset the page names. `record_script` may be
    //! empty, for an unfreeze, which creates no record. `inputs` are inputs the
    //! transaction must contain, and funding leaves them where they are put --
    //! which is what lets a record's signature cover the first of them. `fee_out`,
    //! when asked for, is the fee the wallet charged -- which is how this page
    //! can quote the price of a record without making one.
    bool fundRecordTransaction(const QString& record_script, const QString& asset,
                               const UniValue& inputs, const UniValue& input_weights,
                               QString& funded_hex, QString& error, CAmount* fee_out = nullptr);

    //! The outpoint a funded transaction's first input spends: the one a record's
    //! admission signature has to cover.
    bool firstInput(const QString& tx_hex, QString& txid, int& vout, QString& error);

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
