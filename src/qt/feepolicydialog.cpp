// Copyright (c) 2024 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/feepolicydialog.h>

#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <assetsdir.h>
#include <interfaces/node.h>
#include <util/system.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <climits>

static QTableWidgetItem* roItem(const QString& text)
{
    auto* it = new QTableWidgetItem(text);
    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    return it;
}

FeePolicyDialog::FeePolicyDialog(const PlatformStyle* platformStyle, QWidget* parent)
    : QDialog(parent), m_platform_style(platformStyle)
{
    setWindowTitle(tr("Fee acceptance policy"));
    resize(640, 560);
    auto* layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("Assets this node accepts for transaction fees: the fee "
                                    "whitelist. Sequentia privileges no asset; the policy asset is "
                                    "just one entry. Rates are integer atoms of the asset equal to "
                                    "one reference fee atom (rfa). Entries are set manually below, "
                                    "or maintained automatically by a price server if one is running."),
                                 this));

    // The single fee whitelist (the effective accepted set).
    m_whitelist = new QTableWidget(0, 2, this);
    m_whitelist->setHorizontalHeaderLabels({tr("Asset"), tr("Rate (atoms per rfa)")});
    m_whitelist->horizontalHeader()->setStretchLastSection(true);
    m_whitelist->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_whitelist->verticalHeader()->setVisible(false);
    m_whitelist->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_whitelist->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_whitelist->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_whitelist, 1);

    // Edit the whitelist manually (operator path).
    auto* edit = new QGroupBox(tr("Edit whitelist (manual)"), this);
    auto* form = new QFormLayout(edit);
    m_asset = new QComboBox(edit);
    m_asset->setEditable(true);
    m_asset->setToolTip(tr("Asset label or hex id."));
    form->addRow(tr("Asset:"), m_asset);
    m_rate = new QLineEdit(edit);
    m_rate->setValidator(new QIntValidator(0, INT_MAX, m_rate));
    m_rate->setToolTip(tr("Atoms of the asset equal to one reference fee atom (rfa). 0 = refuse this asset."));
    form->addRow(tr("Rate:"), m_rate);
    auto* btns = new QHBoxLayout();
    m_add = new QPushButton(tr("Add / update"), edit);
    m_remove = new QPushButton(tr("Remove selected"), edit);
    btns->addWidget(m_add);
    btns->addWidget(m_remove);
    btns->addStretch(1);
    form->addRow(btns);
    layout->addWidget(edit);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_refresh = bb->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    m_launch_price_server = bb->addButton(tr("Launch price server"), QDialogButtonBox::ActionRole);
    layout->addWidget(bb);

    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_refresh, &QPushButton::clicked, this, &FeePolicyDialog::refresh);
    connect(m_add, &QPushButton::clicked, this, &FeePolicyDialog::onAddOrUpdate);
    connect(m_remove, &QPushButton::clicked, this, &FeePolicyDialog::onRemoveSelected);
    connect(m_launch_price_server, &QPushButton::clicked, this, &FeePolicyDialog::onLaunchPriceServer);
}

void FeePolicyDialog::setModel(WalletModel* model)
{
    m_wallet_model = model;
    if (m_wallet_model) {
        m_asset->clear();
        for (const CAsset& asset : m_wallet_model->getAssetTypes()) {
            m_asset->addItem(GUIUtil::assetDisplayName(asset),
                             QString::fromStdString(asset.GetHex()));
        }
        m_asset->setCurrentText("");
    }
    refresh();
}

UniValue FeePolicyDialog::callRpc(const std::string& method, const UniValue& params, bool& ok, QString& error)
{
    ok = false;
    if (!m_wallet_model) { error = tr("No wallet loaded."); return UniValue(); }
    try {
        UniValue r = m_wallet_model->node().executeRpc(method, params, std::string()); // node-scoped
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

void FeePolicyDialog::setStatus(const QString& msg, bool error)
{
    m_status->setStyleSheet(error ? "color:#ff6b6b;" : "color:#3ecf7a;");
    m_status->setText(msg);
}

std::map<std::string, int64_t> FeePolicyDialog::currentRates(bool& ok, QString& err)
{
    // The complete current policy: every accepted asset and its rate. We must merge edits on top
    // of this full set, because setfeeexchangerates REPLACES the whole map (anything omitted is
    // dropped). getfeeacceptancepolicy returns { "<asset>": { "rate": <num> }, ... }.
    std::map<std::string, int64_t> out;
    UniValue p = callRpc("getfeeacceptancepolicy", UniValue(UniValue::VARR), ok, err);
    if (!ok) return out;
    for (const std::string& k : p.getKeys()) {
        const UniValue& e = p[k];
        if (e.exists("rate") && e["rate"].isNum()) {
            out[k] = e["rate"].get_int64();
        }
    }
    return out;
}

void FeePolicyDialog::refresh()
{
    bool ok = false; QString err;

    // The single fee whitelist.
    UniValue policy = callRpc("getfeeacceptancepolicy", UniValue(UniValue::VARR), ok, err);
    m_whitelist->setRowCount(0);
    m_remove->setEnabled(false);
    if (ok) {
        // getfeeacceptancepolicy returns { "<asset>": { "rate": <num> }, ... }. There is no
        // per-entry origin/source — it is one flat whitelist. Show asset + rate; every row is
        // editable/removable by the operator.
        const std::vector<std::string> keys = policy.getKeys();
        m_whitelist->setRowCount((int)keys.size());
        for (int row = 0; row < (int)keys.size(); ++row) {
            const std::string& k = keys[row];
            const UniValue& e = policy[k];
            m_whitelist->setItem(row, 0, roItem(QString::fromStdString(k)));
            m_whitelist->setItem(row, 1, roItem(e["rate"].isNum() ? QString::number(e["rate"].get_int64()) : QString()));
        }
        setStatus(tr("Loaded %1 accepted asset(s).").arg(keys.size()));
    } else {
        setStatus(err, true);
    }

    // Enable Remove whenever any row is selected (connect once).
    if (!m_remove_selection_connected) {
        m_remove_selection_connected = true;
        connect(m_whitelist->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
            m_remove->setEnabled(!m_whitelist->selectionModel()->selectedRows().isEmpty());
        });
    }
}

void FeePolicyDialog::onAddOrUpdate()
{
    const std::string assetKey = m_asset->currentText().trimmed().toStdString();
    if (assetKey.empty()) { setStatus(tr("Choose an asset."), true); return; }
    bool rateOk = false;
    const long long rate = m_rate->text().toLongLong(&rateOk);
    if (!rateOk || rate < 0) { setStatus(tr("Rate must be a non-negative integer (0 = refuse)."), true); return; }

    bool ok = false; QString err;
    // Start from the FULL current policy so every other accepted asset is preserved
    // (setfeeexchangerates replaces the whole map), then set/overwrite just this asset.
    std::map<std::string, int64_t> base = currentRates(ok, err);
    if (!ok) { setStatus(err, true); return; }
    base[assetKey] = (int64_t)rate;

    UniValue obj(UniValue::VOBJ);
    for (const auto& kv : base) obj.pushKV(kv.first, kv.second);
    UniValue params(UniValue::VARR);
    params.push_back(obj);
    callRpc("setfeeexchangerates", params, ok, err);
    if (ok) { setStatus(tr("Saved.")); refresh(); } else { setStatus(err, true); }
}

void FeePolicyDialog::onRemoveSelected()
{
    const auto sel = m_whitelist->selectionModel()->selectedRows();
    if (sel.isEmpty()) { setStatus(tr("Select an entry to remove."), true); return; }
    const int row = sel.first().row();
    QTableWidgetItem* assetItem = m_whitelist->item(row, 0);
    if (!assetItem) { setStatus(tr("Select an entry to remove."), true); return; }
    const std::string assetKey = assetItem->text().toStdString();

    bool ok = false; QString err;
    // Start from the FULL current policy, drop just this asset, and write the rest back so the
    // other accepted assets survive (setfeeexchangerates replaces the whole map).
    std::map<std::string, int64_t> base = currentRates(ok, err);
    if (!ok) { setStatus(err, true); return; }
    base.erase(assetKey);

    UniValue obj(UniValue::VOBJ);
    for (const auto& kv : base) obj.pushKV(kv.first, kv.second);
    UniValue params(UniValue::VARR);
    params.push_back(obj);
    callRpc("setfeeexchangerates", params, ok, err);
    if (ok) { setStatus(tr("Removed.")); refresh(); } else { setStatus(err, true); }
}

void FeePolicyDialog::onLaunchPriceServer()
{
    // Resolve the bundled sidecar the SAME way BitcoinGUI::launchPriceServer does
    // (GUIUtil::findPriceServerFile). Never hardcode a /home/<user> path — that is
    // dead on every shipped install.
    const QString appDir = QCoreApplication::applicationDirPath();
    QString script = GUIUtil::findPriceServerFile(QStringLiteral("price_server.py"));
    if (script.isEmpty()) {
        // Offer to be pointed at it rather than leaving a dead end; the choice is
        // remembered, so this is asked once.
        QMessageBox box(QMessageBox::Warning, tr("Price server"),
                        tr("Could not find the price-server script (price_server.py). "
                           "It ships beside the node binary, or in contrib/price-server/ in the source tree."),
                        QMessageBox::Cancel, this);
        QPushButton* locate = box.addButton(tr("Locate…"), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() != locate) {
            setStatus(tr("Could not find the price-server script (price_server.py)."), true);
            return;
        }
        script = GUIUtil::promptForPriceServerScript(this);
        if (script.isEmpty()) return;
    }
    const QString sdir = QFileInfo(script).absolutePath();

    // A Python interpreter: bundled next to the script (packaged build), else the system python3.
    QString python;
    const QStringList pyCands{ sdir + "/python/python3", sdir + "/python/python.exe",
                              appDir + "/python/python3", appDir + "/python/python.exe" };
    for (const QString& c : pyCands) {
        if (QFileInfo::exists(c)) { python = QFileInfo(c).absoluteFilePath(); break; }
    }
    if (python.isEmpty()) python = QStringLiteral("python3");

    // Per-user config in the app data dir; seed it from the bundled config.example.json on first run.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dataDir.isEmpty()) QDir().mkpath(dataDir);
    const QString cfg = (dataDir.isEmpty() ? sdir : dataDir) + "/price-server.json";
    if (!QFileInfo::exists(cfg)) {
        QString example = GUIUtil::findPriceServerFile(QStringLiteral("config.example.json"));
        if (example.isEmpty()) {
            const QString beside = sdir + QStringLiteral("/config.example.json");
            if (QFileInfo::exists(beside)) example = beside;
        }
        if (example.isEmpty() || !QFile::copy(example, cfg)) {
            setStatus(tr("Could not create a default price-server config at %1.").arg(cfg), true);
            return;
        }
    }

    const int uiPort = 8089;
    const QStringList args{script, "--config", cfg, "--ui-port", QString::number(uiPort), "--ui-host", "127.0.0.1"};
    if (!QProcess::startDetached(python, args, sdir)) {
        setStatus(tr("Failed to start the price server using '%1'. Ensure Python is available.").arg(python), true);
        return;
    }

    // Give the sidecar a moment to bind, then open its configuration UI in the browser.
    const QString url = QString("http://127.0.0.1:%1/").arg(uiPort);
    QTimer::singleShot(1500, this, [url]{ QDesktopServices::openUrl(QUrl(url)); });
    setStatus(tr("The price server is starting; its configuration page will open at %1. "
                 "It will maintain the whitelist; Refresh to see updates.").arg(url));
}
