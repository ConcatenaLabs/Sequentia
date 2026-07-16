// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/assetspage.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <assetsdir.h>
#include <interfaces/node.h>
#include <rpc/util.h>

#include <cmath>

#include <QCheckBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

AssetsPage::AssetsPage(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent), m_platform_style(platformStyle)
{
    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* title = new QLabel(tr("Assets"), this);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.4);
    tf.setBold(true);
    title->setFont(tf);
    layout->addWidget(title);

    QLabel* intro = new QLabel(
        tr("Issue your own assets, mint more of ones you control, and see what you hold. "
           "To send an asset, use the Send tab and pick the asset there."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // --- Balances ---
    QGroupBox* balGroup = new QGroupBox(tr("Your asset balances"), this);
    QVBoxLayout* balLayout = new QVBoxLayout(balGroup);
    m_balances = new QTableWidget(0, 3, balGroup);
    m_balances->setHorizontalHeaderLabels({tr("Asset"), tr("Balance"), tr("Value")});
    m_balances->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_balances->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_balances->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_balances->verticalHeader()->setVisible(false);
    m_balances->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_balances->setSelectionBehavior(QAbstractItemView::SelectRows);
    balLayout->addWidget(m_balances);
    // Empty state: an empty wallet holds no assets at all — never a "0" row for any
    // particular asset (no asset is privileged, so none deserves a phantom line).
    m_balances_empty = new QLabel(tr("No assets yet. Receive any asset - or issue your own below - and it will show up here."), balGroup);
    m_balances_empty->setWordWrap(true);
    m_balances_empty->setVisible(false);
    balLayout->addWidget(m_balances_empty);
    QPushButton* refreshBtn = new QPushButton(tr("Refresh"), balGroup);
    QHBoxLayout* refreshRow = new QHBoxLayout();
    refreshRow->addStretch();
    refreshRow->addWidget(refreshBtn);
    balLayout->addLayout(refreshRow);
    layout->addWidget(balGroup);

    // --- Issue ---
    QGroupBox* issueGroup = new QGroupBox(tr("Issue a new asset"), this);
    QFormLayout* issueForm = new QFormLayout(issueGroup);
    m_issue_amount = new QLineEdit(issueGroup);
    m_issue_amount->setPlaceholderText(tr("e.g. 1000000"));
    m_issue_tokens = new QLineEdit(issueGroup);
    m_issue_tokens->setText("1");
    m_issue_blind = new QCheckBox(tr("Confidential (blinded) issuance"), issueGroup);
    m_issue_blind->setChecked(false);
    m_issue_button = new QPushButton(tr("Issue asset"), issueGroup);
    m_issue_result = new QLabel(issueGroup);
    m_issue_result->setWordWrap(true);
    m_issue_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    issueForm->addRow(tr("Amount of units:"), m_issue_amount);
    issueForm->addRow(tr("Reissuance tokens:"), m_issue_tokens);
    issueForm->addRow(QString(), m_issue_blind);
    issueForm->addRow(QString(), m_issue_button);
    issueForm->addRow(tr("Result:"), m_issue_result);
    layout->addWidget(issueGroup);

    // --- Reissue ---
    QGroupBox* reissueGroup = new QGroupBox(tr("Mint more of an existing asset (reissue)"), this);
    QFormLayout* reissueForm = new QFormLayout(reissueGroup);
    m_reissue_asset = new QLineEdit(reissueGroup);
    m_reissue_asset->setPlaceholderText(tr("asset id (hex); you must hold its reissuance token"));
    m_reissue_amount = new QLineEdit(reissueGroup);
    m_reissue_amount->setPlaceholderText(tr("amount to mint"));
    m_reissue_button = new QPushButton(tr("Reissue"), reissueGroup);
    reissueForm->addRow(tr("Asset id:"), m_reissue_asset);
    reissueForm->addRow(tr("Amount:"), m_reissue_amount);
    reissueForm->addRow(QString(), m_reissue_button);
    layout->addWidget(reissueGroup);

    // --- Issuances ---
    QGroupBox* issGroup = new QGroupBox(tr("Your issuances"), this);
    QVBoxLayout* issLayout = new QVBoxLayout(issGroup);
    m_issuances = new QTableWidget(0, 3, issGroup);
    m_issuances->setHorizontalHeaderLabels({tr("Asset"), tr("Reissuance token"), tr("Issued amount")});
    m_issuances->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_issuances->verticalHeader()->setVisible(false);
    m_issuances->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issLayout->addWidget(m_issuances);
    layout->addWidget(issGroup);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    layout->addStretch();

    // Numeric validation on amount fields
    {
        QLocale lc(QLocale::C); lc.setNumberOptions(QLocale::RejectGroupSeparator);
        auto* issueAmt = new QDoubleValidator(0, 1e15, 8, this); issueAmt->setLocale(lc);
        m_issue_amount->setValidator(issueAmt);
        m_issue_tokens->setValidator(new QIntValidator(0, 1000000, this));
        auto* reissueAmt = new QDoubleValidator(0, 1e15, 8, this); reissueAmt->setLocale(lc);
        m_reissue_amount->setValidator(reissueAmt);
    }

    connect(refreshBtn, &QPushButton::clicked, this, &AssetsPage::refresh);
    connect(m_issue_button, &QPushButton::clicked, this, &AssetsPage::onIssue);
    connect(m_reissue_button, &QPushButton::clicked, this, &AssetsPage::onReissue);
}

void AssetsPage::setModel(WalletModel* model)
{
    m_wallet_model = model;
    if (m_wallet_model) refresh();
}

std::string AssetsPage::walletUri() const
{
    if (!m_wallet_model) return std::string();
    return "/wallet/" + m_wallet_model->getWalletName().toStdString();
}

UniValue AssetsPage::callWalletRpc(const std::string& method, const UniValue& params, bool& ok, QString& error)
{
    ok = false;
    if (!m_wallet_model) { error = tr("No wallet loaded."); return UniValue(); }
    try {
        UniValue r = m_wallet_model->node().executeRpc(method, params, walletUri());
        ok = true;
        return r;
    } catch (const UniValue& e) {
        if (e.isObject() && e.exists("message")) error = QString::fromStdString(e["message"].get_str());
        else error = QString::fromStdString(e.write());
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
    } catch (...) {
        error = tr("Unknown error.");
    }
    return UniValue();
}

void AssetsPage::setStatus(const QString& msg, bool error)
{
    m_status->setStyleSheet(error ? "color:#ff6b6b;" : "color:#3ecf7a;");
    m_status->setText(msg);
}

void AssetsPage::refresh()
{
    if (!m_wallet_model) return;
    bool ok; QString err;

    // Per-asset balances: getbalance "*"
    UniValue bp(UniValue::VARR);
    bp.push_back("*");
    UniValue bal = callWalletRpc("getbalance", bp, ok, err);
    if (ok && bal.isObject()) {
        const std::vector<std::string>& keys = bal.getKeys();
        m_balances->setRowCount(0);
        const int display_unit = m_wallet_model->getOptionsModel() ? m_wallet_model->getOptionsModel()->getDisplayUnit() : BitcoinUnits::BTC;
        for (size_t i = 0; i < keys.size(); ++i) {
            // getbalance reports every amount 1e8-scaled (atoms/1e8), like all RPC
            // output. Recover the exact atom count and render it at the asset's own
            // precision, so an asset with a denomination other than 8 shows the
            // right number of decimals here instead of always eight.
            CAmount atoms = 0;
            try { atoms = AmountFromValue(bal[i], /*check_range=*/false); } catch (...) { continue; }
            // Zero balances are assets the wallet does not hold; listing them (the
            // policy asset included) would just be noise. No asset is privileged.
            if (atoms <= 0) continue;
            const std::string& key = keys[i];
            const CAsset asset = GetAssetFromString(key);
            const QString label = QString::fromStdString(key);
            const double wholeUnits = static_cast<double>(atoms) / std::pow(10.0, GUIUtil::assetPrecision(asset));

            int row = m_balances->rowCount();
            m_balances->insertRow(row);
            m_balances->setItem(row, 0, new QTableWidgetItem(label));
            m_balances->setItem(row, 1, new QTableWidgetItem(GUIUtil::formatAssetAmount(asset, atoms, display_unit, BitcoinUnits::SeparatorStyle::STANDARD, false)));
            // SEQUENTIA: value the balance in the user's chosen reference currency (blank if unpriced).
            m_balances->setItem(row, 2, new QTableWidgetItem(GUIUtil::formatReferenceApproxByLabel(label, wholeUnits, QString())));
        }
        const bool empty = m_balances->rowCount() == 0;
        m_balances->setVisible(!empty);
        if (m_balances_empty) m_balances_empty->setVisible(empty);
    } else if (!ok) {
        setStatus(tr("Could not load balances: %1").arg(err), true);
    }

    // Issuances: listissuances
    UniValue iss = callWalletRpc("listissuances", UniValue(UniValue::VARR), ok, err);
    if (ok && iss.isArray()) {
        m_issuances->setRowCount(0);
        for (size_t i = 0; i < iss.size(); ++i) {
            const UniValue& e = iss[i];
            int row = m_issuances->rowCount();
            m_issuances->insertRow(row);
            auto field = [&](const char* k) {
                return e.exists(k) ? QString::fromStdString(e[k].getValStr()) : QString();
            };
            m_issuances->setItem(row, 0, new QTableWidgetItem(field("asset")));
            m_issuances->setItem(row, 1, new QTableWidgetItem(field("token")));
            // assetamount is 1e8-scaled like all RPC amounts, or -1 when the amount
            // is blinded/unknown. Render known amounts at the asset's precision.
            QString amtStr = field("assetamount");
            if (amtStr != "-1" && e.exists("assetamount")) {
                try {
                    const CAmount a = AmountFromValue(e["assetamount"], /*check_range=*/false);
                    const CAsset as = GetAssetFromString(e["asset"].getValStr());
                    amtStr = GUIUtil::formatAssetAmount(as, a, BitcoinUnits::BTC, BitcoinUnits::SeparatorStyle::STANDARD, false);
                } catch (...) {}
            }
            m_issuances->setItem(row, 2, new QTableWidgetItem(amtStr));
        }
    }
}

void AssetsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_wallet_model) refresh();
}

void AssetsPage::onIssue()
{
    if (!m_wallet_model) return;
    const QString amount = m_issue_amount->text().trimmed();
    const QString tokens = m_issue_tokens->text().trimmed();
    if (amount.isEmpty()) { setStatus(tr("Enter an amount of units to issue."), true); return; }
    { bool aok=false; const double av=amount.toDouble(&aok); if(!aok||av<=0){ setStatus(tr("Amount must be a positive number."), true); return; } }

    UniValue params(UniValue::VARR);
    params.push_back(UniValue(UniValue::VNUM, amount.toStdString()));
    params.push_back(UniValue(UniValue::VNUM, tokens.isEmpty() ? std::string("0") : tokens.toStdString()));
    params.push_back(m_issue_blind->isChecked());

    bool ok; QString err;
    UniValue r = callWalletRpc("issueasset", params, ok, err);
    if (!ok) { setStatus(tr("Issue failed: %1").arg(err), true); return; }

    QString asset = r.exists("asset") ? QString::fromStdString(r["asset"].get_str()) : QString();
    QString token = r.exists("token") ? QString::fromStdString(r["token"].get_str()) : QString();
    m_issue_result->setText(tr("Asset id: %1\nReissuance token: %2").arg(asset, token));
    setStatus(tr("Issued. Save the asset id above; it identifies your asset."), false);
    refresh();
}

void AssetsPage::onReissue()
{
    if (!m_wallet_model) return;
    const QString asset = m_reissue_asset->text().trimmed();
    const QString amount = m_reissue_amount->text().trimmed();
    if (asset.isEmpty() || amount.isEmpty()) { setStatus(tr("Enter both an asset id and an amount."), true); return; }
    { bool aok=false; const double av=amount.toDouble(&aok); if(!aok||av<=0){ setStatus(tr("Amount must be a positive number."), true); return; } }

    UniValue params(UniValue::VARR);
    params.push_back(asset.toStdString());
    params.push_back(UniValue(UniValue::VNUM, amount.toStdString()));

    bool ok; QString err;
    callWalletRpc("reissueasset", params, ok, err);
    if (!ok) { setStatus(tr("Reissue failed: %1").arg(err), true); return; }
    setStatus(tr("Reissued %1 units of %2.").arg(amount, asset), false);
    refresh();
}
