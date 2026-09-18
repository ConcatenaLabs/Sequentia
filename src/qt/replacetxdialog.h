// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_REPLACETXDIALOG_H
#define BITCOIN_QT_REPLACETXDIALOG_H

#include <asset.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <QDialog>
#include <QList>
#include <QString>

#include <cstdint>

class WalletModel;

QT_BEGIN_NAMESPACE
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QTimer;
QT_END_NAMESPACE

/**
 * SEQUENTIA: replace one still-unconfirmed transaction with another that pays a
 * better fee (BIP125).
 *
 * The mempool's test is about fees and inputs, not about outputs: a replacement
 * may legally pay a different recipient, a different amount, even a different
 * asset. That freedom is the whole point of this window as opposed to "Increase
 * transaction fee", which re-sends the same payment untouched — but it is not
 * what anyone opening it normally wants, so recipient, amount and asset all
 * start as the original's and say so, and any departure from them is called out
 * rather than silently accepted.
 *
 * What the window has to answer is "will this one get in": paying more than the
 * stuck transaction is NOT sufficient, since the node also demands an increment
 * proportional to the replacement's own size. So the fee is shown against the
 * original's — rate and total, in the paying asset and in the reference currency
 * — and the figure the node will actually test is stated outright.
 */
class ReplaceTxDialog : public QDialog
{
    Q_OBJECT

public:
    //! Bump keeps the payment and changes only the fee (the menu's "Increase
    //! transaction fee"); Replace also re-addresses it. The window is the same
    //! because the question is the same -- will this one get in, and at what
    //! cost -- and only the payment fields differ, which is exactly what made
    //! having two unrelated windows a bad trade: one let you set the fee and
    //! forced you to retype the payment, the other kept the payment and set the
    //! fee for you.
    enum class Mode { Replace, Bump };

    ReplaceTxDialog(WalletModel* model, const uint256& hash, Mode mode = Mode::Replace, QWidget* parent = nullptr);

    //! Meaningful once exec() has returned Accepted.
    QString address() const;
    CAsset sendAsset() const;
    CAmount amount() const;      //!< atoms of sendAsset()
    CAsset feeAsset() const;
    //! The fee rate the user settled on, in reference fee atoms per kvB — the
    //! unit CCoinControl::m_feerate carries. Deliberately not a unit the window
    //! ever shows: what it shows is an amount of an asset, and its worth.
    CAmount referencePerKvb() const { return m_reference_per_kvb; }

private Q_SLOTS:
    void onRecipientEdited();
    void onFeeAssetChanged();
    void onFeeCellEdited();
    void recompute();

private:
    void buildUi();
    bool loadOriginal();
    CAsset selectedSendAsset() const;
    CAsset selectedFeeAsset() const;
    bool parsedAmount(CAmount& out) const;
    //! Reference fee atoms -> atoms of `asset`, through this node's own fee
    //! whitelist: the same conversion the mempool will apply.
    CAmount toAsset(CAmount reference_atoms, const CAsset& asset) const;

    WalletModel* const m_model;
    const uint256 m_hash;
    const Mode m_mode;
    //! The payment fields, hidden in Bump mode where the payment is not the
    //! subject.
    QList<QWidget*> m_payment_widgets;

    // The transaction being replaced.
    CTransactionRef m_orig;
    CAsset m_orig_fee_asset;
    CAmount m_orig_fee_atoms{0};
    CAmount m_orig_fee_reference{0};
    int64_t m_orig_vsize{0};
    QString m_orig_address;
    CAsset m_orig_asset;
    CAmount m_orig_amount{0};
    int m_orig_recipients{0};
    //! The transaction being replaced has dropped out of this node's mempool: it
    //! was not mined and not abandoned, it simply is not there any more, so the
    //! coins it holds are behind something that no longer exists.
    bool m_orig_out_of_mempool{false};
    //! An input of the transaction being replaced whose own transaction has not
    //! confirmed. While that is true the coin being spent does not exist for the
    //! network -- peers answer "missing-inputs" -- so no fee set here changes
    //! anything, and the transaction that needs replacing is that one.
    QString m_unconfirmed_parent;
    //! Seconds this transaction has been waiting, and the rate it is paying, in
    //! reference atoms per kvB. A transaction paying ABOVE the going rate that
    //! still has not confirmed is not held back by its fee, and that is the one
    //! thing the window can say for certain without knowing any other
    //! producer's whitelist.
    int64_t m_orig_age{0};
    CAmount m_orig_rate_reference{0};
    //! Whether the output being replaced was blinded. A transaction records the
    //! script it paid but never the blinding key its payer was handed, so the
    //! recipient prefilled here is the plain form of it -- which costs the
    //! replacement its confidentiality, but only if the original had any.
    bool m_orig_confidential{false};

    // What the replacement would cost.
    CAmount m_reference_per_kvb{0};
    int64_t m_vsize{0};
    bool m_vsize_is_estimate{true};
    QString m_probe_error;

    QLabel* m_subject{nullptr};
    QLineEdit* m_address{nullptr};
    QLabel* m_address_hint{nullptr};
    QLineEdit* m_amount{nullptr};
    QComboBox* m_asset{nullptr};
    QLabel* m_amount_hint{nullptr};
    QComboBox* m_fee_asset{nullptr};
    QLabel* m_recipients_note{nullptr};

    QLabel* m_row_label[4]{nullptr, nullptr, nullptr, nullptr};
    QLineEdit* m_cell_original[4]{nullptr, nullptr, nullptr, nullptr};
    QLineEdit* m_cell_new[4]{nullptr, nullptr, nullptr, nullptr};
    QLineEdit* m_cell_delta[4]{nullptr, nullptr, nullptr, nullptr};

    QLabel* m_verdict{nullptr};
    QLabel* m_notes{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    QTimer* m_debounce{nullptr};

    //! Guards the cells against being rewritten by the refresh they triggered.
    bool m_updating{false};
    QLineEdit* m_editing{nullptr};
    //! Set while restating the grid for a keystroke in it: drafting a
    //! transaction per character typed is coin selection per character, so the
    //! figures follow the keystroke and the size follows the pause.
    bool m_skip_draft{false};
};

#endif // BITCOIN_QT_REPLACETXDIALOG_H
