// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_FEESELECTIONWIDGET_H
#define BITCOIN_QT_FEESELECTIONWIDGET_H

#include <asset.h>
#include <consensus/amount.h>
#include <interfaces/wallet.h>

#include <QWidget>

class WalletModel;

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QShowEvent;
class QVBoxLayout;
QT_END_NAMESPACE

/**
 * SEQUENTIA: which asset pays a transaction's fee, and at what rate.
 *
 * One widget rather than one per page, and that is the whole reason it exists.
 * Paying a fee in an arbitrary asset is not a display detail -- it decides
 * whether this node will relay the transaction, whether other producers will
 * value it, and what the user is actually being charged -- so the rules for
 * judging an asset, and the words used to explain them, are reviewed once. The
 * Send tab and the Supervision tab both embed this; neither knows anything about
 * the other.
 *
 * What it owns is exactly what answers "which asset, and how much":
 *
 *  - the "Pay the fee in" selector, filled from the assets this wallet holds
 *    that a fee may be paid in;
 *  - the four-cell grid -- total and per-1000-bytes, in the paying asset and in
 *    the reference currency -- which is one number said four ways, and is the
 *    input as well as the readout when the host is in custom mode;
 *  - the warnings: whether this node accepts the asset at all, whether the wider
 *    network is likely to, and whether the asset is too valuable to be divided
 *    finely enough to pay a small fee.
 *
 * What it does NOT own is what is being paid FOR. Recipients, subtract-fee-from-
 * amount and Replace-By-Fee stay with the page that has them; a host with its own
 * Recommended/Custom controls hands them over with setRateModeWidget(), so they
 * appear between the asset and the numbers they produce without this widget
 * having to know what they are.
 *
 * Everything it displays is derived from three things that arrive AFTER a wallet
 * is attached and keep moving afterwards: the fee whitelist, the asset registry
 * and the price feed. So refresh() is public and is meant to be wired to
 * numBlocksChanged and balanceChanged, not called once at setModel() time. A
 * figure computed before the registry answered and never recomputed is the
 * failure this widget is most likely to have, because it is the failure the code
 * it came from had four times over.
 */
class FeeSelectionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FeeSelectionWidget(QWidget* parent = nullptr);

    void setModel(WalletModel* model);

    //! Put the host's own rate-mode controls between the asset choice and the
    //! numbers it produces. The widget is reparented; passing nothing leaves the
    //! slot empty, which is what a page with no such controls wants.
    void setRateModeWidget(QWidget* widget);

    //! The asset the fee will be paid in.
    CAsset feeAsset() const;

    //! Whether the host is in custom mode, and the rate the cells hold if so, in
    //! reference-unit atoms per 1000 bytes -- the unit CCoinControl::m_feerate
    //! carries and the unit fundrawtransaction's fee_rate expects.
    bool hasCustomRate() const { return m_custom; }
    CAmount customRate() const { return m_custom_rate; }

    //! The confirmation target the recommendation is estimated for.
    int confTarget() const { return m_conf_target; }

    //! The size of the transaction being priced, or 0 when the host cannot size
    //! it yet. Without it, or without a total from setKnownTotal(), there is no
    //! honest total -- only a rate.
    void setTransactionSize(unsigned int vsize);

    //! The total this transaction will actually pay, in atoms of the fee asset,
    //! for a host that learns the fee directly rather than the size. Funding a
    //! transaction returns the fee it charged, which is a better answer than any
    //! size this widget could multiply out: it is the wallet's own figure, over
    //! the wallet's own dummy-signed estimate, and it is what will be paid. Pass
    //! -1 when there is no such figure.
    void setKnownTotal(CAmount fee_asset_atoms);

    //! Take the estimator's rate for this confirmation target.
    void setRecommendedMode(int conf_target);
    //! Take the rate the four cells hold, and let them be typed into.
    void setCustomMode(CAmount reference_per_kvb);

    //! The asset the host would rather pay in -- on the Send tab, the asset being
    //! sent -- honoured while this node accepts a fee in it, and until the user
    //! picks for themselves. A null asset leaves the choice to the wallet's
    //! largest acceptable holding.
    void setPreferredAsset(const CAsset& asset);

    //! Whether the host's transaction signals Replace-By-Fee, which decides which
    //! remedy the "other producers may not take this asset" warning offers.
    void setReplaceable(bool replaceable);

    //! Forget the user's pick, so the preferred asset applies again. For a host
    //! that clears its form.
    void clearUserChoice();

    //! Render a fee rate, already denominated in the selected fee asset, the way a
    //! user reads money: the amount that will actually be paid, its worth in the
    //! reference currency, and the unit price behind that. Rich text.
    QString formatFeeRate(const CAmount& fee_asset_atoms_per_kvb) const;

public Q_SLOTS:
    //! Re-read the whitelist, the registry, the price feed and the congestion, and
    //! restate everything. Cheap: all four are in-memory.
    void refresh();

Q_SIGNALS:
    //! The asset changed, so whatever the host priced with the old one is stale.
    void feeAssetChanged();
    //! A cell was typed into. The argument is the new rate in reference-unit atoms
    //! per 1000 bytes.
    void feeRateEdited(CAmount reference_per_kvb);

protected:
    void showEvent(QShowEvent* event) override;

private:
    WalletModel* m_model{nullptr};

    QVBoxLayout* m_layout{nullptr};
    QComboBox* m_asset_selector{nullptr};
    QWidget* m_rate_mode_slot{nullptr};
    int m_rate_mode_index{0};
    //! The pair built above, kept so a host handing over its own can replace it.
    //! A page that never calls setRateModeWidget() would otherwise be stuck on
    //! the recommendation with no way to pay more -- which is the wrong default
    //! for a freeze, where waiting is the whole risk.
    QWidget* m_own_rate_mode{nullptr};
    QRadioButton* m_radio_recommended{nullptr};
    QRadioButton* m_radio_custom{nullptr};

    //! The four cells: what this costs, in the asset that pays and in the
    //! reference currency, per transaction and per 1000 bytes.
    QLineEdit* m_total_asset{nullptr};
    QLineEdit* m_total_ref{nullptr};
    QLineEdit* m_kvb_asset{nullptr};
    QLineEdit* m_kvb_ref{nullptr};
    QLabel* m_asset_header{nullptr};

    //! Whether this node and the network will take a fee in the chosen asset.
    QLabel* m_warning{nullptr};
    //! What paying in THIS asset costs, beside the choice rather than at the
    //! bottom with the notes about estimation.
    QLabel* m_asset_note{nullptr};
    //! Congestion, floors, and what the chosen rate buys.
    QLabel* m_note{nullptr};

    //! The cell being typed into, which must not be rewritten under the cursor
    //! while the other three are restated.
    QLineEdit* m_cell_editing{nullptr};
    //! Guards the four cells against each other while one recomputes the rest.
    bool m_grid_updating{false};

    bool m_custom{false};
    CAmount m_custom_rate{0};
    int m_conf_target{0};
    unsigned int m_tx_vsize{0};
    //! A total supplied outright, in fee-asset atoms; -1 when none was.
    CAmount m_known_total{-1};
    bool m_replaceable{true};
    CAsset m_preferred_asset;
    //! Set by a real pick in the selector, never by a programmatic one. From then
    //! on the user's choice stands until the host clears it.
    bool m_user_choice{false};
    //! Balances as of the last balanceChanged, so ranking the holdings does not
    //! recompute them on every keystroke.
    interfaces::WalletBalances m_cached_balances;

    void buildUi();
    void populateAssets();
    /** Rewrite the selector's item text from the registry, leaving the items,
     *  their asset ids and the current choice exactly as they are. The names
     *  arrive after the selector is first filled, and an asset whose label
     *  resolved later would otherwise stay a 64-hex id for the life of the
     *  window -- which is what it did. */
    void relabelAssets();
    /** Build the Recommended/Custom pair this widget owns, for a host that has
     *  none of its own. Discarded if the host later hands over its own with
     *  setRateModeWidget(). */
    void buildOwnRateMode();
    /** The rate now on display, converted to the reference unit setCustomMode()
     *  speaks. Used to seed Custom from the recommendation, so switching to it
     *  starts from the figure that was just being quoted rather than from zero. */
    CAmount shownRateAsReference() const;
    void applyDefaultAsset();
    void updateGrid(const CAmount& asset_atoms_per_kvb);
    void onCellEdited(QLineEdit* source);
    void updateWarning();
    void updateNotes(const CAmount& reference_per_kvb, bool have_estimate);

    //! Of the assets in the selector this node actually accepts fees in, the one
    //! this wallet holds the most VALUE of -- ranked by the whitelist rate, which
    //! is the valuation the mempool itself applies. Null when the wallet holds
    //! nothing acceptable.
    CAsset largestAcceptedHolding() const;

    //! The estimator's rate for the current target, in fee-asset atoms per 1000
    //! bytes, and whether it came from real fee history.
    CAmount recommendedRate(bool& have_estimate) const;
};

#endif // BITCOIN_QT_FEESELECTIONWIDGET_H
