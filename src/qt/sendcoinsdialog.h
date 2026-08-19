// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SENDCOINSDIALOG_H
#define BITCOIN_QT_SENDCOINSDIALOG_H

#include <qt/walletmodel.h>

#include <QDialog>
#include <QMessageBox>
#include <QString>
#include <QTimer>

class ClientModel;
QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
QT_END_NAMESPACE
class QSpinBox;
class QRadioButton;
class QFrame;
class FeeSelectionWidget;
class PlatformStyle;
class SendCoinsEntry;
class SendCoinsRecipient;
enum class SynchronizationState;
namespace wallet {
struct BlindDetails;
class CCoinControl;
} // namespace wallet

namespace Ui {
    class SendCoinsDialog;
}

QT_BEGIN_NAMESPACE
class QUrl;
QT_END_NAMESPACE

/** Dialog for sending bitcoins */
class SendCoinsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SendCoinsDialog(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~SendCoinsDialog();

    void setClientModel(ClientModel *clientModel);
    void setModel(WalletModel *model);

    /** Set up the tab chain manually, as Qt messes up the tab chain by default in some cases (issue https://bugreports.qt-project.org/browse/QTBUG-10907).
     */
    QWidget *setupTabChain(QWidget *prev);

    void setAddress(const QString &address);
    void pasteEntry(const SendCoinsRecipient &rv);
    bool handlePaymentRequest(const SendCoinsRecipient &recipient);

protected:
    /** Registry asset names arrive after startup; re-label the asset selectors
        each time the page is shown so a held asset never stays a raw hex id. */
    void showEvent(QShowEvent* event) override;

public Q_SLOTS:
    void clear();
    void reject() override;
    void accept() override;
    SendCoinsEntry *addEntry();
    void updateTabsAndLabels();
    void setBalance(const interfaces::WalletBalances& balances);

Q_SIGNALS:
    void coinsSent(const uint256& txid);

private:
    Ui::SendCoinsDialog *ui;
    ClientModel *clientModel;
    WalletModel *model;
    std::unique_ptr<wallet::CCoinControl> m_coin_control;
    std::unique_ptr<WalletModelTransaction> m_current_transaction;
    std::unique_ptr<wallet::BlindDetails> m_current_blind_details;
    bool fNewRecipientAllowed;
    bool fFeeMinimized;
    const PlatformStyle *platformStyle;

    // Copy PSBT to clipboard and offer to save it.
    void presentPSBT(PartiallySignedTransaction& psbt);
    // Process WalletModel::SendCoinsReturn and generate a pair consisting
    // of a message and message flags for use in Q_EMIT message().
    // Additional parameter msgArg can be used via .arg(msgArg).
    void processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg = QString());
    void minimizeFeeSection(bool fMinimize);
    // Format confirmation message
    bool PrepareSendText(QString& question_string, QString& informative_text, QString& detailed_text);
    //! When a recipient chose native Bitcoin, run the parent-chain send path; returns true when it handled the click.
    bool trySendParentBtc();
    /* Sign PSBT using external signer.
     *
     * @param[in,out] psbtx the PSBT to sign
     * @param[in,out] mtx needed to attempt to finalize
     * @param[in,out] complete whether the PSBT is complete (a successfully signed multisig transaction may not be complete)
     *
     * @returns false if any failure occurred, which may include the user rejection of a transaction on the device.
     */
    bool signWithExternalSigner(PartiallySignedTransaction& psbt, CMutableTransaction& mtx, bool& complete);
    void updateFeeMinimizedLabel();
    void updateCoinControlState();

    /** SEQUENTIA any-asset fees: which asset pays and at what rate, in a widget
        shared with the other pages that have to answer the same question. This
        dialog keeps what is genuinely its own -- recipients, subtract-fee-from-
        amount, Replace-By-Fee -- and hands the widget its Recommended/Custom
        controls so they keep their place among the numbers they produce. */
    FeeSelectionWidget* m_fee_widget{nullptr};

    /** Tell the widget which asset this transaction is about, so it can default
        the fee to it while this node accepts a fee in that asset. */
    void updatePreferredFeeAsset();

    // The Bitcoin fee controls, which stand in for the asset fee panel when every
    // recipient is being paid in bitcoin: that panel prices Sequentia assets and
    // has nothing to say about sat/vB on the parent chain.
    QFrame* m_btc_fee_frame{nullptr};
    // Says, while the form is being filled, that Bitcoin and a Sequentia asset
    // cannot travel together -- rather than waiting for Send to refuse.
    QLabel* m_mixed_chain_warning{nullptr};
    QRadioButton* m_btc_fee_recommended{nullptr};
    QRadioButton* m_btc_fee_custom{nullptr};
    QSpinBox* m_btc_fee_spin{nullptr};
    int m_btc_fee_hint{0};          //!< the parent chain's estimate, in sat/vB
    void buildBtcFeeControls();
    void refreshBtcFeeHint();
    int chosenBtcFeeRate() const;   //!< 0 = let the node use its own estimate

    /** vsize of the transaction as currently composed, 0 when it cannot be sized
        yet (no recipient, no amount, or the wallet cannot fund it). Without it
        there is no honest total, only a rate. */
    unsigned int m_tx_vsize{0};
    QTimer* m_size_timer{nullptr};

    /** Size the transaction as composed, so the totals can be real rather than
        assumed. Debounced: it runs coin selection. */
    void refreshTxSize();
private Q_SLOTS:
    void sendButtonClicked(bool checked);
    void on_buttonChooseFee_clicked();
    void on_buttonMinimizeFee_clicked();
    void removeEntry(SendCoinsEntry* entry);
    void useAvailableBalance(SendCoinsEntry* entry);
    void updateDisplayUnit();
    void coinControlFeatureChanged(bool);
    void coinControlButtonClicked();
    void coinControlChangeChecked(int);
    void coinControlChangeEdited(const QString &);
    void coinControlUpdateLabels();
    void coinControlClipboardQuantity();
    void coinControlClipboardAmount();
    void coinControlClipboardFee();
    void coinControlClipboardAfterFee();
    void coinControlClipboardBytes();
    void coinControlClipboardLowOutput();
    void coinControlClipboardChange();
    void updateFeeSectionControls();
    void updateNumberOfBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool headers, SynchronizationState sync_state);
    void updateSmartFeeLabel();

Q_SIGNALS:
    // Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);
};


#define SEND_CONFIRM_DELAY   3

class SendConfirmationDialog : public QMessageBox
{
    Q_OBJECT

public:
    SendConfirmationDialog(const QString& title, const QString& text, const QString& informative_text = "", const QString& detailed_text = "", int secDelay = SEND_CONFIRM_DELAY, bool enable_send = true, bool always_show_unsigned = true, QWidget* parent = nullptr);
    /* Returns QMessageBox::Cancel, QMessageBox::Yes when "Send" is
       clicked and QMessageBox::Save when "Create Unsigned" is clicked. */
    int exec() override;

private Q_SLOTS:
    void countDown();
    void updateButtons();

private:
    QAbstractButton *yesButton;
    QAbstractButton *m_psbt_button;
    QTimer countDownTimer;
    int secDelay;
    QString confirmButtonText{tr("Send")};
    bool m_enable_send;
    QString m_psbt_button_text{tr("Create Unsigned")};
};

#endif // BITCOIN_QT_SENDCOINSDIALOG_H
