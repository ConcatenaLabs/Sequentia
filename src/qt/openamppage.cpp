// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/openamppage.h>

#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <util/strencodings.h>

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

namespace {
//! Where this wallet's OpenAMP account is remembered. Keyed by wallet name: two
//! wallets are two holders, and sharing one enclave key between them would put
//! one wallet's restricted assets behind the other's key.
QString settingsKey(const QString& wallet_name, const QString& leaf)
{
    return QStringLiteral("openamp/%1/%2").arg(wallet_name, leaf);
}
} // namespace

OpenAmpPage::OpenAmpPage(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent)
{
    setupUi(platformStyle);
}

void OpenAmpPage::setupUi(const PlatformStyle* platformStyle)
{
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget* inner = new QWidget(scroll);
    QVBoxLayout* layout = new QVBoxLayout(inner);

    QLabel* intro = new QLabel(inner);
    intro->setWordWrap(true);
    intro->setText(tr(
        "<b>OpenAMP restricted assets.</b> An issuer-governed asset is held in an <i>enclave</i>: a "
        "2-of-2 output needing both your key and the issuer's policy server, which is how the issuer "
        "enforces who may hold and transfer it. The server belongs to the issuer, not to this node.<br><br>"
        "This page is your side of it. It derives your account id from a key of this wallet, derives the "
        "enclave address that asset arrives at, and signs the transfers the issuer's server builds. "
        "Your private key never leaves the wallet, and a transfer is checked against its own transaction "
        "before anything is signed."));
    layout->addWidget(intro);

    // --- account -----------------------------------------------------------
    QGroupBox* account = new QGroupBox(tr("Your account"), inner);
    QFormLayout* account_form = new QFormLayout(account);

    m_key_label = new QLabel(account);
    m_key_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_key_label->setWordWrap(true);
    QFont mono = GUIUtil::fixedPitchFont();
    m_key_label->setFont(mono);
    m_new_key_button = new QPushButton(tr("Use a new key from this wallet"), account);
    m_copy_key = new QPushButton(tr("Copy"), account);
    QHBoxLayout* key_row = new QHBoxLayout();
    key_row->addWidget(m_key_label, 1);
    key_row->addWidget(m_copy_key);
    key_row->addWidget(m_new_key_button);
    account_form->addRow(tr("Enclave key:"), key_row);

    m_aid_label = new QLabel(account);
    m_aid_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_aid_label->setFont(mono);
    m_copy_aid = new QPushButton(tr("Copy"), account);
    QHBoxLayout* aid_row = new QHBoxLayout();
    aid_row->addWidget(m_aid_label, 1);
    aid_row->addWidget(m_copy_aid);
    account_form->addRow(tr("Account id (AID):"), aid_row);

    QLabel* account_help = new QLabel(account);
    account_help->setWordWrap(true);
    account_help->setText(tr(
        "The account id follows from the key, so it is derived here rather than granted: give the issuer "
        "(or a platform built on one, such as SeqPal) the <b>enclave key</b>, and the account they "
        "register for it is this same id. Replacing the key replaces the account, and any restricted "
        "asset already held under the old one stays there, so change it only before you hold anything."));
    account_form->addRow(account_help);
    layout->addWidget(account);

    // --- enclave -----------------------------------------------------------
    QGroupBox* enclave = new QGroupBox(tr("Where an asset arrives"), inner);
    QFormLayout* enclave_form = new QFormLayout(enclave);

    m_policy_key = new QLineEdit(enclave);
    m_policy_key->setPlaceholderText(tr("the asset's policy key, 32 bytes of hex"));
    enclave_form->addRow(tr("Policy key:"), m_policy_key);

    m_issuer_key = new QLineEdit(enclave);
    m_issuer_key->setPlaceholderText(tr("only if the asset was issued with clawback"));
    enclave_form->addRow(tr("Issuer key:"), m_issuer_key);

    m_derive_button = new QPushButton(tr("Derive the enclave address"), enclave);
    enclave_form->addRow(m_derive_button);

    m_address_label = new QLabel(enclave);
    m_address_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_address_label->setWordWrap(true);
    m_address_label->setFont(mono);
    m_copy_address = new QPushButton(tr("Copy"), enclave);
    QHBoxLayout* addr_row = new QHBoxLayout();
    addr_row->addWidget(m_address_label, 1);
    addr_row->addWidget(m_copy_address);
    enclave_form->addRow(tr("Enclave address:"), addr_row);

    QLabel* enclave_help = new QLabel(enclave);
    enclave_help->setWordWrap(true);
    enclave_help->setText(tr(
        "Both keys are public and are named in the asset's issuance contract, which its asset id commits "
        "to. Deriving the address here rather than accepting one from the server is the point: it lets "
        "you confirm the address you are about to be paid at is the one that asset's own id implies."));
    enclave_form->addRow(enclave_help);
    layout->addWidget(enclave);

    // --- signing -----------------------------------------------------------
    QGroupBox* signing = new QGroupBox(tr("Sign a transfer"), inner);
    QVBoxLayout* signing_layout = new QVBoxLayout(signing);

    QLabel* sign_help = new QLabel(signing);
    sign_help->setWordWrap(true);
    sign_help->setText(tr(
        "Paste what the policy server answered when it built the transfer -- the object carrying "
        "<tt>tx</tt> and <tt>to_sign</tt>. The node recomputes every sighash from that transaction and "
        "signs only what matches, so nothing else can be signed by pasting the wrong thing here. Send "
        "the result back to the server to complete the transfer."));
    signing_layout->addWidget(sign_help);

    m_sign_input = new QPlainTextEdit(signing);
    m_sign_input->setPlaceholderText(QStringLiteral("{\"id\": \"...\", \"tx\": \"...\", \"to_sign\": [ ... ]}"));
    m_sign_input->setFont(mono);
    m_sign_input->setMinimumHeight(90);
    signing_layout->addWidget(m_sign_input);

    m_sign_button = new QPushButton(tr("Check and sign"), signing);
    signing_layout->addWidget(m_sign_button);

    m_sign_output = new QPlainTextEdit(signing);
    m_sign_output->setReadOnly(true);
    m_sign_output->setFont(mono);
    m_sign_output->setMinimumHeight(70);
    signing_layout->addWidget(m_sign_output);

    m_copy_signatures = new QPushButton(tr("Copy the signatures"), signing);
    signing_layout->addWidget(m_copy_signatures);
    layout->addWidget(signing);

    m_status = new QLabel(inner);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    layout->addStretch(1);

    scroll->setWidget(inner);
    QVBoxLayout* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(m_new_key_button, &QPushButton::clicked, this, &OpenAmpPage::onNewEnclaveKey);
    connect(m_copy_key, &QPushButton::clicked, this, &OpenAmpPage::onCopyKey);
    connect(m_copy_aid, &QPushButton::clicked, this, &OpenAmpPage::onCopyAid);
    connect(m_derive_button, &QPushButton::clicked, this, &OpenAmpPage::onDeriveEnclave);
    connect(m_copy_address, &QPushButton::clicked, this, &OpenAmpPage::onCopyAddress);
    connect(m_sign_button, &QPushButton::clicked, this, &OpenAmpPage::onSign);
    connect(m_copy_signatures, &QPushButton::clicked, this, &OpenAmpPage::onCopySignatures);
}

void OpenAmpPage::setModel(WalletModel* model)
{
    m_wallet_model = model;
    loadSettings();
    refreshAccount();
}

std::string OpenAmpPage::walletUri() const
{
    if (!m_wallet_model) return std::string();
    return "/wallet/" + m_wallet_model->getWalletName().toStdString();
}

UniValue OpenAmpPage::callWalletRpc(const std::string& method, const UniValue& params, bool& ok, QString& error)
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

UniValue OpenAmpPage::callNodeRpc(const std::string& method, const UniValue& params, bool& ok, QString& error)
{
    ok = false;
    if (!m_wallet_model) { error = tr("No wallet loaded."); return UniValue(); }
    try {
        UniValue r = m_wallet_model->node().executeRpc(method, params, std::string());
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

void OpenAmpPage::setStatus(const QString& msg, bool error)
{
    m_status->setStyleSheet(error ? "color:#ff6b6b;" : "color:#3ecf7a;");
    m_status->setText(msg);
}

void OpenAmpPage::loadSettings()
{
    if (!m_wallet_model) return;
    QSettings settings;
    const QString name = m_wallet_model->getWalletName();
    m_enclave_key = settings.value(settingsKey(name, QStringLiteral("key"))).toString();
    m_policy_key->setText(settings.value(settingsKey(name, QStringLiteral("policykey"))).toString());
    m_issuer_key->setText(settings.value(settingsKey(name, QStringLiteral("issuerkey"))).toString());
}

void OpenAmpPage::saveSettings()
{
    if (!m_wallet_model) return;
    QSettings settings;
    const QString name = m_wallet_model->getWalletName();
    settings.setValue(settingsKey(name, QStringLiteral("key")), m_enclave_key);
    settings.setValue(settingsKey(name, QStringLiteral("policykey")), m_policy_key->text().trimmed());
    settings.setValue(settingsKey(name, QStringLiteral("issuerkey")), m_issuer_key->text().trimmed());
}

void OpenAmpPage::refreshAccount()
{
    if (m_enclave_key.isEmpty()) {
        m_key_label->setText(tr("none yet"));
        m_aid_label->setText(tr("derived once there is a key"));
        m_aid.clear();
        return;
    }
    m_key_label->setText(m_enclave_key);

    UniValue keys(UniValue::VARR);
    keys.push_back(m_enclave_key.toStdString());
    UniValue params(UniValue::VARR);
    params.push_back(keys);

    bool ok = false;
    QString err;
    const UniValue r = callNodeRpc("getopenampaccount", params, ok, err);
    if (!ok || !r.exists("aid")) {
        m_aid.clear();
        m_aid_label->setText(tr("could not be derived"));
        setStatus(tr("The account id could not be derived: %1").arg(err), true);
        return;
    }
    m_aid = QString::fromStdString(r["aid"].get_str());
    m_aid_label->setText(m_aid);
}

bool OpenAmpPage::freshEnclaveKey(QString& out, QString& error)
{
    bool ok = false;
    UniValue addr_params(UniValue::VARR);
    addr_params.push_back("");
    addr_params.push_back("bech32");
    const UniValue addr = callWalletRpc("getnewaddress", addr_params, ok, error);
    if (!ok) return false;

    UniValue info_params(UniValue::VARR);
    info_params.push_back(addr.get_str());
    const UniValue info = callWalletRpc("getaddressinfo", info_params, ok, error);
    if (!ok) return false;
    if (!info.exists("pubkey")) {
        error = tr("The wallet gave an address with no public key.");
        return false;
    }
    // x-only is the 33-byte compressed key without its parity byte.
    const QString compressed = QString::fromStdString(info["pubkey"].get_str());
    if (compressed.size() != 66) {
        error = tr("The wallet gave a public key of an unexpected length.");
        return false;
    }
    out = compressed.mid(2);
    return true;
}

void OpenAmpPage::onNewEnclaveKey()
{
    QString key, err;
    if (!freshEnclaveKey(key, err)) {
        setStatus(tr("Could not take a key from this wallet: %1").arg(err), true);
        return;
    }
    m_enclave_key = key;
    saveSettings();
    refreshAccount();
    setStatus(tr("A new enclave key is in place. Give it to the issuer to be registered; the account id "
                 "below is what they will know you by."));
}

void OpenAmpPage::onCopyKey()
{
    if (m_enclave_key.isEmpty()) return;
    QApplication::clipboard()->setText(m_enclave_key);
    setStatus(tr("Enclave key copied."));
}

void OpenAmpPage::onCopyAid()
{
    if (m_aid.isEmpty()) return;
    QApplication::clipboard()->setText(m_aid);
    setStatus(tr("Account id copied."));
}

void OpenAmpPage::onDeriveEnclave()
{
    if (m_enclave_key.isEmpty()) {
        setStatus(tr("Take a key from this wallet first: the enclave is built from it."), true);
        return;
    }
    const QString policy = m_policy_key->text().trimmed();
    if (policy.size() != 64) {
        setStatus(tr("The policy key is 32 bytes of hex, from the asset's issuance contract."), true);
        return;
    }
    const QString issuer = m_issuer_key->text().trimmed();

    UniValue keys(UniValue::VARR);
    keys.push_back(m_enclave_key.toStdString());
    UniValue params(UniValue::VARR);
    params.push_back(keys);
    params.push_back(policy.toStdString());
    if (!issuer.isEmpty()) params.push_back(issuer.toStdString());

    bool ok = false;
    QString err;
    const UniValue r = callNodeRpc("getopenampaccount", params, ok, err);
    if (!ok) {
        setStatus(tr("The enclave could not be derived: %1").arg(err), true);
        return;
    }
    m_address_label->setText(QString::fromStdString(r["address"].get_str()));
    m_transfer_leaf = QString::fromStdString(r["transfer_leaf"].get_str());
    m_transfer_control = QString::fromStdString(r["transfer_control"].get_str());
    saveSettings();
    setStatus(tr("This is where units of that asset are held for you. It is spendable only with the "
                 "issuer's co-signature, which is what makes the asset restricted."));
}

void OpenAmpPage::onCopyAddress()
{
    const QString addr = m_address_label->text();
    if (addr.isEmpty()) return;
    QApplication::clipboard()->setText(addr);
    setStatus(tr("Enclave address copied."));
}

void OpenAmpPage::onSign()
{
    m_sign_output->clear();
    if (m_transfer_leaf.isEmpty() || m_transfer_control.isEmpty()) {
        setStatus(tr("Derive the enclave address first: signing needs the leaf and control block that "
                     "go with it."), true);
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(m_sign_input->toPlainText().toUtf8(), &parse_error);
    if (doc.isNull() || !doc.isObject()) {
        setStatus(tr("That is not the JSON object the server answered with: %1").arg(parse_error.errorString()), true);
        return;
    }
    const QJsonObject root = doc.object();
    const QString tx = root.value(QStringLiteral("tx")).toString();
    const QJsonArray to_sign = root.value(QStringLiteral("to_sign")).toArray();
    if (tx.isEmpty() || to_sign.isEmpty()) {
        setStatus(tr("The pasted object has no <tt>tx</tt> and <tt>to_sign</tt> to work from."), true);
        return;
    }

    UniValue inputs(UniValue::VARR);
    for (const QJsonValue& v : to_sign) {
        const QJsonObject item = v.toObject();
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("vin", item.value(QStringLiteral("input")).toInt());
        entry.pushKV("sighash", item.value(QStringLiteral("sighash")).toString().toStdString());
        const QString pubkey = item.value(QStringLiteral("pubkey")).toString();
        entry.pushKV("xonlykey", (pubkey.isEmpty() ? m_enclave_key : pubkey).toStdString());
        entry.pushKV("leaf", m_transfer_leaf.toStdString());
        entry.pushKV("control", m_transfer_control.toStdString());
        inputs.push_back(entry);
    }

    UniValue params(UniValue::VARR);
    params.push_back(tx.toStdString());
    params.push_back(inputs);

    bool ok = false;
    QString err;
    const UniValue r = callWalletRpc("signopenamptransfer", params, ok, err);
    if (!ok) {
        setStatus(tr("Nothing was signed: %1").arg(err), true);
        return;
    }

    // Shaped as the server's completion endpoint wants it: {"sigs": {"<vin>": "<sig>"}}.
    QJsonObject sigs;
    const UniValue& signatures = r["signatures"];
    for (size_t i = 0; i < signatures.size(); ++i) {
        sigs.insert(QString::number(signatures[i]["vin"].get_int()),
                    QString::fromStdString(signatures[i]["signature"].get_str()));
    }
    QJsonObject out;
    out.insert(QStringLiteral("sigs"), sigs);
    m_sign_output->setPlainText(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
    setStatus(tr("Signed. Every sighash matched the transaction it came with. Send this to the server's "
                 "completion endpoint to finish the transfer."));
}

void OpenAmpPage::onCopySignatures()
{
    const QString out = m_sign_output->toPlainText();
    if (out.isEmpty()) return;
    QApplication::clipboard()->setText(out);
    setStatus(tr("Signatures copied."));
}
