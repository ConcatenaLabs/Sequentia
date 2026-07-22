// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/assetspage.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <assetcontract.h>
#include <assetsdir.h>
#include <interfaces/node.h>
#include <netbase.h>
#include <rpc/util.h>

#include <cmath>

#include <QAbstractButton>
#include <QByteArray>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDoubleValidator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <vector>

AssetsPage::AssetsPage(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent), m_platform_style(platformStyle)
{
    // The page is a tall stack of cards; host it in a scroll area so a larger
    // font (or a shrunk window) can never clip the lower sections, and so this
    // page imposes only a small minimum height on the main window instead of
    // its full content height.
    QVBoxLayout* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    outer->addWidget(scroll);
    QWidget* content = new QWidget(scroll);
    scroll->setWidget(content);

    QVBoxLayout* layout = new QVBoxLayout(content);

    QLabel* title = new QLabel(tr("Assets"), content);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.4);
    tf.setBold(true);
    title->setFont(tf);
    layout->addWidget(title);

    QLabel* intro = new QLabel(
        tr("Issue your own assets and mint more of ones you control. "
           "To send an asset, use the Send tab and pick the asset there."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // The balances table used to live here too, but it duplicated the Overview
    // page's balances table (which is the canonical one, with the reference-value
    // column). Assets now focuses on what is unique to it: issuing and minting.
    // A Refresh button remains for the issuances list below.
    QPushButton* refreshBtn = new QPushButton(tr("Refresh"), this);
    QHBoxLayout* refreshRow = new QHBoxLayout();
    refreshRow->addStretch();
    refreshRow->addWidget(refreshBtn);
    layout->addLayout(refreshRow);

    // --- Issue ---
    QGroupBox* issueGroup = new QGroupBox(tr("Issue a new asset"), this);
    QVBoxLayout* issueOuter = new QVBoxLayout(issueGroup);

    // The name, ticker and domain are not decoration: they are hashed into the
    // asset id at issuance. An asset issued without them is a bare hex id in every
    // wallet, for good, so the page asks for them rather than offering them.
    QLabel* issueIntro = new QLabel(
        tr("An asset's name, ticker and domain become part of its identity the moment you issue it, "
           "and can never be changed afterwards. Take a moment over them. "
           "<a href=\"https://github.com/GracedEternalKingCabbageMan/Sequentia/blob/master/doc/sequentia/issuing-an-asset-guide.md\">"
           "Step-by-step guide, including how to put the file on your site</a>."), issueGroup);
    issueIntro->setWordWrap(true);
    issueIntro->setOpenExternalLinks(true);
    issueOuter->addWidget(issueIntro);

    QFormLayout* issueForm = new QFormLayout();
    issueOuter->addLayout(issueForm);
    m_issue_name = new QLineEdit(issueGroup);
    m_issue_name->setPlaceholderText(tr("e.g. Gold (troy ounce)"));
    m_issue_name->setMaxLength(255);
    m_issue_ticker = new QLineEdit(issueGroup);
    m_issue_ticker->setPlaceholderText(tr("e.g. GOLD"));
    m_issue_ticker->setMaxLength(12);
    m_issue_domain = new QLineEdit(issueGroup);
    m_issue_domain->setPlaceholderText(tr("e.g. example.com"));
    m_issue_precision = new QSpinBox(issueGroup);
    m_issue_precision->setRange(0, 8);
    m_issue_precision->setValue(8);
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

    issueForm->addRow(tr("Name:"), m_issue_name);
    issueForm->addRow(tr("Ticker:"), m_issue_ticker);

    // The www question decides whether the asset can ever be verified, and Core
    // cannot answer it (it speaks no HTTPS, so it cannot see a redirect). The
    // browser can, so hand the job to the browser rather than pretend.
    m_issue_domain_open = new QPushButton(tr("Open my site"), issueGroup);
    m_issue_domain_open->setToolTip(tr("Opens your site so you can copy its real address out of the address bar."));
    QHBoxLayout* domainRow = new QHBoxLayout();
    domainRow->addWidget(m_issue_domain, 1);
    domainRow->addWidget(m_issue_domain_open);
    issueForm->addRow(tr("Your domain:"), domainRow);
    // The www question decides whether this asset can ever be verified, and it
    // cannot be answered here: the node speaks no HTTPS, so it cannot see the
    // redirect that settles it. Rather than ask the issuer to judge, ask them to
    // copy -- the address bar already holds the right answer once the page loads.
    QLabel* domainHint = new QLabel(
        tr("<b>This one cannot be changed later, so get it right now.</b> Many sites answer to two "
           "names - <tt>example.com</tt> and <tt>www.example.com</tt> - and only the one your site "
           "really uses will work. Do not guess:"
           "<br><br><b>Press Open my site, let the page finish loading, then copy the address out of "
           "your browser's address bar and paste it here.</b> Whatever it says once loaded is the right "
           "answer, and pasting the whole thing is fine - the <tt>https://</tt> and anything after the "
           "next slash get trimmed for you."
           "<br><br>This is the issuer's identity: it is how wallets tell your asset from an imitation, "
           "so you will need to put a small file on it afterwards to prove it is yours."),
        issueGroup);
    domainHint->setWordWrap(true);
    // Span the full form width as its own row. In the field column a wordwrapped
    // QLabel is clipped -- QFormLayout under-computes its heightForWidth there, so
    // only the top of the text shows (it read as stray dots above the next row).
    // A spanning row gets the full width and the correct height.
    domainHint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    issueForm->addRow(domainHint);
    issueForm->addRow(tr("Decimal places:"), m_issue_precision);
    issueForm->addRow(tr("Amount of units:"), m_issue_amount);
    issueForm->addRow(tr("Reissuance tokens:"), m_issue_tokens);
    issueForm->addRow(QString(), m_issue_blind);
    issueForm->addRow(QString(), m_issue_button);
    issueForm->addRow(tr("Result:"), m_issue_result);

    // Shown only once there is a proof to publish.
    m_proof_explainer = new QLabel(issueGroup);
    m_proof_explainer->setWordWrap(true);
    m_proof_explainer->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_proof_explainer->setVisible(false);
    issueOuter->addWidget(m_proof_explainer);
    m_proof_save_button = new QPushButton(tr("Save the proof file..."), issueGroup);
    m_proof_save_button->setVisible(false);
    m_contract_save_button = new QPushButton(tr("Save the contract..."), issueGroup);
    m_contract_save_button->setVisible(false);
    m_contract_save_button->setToolTip(tr("The chain keeps only this contract's fingerprint. Lose it and the "
                                          "asset can never be registered."));
    QHBoxLayout* proofRow = new QHBoxLayout();
    proofRow->addStretch();
    proofRow->addWidget(m_contract_save_button);
    proofRow->addWidget(m_proof_save_button);
    issueOuter->addLayout(proofRow);

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

    // --- Register ---
    QGroupBox* regGroup = new QGroupBox(tr("Register an asset with the registry"), this);
    QVBoxLayout* regLayout = new QVBoxLayout(regGroup);
    QLabel* regIntro = new QLabel(
        tr("Once your proof file is live on your domain, register the asset here and wallets will show "
           "its name. The registry checks your file itself, so there is no harm in trying: if the file "
           "is not up yet, it simply says so."), regGroup);
    regIntro->setWordWrap(true);
    regLayout->addWidget(regIntro);
    QHBoxLayout* regRow = new QHBoxLayout();
    m_register_asset = new QLineEdit(regGroup);
    m_register_asset->setPlaceholderText(tr("asset id (hex) - the one you issued"));
    m_register_contract_button = new QPushButton(tr("Load the contract..."), regGroup);
    m_register_contract_button->setToolTip(tr("Only needed when this wallet no longer holds the contract - it was "
                                              "issued from another wallet, or this one was restored from its seed. "
                                              "Pick the .json contract file you saved at issuance, named "
                                              "sequentia-asset-contract-(asset id).json."));
    m_register_button = new QPushButton(tr("Register"), regGroup);
    regRow->addWidget(m_register_asset, 1);
    regRow->addWidget(m_register_contract_button);
    regRow->addWidget(m_register_button);
    regLayout->addLayout(regRow);
    layout->addWidget(regGroup);

    // --- Issuances ---
    QGroupBox* issGroup = new QGroupBox(tr("Your issuances"), this);
    QVBoxLayout* issLayout = new QVBoxLayout(issGroup);
    m_issuances = new QTableWidget(0, 4, issGroup);
    m_issuances->setHorizontalHeaderLabels({tr("Asset"), tr("Reissuance token"), tr("Issued amount"), tr("Registry")});
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
        // Keep the ticker to what a registry will accept, while it can still be
        // retyped. The registry's rule is the same set of characters.
        m_issue_ticker->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9.\\-]{0,12}"), this));
    }

    connect(refreshBtn, &QPushButton::clicked, this, &AssetsPage::refresh);
    connect(m_issue_button, &QPushButton::clicked, this, &AssetsPage::onIssue);
    connect(m_reissue_button, &QPushButton::clicked, this, &AssetsPage::onReissue);
    connect(m_proof_save_button, &QPushButton::clicked, this, &AssetsPage::onSaveProofFile);
    connect(m_contract_save_button, &QPushButton::clicked, this, &AssetsPage::onSaveContract);
    connect(m_issue_domain_open, &QPushButton::clicked, this, &AssetsPage::onOpenDomain);
    connect(m_register_contract_button, &QPushButton::clicked, this, &AssetsPage::onLoadContract);
    connect(m_register_button, &QPushButton::clicked, this, &AssetsPage::onRegister);
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

    // (Balances now live only on the Overview page.)

    // Which assets does the registry vouch for? The node merges an entry only once
    // the registry marks it verified, so an asset having a label here is exactly
    // the confirmation an issuer is waiting for -- and its absence is the honest
    // answer that the proof or the registration is still missing.
    QHash<QString, QString> verified_labels;
    UniValue labels = callWalletRpc("dumpassetlabels", UniValue(UniValue::VARR), ok, err);
    if (ok && labels.isObject()) {
        const std::vector<std::string>& keys = labels.getKeys();
        for (size_t i = 0; i < keys.size(); ++i) {
            verified_labels.insert(QString::fromStdString(labels[i].getValStr()),
                                   QString::fromStdString(keys[i]));
        }
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
            const QString asset = field("asset");
            m_issuances->setItem(row, 0, new QTableWidgetItem(asset));
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

            QTableWidgetItem* registry = new QTableWidgetItem();
            const auto found = verified_labels.constFind(asset);
            if (found != verified_labels.constEnd()) {
                registry->setText(found.value());
                registry->setToolTip(tr("The asset registry vouches for this asset, so wallets show this name."));
            } else {
                registry->setText(tr("not registered yet"));
                registry->setToolTip(tr("Wallets show this asset as a hex id. Publish the proof file on your "
                                        "domain and register the asset, then this becomes its name."));
            }
            m_issuances->setItem(row, 3, registry);
        }
    }

    // Stamp the throttle state so scheduleRefresh() can skip a redundant re-run
    // (same tip, refreshed a moment ago) next time the tab is shown.
    if (m_wallet_model) {
        m_last_refresh_blocks = m_wallet_model->node().getNumBlocks();
        m_last_refresh_ms = QDateTime::currentMSecsSinceEpoch();
    }
}

void AssetsPage::scheduleRefresh(bool force)
{
    if (!m_wallet_model) return;
    if (!force && m_last_refresh_blocks >= 0) {
        // Nothing new to show since the last refresh: same tip and it ran within
        // the last couple of seconds. The tables already hold that result, so a
        // re-run would only freeze the GUI thread for no visible change.
        const int blocks = m_wallet_model->node().getNumBlocks();
        const qint64 age = QDateTime::currentMSecsSinceEpoch() - m_last_refresh_ms;
        if (blocks == m_last_refresh_blocks && age < 2000) return;
    }
    if (m_refresh_pending) return; // one deferred refresh is enough
    m_refresh_pending = true;
    QPointer<AssetsPage> self(this);
    // Let the switch paint first, then do the wallet RPCs on the next turn.
    QTimer::singleShot(0, this, [self] {
        if (!self) return;
        self->m_refresh_pending = false;
        self->refresh();
    });
}

void AssetsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // Never refresh synchronously here: the wallet RPCs on the GUI thread would
    // block the new tab from painting and make the switch feel like a stall.
    scheduleRefresh(/*force=*/false);
}

QString AssetsPage::issuerDomain() const
{
    // People paste a URL when asked for a domain; take the host out of it rather
    // than refusing, but keep the result strict enough for the registry.
    QString domain = m_issue_domain->text().trimmed().toLower();
    domain.remove(QRegularExpression("^[a-z]+://"));
    return domain.section('/', 0, 0);
}

void AssetsPage::onOpenDomain()
{
    const QString domain = issuerDomain();
    if (domain.isEmpty()) { setStatus(tr("Type your website's address first."), true); m_issue_domain->setFocus(); return; }
    QDesktopServices::openUrl(QUrl("https://" + domain));
    setStatus(tr("Opened https://%1 - once it has loaded, read your browser's address bar. "
                 "If it now shows a different name, that one is your domain, not this one.").arg(domain), false);
}

bool AssetsPage::domainResolves(const QString& domain) const
{
    std::vector<CNetAddr> ips;
    return LookupHost(domain.toStdString(), ips, 1, /*fAllowLookup=*/true) && !ips.empty();
}

bool AssetsPage::confirmIssuance(const QString& name, const QString& ticker, const QString& domain)
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Issue this asset?"));
    box.setText(tr("Issue %1 (%2), from %3?").arg(name, ticker, domain));
    box.setInformativeText(
        tr("The name (%1), the ticker (%2) and the domain (%3) become part of this asset's identity "
           "for good. They cannot be edited, and no one can change them later - if the domain is "
           "wrong, the only way out is to abandon this asset and issue another one.\n\n"
           "After issuing you will need to put a small file on %3 to prove the domain is yours. "
           "Until you do, wallets show this asset as a long string of digits instead of its name.")
            .arg(name, ticker, domain));
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    if (QAbstractButton* ok_button = box.button(QMessageBox::Ok)) ok_button->setText(tr("Issue it"));
    box.setDefaultButton(QMessageBox::Cancel);
    return box.exec() == QMessageBox::Ok;
}

void AssetsPage::onIssue()
{
    if (!m_wallet_model) return;
    const QString name = m_issue_name->text().trimmed();
    const QString ticker = m_issue_ticker->text().trimmed();
    const QString amount = m_issue_amount->text().trimmed();
    const QString tokens = m_issue_tokens->text().trimmed();
    const QString domain = issuerDomain();

    if (name.isEmpty()) { setStatus(tr("Give the asset a name. It cannot be added later."), true); m_issue_name->setFocus(); return; }
    if (ticker.isEmpty()) { setStatus(tr("Give the asset a ticker, such as GOLD. It cannot be added later."), true); m_issue_ticker->setFocus(); return; }
    if (domain.isEmpty()) { setStatus(tr("Enter the domain of whoever issues this asset. It cannot be added later."), true); m_issue_domain->setFocus(); return; }
    if (amount.isEmpty()) { setStatus(tr("Enter an amount of units to issue."), true); m_issue_amount->setFocus(); return; }
    { bool aok=false; const double av=amount.toDouble(&aok); if(!aok||av<=0){ setStatus(tr("Amount must be a positive number."), true); return; } }

    // Check the contract here as well as in the RPC, so a bad field is a message
    // next to the form rather than an RPC error string.
    const UniValue candidate = AssetContract::Build(
        name.toStdString(), ticker.toStdString(), m_issue_precision->value(), domain.toStdString(),
        // A placeholder purely for this local check: the node fills in the real
        // issuer key from the wallet, and only that one is ever committed.
        "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    const std::vector<std::string> problems = AssetContract::Validate(candidate);
    if (!problems.empty()) {
        setStatus(tr("That will not do: %1").arg(QString::fromStdString(problems.front())), true);
        return;
    }

    // A domain that does not answer is usually a typo, and a typo here is
    // permanent. Warn, but do not refuse: DNS may simply be unreachable from here.
    if (!domainResolves(domain)) {
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this, tr("That domain does not answer"),
            tr("%1 could not be looked up. If it is misspelt, the asset you are about to issue "
               "can never be verified, and that cannot be undone.\n\nIssue it anyway?").arg(domain),
            QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
        if (answer != QMessageBox::Ok) { m_issue_domain->setFocus(); return; }
    }

    if (!confirmIssuance(name, ticker, domain)) return;

    UniValue contract(UniValue::VOBJ);
    contract.pushKV("name", name.toStdString());
    contract.pushKV("ticker", ticker.toStdString());
    contract.pushKV("domain", domain.toStdString());
    contract.pushKV("precision", m_issue_precision->value());

    UniValue params(UniValue::VARR);
    params.push_back(UniValue(UniValue::VNUM, amount.toStdString()));
    params.push_back(UniValue(UniValue::VNUM, tokens.isEmpty() ? std::string("0") : tokens.toStdString()));
    params.push_back(m_issue_blind->isChecked());
    params.push_back(NullUniValue); // contract_hash: the contract below supersedes it
    params.push_back(NullUniValue); // fee_asset
    params.push_back(NullUniValue); // denomination: taken from the contract's precision
    params.push_back(contract);

    bool ok; QString err;
    UniValue r = callWalletRpc("issueasset", params, ok, err);
    if (!ok) { setStatus(tr("Issue failed: %1").arg(err), true); return; }

    const QString asset = r.exists("asset") ? QString::fromStdString(r["asset"].get_str()) : QString();
    const QString token = r.exists("token") ? QString::fromStdString(r["token"].get_str()) : QString();
    m_issue_result->setText(tr("Asset id: %1\nReissuance token: %2").arg(asset, token));

    m_proof_asset = asset;
    m_proof_domain = domain;
    m_proof_line = r.exists("proof_line") ? QString::fromStdString(r["proof_line"].get_str()) : QString();
    // The contract is the one thing here that cannot be reconstructed: the chain
    // holds only its hash, and the issuer key came out of the wallet. Hold on to
    // exactly what the node committed rather than rebuilding it from the form.
    m_proof_contract = r.exists("contract") ? QString::fromStdString(r["contract"].write()) : QString();
    const QString proof_url = r.exists("proof_url") ? QString::fromStdString(r["proof_url"].get_str()) : QString();
    if (!m_proof_line.isEmpty()) {
        m_proof_explainer->setText(
            tr("<b>%1 exists. Two steps left before wallets show its name.</b>"
               "<br><br><b>1. Put a file on your website.</b> Save it with the button below, then upload it "
               "to a folder called <tt>.well-known</tt> at the very top of %2 - the folder that holds your "
               "site itself, not your media or uploads folder. Do not rename the file. When it is right, "
               "opening this in a browser shows one line of plain text and nothing else:"
               "<br><br><tt>%3</tt>"
               "<br><br>If the browser downloads the file instead of showing it, that is fine - the "
               "registry reads it raw either way. What fails is the line shown inside your site's design; "
               "the guide has the two-line fix for that."
               "<br><br><b>2. Press Register below</b> once the file is up. This wallet kept the contract "
               "and will submit it for you; the registry then reads your file and decides. Save the "
               "contract too, with its button below - it is a <tt>.json</tt> file named after your asset. "
               "The chain kept only its fingerprint, so this wallet holds the only copy; a wallet that no "
               "longer has it (restored from seed, or a different one) can only register by loading that "
               "<tt>.json</tt> file back with <i>Load the contract...</i>:"
               "<br><br><tt>%4</tt>"
               "<br><br>You will know it worked when the Registry column below stops saying "
               "<i>not registered yet</i>. <a href=\"%5\">The step-by-step guide</a> covers putting the "
               "file on the usual website platforms, and what to do when it does not work.")
                .arg(ticker.toHtmlEscaped(), domain.toHtmlEscaped(), proof_url.toHtmlEscaped(),
                     m_proof_contract.toHtmlEscaped(),
                     "https://github.com/GracedEternalKingCabbageMan/Sequentia/blob/master/doc/sequentia/issuing-an-asset-guide.md"));
        m_proof_explainer->setVisible(true);
        m_proof_save_button->setVisible(true);
        m_contract_save_button->setVisible(!m_proof_contract.isEmpty());
        // Save the issuer retyping 64 hex characters into the very next field.
        m_register_asset->setText(asset);
    }
    setStatus(tr("Issued %1. Save the asset id above; it is what identifies your asset.").arg(ticker), false);
    refresh();
}

void AssetsPage::onSaveProofFile()
{
    if (m_proof_line.isEmpty() || m_proof_asset.isEmpty()) return;

    const QString suggested = QString::fromStdString(AssetContract::AssetProofPath(m_proof_asset.toStdString())).section('/', -1);
    const QString path = QFileDialog::getSaveFileName(this, tr("Save the proof file"), suggested, tr("All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(tr("Could not write %1: %2").arg(path, file.errorString()), true);
        return;
    }
    // The registry compares the whole body against this one line, so write the
    // line and nothing else -- no newline, no byte order mark, no markup.
    const QByteArray body = m_proof_line.toUtf8();
    const bool written = file.write(body) == body.size();
    file.close();
    if (!written) {
        setStatus(tr("Could not write %1: %2").arg(path, file.errorString()), true);
        return;
    }
    setStatus(tr("Saved. Upload it to %1 so it is served at the address above, as plain text.").arg(m_proof_domain), false);
}

void AssetsPage::onLoadContract()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Load the contract"), QString(), tr("JSON file (*.json)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(tr("Could not read %1: %2").arg(path, file.errorString()), true);
        return;
    }
    const QByteArray body = file.readAll();
    file.close();

    UniValue parsed;
    if (!parsed.read(std::string(body.constData(), size_t(body.size()))) || !parsed.isObject()) {
        setStatus(tr("%1 does not contain a contract: it does not parse as a JSON object.").arg(path), true);
        return;
    }
    m_register_contract = QString::fromUtf8(body);

    // The save button named the file after its asset; give that id back to the
    // field rather than making the user retype 64 characters.
    static const QRegularExpression contract_file_re("sequentia-asset-contract-([0-9a-f]{64})\\.json$");
    const auto match = contract_file_re.match(QFileInfo(path).fileName());
    if (match.hasMatch() && m_register_asset->text().trimmed().isEmpty()) {
        m_register_asset->setText(match.captured(1));
    }
    setStatus(tr("Contract loaded. It will be sent along when you press Register."), false);
}

void AssetsPage::onRegister()
{
    if (!m_wallet_model) return;
    const QString asset = m_register_asset->text().trimmed().toLower();
    if (asset.isEmpty()) { setStatus(tr("Enter the asset id you want to register."), true); m_register_asset->setFocus(); return; }

    UniValue params(UniValue::VARR);
    params.push_back(asset.toStdString());
    if (!m_register_contract.isEmpty()) {
        UniValue contract;
        contract.read(m_register_contract.toStdString());
        params.push_back(contract);
    }

    m_register_button->setEnabled(false);
    setStatus(tr("Asking the registry to check your domain..."), false);
    bool ok; QString err;
    UniValue r = callWalletRpc("registerasset", params, ok, err);
    m_register_button->setEnabled(true);

    if (!ok) {
        // The registry's own words: it knows why it refused, and every reason is
        // something the issuer can go and fix.
        if (m_register_contract.isEmpty() && err.contains(QStringLiteral("no contract"))) {
            err += QLatin1Char(' ') + tr("If you saved the contract as a .json file at issuance, press \"Load the contract...\" and try again.");
        }
        setStatus(err, true);
        return;
    }
    const bool verified = r.exists("verified") && r["verified"].isBool() && r["verified"].get_bool();
    if (verified) {
        m_register_contract.clear();
        setStatus(tr("Registered. %1 is verified - wallets will show its name within a few minutes.").arg(asset), false);
    } else {
        setStatus(tr("The registry accepted the submission but does not vouch for the asset yet."), true);
    }
    refresh();
}

void AssetsPage::onSaveContract()
{
    if (m_proof_contract.isEmpty() || m_proof_asset.isEmpty()) return;

    const QString suggested = QString("sequentia-asset-contract-%1.json").arg(m_proof_asset);
    const QString path = QFileDialog::getSaveFileName(this, tr("Save the contract"), suggested, tr("JSON file (*.json)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(tr("Could not write %1: %2").arg(path, file.errorString()), true);
        return;
    }
    const QByteArray body = m_proof_contract.toUtf8();
    const bool written = file.write(body) == body.size();
    file.close();
    if (!written) {
        setStatus(tr("Could not write %1: %2").arg(path, file.errorString()), true);
        return;
    }
    setStatus(tr("Contract saved. Keep that .json file: it is the only copy, and registering the asset needs it."), false);
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
