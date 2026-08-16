// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/supervisionpage.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <assetsdir.h>
#include <core_io.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <policy/policy.h>
#include <rpc/util.h>
#include <script/standard.h>
#include <util/strencodings.h>

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace {
//! What a record transaction pays the producer. A record is tiny and wants to
//! confirm quickly -- a freeze that waits is a freeze that can be outrun -- so it
//! is not worth economising on, and it is paid in the policy asset because that is
//! the one every producer accepts.
const CAmount RECORD_FEE = 100000; // 0.001

//! Below this the change output would be dust, and the transaction would be
//! refused for it rather than for anything to do with supervision.
const CAmount MIN_CHANGE = 10000; // 0.0001

//! A supervision record output's script begins with a push of "SEQFRZ"
//! (SUPERVISION_RECORD_MARKER). Matching that in the raw transaction hex tells us
//! in one string search that a wallet transaction is worth decoding, which keeps
//! the record scan off the thousands of ordinary transactions in a working wallet.
const char* RECORD_MARKER_HEX = "0653455146525a";

//! The wildcard target a pause names instead of a script (SUPERVISION_PAUSE_TARGET).
const char* PAUSE_TARGET_HEX = "0000000000000000000000000000000000000000000000000000000000000000";
} // namespace

SupervisionPage::SupervisionPage(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent), m_platform_style(platformStyle)
{
    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* title = new QLabel(tr("Supervision"), this);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.4);
    tf.setBold(true);
    title->setFont(tf);
    layout->addWidget(title);

    QLabel* intro = new QLabel(
        tr("The assets you issued with supervision, and what you can do about them: freeze an address, "
           "lift a freeze, pause the asset if it was issued with that capability, and replace either key."
           "<br><br>Freezing stops a spend. It never moves anyone's coins, and it cannot reach funds in a "
           "Lightning channel, an atomic swap or any other shared contract, because someone who did nothing "
           "would be trapped with them. A seizure order is answered by freezing and reissuing."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QHBoxLayout* topRow = new QHBoxLayout();
    m_asset_selector = new QComboBox(this);
    m_asset_selector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    QPushButton* refreshBtn = new QPushButton(tr("Refresh"), this);
    topRow->addWidget(new QLabel(tr("Asset:"), this));
    topRow->addWidget(m_asset_selector, 1);
    topRow->addWidget(refreshBtn);
    layout->addLayout(topRow);

    m_asset_summary = new QLabel(this);
    m_asset_summary->setWordWrap(true);
    m_asset_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_asset_summary);

    m_keys_summary = new QLabel(this);
    m_keys_summary->setWordWrap(true);
    m_keys_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_keys_summary);

    // --- Freezes in force ---
    QGroupBox* freezeGroup = new QGroupBox(tr("Freezes in force"), this);
    QVBoxLayout* freezeLayout = new QVBoxLayout(freezeGroup);
    m_freezes = new QTableWidget(0, 3, freezeGroup);
    m_freezes->setHorizontalHeaderLabels({tr("Frozen script"), tr("Records"), tr("Can be lifted here")});
    m_freezes->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_freezes->verticalHeader()->setVisible(false);
    m_freezes->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_freezes->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_freezes->setSelectionMode(QAbstractItemView::SingleSelection);
    freezeLayout->addWidget(m_freezes);
    // Scripts are named by hash, because a record is fixed-width whatever it
    // freezes. Nothing on the chain maps a hash back to the address it was made
    // from, so the address that was frozen is the issuer's own record to keep.
    QLabel* freezeHint = new QLabel(
        tr("A freeze names a script by its hash, so this list shows hashes rather than addresses; "
           "use the check below to see whether a particular address is among them. Lifting a freeze "
           "means spending the record that made it, which this wallet can only do for records it "
           "created itself."), freezeGroup);
    freezeHint->setWordWrap(true);
    freezeLayout->addWidget(freezeHint);
    m_unfreeze_button = new QPushButton(tr("Lift the selected freeze"), freezeGroup);
    m_unfreeze_button->setEnabled(false);
    QHBoxLayout* unfreezeRow = new QHBoxLayout();
    unfreezeRow->addStretch();
    unfreezeRow->addWidget(m_unfreeze_button);
    freezeLayout->addLayout(unfreezeRow);
    layout->addWidget(freezeGroup);

    // --- Freeze ---
    QGroupBox* newFreezeGroup = new QGroupBox(tr("Freeze an address or script"), this);
    QFormLayout* newFreezeForm = new QFormLayout(newFreezeGroup);
    m_freeze_target = new QLineEdit(newFreezeGroup);
    m_freeze_target->setPlaceholderText(tr("address, or a scriptPubKey in hex"));
    QPushButton* checkBtn = new QPushButton(tr("Check"), newFreezeGroup);
    QHBoxLayout* targetRow = new QHBoxLayout();
    targetRow->addWidget(m_freeze_target, 1);
    targetRow->addWidget(checkBtn);
    newFreezeForm->addRow(tr("Target:"), targetRow);
    m_freeze_check = new QLabel(newFreezeGroup);
    m_freeze_check->setWordWrap(true);
    m_freeze_check->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    newFreezeForm->addRow(m_freeze_check);
    m_freeze_private = new QCheckBox(tr("Submit straight to this node's block producer"), newFreezeGroup);
    m_freeze_private->setToolTip(
        tr("A freeze sitting in the public mempool is a warning: the holder can move the funds to a fresh "
           "script before it confirms, and since a freeze names a script rather than a person, you would "
           "then be chasing. Submitted this way the record is never announced and is included in the next "
           "block THIS node produces -- so it only helps if this node produces blocks. Producers accept "
           "these only from operators they have configured."));
    newFreezeForm->addRow(QString(), m_freeze_private);
    m_freeze_button = new QPushButton(tr("Freeze"), newFreezeGroup);
    newFreezeForm->addRow(QString(), m_freeze_button);
    QLabel* freezeReach = new QLabel(
        tr("The freeze takes effect from the block <i>after</i> the one carrying the record, and it stops "
           "spending only: the address can still be paid, which is what makes freezing a known destination "
           "in advance useful."), newFreezeGroup);
    freezeReach->setWordWrap(true);
    freezeReach->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    newFreezeForm->addRow(freezeReach);
    layout->addWidget(newFreezeGroup);

    // --- Pause ---
    QGroupBox* pauseGroup = new QGroupBox(tr("Pause the whole asset"), this);
    QVBoxLayout* pauseLayout = new QVBoxLayout(pauseGroup);
    m_pause_hint = new QLabel(pauseGroup);
    m_pause_hint->setWordWrap(true);
    pauseLayout->addWidget(m_pause_hint);
    m_pause_button = new QPushButton(tr("Pause"), pauseGroup);
    QHBoxLayout* pauseRow = new QHBoxLayout();
    pauseRow->addStretch();
    pauseRow->addWidget(m_pause_button);
    pauseLayout->addLayout(pauseRow);
    layout->addWidget(pauseGroup);

    // --- Rotate ---
    QGroupBox* rotateGroup = new QGroupBox(tr("Replace a key"), this);
    QFormLayout* rotateForm = new QFormLayout(rotateGroup);
    m_rotate_which = new QComboBox(rotateGroup);
    m_rotate_which->addItem(tr("Operational key (freezes and unfreezes)"), QStringLiteral("rotateoperational"));
    m_rotate_which->addItem(tr("Recovery key (replaces keys, and nothing else)"), QStringLiteral("rotaterecovery"));
    rotateForm->addRow(tr("Replace:"), m_rotate_which);
    m_rotate_new_key = new QLineEdit(rotateGroup);
    m_rotate_new_key->setPlaceholderText(tr("the new x-only key, 32 bytes of hex"));
    m_rotate_generate = new QPushButton(tr("Make one in this wallet"), rotateGroup);
    QHBoxLayout* keyRow = new QHBoxLayout();
    keyRow->addWidget(m_rotate_new_key, 1);
    keyRow->addWidget(m_rotate_generate);
    rotateForm->addRow(tr("New key:"), keyRow);
    m_rotate_button = new QPushButton(tr("Replace the key"), rotateGroup);
    rotateForm->addRow(QString(), m_rotate_button);
    QLabel* rotateHint = new QLabel(
        tr("Both rotations are signed by the <b>recovery</b> key, never by the operational one. That "
           "asymmetry is the reason there are two keys: someone who steals the operational key can freeze "
           "holders, visibly and on the chain, but can never take the authority away from you, because "
           "rotating is beyond that key. If the operational key is compromised, replace it from cold "
           "storage and the incident is over."
           "<br><br>The keys the asset was issued with are committed in its id and never change; what "
           "rotates is which key is in force now."), rotateGroup);
    rotateHint->setWordWrap(true);
    rotateHint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    rotateForm->addRow(rotateHint);
    layout->addWidget(rotateGroup);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    layout->addStretch();

    connect(refreshBtn, &QPushButton::clicked, this, &SupervisionPage::refresh);
    connect(m_asset_selector, qOverload<int>(&QComboBox::currentIndexChanged), this, &SupervisionPage::onAssetChanged);
    connect(m_freezes, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_freezes->currentRow();
        // Only a freeze whose record this wallet located can be lifted from here:
        // lifting is spending that output, and an output nobody can point at
        // cannot be spent.
        m_unfreeze_button->setEnabled(row >= 0 && m_freezes->item(row, 0) &&
                                      m_records.contains(m_freezes->item(row, 0)->data(Qt::UserRole).toString()));
    });
    connect(m_unfreeze_button, &QPushButton::clicked, this, &SupervisionPage::onUnfreeze);
    connect(checkBtn, &QPushButton::clicked, this, &SupervisionPage::onCheckTarget);
    connect(m_freeze_button, &QPushButton::clicked, this, &SupervisionPage::onFreeze);
    connect(m_pause_button, &QPushButton::clicked, this, &SupervisionPage::onPause);
    connect(m_rotate_generate, &QPushButton::clicked, this, &SupervisionPage::onNewKeyFromWallet);
    connect(m_rotate_button, &QPushButton::clicked, this, &SupervisionPage::onRotate);
}

void SupervisionPage::setModel(WalletModel* model)
{
    m_wallet_model = model;
    if (!m_wallet_model) return;
    // The sidebar tab appears only once this wallet has something to supervise, so
    // the check has to run whether or not the page is ever shown -- and again
    // whenever the balance moves, because that is what a freshly issued supervised
    // asset looks like from here.
    refreshAssets();
    connect(m_wallet_model, &WalletModel::assetTypesChanged, this, &SupervisionPage::refreshAssets);
}

std::string SupervisionPage::walletUri() const
{
    if (!m_wallet_model) return std::string();
    return "/wallet/" + m_wallet_model->getWalletName().toStdString();
}

UniValue SupervisionPage::callWalletRpc(const std::string& method, const UniValue& params, bool& ok, QString& error)
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

void SupervisionPage::setStatus(const QString& msg, bool error)
{
    m_status->setStyleSheet(error ? "color:#ff6b6b;" : "color:#3ecf7a;");
    m_status->setText(msg);
}

bool SupervisionPage::walletHoldsKey(const QString& xonly_hex) const
{
    if (!m_wallet_model || xonly_hex.size() != 64 || !IsHex(xonly_hex.toStdString())) return false;
    // x-only carries no parity, so both candidates are asked about. Judged through
    // the wallet's own view of its scripts, which answers correctly even while the
    // wallet is locked: the Assets page hands out supervision keys via getnewaddress,
    // so they live here as ordinary address keys.
    const std::vector<unsigned char> x = ParseHex(xonly_hex.toStdString());
    for (const unsigned char parity : {0x02, 0x03}) {
        std::vector<unsigned char> compressed{parity};
        compressed.insert(compressed.end(), x.begin(), x.end());
        const CPubKey pubkey(compressed);
        if (!pubkey.IsFullyValid()) continue;
        if (m_wallet_model->wallet().isSpendable(WitnessV0KeyHash(pubkey))) return true;
        if (m_wallet_model->wallet().isSpendable(PKHash(pubkey))) return true;
    }
    return false;
}

void SupervisionPage::refreshAssets()
{
    if (!m_wallet_model) return;
    bool ok; QString err;

    // What this wallet issued. A supervised asset is operated by whoever holds its
    // keys, but the wallet that issued it is the one that can pay for a record and
    // is where an issuer will look, so both count -- and an ordinary holder of a
    // supervised asset, who can do none of this, matches neither and never sees the
    // tab.
    QSet<QString> issued_here;
    UniValue iss = callWalletRpc("listissuances", UniValue(UniValue::VARR), ok, err);
    if (ok && iss.isArray()) {
        for (size_t i = 0; i < iss.size(); ++i) {
            if (iss[i].exists("asset")) issued_here.insert(QString::fromStdString(iss[i]["asset"].getValStr()));
        }
    }

    UniValue supervised = callWalletRpc("getsupervisedassets", UniValue(UniValue::VARR), ok, err);
    const bool was_available = hasSupervision();
    const QString previous = m_asset_selector->currentData().toString();
    m_assets.clear();
    if (ok && supervised.isArray()) {
        for (size_t i = 0; i < supervised.size(); ++i) {
            const UniValue& e = supervised[i];
            if (!e.exists("asset")) continue;
            Asset a;
            a.id = QString::fromStdString(e["asset"].get_str());
            if (e.exists("operationalkey")) a.operational_key = QString::fromStdString(e["operationalkey"].get_str());
            if (e.exists("recoverykey")) a.recovery_key = QString::fromStdString(e["recoverykey"].get_str());
            a.pause_allowed = e.exists("pauseallowed") && e["pauseallowed"].get_bool();
            a.paused = e.exists("paused") && e["paused"].get_bool();
            a.frozen = e.exists("frozen") ? e["frozen"].get_int() : 0;
            a.wallet_has_operational = walletHoldsKey(a.operational_key);
            a.wallet_has_recovery = walletHoldsKey(a.recovery_key);
            if (!a.wallet_has_operational && !a.wallet_has_recovery && !issued_here.contains(a.id)) continue;
            m_assets.append(a);
        }
    }

    m_asset_selector->blockSignals(true);
    m_asset_selector->clear();
    for (const Asset& a : m_assets) {
        const CAsset asset = GetAssetFromString(a.id.toStdString());
        const QString label = GUIUtil::assetIsNamed(asset)
            ? tr("%1 (%2)").arg(GUIUtil::assetDisplayName(asset), GUIUtil::ellipsizeMiddle(a.id))
            : GUIUtil::ellipsizeMiddle(a.id);
        m_asset_selector->addItem(label, a.id);
    }
    const int index = m_asset_selector->findData(previous);
    if (index >= 0) m_asset_selector->setCurrentIndex(index);
    m_asset_selector->blockSignals(false);

    if (was_available != hasSupervision()) Q_EMIT availabilityChanged(hasSupervision());
    if (isVisible()) onAssetChanged();
}

const SupervisionPage::Asset* SupervisionPage::selectedAsset() const
{
    const QString id = m_asset_selector->currentData().toString();
    for (const Asset& a : m_assets) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

void SupervisionPage::onAssetChanged()
{
    const Asset* asset = selectedAsset();
    m_freezes->setRowCount(0);
    m_records.clear();
    m_unfreeze_button->setEnabled(false);
    m_freeze_check->clear();

    if (!asset) {
        m_asset_summary->setText(tr("This wallet has no supervised asset to operate."));
        m_keys_summary->clear();
        m_freeze_button->setEnabled(false);
        m_pause_button->setEnabled(false);
        m_rotate_button->setEnabled(false);
        m_pause_hint->clear();
        return;
    }

    QStringList state;
    if (asset->paused) state << tr("<b>PAUSED</b> — every single-owner holding is stopped");
    if (asset->frozen > 0) state << tr("%n script(s) frozen", "", asset->frozen);
    if (state.isEmpty()) state << tr("nothing frozen");
    m_asset_summary->setText(tr("<b>%1</b><br>%2<br>%3")
                                 .arg(asset->id, state.join(QStringLiteral(", ")),
                                      asset->pause_allowed
                                          ? tr("Issued with the pause capability.")
                                          : tr("Issued without the pause capability, which cannot be added.")));

    // Which key signs what, and whether this wallet can produce that signature, is
    // the thing an operator most needs to know before starting: it decides whether
    // the next dialog signs by itself or asks for a signature from elsewhere.
    auto keyLine = [this](const QString& role, const QString& key, bool ours) {
        return tr("%1: <tt>%2</tt> — %3")
            .arg(role, key.isEmpty() ? tr("unknown") : key,
                 ours ? tr("this wallet can sign") : tr("signed elsewhere (HSM, FROST, an offline key)"));
    };
    m_keys_summary->setText(keyLine(tr("Operational key"), asset->operational_key, asset->wallet_has_operational) +
                            QStringLiteral("<br>") +
                            keyLine(tr("Recovery key"), asset->recovery_key, asset->wallet_has_recovery));

    m_freeze_button->setEnabled(true);
    m_rotate_button->setEnabled(true);
    m_pause_button->setEnabled(asset->pause_allowed);
    m_pause_button->setText(asset->paused ? tr("Lift the pause") : tr("Pause"));
    if (!asset->pause_allowed) {
        m_pause_hint->setText(tr("This asset was issued without the pause capability. The bit is committed in "
                                 "the asset id, so it can never be granted -- which is also why a holder could "
                                 "see, before accepting the asset, that it cannot be stopped wholesale."));
    } else if (asset->paused) {
        m_pause_hint->setText(tr("Every single-owner holding of this asset is frozen. Lifting the pause means "
                                 "spending the record that made it, exactly like any other freeze."));
    } else {
        m_pause_hint->setText(tr("One record freezes every single-owner holding of this asset at once, without "
                                 "naming any of them. Shared scripts stay out of reach, as they are for a "
                                 "targeted freeze."));
    }

    refreshRecords(asset->id);
}

void SupervisionPage::refreshRecords(const QString& asset)
{
    if (!m_wallet_model) return;
    bool ok; QString err;

    // The registry knows which target hashes are frozen and how many records name
    // each; it does not know where those records are. Only the outputs matter for
    // lifting a freeze, and the ones this wallet created are in its own
    // transactions, so that is where they are looked for.
    UniValue params(UniValue::VARR);
    params.push_back(asset.toStdString());
    UniValue freezes = callWalletRpc("getassetfreezes", params, ok, err);
    if (!ok) {
        setStatus(tr("Could not read the freezes for this asset: %1").arg(err), true);
        return;
    }

    // Wallet transactions this session has not looked inside yet. A record output
    // never appears in a transaction later, so each one is examined once.
    UniValue list_params(UniValue::VARR);
    list_params.push_back("*");
    list_params.push_back(1000);
    list_params.push_back(0);
    list_params.push_back(true);
    UniValue txs = callWalletRpc("listtransactions", list_params, ok, err);
    if (ok && txs.isArray()) {
        for (size_t i = 0; i < txs.size(); ++i) {
            if (!txs[i].exists("txid")) continue;
            const QString txid = QString::fromStdString(txs[i]["txid"].get_str());
            if (m_scanned_txids.contains(txid)) continue;
            m_scanned_txids.insert(txid);

            UniValue get_params(UniValue::VARR);
            get_params.push_back(txid.toStdString());
            UniValue tx = callWalletRpc("gettransaction", get_params, ok, err);
            if (!ok || !tx.exists("hex")) continue;
            const QString hex = QString::fromStdString(tx["hex"].get_str());
            // One string search instead of a decode per transaction: an ordinary
            // payment cannot contain the record marker, and a working wallet is
            // almost entirely ordinary payments.
            if (!hex.contains(QLatin1String(RECORD_MARKER_HEX))) continue;

            UniValue decode_params(UniValue::VARR);
            decode_params.push_back(hex.toStdString());
            UniValue decoded = callWalletRpc("decoderawtransaction", decode_params, ok, err);
            if (!ok || !decoded.exists("vout")) continue;
            const UniValue& vout = decoded["vout"];
            for (size_t n = 0; n < vout.size(); ++n) {
                if (!vout[n].exists("scriptPubKey") || !vout[n]["scriptPubKey"].exists("hex")) continue;
                UniValue script_params(UniValue::VARR);
                script_params.push_back(vout[n]["scriptPubKey"]["hex"].get_str());
                UniValue record = callWalletRpc("decodesupervisionscript", script_params, ok, err);
                if (!ok || !record.exists("type") || record["type"].getValStr() != "record") continue;
                if (!record.exists("targethash")) continue; // a rotation, which can never be spent
                if (!record.exists("asset")) continue;
                Record r;
                // Every record found is kept, whatever asset it governs: a wallet
                // may issue several supervised assets, and each transaction is
                // examined only once, so filtering by the asset selected at the
                // time would lose the others for the rest of the session.
                r.asset = QString::fromStdString(record["asset"].get_str());
                r.target_hash = QString::fromStdString(record["targethash"].get_str());
                r.txid = txid;
                r.vout = vout[n].exists("n") ? vout[n]["n"].get_int() : (int)n;
                m_known_records.append(r);
            }
        }
    }

    // Which of them are still unspent, asked fresh every time: an unspent record is
    // a freeze in force, and a spent one is a freeze already lifted.
    m_records.clear();
    for (const Record& r : m_known_records) {
        if (r.asset != asset) continue;
        UniValue txout_params(UniValue::VARR);
        txout_params.push_back(r.txid.toStdString());
        txout_params.push_back(r.vout);
        txout_params.push_back(true); // include the mempool, so a lift in flight stops counting
        UniValue txout = callWalletRpc("gettxout", txout_params, ok, err);
        if (!ok || txout.isNull()) continue;
        m_records.insert(r.target_hash, r);
    }

    m_freezes->setRowCount(0);
    if (!freezes.isArray()) return;
    for (size_t i = 0; i < freezes.size(); ++i) {
        const UniValue& f = freezes[i];
        if (!f.exists("targethash")) continue;
        const QString target = QString::fromStdString(f["targethash"].get_str());
        const int row = m_freezes->rowCount();
        m_freezes->insertRow(row);

        const bool is_pause = (target == QLatin1String(PAUSE_TARGET_HEX));
        QTableWidgetItem* what = new QTableWidgetItem(
            is_pause ? tr("the whole asset (pause)") : GUIUtil::ellipsizeMiddle(target));
        what->setToolTip(is_pause ? tr("A pause names every single-owner script at once.") : target);
        what->setData(Qt::UserRole, target);
        m_freezes->setItem(row, 0, what);

        m_freezes->setItem(row, 1, new QTableWidgetItem(
            QString::number(f.exists("records") ? f["records"].get_int() : 1)));

        const auto found = m_records.constFind(target);
        QTableWidgetItem* liftable = new QTableWidgetItem();
        if (found != m_records.constEnd()) {
            liftable->setText(tr("yes"));
            liftable->setToolTip(tr("Record output %1:%2").arg(found.value().txid).arg(found.value().vout));
        } else {
            liftable->setText(tr("not from this wallet"));
            liftable->setToolTip(tr("The record that made this freeze is not among this wallet's transactions, "
                                    "so this wallet cannot point at the output that has to be spent. Lift it "
                                    "from the wallet that created it."));
        }
        m_freezes->setItem(row, 2, liftable);
    }
}

void SupervisionPage::refresh()
{
    // refreshAssets() redraws the selected asset itself when the page is visible,
    // and stays cheap when it is not: the record scan is not worth running for a
    // tab nobody is looking at.
    refreshAssets();
}

void SupervisionPage::scheduleRefresh()
{
    if (m_refresh_pending) return;
    m_refresh_pending = true;
    QPointer<SupervisionPage> self(this);
    // Let the tab switch paint before the wallet RPCs run on this thread, the same
    // way the Assets page does: the record scan is the slowest thing here.
    QTimer::singleShot(0, this, [self] {
        if (!self) return;
        self->m_refresh_pending = false;
        self->refresh();
    });
}

void SupervisionPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    scheduleRefresh();
}

bool SupervisionPage::fundingOutpoint(QString& txid, int& vout, CAmount& amount, QString& error)
{
    bool ok; QString err;
    UniValue params(UniValue::VARR);
    params.push_back(1); // confirmed only: the sighash binds this outpoint
    UniValue unspent = callWalletRpc("listunspent", params, ok, err);
    if (!ok) { error = err; return false; }

    const QString policy = QString::fromStdString(::policyAsset.GetHex());
    static const QString explicit_blinder(64, QLatin1Char('0'));
    for (size_t i = 0; i < unspent.size(); ++i) {
        const UniValue& u = unspent[i];
        if (!u.exists("asset") || QString::fromStdString(u["asset"].getValStr()) != policy) continue;
        if (u.exists("spendable") && !u["spendable"].get_bool()) continue;
        // The record transaction is built and signed raw here, with explicit values
        // throughout. A confidential input would need blinding, which this path does
        // not do and consensus would reject the result of.
        if (u.exists("amountblinder") &&
            QString::fromStdString(u["amountblinder"].getValStr()) != explicit_blinder) continue;
        CAmount value = 0;
        try { value = AmountFromValue(u["amount"], /*check_range=*/false); } catch (...) { continue; }
        if (value < RECORD_FEE + MIN_CHANGE) continue;
        txid = QString::fromStdString(u["txid"].get_str());
        vout = u["vout"].get_int();
        amount = value;
        return true;
    }
    error = tr("No confirmed, unblinded %1 output large enough to pay for a record transaction. "
               "A record needs a fee of %2, paid in %1 because that is the asset every producer accepts.")
                .arg(GUIUtil::assetDisplayName(::policyAsset),
                     GUIUtil::formatAssetAmount(::policyAsset, RECORD_FEE, BitcoinUnits::BTC,
                                                BitcoinUnits::SeparatorStyle::STANDARD, true));
    return false;
}

QString SupervisionPage::requestSignature(const QString& sighash, const QString& key, const QString& role,
                                          const QString& what)
{
    // The automatic path: the keys came out of this wallet at issuance, so the
    // wallet is asked for the signature. Nothing about the record changes -- the
    // node still says what to sign and still assembles the result -- and the
    // message being signed is the same one the manual path shows.
    if (walletHoldsKey(key)) {
        bool ok; QString err;
        UniValue params(UniValue::VARR);
        params.push_back(sighash.toStdString());
        params.push_back(key.toStdString());
        UniValue signed_value = callWalletRpc("signsupervisionhash", params, ok, err);
        if (ok && signed_value.exists("signature")) {
            return QString::fromStdString(signed_value["signature"].get_str());
        }
        setStatus(tr("This wallet holds the %1 key but could not sign with it: %2").arg(role, err), true);
        // Fall through to the manual path rather than giving up: a locked wallet or
        // a key that has moved is exactly when an issuer reaches for the offline one.
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Sign this message"));
    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    QLabel* explain = new QLabel(
        tr("<b>%1</b><br><br>This must be signed with the asset's current <b>%2</b> key, wherever that key "
           "lives -- an HSM, a FROST quorum, an offline signer. The signature is BIP340 Schnorr over the "
           "32 bytes below, and this node never sees the key."
           "<br><br>The message binds the first input the transaction will spend, so the signature works "
           "in this transaction and no other: it cannot be lifted off the chain and replayed to re-freeze "
           "something you have deliberately released.").arg(what, role), &dlg);
    explain->setWordWrap(true);
    explain->setMinimumWidth(520);
    lay->addWidget(explain);

    lay->addWidget(new QLabel(tr("Key that must sign:"), &dlg));
    QLineEdit* key_field = new QLineEdit(key, &dlg);
    key_field->setReadOnly(true);
    lay->addWidget(key_field);

    lay->addWidget(new QLabel(tr("Message to sign (32 bytes, hex):"), &dlg));
    QLineEdit* hash_field = new QLineEdit(sighash, &dlg);
    hash_field->setReadOnly(true);
    QPushButton* copy = new QPushButton(tr("Copy"), &dlg);
    QHBoxLayout* hash_row = new QHBoxLayout();
    hash_row->addWidget(hash_field, 1);
    hash_row->addWidget(copy);
    lay->addLayout(hash_row);
    connect(copy, &QPushButton::clicked, &dlg, [sighash] { QApplication::clipboard()->setText(sighash); });

    lay->addWidget(new QLabel(tr("Signature (64 bytes, hex):"), &dlg));
    QLineEdit* sig_field = new QLineEdit(&dlg);
    sig_field->setPlaceholderText(tr("paste the signature here"));
    lay->addWidget(sig_field);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return QString();

    const QString signature = sig_field->text().trimmed().toLower();
    if (signature.size() != 128 || !IsHex(signature.toStdString())) {
        setStatus(tr("That is not a 64-byte signature in hex."), true);
        return QString();
    }
    return signature;
}

QString SupervisionPage::sendRecord(const QString& kind, const QString& asset, const UniValue& target,
                                    const UniValue& old_key, bool submit_privately)
{
    const Asset* selected = selectedAsset();
    if (!selected) return QString();
    bool ok; QString err;

    QString funding_txid; int funding_vout = 0; CAmount funding_amount = 0;
    if (!fundingOutpoint(funding_txid, funding_vout, funding_amount, err)) {
        setStatus(err, true);
        return QString();
    }

    // 1. What to sign, and which key must sign it. The node's answer, not ours: the
    // rules about which key signs what are consensus rules.
    UniValue hash_params(UniValue::VARR);
    hash_params.push_back(kind.toStdString());
    hash_params.push_back(asset.toStdString());
    hash_params.push_back(target);
    hash_params.push_back(old_key);
    hash_params.push_back(funding_txid.toStdString());
    hash_params.push_back(funding_vout);
    UniValue sighash_result = callWalletRpc("getsupervisionrecordhash", hash_params, ok, err);
    if (!ok || !sighash_result.exists("sighash")) {
        setStatus(tr("The node would not describe that record: %1").arg(err), true);
        return QString();
    }
    const QString sighash = QString::fromStdString(sighash_result["sighash"].get_str());
    const bool recovery = sighash_result.exists("signwith") &&
                          sighash_result["signwith"].getValStr() == "recovery";
    const QString role = recovery ? tr("recovery") : tr("operational");
    const QString key = recovery ? selected->recovery_key : selected->operational_key;

    // 2. Sign it, here or elsewhere.
    const QString signature = requestSignature(sighash, key, role, kind);
    if (signature.isEmpty()) return QString();

    // 3. Assemble, attach, broadcast.
    UniValue build_params(UniValue::VARR);
    build_params.push_back(kind.toStdString());
    build_params.push_back(asset.toStdString());
    build_params.push_back(target);
    build_params.push_back(old_key);
    build_params.push_back(signature.toStdString());
    UniValue built = callWalletRpc("buildsupervisionrecord", build_params, ok, err);
    if (!ok || !built.exists("script")) {
        setStatus(tr("The record would not assemble: %1").arg(err), true);
        return QString();
    }

    UniValue change = callWalletRpc("getnewaddress", UniValue(UniValue::VARR), ok, err);
    if (!ok) { setStatus(tr("Could not get a change address: %1").arg(err), true); return QString(); }

    UniValue inputs(UniValue::VARR);
    UniValue input(UniValue::VOBJ);
    input.pushKV("txid", funding_txid.toStdString());
    input.pushKV("vout", funding_vout);
    inputs.push_back(input);

    UniValue outputs(UniValue::VARR);
    UniValue change_out(UniValue::VOBJ);
    change_out.pushKV(change.getValStr(), ValueFromAmount(funding_amount - RECORD_FEE));
    outputs.push_back(change_out);
    UniValue fee_out(UniValue::VOBJ);
    fee_out.pushKV("fee", ValueFromAmount(RECORD_FEE));
    outputs.push_back(fee_out);

    UniValue raw_params(UniValue::VARR);
    raw_params.push_back(inputs);
    raw_params.push_back(outputs);
    UniValue raw = callWalletRpc("createrawtransaction", raw_params, ok, err);
    if (!ok) { setStatus(tr("Could not build the transaction: %1").arg(err), true); return QString(); }

    // The record output carries zero of the asset it governs, which keeps it clear
    // of the dust rule and stops an issuer burning value into a script only
    // consensus can release.
    UniValue add_params(UniValue::VARR);
    add_params.push_back(raw.getValStr());
    add_params.push_back(built["script"].get_str());
    add_params.push_back(asset.toStdString());
    UniValue with_record = callWalletRpc("addsupervisionrecordoutput", add_params, ok, err);
    if (!ok) { setStatus(tr("Could not attach the record: %1").arg(err), true); return QString(); }

    UniValue sign_params(UniValue::VARR);
    sign_params.push_back(with_record.getValStr());
    UniValue signed_tx = callWalletRpc("signrawtransactionwithwallet", sign_params, ok, err);
    if (!ok || !signed_tx.exists("hex")) {
        setStatus(tr("The wallet could not sign the transaction: %1").arg(err), true);
        return QString();
    }

    UniValue send_params(UniValue::VARR);
    send_params.push_back(signed_tx["hex"].get_str());
    if (submit_privately) {
        UniValue submitted = callWalletRpc("submitsupervisionrecord", send_params, ok, err);
        if (!ok) { setStatus(tr("The producer would not take the record: %1").arg(err), true); return QString(); }
        return submitted.exists("txid") ? QString::fromStdString(submitted["txid"].get_str()) : QString("?");
    }
    UniValue txid = callWalletRpc("sendrawtransaction", send_params, ok, err);
    if (!ok) { setStatus(tr("The record was refused: %1").arg(err), true); return QString(); }
    return QString::fromStdString(txid.getValStr());
}

QString SupervisionPage::sendUnfreeze(const QString& asset, const Record& record)
{
    bool ok; QString err;
    const Asset* selected = selectedAsset();
    if (!selected) return QString();

    QString funding_txid; int funding_vout = 0; CAmount funding_amount = 0;
    if (!fundingOutpoint(funding_txid, funding_vout, funding_amount, err)) {
        setStatus(err, true);
        return QString();
    }

    UniValue hash_params(UniValue::VARR);
    hash_params.push_back(record.txid.toStdString());
    hash_params.push_back(record.vout);
    hash_params.push_back(asset.toStdString());
    hash_params.push_back(record.target_hash.toStdString());
    UniValue sighash_result = callWalletRpc("getsupervisionunfreezehash", hash_params, ok, err);
    if (!ok) { setStatus(tr("The node would not describe that unfreeze: %1").arg(err), true); return QString(); }
    const QString sighash = QString::fromStdString(sighash_result.getValStr());

    // The current operational key, never the one the record was made under: a
    // rotation retires the old key for unfreezing too, which is what stops a stolen
    // key from lifting the freezes it was used to place.
    const QString signature = requestSignature(sighash, selected->operational_key, tr("operational"),
                                               tr("lift a freeze"));
    if (signature.isEmpty()) return QString();

    UniValue inputs(UniValue::VARR);
    UniValue record_in(UniValue::VOBJ);
    record_in.pushKV("txid", record.txid.toStdString());
    record_in.pushKV("vout", record.vout);
    inputs.push_back(record_in);
    UniValue funding_in(UniValue::VOBJ);
    funding_in.pushKV("txid", funding_txid.toStdString());
    funding_in.pushKV("vout", funding_vout);
    inputs.push_back(funding_in);

    UniValue change = callWalletRpc("getnewaddress", UniValue(UniValue::VARR), ok, err);
    if (!ok) { setStatus(tr("Could not get a change address: %1").arg(err), true); return QString(); }

    UniValue outputs(UniValue::VARR);
    UniValue change_out(UniValue::VOBJ);
    change_out.pushKV(change.getValStr(), ValueFromAmount(funding_amount - RECORD_FEE));
    outputs.push_back(change_out);
    UniValue fee_out(UniValue::VOBJ);
    fee_out.pushKV("fee", ValueFromAmount(RECORD_FEE));
    outputs.push_back(fee_out);

    UniValue raw_params(UniValue::VARR);
    raw_params.push_back(inputs);
    raw_params.push_back(outputs);
    UniValue raw = callWalletRpc("createrawtransaction", raw_params, ok, err);
    if (!ok) { setStatus(tr("Could not build the transaction: %1").arg(err), true); return QString(); }

    // The wallet signs its own input and leaves the record's alone -- no wallet can
    // produce that one, because the record's script is deliberately not the spend
    // authority. Incomplete here is expected, not a failure.
    UniValue sign_params(UniValue::VARR);
    sign_params.push_back(raw.getValStr());
    UniValue signed_tx = callWalletRpc("signrawtransactionwithwallet", sign_params, ok, err);
    if (!ok || !signed_tx.exists("hex")) {
        setStatus(tr("The wallet could not sign the transaction: %1").arg(err), true);
        return QString();
    }
    QString hex = QString::fromStdString(signed_tx["hex"].get_str());

    UniValue decode_params(UniValue::VARR);
    decode_params.push_back(hex.toStdString());
    UniValue decoded = callWalletRpc("decoderawtransaction", decode_params, ok, err);
    if (!ok || !decoded.exists("vin")) { setStatus(tr("Could not read the transaction back: %1").arg(err), true); return QString(); }
    int record_index = -1;
    for (size_t i = 0; i < decoded["vin"].size(); ++i) {
        const UniValue& in = decoded["vin"][i];
        if (in.exists("txid") && QString::fromStdString(in["txid"].get_str()) == record.txid &&
            in.exists("vout") && in["vout"].get_int() == record.vout) {
            record_index = (int)i;
            break;
        }
    }
    if (record_index < 0) { setStatus(tr("The record's input went missing from the transaction."), true); return QString(); }

    UniValue set_params(UniValue::VARR);
    set_params.push_back(hex.toStdString());
    set_params.push_back(record_index);
    set_params.push_back(signature.toStdString());
    UniValue with_sig = callWalletRpc("setsupervisionunfreezesig", set_params, ok, err);
    if (!ok) { setStatus(tr("Could not attach the unfreeze signature: %1").arg(err), true); return QString(); }

    UniValue send_params(UniValue::VARR);
    send_params.push_back(with_sig.getValStr());
    UniValue txid = callWalletRpc("sendrawtransaction", send_params, ok, err);
    if (!ok) { setStatus(tr("The unfreeze was refused: %1").arg(err), true); return QString(); }
    return QString::fromStdString(txid.getValStr());
}

void SupervisionPage::onCheckTarget()
{
    const Asset* asset = selectedAsset();
    if (!asset) return;
    const QString target = m_freeze_target->text().trimmed();
    if (target.isEmpty()) { m_freeze_check->clear(); return; }

    bool ok; QString err;
    UniValue params(UniValue::VARR);
    params.push_back(asset->id.toStdString());
    params.push_back(target.toStdString());
    UniValue status = callWalletRpc("isassetfrozen", params, ok, err);
    if (!ok) { m_freeze_check->setText(tr("That target could not be read: %1").arg(err)); return; }

    const bool frozen = status.exists("frozen") && status["frozen"].get_bool();
    const bool freezable = status.exists("freezable") && status["freezable"].get_bool();
    const bool paused = status.exists("paused") && status["paused"].get_bool();

    QStringList lines;
    if (paused) {
        lines << tr("The whole asset is paused, so this holding is stopped whether or not a freeze names it.");
    } else if (frozen) {
        lines << tr("<b>Already frozen.</b> A second record naming it would freeze it twice over, and the "
                    "freeze would then lift only when both records are spent.");
    } else {
        lines << tr("Not frozen.");
    }
    if (!freezable) {
        // The property that lets supervised assets exist on Lightning and on the
        // DEX at all, and the one that surprises issuers.
        lines << tr("<b>A freeze could not bind this script.</b> It is not single-owner -- a channel "
                    "funding output, an HTLC, a covenant -- and freezing it would strand a counterparty "
                    "who did nothing. The record can still be made, and it would bind nothing.");
    }
    if (status.exists("targethash")) {
        lines << tr("It appears in the list above as <tt>%1</tt>.")
                     .arg(QString::fromStdString(status["targethash"].get_str()));
    }
    m_freeze_check->setText(lines.join(QStringLiteral("<br>")));
}

void SupervisionPage::onFreeze()
{
    const Asset* asset = selectedAsset();
    if (!asset) return;
    const QString target = m_freeze_target->text().trimmed();
    if (target.isEmpty()) {
        setStatus(tr("Enter the address or script to freeze."), true);
        m_freeze_target->setFocus();
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Freeze this holder?"));
    box.setText(tr("Freeze %1 for this asset?").arg(target));
    box.setInformativeText(
        tr("From the block after the one carrying the record, that script can no longer spend this asset. "
           "It can still be paid, and you never gain the power to spend its coins.\n\n"
           "You can lift this again by spending the record, from this wallet."));
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    if (QAbstractButton* okb = box.button(QMessageBox::Ok)) okb->setText(tr("Freeze it"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok) return;

    const QString txid = sendRecord(QStringLiteral("freeze"), asset->id, UniValue(target.toStdString()),
                                    NullUniValue, m_freeze_private->isChecked());
    if (txid.isEmpty()) return;
    if (m_freeze_private->isChecked()) {
        setStatus(tr("Held privately by this node's producer as %1. It is in no mempool and has been "
                     "announced to nobody, so it is invisible until it is in a block -- which happens only "
                     "if this node produces one. Submit it to every producer you have an arrangement with.")
                      .arg(txid), false);
    } else {
        setStatus(tr("Freeze broadcast as %1. It binds from the block after the one that carries it.").arg(txid), false);
    }
    m_freeze_target->clear();
    m_freeze_check->clear();
    scheduleRefresh();
}

void SupervisionPage::onUnfreeze()
{
    const Asset* asset = selectedAsset();
    if (!asset) return;
    const int row = m_freezes->currentRow();
    if (row < 0 || !m_freezes->item(row, 0)) return;
    const QString target = m_freezes->item(row, 0)->data(Qt::UserRole).toString();
    const auto found = m_records.constFind(target);
    if (found == m_records.constEnd()) return;

    const bool is_pause = (target == QLatin1String(PAUSE_TARGET_HEX));
    const int records = m_freezes->item(row, 1) ? m_freezes->item(row, 1)->text().toInt() : 1;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(is_pause ? tr("Lift the pause?") : tr("Lift this freeze?"));
    box.setText(is_pause ? tr("Let every holding of this asset move again?")
                         : tr("Let this script spend this asset again?"));
    QString detail = tr("Lifting a freeze is spending the record that made it. It takes a transaction, and a fee.");
    if (records > 1) {
        detail += tr("\n\nTwo records name this target. The freeze lifts only when the last of them is spent, "
                     "so this will not release it on its own.");
    }
    box.setInformativeText(detail);
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok) return;

    const QString txid = sendUnfreeze(asset->id, found.value());
    if (txid.isEmpty()) return;
    setStatus(tr("Unfreeze broadcast as %1.").arg(txid), false);
    scheduleRefresh();
}

void SupervisionPage::onPause()
{
    const Asset* asset = selectedAsset();
    if (!asset || !asset->pause_allowed) return;

    if (asset->paused) {
        // Lifting a pause is spending its record, exactly like any other freeze, so
        // it goes through the same row in the table.
        const auto found = m_records.constFind(QString::fromLatin1(PAUSE_TARGET_HEX));
        if (found == m_records.constEnd()) {
            setStatus(tr("The record that paused this asset is not among this wallet's transactions, so the "
                         "pause has to be lifted from the wallet that made it."), true);
            return;
        }
        // Asked for in the safe direction too: lifting spends the record, and
        // pausing again costs another transaction and another fee.
        QMessageBox lift(this);
        lift.setIcon(QMessageBox::Question);
        lift.setWindowTitle(tr("Lift the pause?"));
        lift.setText(tr("Let every holding of this asset move again?"));
        lift.setInformativeText(tr("Lifting a pause is spending the record that made it. It takes a "
                                   "transaction, and a fee."));
        lift.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
        lift.setDefaultButton(QMessageBox::Cancel);
        if (lift.exec() != QMessageBox::Ok) return;
        const QString txid = sendUnfreeze(asset->id, found.value());
        if (txid.isEmpty()) return;
        setStatus(tr("The pause is being lifted, in %1.").arg(txid), false);
        scheduleRefresh();
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Pause the whole asset?"));
    box.setText(tr("Stop every single-owner holding of this asset at once?"));
    box.setInformativeText(
        tr("Nobody holding this asset in an ordinary address will be able to spend it until you lift the "
           "pause, and none of them is named by the record. Shared scripts -- channels, swaps, covenants -- "
           "stay out of reach, so a long pause cannot swing a contract racing a deadline.\n\n"
           "Everyone can still be paid."));
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    if (QAbstractButton* okb = box.button(QMessageBox::Ok)) okb->setText(tr("Pause it"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok) return;

    const QString txid = sendRecord(QStringLiteral("pause"), asset->id, NullUniValue, NullUniValue,
                                    m_freeze_private->isChecked());
    if (txid.isEmpty()) return;
    setStatus(tr("Pause record sent as %1.").arg(txid), false);
    scheduleRefresh();
}

void SupervisionPage::onNewKeyFromWallet()
{
    if (!m_wallet_model) return;
    bool ok; QString err;
    UniValue addr = callWalletRpc("getnewaddress", UniValue(UniValue::VARR), ok, err);
    if (!ok) { setStatus(tr("Could not make a key: %1").arg(err), true); return; }
    UniValue info_params(UniValue::VARR);
    info_params.push_back(addr.getValStr());
    UniValue info = callWalletRpc("getaddressinfo", info_params, ok, err);
    if (!ok || !info.exists("pubkey")) { setStatus(tr("Could not read the key back: %1").arg(err), true); return; }
    // A compressed pubkey is 33 bytes; the x-only form is the 32 after the parity
    // byte, which is what BIP340 signs under. Same derivation the Assets page uses
    // at issuance, so a key made here is one this wallet can sign with.
    const QString compressed = QString::fromStdString(info["pubkey"].get_str());
    if (compressed.size() != 66) { setStatus(tr("This wallet returned a key supervision cannot use."), true); return; }
    m_rotate_new_key->setText(compressed.mid(2));
    setStatus(tr("New key made in this wallet. Back the wallet up before you rely on it."), false);
}

void SupervisionPage::onRotate()
{
    const Asset* asset = selectedAsset();
    if (!asset) return;
    const QString kind = m_rotate_which->currentData().toString();
    const bool operational_role = (kind == QLatin1String("rotateoperational"));
    const QString new_key = m_rotate_new_key->text().trimmed().toLower();
    const QString old_key = operational_role ? asset->operational_key : asset->recovery_key;

    if (new_key.size() != 64 || !IsHex(new_key.toStdString())) {
        setStatus(tr("The new key must be 32 bytes of hex: an x-only BIP340 public key."), true);
        m_rotate_new_key->setFocus();
        return;
    }
    if (new_key == old_key) {
        setStatus(tr("That is the key already in force."), true);
        return;
    }
    if (new_key == (operational_role ? asset->recovery_key : asset->operational_key)) {
        setStatus(tr("The two keys must differ, or the separation between using the authority and "
                     "rotating it collapses."), true);
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Replace this key?"));
    box.setText(operational_role ? tr("Replace the operational key?") : tr("Replace the recovery key?"));
    QString detail = tr("From the moment this confirms, %1.\n\nThis record must be signed by the recovery "
                        "key. The operational key cannot rotate anything, not even itself.")
                         .arg(operational_role
                                  ? tr("the old operational key freezes nothing and lifts nothing -- "
                                       "including the freezes it placed")
                                  : tr("only the new recovery key can replace either key"));
    if (!walletHoldsKey(asset->recovery_key)) {
        detail += tr("\n\nThis wallet does not hold the recovery key, so you will be asked for a signature "
                     "made wherever it lives.");
    }
    if (!walletHoldsKey(new_key)) {
        detail += tr("\n\nThe new key is not one this wallet can sign with either. Make sure whoever holds "
                     "it can, because nothing can be undone afterwards without it.");
    }
    box.setInformativeText(detail);
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    if (QAbstractButton* okb = box.button(QMessageBox::Ok)) okb->setText(tr("Replace it"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok) return;

    const QString txid = sendRecord(kind, asset->id, UniValue(new_key.toStdString()),
                                    UniValue(old_key.toStdString()), /*submit_privately=*/false);
    if (txid.isEmpty()) return;
    setStatus(tr("Rotation broadcast as %1. The key the asset was issued with does not change; what changes "
                 "is which key is in force.").arg(txid), false);
    m_rotate_new_key->clear();
    scheduleRefresh();
}
