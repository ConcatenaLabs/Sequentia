// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CHILDPAYSDIALOG_H
#define BITCOIN_QT_CHILDPAYSDIALOG_H

#include <asset.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <QDialog>
#include <QString>

#include <cstdint>
#include <vector>

class WalletModel;

QT_BEGIN_NAMESPACE
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QTimer;
QT_END_NAMESPACE

/**
 * SEQUENTIA: speed up a stuck transaction by spending one of its outputs with a
 * generous fee — child pays for parent.
 *
 * A producer weighs the pair together, so the figure that decides whether the
 * stuck transaction moves is neither the parent's fee nor the child's but the
 * PACKAGE rate: both fees over both sizes. A window that asks only which asset
 * to pay in hides precisely the number that matters, and leaves the user to
 * find out by waiting. So the package rate is computed and shown against what
 * the next block is taking, in the asset and in the reference currency, with
 * the child's fee editable in any of those terms.
 *
 * The child pays back into this wallet, which is what makes it a fee-carrying
 * transaction rather than a payment; the address and the amount are shown and
 * can be changed, because a window that does something on your behalf without
 * showing it is asking to be trusted rather than read.
 */
class ChildPaysDialog : public QDialog
{
    Q_OBJECT

public:
    ChildPaysDialog(WalletModel* model, const uint256& parent_hash, QWidget* parent = nullptr);

    //! False when the parent has no spendable output to attach a child to; the
    //! dialog must not be shown then.
    bool isUsable() const { return m_usable; }

    // Meaningful once exec() has returned Accepted.
    uint32_t outputIndex() const;
    QString address() const;
    CAmount amount() const;      //!< atoms of the pinned output's asset
    CAsset feeAsset() const;
    CAmount referencePerKvb() const { return m_reference_per_kvb; }

private Q_SLOTS:
    void onOutputChanged();
    void onRecipientEdited();
    void onFeeAssetChanged();
    void onFeeCellEdited();
    void recompute();

private:
    void buildUi();
    bool loadParent();
    CAsset selectedFeeAsset() const;
    CAsset pinnedAsset() const;
    bool parsedAmount(CAmount& out) const;

    WalletModel* const m_model;
    const uint256 m_parent_hash;
    bool m_usable{false};

    //! The parent's spendable outputs, in the order the selector lists them.
    struct Candidate {
        uint32_t n{0};
        CAsset asset;
        CAmount value{0};
        bool is_change{false};
    };
    std::vector<Candidate> m_candidates;

    CTransactionRef m_parent;
    CAsset m_parent_fee_asset;
    CAmount m_parent_fee_atoms{0};
    CAmount m_parent_fee_reference{0};
    int64_t m_parent_vsize{0};
    //! See ReplaceTxDialog: a parent already paying above the going rate and
    //! still waiting is not waiting for money.
    int64_t m_parent_age{0};
    CAmount m_parent_rate_reference{0};

    CAmount m_reference_per_kvb{0};
    int64_t m_child_vsize{0};
    bool m_child_vsize_is_estimate{true};
    QString m_probe_error;

    QLabel* m_subject{nullptr};
    QComboBox* m_output{nullptr};
    QLabel* m_output_hint{nullptr};
    QLineEdit* m_address{nullptr};
    QLabel* m_address_hint{nullptr};
    QLineEdit* m_amount{nullptr};
    QComboBox* m_fee_asset{nullptr};
    QLabel* m_fee_asset_hint{nullptr};

    QLabel* m_row_label[4]{nullptr, nullptr, nullptr, nullptr};
    QLineEdit* m_cell_parent[4]{nullptr, nullptr, nullptr, nullptr};
    QLineEdit* m_cell_child[4]{nullptr, nullptr, nullptr, nullptr};
    QLineEdit* m_cell_package[4]{nullptr, nullptr, nullptr, nullptr};

    QLabel* m_verdict{nullptr};
    QLabel* m_notes{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    QTimer* m_debounce{nullptr};

    bool m_updating{false};
    bool m_skip_draft{false};
    QLineEdit* m_editing{nullptr};
};

#endif // BITCOIN_QT_CHILDPAYSDIALOG_H
