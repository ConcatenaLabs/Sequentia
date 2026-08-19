// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/feeselectionwidget.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>

#include <assetsdir.h>
#include <chainparams.h>
#include <exchangerates.h>
#include <feeassets.h>
#include <interfaces/node.h>
#include <policy/feerate.h>
#include <wallet/coincontrol.h>

#include <QComboBox>
#include <QDoubleValidator>
#include <QShowEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpacerItem>
#include <QVBoxLayout>

#include <cmath>

namespace {
//! 10^precision, the atoms in one whole unit of an asset.
double AtomsPerUnit(uint8_t precision)
{
    double f = 1.0;
    for (uint8_t i = 0; i < precision; ++i) f *= 10.0;
    return f;
}
//! Enough decimals to show the asset's smallest unit, and no more.
QString FormatUnits(double units, uint8_t precision)
{
    QString s = QString::number(units, 'f', precision);
    if (s.contains('.')) { while (s.endsWith('0')) s.chop(1); if (s.endsWith('.')) s.chop(1); }
    return s;
}
} // namespace

FeeSelectionWidget::FeeSelectionWidget(QWidget* parent) : QWidget(parent)
{
    buildUi();
}

void FeeSelectionWidget::buildUi()
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    // 1. The asset that pays.
    auto* asset_row = new QHBoxLayout();
    asset_row->addWidget(new QLabel(tr("Pay the fee in:"), this));
    m_asset_selector = new QComboBox(this);
    m_asset_selector->setToolTip(
        tr("Pay the network fee in this asset. By default it follows the asset you are sending, as long as "
           "this node accepts a fee in it — no asset is privileged. Each block producer decides which assets "
           "it will take, and prices them from the asset registry and a market feed: an asset that is "
           "unregistered, unpriced or barely traded risks a payment that never confirms."));
    asset_row->addWidget(m_asset_selector);
    asset_row->addStretch(1);
    m_layout->addLayout(asset_row);

    // The note about the CHOSEN ASSET belongs under the choice rather than at the
    // bottom with the notes about estimation: it is a fact about the asset just
    // picked, not a footnote to the numbers.
    m_asset_note = new QLabel(this);
    m_asset_note->setWordWrap(true);
    m_asset_note->setStyleSheet(QStringLiteral("color:#888;"));
    m_asset_note->setVisible(false);
    m_layout->addWidget(m_asset_note);

    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setVisible(false);
    m_layout->addWidget(m_warning);

    // 2. The host's Recommended/Custom controls go here, between the asset and the
    // numbers it produces. Empty for a page that has none.
    m_rate_mode_index = m_layout->count();

    // 3. Four cells, one number. A fee is a rate on the space a transaction takes,
    // but what anyone wants to know is what THIS transaction costs, and neither
    // figure means much in an asset whose unit price they do not carry in their
    // head -- so both are shown, in the asset that pays and in the reference
    // currency, and under Custom any of the four can be the one you type into.
    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 6);
    grid->setHorizontalSpacing(12);

    m_asset_header = new QLabel(this);
    auto* ref_header = new QLabel(GUIUtil::referenceCurrency(), this);
    QFont hf = m_asset_header->font();
    hf.setBold(true);
    m_asset_header->setFont(hf);
    ref_header->setFont(hf);
    grid->addWidget(m_asset_header, 0, 1);
    grid->addWidget(ref_header, 0, 2);

    auto cell = [this](QGridLayout* g, int row, const QString& label) {
        g->addWidget(new QLabel(label, this), row, 0);
        auto* a = new QLineEdit(this);
        auto* r = new QLineEdit(this);
        for (QLineEdit* e : {a, r}) {
            e->setAlignment(Qt::AlignRight);
            auto* v = new QDoubleValidator(0.0, 1e12, 12, e);
            v->setNotation(QDoubleValidator::StandardNotation);
            e->setValidator(v);
            connect(e, &QLineEdit::textEdited, this, [this, e](const QString&) { onCellEdited(e); });
        }
        g->addWidget(a, row, 1);
        g->addWidget(r, row, 2);
        return std::make_pair(a, r);
    };

    auto total = cell(grid, 1, tr("Total for this transaction"));
    m_total_asset = total.first; m_total_ref = total.second;
    auto perkvb = cell(grid, 2, tr("Per 1000 bytes"));
    m_kvb_asset = perkvb.first; m_kvb_ref = perkvb.second;
    grid->setColumnStretch(3, 1);
    m_layout->addLayout(grid);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setStyleSheet(QStringLiteral("color:#888;"));
    m_layout->addWidget(m_note);
}

void FeeSelectionWidget::setRateModeWidget(QWidget* widget)
{
    if (!widget) return;
    m_rate_mode_slot = widget;
    m_layout->insertWidget(m_rate_mode_index, widget);
}

void FeeSelectionWidget::setModel(WalletModel* model)
{
    m_model = model;
    // Not a chain with an open fee market: there is no choice to offer, and
    // showing a selector with one entry in it would invent one.
    m_asset_selector->setVisible(g_con_any_asset_fees);
    if (!model) return;

    m_cached_balances = model->wallet().getBalances();

    populateAssets();
    connect(model, &WalletModel::assetTypesChanged, this, [this] { populateAssets(); });
    // Registry names arrive late and the balances that rank the default arrive
    // later still. Both come back through here.
    connect(model, &WalletModel::balanceChanged, this,
            [this](const interfaces::WalletBalances& balances) {
                m_cached_balances = balances;
                applyDefaultAsset();
                refresh();
            });

    connect(m_asset_selector, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        refresh();
        Q_EMIT feeAssetChanged();
    });
    // activated() fires only on a real user pick, never a programmatic one: from
    // then on the user's choice is respected until the host clears it.
    connect(m_asset_selector, qOverload<int>(&QComboBox::activated), this,
            [this](int) { m_user_choice = true; });

    refresh();
}

void FeeSelectionWidget::populateAssets()
{
    if (!m_model || !g_con_any_asset_fees) return;
    const QString prev = m_asset_selector->currentData().toString();
    m_asset_selector->clear();
    m_asset_selector->addItem(GUIUtil::assetDisplayName(::policyAsset),
                              QString::fromStdString(::policyAsset.GetHex()));
    // Reissuance tokens are not on offer: the fee is paid to whichever producer
    // mines this transaction, into its coinbase, so a fee in an inflation key
    // would give that producer the power to mint the asset without limit. Any
    // fraction of the token carries the whole power, which is why there is no
    // "small enough" amount to spend on a fee.
    for (const CAsset& asset : m_model->getFeePayableAssetTypes()) {
        if (asset == ::policyAsset) continue;
        m_asset_selector->addItem(GUIUtil::assetDisplayName(asset),
                                  QString::fromStdString(asset.GetHex()));
    }
    const int idx = m_asset_selector->findData(prev);
    if (idx >= 0) m_asset_selector->setCurrentIndex(idx);
    applyDefaultAsset();
}

void FeeSelectionWidget::relabelAssets()
{
    if (!m_asset_selector) return;
    for (int i = 0; i < m_asset_selector->count(); ++i) {
        const CAsset asset = GetAssetFromString(m_asset_selector->itemData(i).toString().toStdString());
        if (asset.IsNull()) continue;
        const QString name = GUIUtil::assetDisplayName(asset);
        // Only when it actually changed: setItemText on the current item makes
        // the combo emit, and re-entering refresh() from inside refresh() is a
        // loop with no reason to exist.
        if (m_asset_selector->itemText(i) != name) m_asset_selector->setItemText(i, name);
    }
}

void FeeSelectionWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // The registry is fetched over the network and merged long after this widget
    // was built. Coming back to the page is the moment a stale hex id is most
    // visible, and the cheapest moment to correct it.
    relabelAssets();
}

CAsset FeeSelectionWidget::feeAsset() const
{
    if (!g_con_any_asset_fees || m_asset_selector->count() == 0) return ::policyAsset;
    const CAsset sel = GetAssetFromString(m_asset_selector->currentData().toString().toStdString());
    return sel.IsNull() ? ::policyAsset : sel;
}

void FeeSelectionWidget::setPreferredAsset(const CAsset& asset)
{
    if (m_preferred_asset == asset) return;
    m_preferred_asset = asset;
    applyDefaultAsset();
}

void FeeSelectionWidget::clearUserChoice()
{
    m_user_choice = false;
    applyDefaultAsset();
}

void FeeSelectionWidget::applyDefaultAsset()
{
    if (!g_con_any_asset_fees || !m_model || m_asset_selector->count() == 0) return;
    if (m_user_choice) { updateWarning(); return; }

    // The host's preference is the least surprising default -- on the Send tab it
    // is the asset being sent, which needs no extra asset in the wallet -- but only
    // while this node accepts a fee in it. The test is the fee whitelist, not the
    // display price feed: the whitelist is what the mempool consults, so an asset
    // missing from it fails here and now, while an asset missing from the feed
    // merely cannot be shown in dollars.
    CAsset pick = m_preferred_asset.IsNull() ? ::policyAsset : m_preferred_asset;
    if (!m_model->node().getFeeAssetInfo(pick).accepted) {
        const CAsset fallback = largestAcceptedHolding();
        // Nothing acceptable in the wallet: leave the selection alone rather than
        // moving it somewhere equally unusable, and let the warning explain.
        if (!fallback.IsNull()) pick = fallback;
    }
    const int idx = m_asset_selector->findData(QString::fromStdString(pick.GetHex()));
    if (idx >= 0 && idx != m_asset_selector->currentIndex()) {
        m_asset_selector->setCurrentIndex(idx);
    }
    updateWarning();
}

CAsset FeeSelectionWidget::largestAcceptedHolding() const
{
    if (!m_model) return CAsset();
    CAsset best;
    double best_value = 0.0;
    for (int i = 0; i < m_asset_selector->count(); ++i) {
        const CAsset candidate = GetAssetFromString(m_asset_selector->itemData(i).toString().toStdString());
        if (candidate.IsNull()) continue;
        const FeeAssetInfo info = m_model->node().getFeeAssetInfo(candidate);
        if (!info.accepted) continue;
        // Rank by what the holding is WORTH, not by how many atoms of it there
        // are: "10000 of a millionth-of-a-cent token" must not outrank "3 of a
        // dollar one". The whitelist rate is the right yardstick and not merely
        // the convenient one -- it is the very valuation this node's mempool will
        // apply to the fee, it is denominated in one unit across every asset, and
        // it exists for exactly the assets that can pay. The display price feed
        // would answer the same question in USD, but only while it is up and only
        // for assets it happens to quote, so it would rank on availability as
        // much as on value.
        const double value = static_cast<double>(valueFor(m_cached_balances.balance, candidate))
                           * static_cast<double>(info.rate) / static_cast<double>(exchange_rate_scale);
        if (value > best_value) { best_value = value; best = candidate; }
    }
    return best;
}

void FeeSelectionWidget::setTransactionSize(unsigned int vsize)
{
    if (m_tx_vsize == vsize) return;
    m_tx_vsize = vsize;
    refresh();
}

void FeeSelectionWidget::setKnownTotal(CAmount fee_asset_atoms)
{
    if (m_known_total == fee_asset_atoms) return;
    m_known_total = fee_asset_atoms;
    refresh();
}

void FeeSelectionWidget::setRecommendedMode(int conf_target)
{
    if (!m_custom && m_conf_target == conf_target) return;
    m_custom = false;
    m_conf_target = conf_target;
    refresh();
}

void FeeSelectionWidget::setCustomMode(CAmount reference_per_kvb)
{
    if (m_custom && m_custom_rate == reference_per_kvb) return;
    m_custom = true;
    m_custom_rate = reference_per_kvb;
    refresh();
}

void FeeSelectionWidget::setReplaceable(bool replaceable)
{
    if (m_replaceable == replaceable) return;
    m_replaceable = replaceable;
    // The unpriced-asset warning tells the user to turn RBF on, so it has to stop
    // saying that the moment they do.
    updateWarning();
}

CAmount FeeSelectionWidget::recommendedRate(bool& have_estimate) const
{
    have_estimate = false;
    if (!m_model) return 0;
    wallet::CCoinControl cc;
    if (g_con_any_asset_fees) {
        const CAsset asset = feeAsset();
        if (asset != ::policyAsset) cc.m_fee_asset = asset;
    }
    if (m_conf_target > 0) cc.m_confirm_target = m_conf_target;
    int returned_target = 0;
    FeeReason reason = FeeReason::NONE;
    const CAmount rate = m_model->wallet().getMinimumFee(1000, cc, &returned_target, &reason);
    have_estimate = (reason != FeeReason::FALLBACK);
    return rate;
}

void FeeSelectionWidget::refresh()
{
    if (!m_model || !m_model->getOptionsModel()) return;

    // Names resolve on the same schedule as the numbers below, and for the same
    // reason: both wait on something that arrives after the wallet is attached.
    // refresh() is wired to numBlocksChanged, so a label that resolves while the
    // page is open is corrected within a block instead of never.
    relabelAssets();

    bool have_estimate = false;
    const CAmount recommended = recommendedRate(have_estimate);

    // Under Custom the grid must show what the user set, not what would have been
    // recommended. Both figures reach the grid denominated in the fee asset; the
    // custom one is held in the reference unit and is converted at this node's
    // whitelist rate, which is the rate the mempool will use.
    const CAmount shown_rate = m_custom
        ? CFeeRate(m_custom_rate).GetFee(1000, feeAsset())
        : recommended;

    updateGrid(shown_rate);
    updateWarning();
    updateNotes(m_custom ? m_custom_rate : 0, have_estimate);
}

void FeeSelectionWidget::updateGrid(const CAmount& asset_atoms_per_kvb)
{
    if (!m_kvb_asset || !m_model) return;
    const CAsset asset = feeAsset();
    const FeeAssetInfo info = m_model->node().getFeeAssetInfo(asset);
    const double factor = AtomsPerUnit(info.precision);
    // The reference column is a market valuation, so it comes from the price feed
    // and not from the whitelist rate. The two normally agree, since whitelist
    // rates are derived from the feed -- but an asset an operator listed by hand
    // has a rate and no quote, and reading the rate there printed a USD figure
    // directly beside the warning saying this asset has no published price. No
    // quote, no column: an empty cell is the honest answer.
    const double unit_price = info.has_market_price ? info.market_price : 0.0;

    m_asset_header->setText(GUIUtil::assetDisplayName(asset));

    m_grid_updating = true;
    // The four cells are one number said four ways, so a redraw restates all of
    // them -- except the one being typed into, whose text is the user's and whose
    // cursor would jump to the end on every keystroke if we rewrote it.
    auto put = [this](QLineEdit* cell, const QString& text) {
        if (cell != m_cell_editing) cell->setText(text);
    };
    put(m_kvb_asset, FormatUnits(asset_atoms_per_kvb / factor, info.precision));
    put(m_kvb_ref, unit_price > 0.0
        ? QString::number(asset_atoms_per_kvb / factor * unit_price, 'f', 8)
        : QString());
    // A total the host was given outright beats one multiplied out here: it is the
    // fee the wallet actually charged, over the size the wallet actually estimated.
    const double total_atoms = m_known_total >= 0
        ? static_cast<double>(m_known_total)
        : (m_tx_vsize > 0
           ? std::ceil(static_cast<double>(asset_atoms_per_kvb) * m_tx_vsize / 1000.0)
           : -1.0);
    if (total_atoms >= 0.0) {
        put(m_total_asset, FormatUnits(total_atoms / factor, info.precision));
        put(m_total_ref, unit_price > 0.0
            ? QString::number(total_atoms / factor * unit_price, 'f', 8)
            : QString());
    } else {
        put(m_total_asset, QStringLiteral("—"));
        put(m_total_ref, QStringLiteral("—"));
    }
    for (QLineEdit* e : {m_total_asset, m_total_ref, m_kvb_asset, m_kvb_ref}) {
        e->setReadOnly(!m_custom);
    }
    const bool sizable = m_tx_vsize > 0;  // a total can be shown without one, but not typed into
    m_total_asset->setEnabled(m_custom && sizable);
    m_total_ref->setEnabled(m_custom && sizable && unit_price > 0.0);
    m_kvb_ref->setEnabled(m_custom && unit_price > 0.0);
    // Under Recommended these are a readout; under Custom they are the input, and
    // nothing about a grey box says "type here". Colour only the cells that will
    // actually accept typing: an unpriced reference column and a total with no
    // transaction to size are read-only even under Custom, and highlighting those
    // would promise an edit that does nothing.
    for (QLineEdit* e : {m_total_asset, m_total_ref, m_kvb_asset, m_kvb_ref}) {
        const bool editable = m_custom && e->isEnabled() && !e->isReadOnly();
        e->setStyleSheet(editable
            ? QStringLiteral("QLineEdit { color:#ffb84d; border:1px solid #ffb84d; }")
            : QString());
    }
    m_grid_updating = false;
}

void FeeSelectionWidget::onCellEdited(QLineEdit* source)
{
    if (m_grid_updating || !m_model || !m_custom) return;
    const CAsset asset = feeAsset();
    const FeeAssetInfo info = m_model->node().getFeeAssetInfo(asset);
    if (!info.accepted) return;
    const double factor = AtomsPerUnit(info.precision);
    // Same source as the display: whatever is typed in the reference column is
    // converted back with the price it was shown with, or the round trip lies.
    const double unit_price = info.has_market_price ? info.market_price : 0.0;

    bool ok = false;
    const double typed = source->text().trimmed().toDouble(&ok);
    if (!ok || typed < 0.0) return;

    // Everything is derived from one figure: atoms of the fee asset per 1000
    // bytes. Whichever cell was typed into is converted back to that, and the
    // wallet's own field -- which is a rate in the reference unit -- follows.
    double atoms_per_kvb = 0.0;
    if (source == m_kvb_asset) {
        atoms_per_kvb = typed * factor;
    } else if (source == m_kvb_ref) {
        if (!(unit_price > 0.0)) return;
        atoms_per_kvb = typed / unit_price * factor;
    } else if (source == m_total_asset) {
        if (m_tx_vsize == 0) return;
        atoms_per_kvb = typed * factor * 1000.0 / m_tx_vsize;
    } else if (source == m_total_ref) {
        if (m_tx_vsize == 0 || !(unit_price > 0.0)) return;
        atoms_per_kvb = typed / unit_price * factor * 1000.0 / m_tx_vsize;
    }
    if (!(atoms_per_kvb >= 0.0)) return;

    const CAmount reference_per_kvb = static_cast<CAmount>(
        std::llround(atoms_per_kvb * static_cast<double>(info.rate) / static_cast<double>(exchange_rate_scale)));
    m_custom_rate = std::max<CAmount>(0, reference_per_kvb);

    // Redraw the other three now. Nothing else will: the host recomputes only when
    // the TRANSACTION SIZE changes, and typing a fee does not change it. So the
    // grid kept whatever it last held and refreshed only when some unrelated event
    // moved the size, by which time it was restating an earlier figure. That is
    // what made the table read one edit behind and its two columns disagree: not
    // the arithmetic, which was right, but that nobody asked for it to be run.
    m_cell_editing = source;
    refresh();
    m_cell_editing = nullptr;

    Q_EMIT feeRateEdited(m_custom_rate);
}

QString FeeSelectionWidget::formatFeeRate(const CAmount& fee_asset_atoms_per_kvb) const
{
    // The figure arrives already denominated in the selected fee asset:
    // wallet::GetMinimumFee() ends in GetFee(bytes, coin_control.m_fee_asset),
    // which converts out of the reference unit at this node's whitelist rate. So
    // the asset amount is the fact, and it is the number the user will pay; the
    // reference currency is the aid that makes it legible. Reading the figure as
    // policy-asset atoms and converting it into the fee asset a second time — at
    // display-feed prices, no less — quoted a fee that was wrong by the ratio
    // between the two rates whenever the two assets differed.
    const QString ref = GUIUtil::referenceCurrency();
    const CAsset feeAsset_ = feeAsset();

    // A fee asset this node does not accept converts to zero, and "0/kvB" reads as
    // "free" rather than "impossible". The warning label says why.
    if (g_con_any_asset_fees && m_model && !m_model->node().getFeeAssetInfo(feeAsset_).accepted) {
        return tr("unavailable — this node does not accept fees in %1").arg(GUIUtil::assetDisplayName(feeAsset_));
    }

    QStringList parts;
    parts << GUIUtil::formatAssetAmount(feeAsset_, fee_asset_atoms_per_kvb, BitcoinUnits::BTC,
                                        BitcoinUnits::SeparatorStyle::STANDARD, /*include_asset_name=*/true)
             + QStringLiteral("/kvB");

    double refValue = 0.0;
    if (GUIUtil::referenceValueOf(feeAsset_, fee_asset_atoms_per_kvb, ref, refValue)) {
        QString line = GUIUtil::formatReferenceAmount(refValue, ref) + QStringLiteral("/kvB");
        // The unit price behind it, so the conversion is checkable rather than magic.
        double unitValue = 0.0;
        if (GUIUtil::unitReferenceValue(feeAsset_, ref, unitValue)) {
            line += QStringLiteral(" <span style='color:#888'>(1 ") + GUIUtil::assetDisplayName(feeAsset_)
                  + QString::fromUtf8(" \xE2\x89\x88 ") + GUIUtil::formatReferenceAmount(unitValue, ref) + QStringLiteral(")</span>");
        }
        parts << line;
    }
    return parts.join(QStringLiteral(" = "));
}

void FeeSelectionWidget::updateWarning()
{
    if (!g_con_any_asset_fees || !m_model || m_asset_selector->count() == 0) {
        m_warning->setVisible(false);
        return;
    }
    const CAsset sel = feeAsset();
    const FeeAssetInfo info = m_model->node().getFeeAssetInfo(sel);
    const QString name = GUIUtil::assetDisplayName(sel);

    // Three different things can be wrong with a fee asset and they are not the
    // same warning. Worst first.
    //
    // 1. This node does not accept it. Nothing downstream matters: the wallet's
    //    own mempool refuses the transaction, so it never reaches a producer.
    // 2. This node accepts it but the wider network probably will not, because the
    //    registry does not publish it or no feed prices it — the two inputs every
    //    price server uses to build its whitelist.
    // 3. Nothing is wrong. Not being able to show the fee in dollars is not a
    //    problem with the fee; it stays silent.
    if (!info.accepted) {
        // Being listed at rate 0 and being absent are the same refusal but not the
        // same mistake: one is a policy someone wrote down, the other is an asset
        // nobody has configured. Saying which saves the search.
        const QString why = info.listed
            ? tr("This node's fee policy lists %1 at rate 0, which refuses it.").arg(name)
            : tr("%1 is not in this node's fee whitelist.").arg(name);
        // Red, not amber: this one is not a risk to weigh, it is a transaction that
        // cannot be sent.
        m_warning->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
        m_warning->setText(
            why + QLatin1Char(' ') +
            tr("The transaction would be rejected by your own node before it ever reached a block "
               "producer. Pick an accepted asset, or set a rate for %1 under Settings → Fee policy.").arg(name));
        m_warning->setVisible(true);
        return;
    }

    // The policy asset is judged by the same two questions as every other asset,
    // deliberately. Exempting it from the registry check would be a privilege, and
    // outside staking eligibility no asset here has one; if the answer for it is
    // uncomfortable the fix is to publish it on the registry, not to stop asking.
    if (!info.registry_listed || !info.has_market_price) {
        const QString why = !info.registry_listed
            ? tr("%1 is not published on the Asset Registry, so the price servers other block producers "
                 "run will not discover it.").arg(name)
            : tr("No published market price for %1, so other block producers' price servers cannot "
                 "value it.").arg(name);
        // Replace-By-Fee is the remedy here and only here: this transaction is
        // valid and relayable, it may simply sit unconfirmed, and the fix is to
        // switch the fee to a better-travelled asset later — which is precisely
        // what RBF allows. A host whose transaction cannot be replaced at all does
        // not get offered it.
        const QString remedy = m_replaceable
            ? tr("Replace-By-Fee is on, so you can switch the fee to another asset later if it does not confirm.")
            : tr("Turn on Replace-By-Fee below so you can switch the fee to another asset later if it does not confirm.");
        m_warning->setStyleSheet(QStringLiteral("color: #ffb84d;"));
        m_warning->setText(
            why + QLatin1Char(' ') +
            tr("This node accepts it, so the payment may confirm only in a block this node produces.") +
            QLatin1Char(' ') + remedy);
        m_warning->setVisible(true);
        return;
    }

    m_warning->setVisible(false);
}

void FeeSelectionWidget::updateNotes(const CAmount& custom_reference_per_kvb, bool have_estimate)
{
    if (!m_note || !m_model) return;
    const MempoolCongestion c = m_model->node().getMempoolCongestion();
    const CAsset asset = feeAsset();
    const FeeAssetInfo info = m_model->node().getFeeAssetInfo(asset);
    const QString name = GUIUtil::assetDisplayName(asset);
    auto in_asset = [&](CAmount reference_per_kvb) {
        return formatFeeRate(CFeeRate(reference_per_kvb).GetFee(1000, asset));
    };
    QStringList notes;
    QString asset_note;

    if (m_custom) {
        if (custom_reference_per_kvb < c.relay_min) {
            notes << tr("Below %1 the network will not relay this transaction at all.").arg(in_asset(c.relay_min));
        } else if (c.next_block_full && custom_reference_per_kvb < c.next_block_min) {
            notes << tr("This clears the relay minimum, but the next block is taking nothing below %1. "
                        "Yours will wait until the queue clears.").arg(in_asset(c.next_block_min));
        }
    } else if (!have_estimate && !c.next_block_full) {
        notes << tr("Blocks are not congested, and there is no recent fee history to estimate from. "
                    "This is the least the network will relay, and the confirmation target cannot "
                    "change it until transactions start competing for space.");
    } else if (!have_estimate && c.next_block_full) {
        notes << tr("Blocks are full, but there is no fee history to estimate from yet. Transactions "
                    "making the next block are paying at least %1 — below that, yours waits.").arg(in_asset(c.next_block_min));
    } else if (have_estimate && !c.next_block_full) {
        notes << tr("Blocks are not congested, so a nearer target buys little: at this rate the next "
                    "block has room for you either way.");
    } else {
        notes << tr("Blocks are full, with %1 of transactions waiting. A nearer target pays more, "
                    "which is what puts you ahead of them.")
                    .arg(tr("%1 blocks' worth").arg(QString::number(c.backlog_blocks, 'f', 1)));
    }
    if (c.mempool_min > c.relay_min) {
        notes << tr("This node's queue is full and it is dropping the cheapest transactions. %1 is "
                    "the least it will hold now.").arg(in_asset(c.mempool_min));
    }
    // An asset worth thousands per unit cannot be divided finely enough to pay a
    // small fee: its smallest indivisible piece already exceeds what the network is
    // asking, and the difference is pure overpayment.
    if (info.accepted) {
        const CAmount floor_in_asset = CFeeRate(c.relay_min).GetFee(1000, asset);
        const double floor_units = static_cast<double>(floor_in_asset) / AtomsPerUnit(info.precision);
        const double floor_value = floor_units * UnitPriceFromFeeRate(info.rate, info.precision);
        const double network_min = static_cast<double>(c.relay_min) / static_cast<double>(exchange_rate_scale);
        if (network_min > 0.0 && floor_value > network_min * 3.0) {
            asset_note = tr("Paying in %1 costs at least %2 per 1000 bytes — about %3 times the network "
                            "minimum, because one unit of %1 cannot be divided any further. A less "
                            "valuable asset pays closer to the true cost.")
                            .arg(name, in_asset(c.relay_min), QString::number(floor_value / network_min, 'f', 0));
        }
    }
    if (m_tx_vsize == 0 && m_known_total < 0) {
        notes << tr("The total appears once there is something to size the transaction with.");
    }
    m_note->setText(notes.join(QStringLiteral(" ")));
    m_note->setVisible(!notes.isEmpty());
    m_asset_note->setText(asset_note);
    m_asset_note->setVisible(!asset_note.isEmpty());
}
