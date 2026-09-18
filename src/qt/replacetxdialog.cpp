// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/replacetxdialog.h>

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
#include <wallet/wallet.h> // WALLET_INCREMENTAL_RELAY_FEE

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
//! The four ways one fee is said here: as a rate and as this transaction's
//! total, each in the asset that pays and in the reference currency. The same
//! four as the Send tab's fee grid, in the same order.
enum Row { RATE_ASSET = 0, RATE_REF = 1, TOTAL_ASSET = 2, TOTAL_REF = 3, ROW_COUNT = 4 };

const char* kEditableCell = "QLineEdit { color:#ffb84d; border:1px solid #ffb84d; }";
const char* kQuietCell = "QLineEdit { color:#aaa; }";

//! Eight decimals, as the Send tab's reference column: a fee is routinely worth
//! a small fraction of a cent, and rounding to cents prints 0.00 for every
//! figure this window exists to compare.
QString refFigure(double value) { return QString::number(value, 'f', 8); }
} // namespace

ReplaceTxDialog::ReplaceTxDialog(WalletModel* model, const uint256& hash, Mode mode, QWidget* parent)
    : QDialog(parent), m_model(model), m_hash(hash), m_mode(mode)
{
    setWindowTitle(m_mode == Mode::Bump ? tr("Increase transaction fee (RBF)")
                                        : tr("Replace transaction (RBF)"));
    buildUi();
    if (!loadOriginal()) return;
    if (m_mode == Mode::Bump) {
        // The payment is not in question here, so it is not shown -- and cannot
        // be altered by accident while the user is aiming at the fee.
        for (QWidget* w : m_payment_widgets) w->setVisible(false);
    }

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, &ReplaceTxDialog::recompute);

    recompute();
}

void ReplaceTxDialog::buildUi()
{
    auto* lay = new QVBoxLayout(this);

    auto* intro = new QLabel(m_mode == Mode::Bump
        ? tr("Re-sends this payment untouched, with a fee you choose. Recipient and amount stay exactly "
             "as they are; only what it pays a producer changes.")
        : tr("Re-spends this transaction's inputs, replacing it before it confirms. Recipient, amount and "
             "asset start as the original's — what a replacement normally changes is only the fee."), this);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    // Which transaction this is about. A window that acts on the row you
    // right-clicked, without naming it, is one you have to trust rather than
    // check -- and by the time it is open the list behind it may have moved.
    m_subject = new QLabel(this);
    m_subject->setWordWrap(true);
    m_subject->setStyleSheet(QStringLiteral("color:#aaa;"));
    lay->addWidget(m_subject);

    auto* form = new QGridLayout();
    form->setHorizontalSpacing(12);
    int r = 0;
    auto* address_label = new QLabel(tr("Send to:"), this);
    form->addWidget(address_label, r, 0);
    m_address = new QLineEdit(this);
    form->addWidget(m_address, r, 1, 1, 2);
    ++r;
    m_address_hint = new QLabel(this);
    m_address_hint->setWordWrap(true);
    form->addWidget(m_address_hint, r, 1, 1, 2);
    ++r;
    auto* amount_label = new QLabel(tr("Amount:"), this);
    form->addWidget(amount_label, r, 0);
    // Asset first, then the figure -- same order as the Send tab, and for the
    // same reason: a number typed before its asset is chosen is a number counted
    // in the wrong thing.
    m_asset = new QComboBox(this);
    form->addWidget(m_asset, r, 1);
    m_amount = new QLineEdit(this);
    form->addWidget(m_amount, r, 2);
    ++r;
    m_amount_hint = new QLabel(this);
    m_amount_hint->setWordWrap(true);
    form->addWidget(m_amount_hint, r, 1, 1, 2);
    ++r;
    m_payment_widgets << address_label << m_address << m_address_hint
                      << amount_label << m_asset << m_amount << m_amount_hint;
    form->addWidget(new QLabel(tr("Pay the fee in:"), this), r, 0);
    m_fee_asset = new QComboBox(this);
    form->addWidget(m_fee_asset, r, 1, 1, 2);
    lay->addLayout(form);

    m_recipients_note = new QLabel(this);
    m_recipients_note->setWordWrap(true);
    m_recipients_note->setStyleSheet(QStringLiteral("color:#ffb84d;"));
    m_recipients_note->setVisible(false);
    lay->addWidget(m_recipients_note);

    // Three columns, because the question here is never "what does this cost" on
    // its own but "does it beat the one that is stuck".
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(12);
    // Rows of boxed figures need air between them: with the default spacing the
    // boxes touch and the digits get clipped by the row below.
    grid->setVerticalSpacing(8);
    grid->setContentsMargins(0, 10, 0, 6);
    auto* h1 = new QLabel(tr("Original"), this);
    auto* h2 = new QLabel(tr("Replacement"), this);
    auto* h3 = new QLabel(tr("Difference"), this);
    for (QLabel* h : {h1, h2, h3}) h->setStyleSheet(QStringLiteral("font-weight:bold;"));
    grid->addWidget(h1, 0, 1);
    grid->addWidget(h2, 0, 2);
    grid->addWidget(h3, 0, 3);
    for (int i = 0; i < ROW_COUNT; ++i) {
        m_row_label[i] = new QLabel(this);
        grid->addWidget(m_row_label[i], i + 1, 0);
        m_cell_original[i] = new QLineEdit(this);
        m_cell_original[i]->setMinimumHeight(26);
        m_cell_original[i]->setReadOnly(true);
        m_cell_original[i]->setStyleSheet(QLatin1String(kQuietCell));
        grid->addWidget(m_cell_original[i], i + 1, 1);
        m_cell_new[i] = new QLineEdit(this);
        m_cell_new[i]->setMinimumHeight(26);
        grid->addWidget(m_cell_new[i], i + 1, 2);
        m_cell_delta[i] = new QLineEdit(this);
        m_cell_delta[i]->setMinimumHeight(26);
        m_cell_delta[i]->setReadOnly(true);
        m_cell_delta[i]->setStyleSheet(QLatin1String(kQuietCell));
        grid->addWidget(m_cell_delta[i], i + 1, 3);
        connect(m_cell_new[i], &QLineEdit::textEdited, this, &ReplaceTxDialog::onFeeCellEdited);
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

    connect(m_address, &QLineEdit::textEdited, this, &ReplaceTxDialog::onRecipientEdited);
    connect(m_amount, &QLineEdit::textEdited, this, &ReplaceTxDialog::onRecipientEdited);
    connect(m_asset, qOverload<int>(&QComboBox::currentIndexChanged), this, &ReplaceTxDialog::onRecipientEdited);
    connect(m_fee_asset, qOverload<int>(&QComboBox::currentIndexChanged), this, &ReplaceTxDialog::onFeeAssetChanged);
}

bool ReplaceTxDialog::loadOriginal()
{
    m_orig = m_model->wallet().getTx(m_hash);
    if (!m_orig) return false;

    m_orig_fee_asset = m_orig->GetFeeAsset(::policyAsset);
    m_orig_fee_atoms = GetFeeMap(*m_orig)[m_orig_fee_asset];
    m_orig_vsize = GetVirtualTransactionSize(*m_orig);
    const FeeAssetInfo old_info = m_model->node().getFeeAssetInfo(m_orig_fee_asset);
    // What this node's mempool valued the stuck fee at. Everything the BIP125
    // test compares happens in that unit, whichever assets the two transactions
    // pay in -- which is why a dollar figure can look like an improvement and
    // still not be one.
    m_orig_fee_reference = old_info.rate > 0
        ? static_cast<CAmount>(std::llround(static_cast<double>(m_orig_fee_atoms) *
                                            static_cast<double>(old_info.rate) /
                                            static_cast<double>(exchange_rate_scale)))
        : 0;

    interfaces::WalletTxStatus st;
    interfaces::WalletOrderForm of;
    bool in_mempool = false;
    int nblocks = 0;
    const interfaces::WalletTx wtx = m_model->wallet().getWalletTxDetails(m_hash, st, of, in_mempool, nblocks);
    m_orig_out_of_mempool = !in_mempool && st.depth_in_main_chain == 0;

    // The payment the original was making: its outputs, less the fee and less
    // change. Change is read from txout_is_change, the same flag the transaction
    // list uses, so the recipient prefilled here is the one the list shows rather
    // than a second opinion about which output was the payment.
    for (size_t i = 0; i < m_orig->vout.size(); ++i) {
        if (m_orig->vout[i].IsFee()) continue;
        if (i < wtx.txout_is_change.size() && wtx.txout_is_change[i]) continue;
        const CAmount value = i < wtx.txout_amounts.size() ? wtx.txout_amounts[i] : 0;
        ++m_orig_recipients;
        if (m_orig_recipients == 1 || value > m_orig_amount) {
            m_orig_amount = value;
            m_orig_asset = i < wtx.txout_assets.size() ? wtx.txout_assets[i] : CAsset();
            m_orig_confidential = !m_orig->vout[i].nValue.IsExplicit();
            if (i < wtx.txout_address.size() && !std::get_if<CNoDestination>(&wtx.txout_address[i])) {
                m_orig_address = QString::fromStdString(EncodeDestination(wtx.txout_address[i]));
            }
        }
    }

    // Does this payment stand on a coin the network cannot see? An input still in
    // somebody's mempool is an input peers will not find, and every replacement
    // inherits it: the fee stops being the variable that matters.
    for (const CTxIn& txin : m_orig->vin) {
        interfaces::WalletTxStatus pst;
        int pblocks = 0;
        int64_t ptime = 0;
        if (m_model->wallet().tryGetTxStatus(txin.prevout.hash, pst, pblocks, ptime) &&
            pst.depth_in_main_chain == 0) {
            m_unconfirmed_parent = QString::fromStdString(txin.prevout.hash.ToString());
            break;
        }
    }

    const int unit = m_model->getOptionsModel()->getDisplayUnit();
    m_address->setText(m_orig_address);
    if (m_orig_amount > 0 && !m_orig_asset.IsNull()) {
        m_amount->setText(GUIUtil::formatAssetAmount(m_orig_asset, m_orig_amount, unit,
                                                     BitcoinUnits::SeparatorStyle::NEVER,
                                                     /*include_asset_name=*/false));
    }

    if (g_con_any_asset_fees) {
        for (const CAsset& a : m_model->getAssetTypes()) {
            m_asset->addItem(GUIUtil::assetDisplayName(a), QString::fromStdString(a.GetHex()));
        }
        const int at = m_asset->findData(QString::fromStdString(m_orig_asset.GetHex()));
        if (at >= 0) m_asset->setCurrentIndex(at);
    } else {
        m_asset->addItem(BitcoinUnits::policyAssetTicker(), QString::fromStdString(::policyAsset.GetHex()));
        m_asset->setEnabled(false);
    }

    m_fee_asset->addItem(tr("Keep original (%1)").arg(GUIUtil::assetDisplayName(m_orig_fee_asset)),
                         QString::fromStdString(m_orig_fee_asset.GetHex()));
    if (g_con_any_asset_fees) {
        // A fee goes into a producer's coinbase, so a reissuance token is never
        // offered here -- see WalletModel::getFeePayableAssetTypes.
        for (const CAsset& a : m_model->getFeePayableAssetTypes()) {
            if (a == m_orig_fee_asset) continue;
            m_fee_asset->addItem(GUIUtil::assetDisplayName(a), QString::fromStdString(a.GetHex()));
        }
    } else {
        m_fee_asset->setEnabled(false);
    }

    if (m_orig_recipients > 1) {
        m_recipients_note->setText(tr("The original pays %n recipients, and a replacement made here pays only the "
                                      "one above — the others would not be paid at all.", "", m_orig_recipients));
        m_recipients_note->setVisible(true);
    }

    // Opening rate. Derived from the REQUIREMENT and not from the original's rate
    // plus an increment: the node's test is on the absolute fee, and a rate
    // converted back through a size and rounded down lands a couple of atoms
    // under it -- which opened this window refusing to send, on a figure it had
    // proposed itself. Then one whole increment of headroom on top, because a
    // replacement that clears the floor exactly clears a floor that moves: the
    // fee is compared against the original, but getting MINED is a competition
    // with whatever arrives next. Aiming at the cut with nothing to spare is how
    // a bump pays more and still waits.
    const MempoolCongestion c = m_model->node().getMempoolCongestion();
    // See recompute(): a bump has to clear the wallet's own increment, which is
    // the higher of the two, or the window opens on a figure the wallet refuses.
    const CAmount seed_increment_per_kvb = m_mode == Mode::Bump
        ? std::max<CAmount>(c.replacement_min, wallet::WALLET_INCREMENTAL_RELAY_FEE)
        : c.replacement_min;
    const CAmount increment = static_cast<CAmount>(std::ceil(
        static_cast<double>(seed_increment_per_kvb) * static_cast<double>(m_orig_vsize) / 1000.0));
    const CAmount floor_total = m_orig_fee_reference + increment;
    const CAmount from_floor = m_orig_vsize > 0
        ? static_cast<CAmount>(std::ceil(static_cast<double>(floor_total) * 1000.0 /
                                         static_cast<double>(m_orig_vsize)))
        : 0;
    m_reference_per_kvb = std::max<CAmount>(from_floor + seed_increment_per_kvb,
                                            std::max(c.next_block_min + seed_increment_per_kvb, c.relay_min));
    m_orig_rate_reference = m_orig_vsize > 0 ? m_orig_fee_reference * 1000 / m_orig_vsize : 0;
    m_orig_age = wtx.time > 0 ? (QDateTime::currentSecsSinceEpoch() - wtx.time) : 0;
    m_vsize = m_orig_vsize; // until a draft of the replacement says otherwise

    QString subject = tr("Unconfirmed since %1, %2 bytes, fee %3.")
        .arg(GUIUtil::dateTimeStr(wtx.time),
             QString::number(m_orig_vsize),
             GUIUtil::formatAssetAmount(m_orig_fee_asset, m_orig_fee_atoms, unit,
                                        BitcoinUnits::SeparatorStyle::STANDARD, /*include_asset_name=*/true));
    if (m_orig_amount > 0 && !m_orig_asset.IsNull()) {
        subject = tr("Paying %1%2. ")
                      .arg(GUIUtil::formatAssetAmount(m_orig_asset, m_orig_amount, unit,
                                                      BitcoinUnits::SeparatorStyle::STANDARD, true),
                           m_orig_address.isEmpty() ? QString()
                                                    : tr(" to %1").arg(m_orig_address)) + subject;
    }
    m_subject->setText(subject);
    return true;
}

CAsset ReplaceTxDialog::selectedSendAsset() const
{
    if (!g_con_any_asset_fees) return ::policyAsset;
    const CAsset a = GetAssetFromString(m_asset->currentData().toString().toStdString());
    return a.IsNull() ? ::policyAsset : a;
}

CAsset ReplaceTxDialog::selectedFeeAsset() const
{
    const CAsset a = GetAssetFromString(m_fee_asset->currentData().toString().toStdString());
    return a.IsNull() ? ::policyAsset : a;
}

bool ReplaceTxDialog::parsedAmount(CAmount& out) const
{
    return GUIUtil::parseAssetAmount(selectedSendAsset(), m_amount->text(),
                                     m_model->getOptionsModel()->getDisplayUnit(), &out) && out > 0;
}

CAmount ReplaceTxDialog::toAsset(CAmount reference_atoms, const CAsset& asset) const
{
    // CFeeRate carries reference atoms per kvB, and GetFee(1000, asset) takes
    // 1000 bytes of it -- the figure itself -- then converts at this node's
    // whitelist rate. That is the conversion the mempool performs, not a display
    // one: a figure converted any other way could be shown as accepted here and
    // refused there.
    return CFeeRate(reference_atoms).GetFee(1000, asset);
}

QString ReplaceTxDialog::address() const { return m_address->text().trimmed(); }
CAsset ReplaceTxDialog::sendAsset() const { return selectedSendAsset(); }
CAmount ReplaceTxDialog::amount() const { CAmount a = 0; parsedAmount(a); return a; }
CAsset ReplaceTxDialog::feeAsset() const { return selectedFeeAsset(); }

void ReplaceTxDialog::onRecipientEdited() { if (m_debounce) m_debounce->start(); }
void ReplaceTxDialog::onFeeAssetChanged() { if (m_debounce) m_debounce->start(); }

void ReplaceTxDialog::onFeeCellEdited()
{
    if (m_updating) return;
    auto* source = qobject_cast<QLineEdit*>(sender());
    if (!source) return;
    const CAsset asset = selectedFeeAsset();
    const FeeAssetInfo info = m_model->node().getFeeAssetInfo(asset);
    if (info.rate <= 0) return;
    const double factor = GUIUtil::atomsPerUnit(info.precision);
    // Same price the cell was filled with, or the round trip lies.
    const double unit_price = info.has_market_price ? info.market_price : 0.0;

    bool ok = false;
    const double typed = source->text().trimmed().toDouble(&ok);
    if (!ok || typed < 0.0) return;

    // One figure underneath all four cells: atoms of the fee asset per 1000
    // bytes. Whichever cell was typed into is converted back to it.
    double atoms_per_kvb = 0.0;
    if (source == m_cell_new[RATE_ASSET]) {
        atoms_per_kvb = typed * factor;
    } else if (source == m_cell_new[RATE_REF]) {
        if (!(unit_price > 0.0)) return;
        atoms_per_kvb = typed / unit_price * factor;
    } else if (source == m_cell_new[TOTAL_ASSET]) {
        if (m_vsize <= 0) return;
        atoms_per_kvb = typed * factor * 1000.0 / static_cast<double>(m_vsize);
    } else if (source == m_cell_new[TOTAL_REF]) {
        if (m_vsize <= 0 || !(unit_price > 0.0)) return;
        atoms_per_kvb = typed / unit_price * factor * 1000.0 / static_cast<double>(m_vsize);
    } else {
        return;
    }

    m_reference_per_kvb = std::max<CAmount>(0, static_cast<CAmount>(std::llround(
        atoms_per_kvb * static_cast<double>(info.rate) / static_cast<double>(exchange_rate_scale))));

    // Restate the other three now, leaving the one being typed in alone: its text
    // belongs to the user, and rewriting it would jump the cursor to the end on
    // every keystroke.
    m_editing = source;
    m_skip_draft = true;
    recompute();
    m_skip_draft = false;
    m_editing = nullptr;
    // And re-measure once typing stops: a different fee buys different coins,
    // which is a different size.
    if (m_debounce) m_debounce->start();
}

void ReplaceTxDialog::recompute()
{
    if (!m_orig || !m_model) return;

    const CAsset fee_asset = selectedFeeAsset();
    const FeeAssetInfo info = m_model->node().getFeeAssetInfo(fee_asset);
    const FeeAssetInfo old_info = m_model->node().getFeeAssetInfo(m_orig_fee_asset);
    const MempoolCongestion c = m_model->node().getMempoolCongestion();
    const QString ref = GUIUtil::referenceCurrency();
    const QString asset_name = GUIUtil::assetDisplayName(fee_asset);
    const double factor = GUIUtil::atomsPerUnit(info.precision);
    const double price = info.has_market_price ? info.market_price : 0.0;
    const double old_factor = GUIUtil::atomsPerUnit(old_info.precision);
    const double old_price = old_info.has_market_price ? old_info.market_price : 0.0;
    const bool same_fee_asset = (fee_asset == m_orig_fee_asset);

    m_row_label[RATE_ASSET]->setText(tr("Per 1000 bytes (%1)").arg(asset_name));
    m_row_label[RATE_REF]->setText(tr("Per 1000 bytes (%1)").arg(ref));
    m_row_label[TOTAL_ASSET]->setText(tr("Total (%1)").arg(asset_name));
    m_row_label[TOTAL_REF]->setText(tr("Total (%1)").arg(ref));

    // A fee is a price on size, and the size is not known until the wallet has
    // put the transaction together -- so it drafts one, uncommitted, exactly as
    // the Send tab does to fill its own total. When that cannot be done the
    // original's size stands in, marked as the estimate it is: a replacement pins
    // the same inputs and usually pays the same outputs, so it is close -- but
    // changing the fee asset adds an input and a change output, and then it isn't.
    CAmount send_amount = 0;
    // In Bump mode there is no recipient to validate: the wallet re-signs the
    // payment that is already there, and the draft comes from the wallet's own
    // bump path rather than from a transaction built here -- so the size shown is
    // the size of the transaction that will actually be sent.
    const bool recipient_ok = m_mode == Mode::Bump
        ? true
        : (m_model->validateAddress(address()) && parsedAmount(send_amount));
    m_probe_error.clear();
    if (m_mode == Mode::Bump && info.accepted && !m_skip_draft) {
        int64_t probed = 0;
        CAmount probed_fee = 0;
        QString err;
        if (m_model->probeBumpSize(m_hash, fee_asset, m_reference_per_kvb, probed, probed_fee, err) && probed > 0) {
            m_vsize = probed;
            m_vsize_is_estimate = false;
        } else {
            m_vsize = m_orig_vsize;
            m_vsize_is_estimate = true;
            m_probe_error = err;
        }
    } else if (recipient_ok && info.accepted && !m_skip_draft) {
        int64_t probed = 0;
        QString err;
        if (m_model->probeReplacementSize(m_hash, address(), selectedSendAsset(), send_amount,
                                          fee_asset, m_reference_per_kvb, probed, err) && probed > 0) {
            m_vsize = probed;
            m_vsize_is_estimate = false;
        } else {
            m_vsize = m_orig_vsize;
            m_vsize_is_estimate = true;
            m_probe_error = err;
        }
    } else if (!m_skip_draft) {
        m_vsize = m_orig_vsize;
        m_vsize_is_estimate = true;
    }

    const CAmount new_rate_atoms = toAsset(m_reference_per_kvb, fee_asset);
    const double new_total_atoms = m_vsize > 0
        ? std::ceil(static_cast<double>(new_rate_atoms) * static_cast<double>(m_vsize) / 1000.0) : 0.0;
    const CAmount new_total_reference = m_vsize > 0
        ? static_cast<CAmount>(std::ceil(static_cast<double>(m_reference_per_kvb) *
                                         static_cast<double>(m_vsize) / 1000.0)) : 0;
    const double old_rate_atoms = m_orig_vsize > 0
        ? static_cast<double>(m_orig_fee_atoms) * 1000.0 / static_cast<double>(m_orig_vsize) : 0.0;

    m_updating = true;
    auto put = [this](QLineEdit* cell, const QString& text) {
        if (cell != m_editing) cell->setText(text);
    };
    const QString dash = QStringLiteral("—");

    put(m_cell_new[RATE_ASSET], GUIUtil::formatUnits(new_rate_atoms / factor, info.precision));
    put(m_cell_new[RATE_REF], price > 0.0 ? refFigure(new_rate_atoms / factor * price) : QString());
    put(m_cell_new[TOTAL_ASSET], m_vsize > 0
        ? GUIUtil::formatUnits(new_total_atoms / factor, info.precision) : dash);
    put(m_cell_new[TOTAL_REF], (m_vsize > 0 && price > 0.0)
        ? refFigure(new_total_atoms / factor * price) : QString());

    // The original's rate and total in the asset column only mean anything when
    // that column's asset is the one it actually paid in.
    put(m_cell_original[RATE_ASSET], same_fee_asset
        ? GUIUtil::formatUnits(old_rate_atoms / old_factor, old_info.precision) : dash);
    put(m_cell_original[RATE_REF], old_price > 0.0
        ? refFigure(old_rate_atoms / old_factor * old_price) : dash);
    put(m_cell_original[TOTAL_ASSET], same_fee_asset
        ? GUIUtil::formatUnits(static_cast<double>(m_orig_fee_atoms) / old_factor, old_info.precision) : dash);
    put(m_cell_original[TOTAL_REF], old_price > 0.0
        ? refFigure(static_cast<double>(m_orig_fee_atoms) / old_factor * old_price) : dash);

    // Subtracting one asset from another is not arithmetic, it is a category
    // error, so the asset rows show no difference whenever the replacement pays
    // in something else. The reference column is the only place where the two
    // figures are the same kind of quantity.
    // The return type is spelled out on purpose. This build defines
    // QT_USE_QSTRINGBUILDER, under which QString + QString does not produce a
    // QString but an expression template holding REFERENCES to both operands --
    // so a deduced return type returns that template, its operands (a literal
    // temporary and this local) die at the return, and the conversion to QString
    // at the call site reads freed memory. It crashed the window on opening,
    // reproducibly, with an access violation inside recompute().
    auto delta = [](double value, uint8_t precision, bool as_reference) -> QString {
        const QString body = as_reference ? refFigure(std::abs(value))
                                          : GUIUtil::formatUnits(std::abs(value), precision);
        return (value >= 0 ? QStringLiteral("+") : QStringLiteral("−")) + body;
    };
    if (same_fee_asset) {
        put(m_cell_delta[RATE_ASSET], delta((new_rate_atoms - old_rate_atoms) / factor, info.precision, false));
        put(m_cell_delta[TOTAL_ASSET], m_vsize > 0
            ? delta((new_total_atoms - static_cast<double>(m_orig_fee_atoms)) / factor, info.precision, false)
            : dash);
    } else {
        put(m_cell_delta[RATE_ASSET], QString());
        put(m_cell_delta[TOTAL_ASSET], QString());
    }
    const bool both_priced = price > 0.0 && old_price > 0.0;
    put(m_cell_delta[RATE_REF], both_priced
        ? delta(new_rate_atoms / factor * price - old_rate_atoms / old_factor * old_price, 0, true) : QString());
    put(m_cell_delta[TOTAL_REF], (both_priced && m_vsize > 0)
        ? delta(new_total_atoms / factor * price -
                static_cast<double>(m_orig_fee_atoms) / old_factor * old_price, 0, true) : QString());

    // Only the replacement column takes typing, and only where there is something
    // to type: an unpriced fee asset has no reference figure to work back from,
    // and neither total exists before the size does.
    m_cell_new[RATE_ASSET]->setEnabled(info.accepted);
    m_cell_new[RATE_REF]->setEnabled(info.accepted && price > 0.0);
    m_cell_new[TOTAL_ASSET]->setEnabled(info.accepted && m_vsize > 0);
    m_cell_new[TOTAL_REF]->setEnabled(info.accepted && m_vsize > 0 && price > 0.0);
    for (int i = 0; i < ROW_COUNT; ++i) {
        m_cell_new[i]->setStyleSheet(m_cell_new[i]->isEnabled() ? QString(QLatin1String(kEditableCell)) : QString());
    }
    m_updating = false;

    // Does it get in? Paying more than the stuck transaction is not the test: the
    // node also wants an increment proportional to the replacement's own size
    // (BIP125), and the replacement has to clear the relay floor on its own
    // account. Both are in reference atoms, which is where the comparison
    // happens; the asset figure beside it is that same minimum, converted.
    // A bump is built by the wallet, and the wallet asks for MORE than the node
    // does: feebumper adds max(node incremental, WALLET_INCREMENTAL_RELAY_FEE) --
    // 5000 reference atoms per kvB against the node's 100. Quoting the node's
    // figure here said "enough to replace it" about a fee the wallet then refused
    // to build, with the refusal buried in a grey note below the green verdict.
    // A replacement made HERE (new outputs, not a bump) goes straight to the
    // mempool, so there the node's rule is the whole rule.
    const CAmount increment_per_kvb = m_mode == Mode::Bump
        ? std::max<CAmount>(c.replacement_min, wallet::WALLET_INCREMENTAL_RELAY_FEE)
        : c.replacement_min;
    const CAmount increment = static_cast<CAmount>(std::ceil(
        static_cast<double>(increment_per_kvb) * static_cast<double>(m_vsize) / 1000.0));
    const CAmount relay_floor = static_cast<CAmount>(std::ceil(
        static_cast<double>(c.relay_min) * static_cast<double>(m_vsize) / 1000.0));
    const CAmount floor_reference = std::max(m_orig_fee_reference + increment, relay_floor);
    const CAmount floor_atoms = toAsset(floor_reference, fee_asset);
    const bool clears = info.accepted && new_total_reference >= floor_reference;

    QString floor_text = GUIUtil::formatUnits(static_cast<double>(floor_atoms) / factor, info.precision) +
                         QLatin1Char(' ') + asset_name;
    if (price > 0.0) {
        floor_text += QStringLiteral(" (≈ ") +
                      refFigure(static_cast<double>(floor_atoms) / factor * price) +
                      QLatin1Char(' ') + ref + QLatin1Char(')');
    }

    // What the next block is charging, in the asset that pays: the figure that
    // decides whether a replacement is merely valid or actually goes anywhere.
    const CAmount entry_reference = std::max(c.next_block_min, c.relay_min);
    const CAmount entry_atoms = toAsset(entry_reference, fee_asset);
    QString entry_text = GUIUtil::formatUnits(static_cast<double>(entry_atoms) / factor, info.precision) +
                         QLatin1Char(' ') + asset_name;
    if (price > 0.0) {
        entry_text += QStringLiteral(" (≈ ") +
                      refFigure(static_cast<double>(entry_atoms) / factor * price) +
                      QLatin1Char(' ') + ref + QLatin1Char(')');
    }

    if (!m_unconfirmed_parent.isEmpty()) {
        // First, because it outranks every other answer here: while this holds,
        // the fee is not what is keeping the payment back, and raising it again
        // and again changes nothing. Alberto's case, 2026-09-18: a dozen
        // replacements at any price, all refused by peers as "missing-inputs",
        // because one input came from a transaction that had never travelled.
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("This payment spends an output of %1, which has not confirmed. Until that "
                              "transaction does, peers cannot see the coin being spent and will refuse this "
                              "one whatever it pays — a higher fee here is not what is holding it back. "
                              "Replace THAT transaction instead.").arg(m_unconfirmed_parent));
    } else if (m_orig_age > 4 * GUIUtil::nominalBlockSpacing() &&
               m_orig_rate_reference >= std::max(c.next_block_min, c.relay_min)) {
        // Everything else here reasons about prices. This reasons about an
        // outcome: it has been waiting while paying ABOVE what recent blocks
        // took, so the fee is not the thing holding it back -- either it is not
        // reaching the producers or they are refusing it, and either way paying
        // more is not the answer. The only claim the window can make with
        // certainty without seeing another producer's whitelist.
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        const int waited_minutes = static_cast<int>(m_orig_age / 60);
        const QString waited = waited_minutes == 1 ? tr("1 minute") : tr("%1 minutes").arg(waited_minutes);
        m_verdict->setText(tr("This has been waiting %1 while already paying above what recent blocks took. "
                              "That is not a fee problem: either it is not reaching the producers — a fee "
                              "asset they do not accept, or an input they cannot see — or they are refusing "
                              "it. A higher fee will not change it.").arg(waited));
    } else if (!info.accepted) {
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("This node does not accept fees in %1, so it would not relay the replacement "
                              "at all.").arg(asset_name));
    } else if (!recipient_ok) {
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("Enter a valid recipient and amount: the totals are sized on the transaction "
                              "they would produce."));
    } else if (m_mode == Mode::Bump && !m_probe_error.isEmpty()) {
        // The wallet drafts a bump itself, so its refusal is the answer -- and it
        // is a better answer than any rule restated here, because it is the one
        // that will be enforced when OK is pressed.
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("The wallet will not build this: %1").arg(m_probe_error));
    } else if (!clears) {
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("Too little: this node would keep the original. The replacement has to pay at "
                              "least %1 in total.").arg(floor_text));
    } else if (!info.has_market_price || (info.registry_available && !info.registry_listed)) {
        // Unpriced is a FINDING; unlisted on a node with no registry to ask is an
        // absence of knowledge, and reading the second as the first accuses every
        // asset on the chain (see FeeAssetInfo). Only the first two cases mean
        // other producers cannot value the fee.
        //
        // The node's own whitelist is what prices this fee, which is why a floor
        // can be quoted for an asset the feed has never heard of -- and precisely
        // why "enough" would be the wrong headline. Enough for THIS node to take
        // the replacement says nothing about the producers who will never value
        // the fee at all, and a green verdict above a grey caveat puts the
        // reassurance in front of the fact.
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("This node would take the replacement (it asks %1 in total), but no price server "
                              "it reads quotes %2 — so a producer whose whitelist has no entry for %2 refuses "
                              "it outright, and the payment may confirm only in a block this node produces. "
                              "Paying more %2 does not change that; paying in an asset they accept does.")
                               .arg(floor_text, asset_name));
    } else if (c.next_block_full && m_reference_per_kvb < std::max(c.next_block_min, c.relay_min)) {
        // Two different questions, and clearing the first says nothing about the
        // second: being ACCEPTED in place of the original is the BIP125 test,
        // being MINED is a competition with everything else waiting. A
        // replacement that clears the first and not the second takes the
        // original's place and then waits exactly as long -- which is how a fee
        // increase can feel like it did nothing at all.
        m_verdict->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_verdict->setText(tr("Enough for this node to take the replacement, but under the %1 per 1000 bytes "
                              "the next block is taking: it would replace the original and then wait in the "
                              "same queue.").arg(entry_text));
    } else {
        m_verdict->setStyleSheet(QStringLiteral("color:#7ec87e;"));
        m_verdict->setText(c.next_block_full
            ? tr("Enough to replace it: this node takes the replacement from %1 in total, and blocks being "
                 "full, the rate above also clears the %2 per 1000 bytes the next block is taking.")
                  .arg(floor_text, entry_text)
            : tr("Enough to replace it: this node takes the replacement from %1 in total, and the figure "
                 "above clears that. Blocks are not full, so nothing else is competing for the space.")
                  .arg(floor_text));
    }

    QStringList notes;
    if (m_orig_out_of_mempool) {
        // Worth saying plainly, because the list shows it as merely pending and
        // the coins look spent for no visible reason.
        notes << tr("The transaction being replaced is no longer in this node's mempool — dropped, not mined. "
                    "Sending this replacement is what puts a live transaction back in its place and frees "
                    "the coins it was holding.");
    }
    if (info.accepted) {
        const QString travel = GUIUtil::feeAssetTravelNote(asset_name, info.registry_available,
                                                           info.registry_listed, info.has_market_price);
        // The asset this node takes a fee in and the asset other producers will
        // take one in are two different questions, and replacing a stuck payment
        // is exactly when the second one matters.
        if (!travel.isEmpty()) notes << travel;
    }
    if (m_orig_fee_reference == 0 && m_orig_fee_atoms > 0) {
        notes << tr("This node no longer prices %1, so it cannot say what the original's fee was worth to it. "
                    "The minimum above is only the relay floor, and the real one may be higher.")
                     .arg(GUIUtil::assetDisplayName(m_orig_fee_asset));
    }
    if (!same_fee_asset) {
        notes << tr("The two fees are in different assets, so only the %1 rows can be subtracted — and the "
                    "node compares them at its own rates, not at these prices. The minimum above is the "
                    "figure that decides it.").arg(ref);
    }
    if (m_vsize_is_estimate) {
        notes << (m_probe_error.isEmpty()
            ? tr("Totals are sized on the original's %1 bytes, as an estimate.").arg(QString::number(m_orig_vsize))
            : tr("Totals are sized on the original's %1 bytes, as an estimate: the wallet could not draft the "
                 "replacement (%2).").arg(QString::number(m_orig_vsize), m_probe_error));
    }
    if (m_mode == Mode::Replace && m_orig_confidential && !m_orig_address.isEmpty() && address() == m_orig_address &&
        !GetDestinationBlindingKey(DecodeDestination(m_orig_address.toStdString())).IsFullyValid()) {
        // The original paid a blinded output, and this is the same recipient with
        // the confidentiality stripped off -- worth saying before it is sent
        // rather than after. Said only when there is something to lose: an
        // original that was already explicit loses nothing here.
        notes << tr("A transaction does not carry the recipient's blinding key, so this is the plain form of "
                    "the address the original paid. It reaches the same recipient, but the replacement's "
                    "amount would be public. Paste the confidential address to keep it hidden.");
    }
    m_notes->setText(notes.join(QLatin1Char('\n')));
    m_notes->setVisible(!notes.isEmpty());

    // What has been moved away from the original. Editable on purpose -- undoing
    // a mistaken payment is the reason this window exists rather than "Increase
    // transaction fee" -- but never quietly: a replacement that pays somebody
    // else, or pays less, is a different payment, and that is worth noticing
    // while there is still time to.
    if (m_mode == Mode::Bump) {
        // Nothing to say about a payment that cannot move.
    } else if (m_orig_address.isEmpty()) {
        m_address_hint->setStyleSheet(QStringLiteral("color:#888;"));
        m_address_hint->setText(tr("The original's recipient could not be read from it; enter one."));
    } else if (address() == m_orig_address) {
        m_address_hint->setStyleSheet(QStringLiteral("color:#888;"));
        m_address_hint->setText(tr("Same recipient as the original. You can change it."));
    } else {
        m_address_hint->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_address_hint->setText(tr("Not the original's recipient (%1): this would pay somebody else instead.")
                                    .arg(m_orig_address));
    }

    CAmount typed_amount = 0;
    const bool amount_read = parsedAmount(typed_amount);
    const bool same_payment = (selectedSendAsset() == m_orig_asset) && amount_read &&
                              typed_amount == m_orig_amount;
    if (m_orig_amount <= 0) {
        m_amount_hint->setStyleSheet(QStringLiteral("color:#888;"));
        m_amount_hint->setText(tr("The original's amount could not be read from it; enter one."));
    } else if (same_payment) {
        m_amount_hint->setStyleSheet(QStringLiteral("color:#888;"));
        m_amount_hint->setText(tr("Same amount and asset as the original. You can change them."));
    } else {
        const int unit = m_model->getOptionsModel()->getDisplayUnit();
        m_amount_hint->setStyleSheet(QStringLiteral("color:#ffb84d;"));
        m_amount_hint->setText(tr("The original pays %1. Changing this makes the replacement a different "
                                  "payment, not a faster one.")
                                   .arg(GUIUtil::formatAssetAmount(m_orig_asset, m_orig_amount, unit,
                                                                   BitcoinUnits::SeparatorStyle::STANDARD,
                                                                   /*include_asset_name=*/true)));
    }

    if (QPushButton* ok_button = m_buttons->button(QDialogButtonBox::Ok)) {
        // In Bump mode a draft that would not build is a refusal, not an estimate:
        // pressing OK would only reach the same error in a message box.
        const bool wallet_refuses = (m_mode == Mode::Bump) && !m_probe_error.isEmpty();
        ok_button->setEnabled(recipient_ok && info.accepted && clears && !wallet_refuses);
    }
}
