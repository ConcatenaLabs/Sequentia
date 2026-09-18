// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/childpaysdialog.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>

#include <asset.h>
#include <assetsdir.h>
#include <confidential_validation.h> // GetFeeMap
#include <exchangerates.h>           // exchange_rate_scale
#include <feeassets.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <rpc/util.h> // GetDestinationBlindingKey
#include <wallet/coincontrol.h>
#include <wallet/ismine.h>

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
enum Row { RATE_ASSET = 0, RATE_REF = 1, TOTAL_ASSET = 2, TOTAL_REF = 3, ROW_COUNT = 4 };

const char* kEditableCell = "QLineEdit { color:#ffb84d; border:1px solid #ffb84d; }";
const char* kQuietCell = "QLineEdit { color:#aaa; }";

QString refFigure(double value) { return QString::number(value, 'f', 8); }

//! The child's size before one has been drafted. The same conservative
//! confidential-child estimate the old code used to seed its fee rate.
constexpr int64_t kChildSizeGuess = 1100;
} // namespace

ChildPaysDialog::ChildPaysDialog(WalletModel* model, const uint256& parent_hash, QWidget* parent)
    : QDialog(parent), m_model(model), m_parent_hash(parent_hash)
{
    setWindowTitle(tr("Speed up (child pays for parent)"));
    buildUi();
    m_usable = loadParent();
    if (!m_usable) return;

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, &ChildPaysDialog::recompute);

    recompute();
}

void ChildPaysDialog::buildUi()
{
    auto* lay = new QVBoxLayout(this);

    auto* intro = new QLabel(tr("Spends one of the stuck transaction's outputs with a fee generous enough "
                                "to carry both. A producer weighs the two together, so what decides it is "
                                "the package rate below — not either fee on its own.\n"
                                "A child pays MORE for the stuck transaction; it cannot pay in a different "
                                "asset for it. If producers do not accept the asset its fee is already in, "
                                "no child rescues it — replacing it does."), this);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    // Which transaction is being carried: see the note in ReplaceTxDialog.
    m_subject = new QLabel(this);
    m_subject->setWordWrap(true);
    m_subject->setStyleSheet(QStringLiteral("color:#aaa;"));
    lay->addWidget(m_subject);

    auto* form = new QGridLayout();
    form->setHorizontalSpacing(12);
    int r = 0;
    form->addWidget(new QLabel(tr("Spend which output:"), this), r, 0);
    m_output = new QComboBox(this);
    form->addWidget(m_output, r, 1, 1, 2);
    ++r;
    m_output_hint = new QLabel(this);
    m_output_hint->setWordWrap(true);
    m_output_hint->setStyleSheet(QStringLiteral("color:#888;"));
    form->addWidget(m_output_hint, r, 1, 1, 2);
    ++r;
    form->addWidget(new QLabel(tr("Send it to:"), this), r, 0);
    m_address = new QLineEdit(this);
    form->addWidget(m_address, r, 1, 1, 2);
    ++r;
    m_address_hint = new QLabel(this);
    m_address_hint->setWordWrap(true);
    form->addWidget(m_address_hint, r, 1, 1, 2);
    ++r;
    form->addWidget(new QLabel(tr("Amount:"), this), r, 0);
    m_amount = new QLineEdit(this);
    form->addWidget(m_amount, r, 1, 1, 2);
    ++r;
    form->addWidget(new QLabel(tr("Pay the child fee in:"), this), r, 0);
    m_fee_asset = new QComboBox(this);
    form->addWidget(m_fee_asset, r, 1, 1, 2);
    ++r;
    m_fee_asset_hint = new QLabel(this);
    m_fee_asset_hint->setWordWrap(true);
    m_fee_asset_hint->setStyleSheet(QStringLiteral("color:#888;"));
    form->addWidget(m_fee_asset_hint, r, 1, 1, 2);
    lay->addLayout(form);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(12);
    // See the replacement window: boxed figures need room or they clip.
    grid->setVerticalSpacing(8);
    grid->setContentsMargins(0, 10, 0, 6);
    auto* h1 = new QLabel(tr("Stuck transaction"), this);
    auto* h2 = new QLabel(tr("Child"), this);
    auto* h3 = new QLabel(tr("Both together"), this);
    for (QLabel* h : {h1, h2, h3}) h->setStyleSheet(QStringLiteral("font-weight:bold;"));
    grid->addWidget(h1, 0, 1);
    grid->addWidget(h2, 0, 2);
    grid->addWidget(h3, 0, 3);
    for (int i = 0; i < ROW_COUNT; ++i) {
        m_row_label[i] = new QLabel(this);
        grid->addWidget(m_row_label[i], i + 1, 0);
        m_cell_parent[i] = new QLineEdit(this);
        m_cell_parent[i]->setMinimumHeight(26);
        m_cell_parent[i]->setReadOnly(true);
        m_cell_parent[i]->setStyleSheet(QLatin1String(kQuietCell));
        grid->addWidget(m_cell_parent[i], i + 1, 1);
        m_cell_child[i] = new QLineEdit(this);
        m_cell_child[i]->setMinimumHeight(26);
        grid->addWidget(m_cell_child[i], i + 1, 2);
        m_cell_package[i] = new QLineEdit(this);
        m_cell_package[i]->setMinimumHeight(26);
        m_cell_package[i]->setReadOnly(true);
        m_cell_package[i]->setStyleSheet(QLatin1String(kQuietCell));
        grid->addWidget(m_cell_package[i], i + 1, 3);
        connect(m_cell_child[i], &QLineEdit::textEdited, this, &ChildPaysDialog::onFeeCellEdited);
    }
    lay->addLayout(grid);

    m_verdict = new QLabel(this);
    m_verdict->setWordWrap(true);
    lay->addWidget(m_verdict);
    m_notes = new QLabel(this);
    m_notes->setWordWrap(true);
    m_notes->setStyleSheet(QStringLiteral("color:#888;"));
    lay->addWidget(m_notes);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_output, qOverload<int>(&QComboBox::currentIndexChanged), this, &ChildPaysDialog::onOutputChanged);
    connect(m_address, &QLineEdit::textEdited, this, &ChildPaysDialog::onRecipientEdited);
    connect(m_amount, &QLineEdit::textEdited, this, &ChildPaysDialog::onRecipientEdited);
    connect(m_fee_asset, qOverload<int>(&QComboBox::currentIndexChanged), this, &ChildPaysDialog::onFeeAssetChanged);
}

bool ChildPaysDialog::loadParent()
{
    interfaces::WalletTxStatus st;
    interfaces::WalletOrderForm of;
    bool in_mempool = false;
    int nblocks = 0;
    const interfaces::WalletTx wtx = m_model->wallet().getWalletTxDetails(m_parent_hash, st, of, in_mempool, nblocks);
    if (!wtx.tx || st.depth_in_main_chain != 0 || !in_mempool) return false;
    m_parent = wtx.tx;

    m_parent_fee_asset = m_parent->GetFeeAsset(::policyAsset);
    m_parent_fee_atoms = GetFeeMap(*m_parent)[m_parent_fee_asset];
    m_parent_vsize = GetVirtualTransactionSize(*m_parent);
    const FeeAssetInfo pinfo = m_model->node().getFeeAssetInfo(m_parent_fee_asset);
    m_parent_fee_reference = pinfo.rate > 0
        ? static_cast<CAmount>(std::llround(static_cast<double>(m_parent_fee_atoms) *
                                            static_cast<double>(pinfo.rate) /
                                            static_cast<double>(exchange_rate_scale)))
        : 0;

    // Every unconfirmed output of the parent this wallet can still spend. The old
    // code picked one silently, preferring the change; the choice is kept (it is
    // usually the right one) but it is now visible and can be overridden -- the
    // outputs differ in asset and in amount, and which one carries the child is
    // not a detail the user should have to infer from the result.
    const int unit = m_model->getOptionsModel()->getDisplayUnit();
    int prefer = -1;
    for (size_t n = 0; n < m_parent->vout.size(); ++n) {
        if (m_parent->vout[n].IsFee()) continue;
        if (n >= wtx.txout_is_mine.size() || wtx.txout_is_mine[n] == wallet::ISMINE_NO) continue;
        const auto coins = m_model->wallet().getCoins({COutPoint(m_parent_hash, (uint32_t)n)});
        if (coins.empty() || coins[0].is_spent) continue;
        Candidate c;
        c.n = (uint32_t)n;
        c.asset = n < wtx.txout_assets.size() ? wtx.txout_assets[n] : CAsset();
        c.value = n < wtx.txout_amounts.size() ? wtx.txout_amounts[n] : 0;
        c.is_change = n < wtx.txout_is_change.size() && wtx.txout_is_change[n];
        if (c.value <= 0 || c.asset.IsNull()) continue;
        m_candidates.push_back(c);
        const QString label = GUIUtil::formatAssetAmount(c.asset, c.value, unit,
                                                         BitcoinUnits::SeparatorStyle::STANDARD,
                                                         /*include_asset_name=*/true) +
                              (c.is_change ? QStringLiteral(" — ") + tr("change returned to you")
                                           : QStringLiteral(" — ") + tr("output #%1").arg(n));
        m_output->addItem(label, (uint)n);
        if (c.is_change && prefer < 0) prefer = m_output->count() - 1;
    }
    if (m_candidates.empty()) return false;
    m_output->setCurrentIndex(prefer >= 0 ? prefer : 0);
    m_output->setEnabled(m_candidates.size() > 1);

    CTxDestination dest;
    if (m_model->wallet().getNewDestination(m_model->wallet().getDefaultAddressType(), "CPFP", dest,
                                            /*add_blinding_key=*/true)) {
        m_address->setText(QString::fromStdString(EncodeDestination(dest)));
    }

    // The policy asset first: the child exists to be mined, and its fee has to be
    // one producers accept -- which is exactly what the parent's may not have
    // been. Confining the child's fee to the pinned output's asset was the old
    // bug, and it stranded the child beside the parent.
    m_fee_asset->addItem(BitcoinUnits::policyAssetTicker(), QString::fromStdString(::policyAsset.GetHex()));
    if (g_con_any_asset_fees) {
        for (const CAsset& a : m_model->getFeePayableAssetTypes()) {
            if (a == ::policyAsset) continue;
            m_fee_asset->addItem(GUIUtil::assetDisplayName(a), QString::fromStdString(a.GetHex()));
        }
    } else {
        m_fee_asset->setEnabled(false);
    }

    // Seed at the rate the old code used: five times the entry price, sized so
    // the package clears it even crediting the parent with nothing. Generous on
    // purpose -- a child that lands just at the cut buys nothing, since the cut
    // moves while it waits -- and now visible, and editable.
    const MempoolCongestion c = m_model->node().getMempoolCongestion();
    // The wallet's own minimum, not merely the relay floor. On a chain with room
    // the floor is one atom per kvB, and seeding from it produced a child fee of
    // eleven atoms -- arithmetically a package above the cut, practically a
    // transaction nobody has a reason to mine. This is the figure the wallet
    // charges for an ordinary payment, which is the least a fee meant to BUY
    // something should be.
    wallet::CCoinControl probe_cc;
    const CAmount wallet_min = m_model->wallet().getMinimumFee(1000, probe_cc, nullptr, nullptr);
    const CAmount entry = std::max(wallet_min, std::max(c.next_block_min, c.relay_min));
    m_child_vsize = kChildSizeGuess;
    m_reference_per_kvb = entry * 5 * (m_parent_vsize + m_child_vsize) / m_child_vsize;

    m_parent_rate_reference = m_parent_vsize > 0 ? m_parent_fee_reference * 1000 / m_parent_vsize : 0;
    m_parent_age = wtx.time > 0 ? (QDateTime::currentSecsSinceEpoch() - wtx.time) : 0;
    m_subject->setText(tr("The stuck transaction: unconfirmed since %1, %2 bytes, fee %3.")
                           .arg(GUIUtil::dateTimeStr(wtx.time),
                                QString::number(m_parent_vsize),
                                GUIUtil::formatAssetAmount(m_parent_fee_asset, m_parent_fee_atoms, unit,
                                                           BitcoinUnits::SeparatorStyle::STANDARD, true)));
    onOutputChanged();
    return true;
}

uint32_t ChildPaysDialog::outputIndex() const
{
    const int i = m_output->currentIndex();
    return (i >= 0 && i < (int)m_candidates.size()) ? m_candidates[i].n : 0;
}

CAsset ChildPaysDialog::pinnedAsset() const
{
    const int i = m_output->currentIndex();
    return (i >= 0 && i < (int)m_candidates.size()) ? m_candidates[i].asset : CAsset();
}

QString ChildPaysDialog::address() const { return m_address->text().trimmed(); }

CAsset ChildPaysDialog::feeAsset() const
{
    const CAsset a = GetAssetFromString(m_fee_asset->currentData().toString().toStdString());
    return a.IsNull() ? ::policyAsset : a;
}

CAsset ChildPaysDialog::selectedFeeAsset() const { return feeAsset(); }

bool ChildPaysDialog::parsedAmount(CAmount& out) const
{
    return GUIUtil::parseAssetAmount(pinnedAsset(), m_amount->text(),
                                     m_model->getOptionsModel()->getDisplayUnit(), &out) && out > 0;
}

CAmount ChildPaysDialog::amount() const { CAmount a = 0; parsedAmount(a); return a; }

void ChildPaysDialog::onOutputChanged()
{
    const int i = m_output->currentIndex();
    if (i < 0 || i >= (int)m_candidates.size()) return;
    const Candidate& c = m_candidates[i];
    const int unit = m_model->getOptionsModel()->getDisplayUnit();
    m_amount->setText(GUIUtil::formatAssetAmount(c.asset, c.value, unit,
                                                 BitcoinUnits::SeparatorStyle::NEVER,
                                                 /*include_asset_name=*/false));
    m_output_hint->setText(tr("The child spends this output, so it is this output's %1 that moves. "
                              "Which one carries the child changes nothing about the fee.")
                               .arg(GUIUtil::assetDisplayName(c.asset)));
    if (m_debounce) m_debounce->start();
}

void ChildPaysDialog::onRecipientEdited() { if (m_debounce) m_debounce->start(); }
void ChildPaysDialog::onFeeAssetChanged() { if (m_debounce) m_debounce->start(); }

void ChildPaysDialog::onFeeCellEdited()
{
    if (m_updating) return;
    auto* source = qobject_cast<QLineEdit*>(sender());
    if (!source) return;
    const CAsset asset = selectedFeeAsset();
    const FeeAssetInfo info = m_model->node().getFeeAssetInfo(asset);
    if (info.rate <= 0) return;
    const double factor = GUIUtil::atomsPerUnit(info.precision);
    const double unit_price = info.has_market_price ? info.market_price : 0.0;

    bool ok = false;
    const double typed = source->text().trimmed().toDouble(&ok);
    if (!ok || typed < 0.0) return;

    double atoms_per_kvb = 0.0;
    if (source == m_cell_child[RATE_ASSET]) {
        atoms_per_kvb = typed * factor;
    } else if (source == m_cell_child[RATE_REF]) {
        if (!(unit_price > 0.0)) return;
        atoms_per_kvb = typed / unit_price * factor;
    } else if (source == m_cell_child[TOTAL_ASSET]) {
        if (m_child_vsize <= 0) return;
        atoms_per_kvb = typed * factor * 1000.0 / static_cast<double>(m_child_vsize);
    } else if (source == m_cell_child[TOTAL_REF]) {
        if (m_child_vsize <= 0 || !(unit_price > 0.0)) return;
        atoms_per_kvb = typed / unit_price * factor * 1000.0 / static_cast<double>(m_child_vsize);
    } else {
        return;
    }

    m_reference_per_kvb = std::max<CAmount>(0, static_cast<CAmount>(std::llround(
        atoms_per_kvb * static_cast<double>(info.rate) / static_cast<double>(exchange_rate_scale))));

    m_editing = source;
    m_skip_draft = true;
    recompute();
    m_skip_draft = false;
    m_editing = nullptr;
    if (m_debounce) m_debounce->start();
}

void ChildPaysDialog::recompute()
{
    if (!m_parent || !m_model) return;

    const CAsset fee_asset = selectedFeeAsset();
    const FeeAssetInfo info = m_model->node().getFeeAssetInfo(fee_asset);
    const FeeAssetInfo pinfo = m_model->node().getFeeAssetInfo(m_parent_fee_asset);
    const MempoolCongestion c = m_model->node().getMempoolCongestion();
    const QString ref = GUIUtil::referenceCurrency();
    const QString asset_name = GUIUtil::assetDisplayName(fee_asset);
    const double factor = GUIUtil::atomsPerUnit(info.precision);
    const double price = info.has_market_price ? info.market_price : 0.0;
    const double pfactor = GUIUtil::atomsPerUnit(pinfo.precision);
    const double pprice = pinfo.has_market_price ? pinfo.market_price : 0.0;
    const bool same_fee_asset = (fee_asset == m_parent_fee_asset);

    m_row_label[RATE_ASSET]->setText(tr("Per 1000 bytes (%1)").arg(asset_name));
    m_row_label[RATE_REF]->setText(tr("Per 1000 bytes (%1)").arg(ref));
    m_row_label[TOTAL_ASSET]->setText(tr("Total (%1)").arg(asset_name));
    m_row_label[TOTAL_REF]->setText(tr("Total (%1)").arg(ref));

    CAmount send_amount = 0;
    const bool recipient_ok = m_model->validateAddress(address()) && parsedAmount(send_amount);
    m_probe_error.clear();
    if (recipient_ok && info.accepted && !m_skip_draft) {
        int64_t probed = 0;
        QString err;
        if (m_model->probeChildSize(m_parent_hash, outputIndex(), address(), send_amount,
                                    fee_asset, m_reference_per_kvb, probed, err) && probed > 0) {
            m_child_vsize = probed;
            m_child_vsize_is_estimate = false;
        } else {
            m_child_vsize = kChildSizeGuess;
            m_child_vsize_is_estimate = true;
            m_probe_error = err;
        }
    }

    const CAmount child_rate_atoms = CFeeRate(m_reference_per_kvb).GetFee(1000, fee_asset);
    const double child_total_atoms = std::ceil(static_cast<double>(child_rate_atoms) *
                                               static_cast<double>(m_child_vsize) / 1000.0);
    const CAmount child_total_reference = static_cast<CAmount>(std::ceil(
        static_cast<double>(m_reference_per_kvb) * static_cast<double>(m_child_vsize) / 1000.0));

    // What a producer actually weighs: both fees over both sizes. Computed in
    // reference atoms because that is where the node compares them, and the two
    // transactions need not pay in the same asset -- which is the whole point of
    // being able to choose the child's.
    const int64_t package_vsize = m_parent_vsize + m_child_vsize;
    const CAmount package_reference = m_parent_fee_reference + child_total_reference;
    const CAmount package_rate_reference = package_vsize > 0
        ? static_cast<CAmount>(package_reference * 1000 / package_vsize) : 0;
    const CAmount package_rate_atoms = CFeeRate(package_rate_reference).GetFee(1000, fee_asset);
    const CAmount package_total_atoms = CFeeRate(package_reference).GetFee(1000, fee_asset);

    const double parent_rate_atoms = m_parent_vsize > 0
        ? static_cast<double>(m_parent_fee_atoms) * 1000.0 / static_cast<double>(m_parent_vsize) : 0.0;

    m_updating = true;
    auto put = [this](QLineEdit* cell, const QString& text) {
        if (cell != m_editing) cell->setText(text);
    };
    const QString dash = QStringLiteral("—");

    put(m_cell_child[RATE_ASSET], GUIUtil::formatUnits(child_rate_atoms / factor, info.precision));
    put(m_cell_child[RATE_REF], price > 0.0 ? refFigure(child_rate_atoms / factor * price) : QString());
    put(m_cell_child[TOTAL_ASSET], GUIUtil::formatUnits(child_total_atoms / factor, info.precision));
    put(m_cell_child[TOTAL_REF], price > 0.0 ? refFigure(child_total_atoms / factor * price) : QString());

    put(m_cell_parent[RATE_ASSET], same_fee_asset
        ? GUIUtil::formatUnits(parent_rate_atoms / pfactor, pinfo.precision) : dash);
    put(m_cell_parent[RATE_REF], pprice > 0.0 ? refFigure(parent_rate_atoms / pfactor * pprice) : dash);
    put(m_cell_parent[TOTAL_ASSET], same_fee_asset
        ? GUIUtil::formatUnits(static_cast<double>(m_parent_fee_atoms) / pfactor, pinfo.precision) : dash);
    put(m_cell_parent[TOTAL_REF], pprice > 0.0
        ? refFigure(static_cast<double>(m_parent_fee_atoms) / pfactor * pprice) : dash);

    put(m_cell_package[RATE_ASSET], GUIUtil::formatUnits(package_rate_atoms / factor, info.precision));
    put(m_cell_package[RATE_REF], price > 0.0 ? refFigure(package_rate_atoms / factor * price) : QString());
    put(m_cell_package[TOTAL_ASSET], GUIUtil::formatUnits(package_total_atoms / factor, info.precision));
    put(m_cell_package[TOTAL_REF], price > 0.0 ? refFigure(package_total_atoms / factor * price) : QString());

    m_cell_child[RATE_ASSET]->setEnabled(info.accepted);
    m_cell_child[RATE_REF]->setEnabled(info.accepted && price > 0.0);
    m_cell_child[TOTAL_ASSET]->setEnabled(info.accepted);
    m_cell_child[TOTAL_REF]->setEnabled(info.accepted && price > 0.0);
    for (int i = 0; i < ROW_COUNT; ++i) {
        m_cell_child[i]->setStyleSheet(m_cell_child[i]->isEnabled() ? QString(QLatin1String(kEditableCell)) : QString());
    }
    m_updating = false;

    // The cut the package has to beat. Below it the child is not refused -- it is
    // a perfectly valid transaction -- it simply does not buy what it was made
    // for, so this warns rather than blocks.
    const CAmount cut = std::max(c.next_block_min, c.relay_min);
    const CAmount cut_atoms = CFeeRate(cut).GetFee(1000, fee_asset);
    QString cut_text = GUIUtil::formatUnits(static_cast<double>(cut_atoms) / factor, info.precision) +
                       QLatin1Char(' ') + asset_name;
    if (price > 0.0) {
        cut_text += QStringLiteral(" (≈ ") + refFigure(static_cast<double>(cut_atoms) / factor * price) +
                    QLatin1Char(' ') + ref + QLatin1Char(')');
    }

    // A child rescues a parent that is too CHEAP. It does nothing for one that is
    // inadmissible: a producer whose whitelist has no entry for the parent's fee
    // asset does not weigh the pair and discount it, it refuses the parent
    // outright, and the child goes nowhere with it. So this is the first thing
    // said, because it changes the remedy from "pay more" to "replace it".
    const bool parent_unvaluable = !pinfo.accepted ||
                                   !pinfo.has_market_price ||
                                   (pinfo.registry_available && !pinfo.registry_listed);
    if (m_parent_age > 4 * GUIUtil::nominalBlockSpacing() &&
        m_parent_rate_reference >= std::max(c.next_block_min, c.relay_min) && !parent_unvaluable) {
        // It is already paying more than recent blocks took, so it is not short
        // of money and a child adds none of what it is actually short of.
        const int waited_minutes = static_cast<int>(m_parent_age / 60);
        const QString waited = waited_minutes == 1 ? tr("1 minute") : tr("%1 minutes").arg(waited_minutes);
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("The stuck transaction has been waiting %1 while already paying above what "
                              "recent blocks took, so it is not short of fee — and a child only adds fee. "
                              "It is either not reaching the producers or being refused by them; a "
                              "replacement that pays in an asset they accept is what changes that.")
                               .arg(waited));
    } else if (parent_unvaluable) {
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        // The wording matters more than the finding. "Cannot show to be valued"
        // is true and reads as a limitation of this node; what lands is the Send
        // tab's own sentence -- it may confirm only in a block THIS node produces
        // -- because that is the consequence the user is waiting on. Said the
        // careful way, a user who knows the asset is in his own whitelist reads
        // the warning and reasonably concludes it does not apply to him.
        m_verdict->setText(tr("The stuck transaction pays its fee in %1, and no price server this node reads "
                              "quotes it — so a producer whose whitelist has no entry for %1 refuses it "
                              "outright, and it may confirm only in a block this node produces. A child "
                              "cannot change that: it pays MORE for the parent, not in another asset. "
                              "Replacing the parent with a fee in an asset producers accept is what moves it.")
                               .arg(GUIUtil::assetDisplayName(m_parent_fee_asset)));
    } else if (!info.accepted) {
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("This node does not accept fees in %1, so the child would not travel either.")
                               .arg(asset_name));
    } else if (!recipient_ok) {
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("Enter a valid address and amount for the child."));
    } else if (!info.has_market_price || (info.registry_available && !info.registry_listed)) {
        // See the replacement window: "unpriced" is a finding, "unlisted with no
        // registry to ask" is not. Same reasoning as there: a child exists to be mined by
        // SOMEBODY ELSE, so an unpriced fee asset is not a footnote here -- it is
        // the answer.
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("This node has no published price for %1, so the price servers other producers "
                              "run cannot value the child's fee — however generous it looks here. Pay the "
                              "child in an asset they do value, or the child waits beside the parent.")
                               .arg(asset_name));
    } else if (package_rate_reference >= cut) {
        m_verdict->setStyleSheet(QStringLiteral("color:#7ec87e;"));
        m_verdict->setText(tr("Together they pay above what the next block is taking (%1 per 1000 bytes), "
                              "so a producer has reason to take both.").arg(cut_text));
    } else {
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("Together they still pay less than the next block is taking (%1 per 1000 "
                              "bytes). The child would sit beside the parent instead of carrying it.")
                               .arg(cut_text));
    }

    QStringList notes;
    if (info.accepted) {
        const QString travel = GUIUtil::feeAssetTravelNote(asset_name, info.registry_available,
                                                           info.registry_listed, info.has_market_price);
        // The child is made to be mined by somebody else, so whether its asset
        // travels is the whole question -- more so here than anywhere.
        if (!travel.isEmpty()) notes << travel;
    }
    if (m_parent_fee_reference == 0 && m_parent_fee_atoms > 0) {
        notes << tr("This node no longer prices %1, so the stuck transaction's fee counts as nothing here "
                    "and the package figures credit it with nothing.")
                     .arg(GUIUtil::assetDisplayName(m_parent_fee_asset));
    }
    if (!same_fee_asset) {
        notes << tr("The child pays in %1 and the stuck transaction paid in %2, so those two asset figures "
                    "cannot be added up: \"Both together\" states what the pair is worth, in %1, at this "
                    "node's rates — and the %3 rows are the ones that compare directly. Paying the child in "
                    "an asset producers accept is the whole point: it is what the stuck one may have got "
                    "wrong.").arg(asset_name, GUIUtil::assetDisplayName(m_parent_fee_asset), ref);
    }
    if (m_child_vsize_is_estimate) {
        notes << (m_probe_error.isEmpty()
            ? tr("The child's size is an estimate (%1 bytes) until the wallet can draft it.")
                  .arg(QString::number(m_child_vsize))
            : tr("The child's size is an estimate (%1 bytes): the wallet could not draft it (%2).")
                  .arg(QString::number(m_child_vsize), m_probe_error));
    }
    m_notes->setText(notes.join(QLatin1Char('\n')));
    m_notes->setVisible(!notes.isEmpty());

    m_fee_asset_hint->setText(info.accepted
        ? tr("The child only helps if producers will take its fee, which is what the stuck transaction may "
             "have got wrong. This does not have to be the asset the parent paid in, nor the one being spent.")
        : tr("Pick an asset this node accepts, or the child cannot be relayed."));

    m_address_hint->setStyleSheet(QStringLiteral("color:#888;"));
    m_address_hint->setText(tr("A new address of this wallet: the child exists for its fee, not to move money. "
                               "You can send it somewhere else."));

    if (QPushButton* ok_button = m_buttons->button(QDialogButtonBox::Ok)) {
        ok_button->setEnabled(recipient_ok && info.accepted);
    }
}
