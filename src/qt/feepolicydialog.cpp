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
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
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

    layout->addWidget(new QLabel(tr("Assets this node accepts for transaction fees — the fee "
                                    "whitelist. Sequentia privileges no asset; the policy asset is "
                                    "just one entry. Rates are integer atoms of the asset equal to "
                                    "one reference fee atom (rfa). Entries are set manually below, "
                                    "or maintained automatically by a price server if one is running."),
                                 this));

    // The single fee whitelist (the effective accepted set).
    m_whitelist = new QTableWidget(0, 3, this);
    m_whitelist->setHorizontalHeaderLabels({tr("Asset"), tr("Rate (atoms per rfa)"), tr("Managed by")});
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
    m_status->setStyleSheet(error ? "color:#a00;" : "color:#070;");
    m_status->setText(msg);
}

std::map<std::string, int64_t> FeePolicyDialog::currentManualRates(bool& ok, QString& err)
{
    std::map<std::string, int64_t> out;
    UniValue p = callRpc("getfeeacceptancepolicy", UniValue(UniValue::VARR), ok, err);
    if (!ok) return out;
    for (const std::string& k : p.getKeys()) {
        const UniValue& e = p[k];
        if (e.exists("origin") && e["origin"].isStr() && e["origin"].get_str() == "static" && e["rate"].isNum()) {
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
        const std::vector<std::string> keys = policy.getKeys();
        m_whitelist->setRowCount((int)keys.size());
        for (int row = 0; row < (int)keys.size(); ++row) {
            const std::string& k = keys[row];
            const UniValue& e = policy[k];
            const QString origin = e["origin"].isStr() ? QString::fromStdString(e["origin"].get_str()) : "";
            const QString source = e["source"].isStr() ? QString::fromStdString(e["source"].get_str()) : "";
            // Derive "Managed by" from origin/source: operator-set vs price-server-maintained.
            QString managedBy = "—";
            if (origin == "static") {
                managedBy = tr("operator");
            } else if (origin == "dynamic" || !source.isEmpty()) {
                managedBy = source.isEmpty() ? tr("price server") : source;
            }
            // Stash the raw origin in the Asset item so onRemoveSelected can tell manual entries apart.
            auto* assetItem = roItem(QString::fromStdString(k));
            assetItem->setData(Qt::UserRole, origin);
            m_whitelist->setItem(row, 0, assetItem);
            m_whitelist->setItem(row, 1, roItem(e["rate"].isNum() ? QString::number(e["rate"].get_int64()) : QString()));
            m_whitelist->setItem(row, 2, roItem(managedBy));
        }
        setStatus(tr("Loaded %1 accepted asset(s).").arg(keys.size()));
    } else {
        setStatus(err, true);
    }

    // Enable Remove only when a manually-set (operator) row is selected.
    connect(m_whitelist->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        const auto sel = m_whitelist->selectionModel()->selectedRows();
        bool isManual = false;
        if (!sel.isEmpty()) {
            QTableWidgetItem* a = m_whitelist->item(sel.first().row(), 0);
            isManual = a && a->data(Qt::UserRole).toString() == "static";
        }
        m_remove->setEnabled(isManual);
    });
}

void FeePolicyDialog::onAddOrUpdate()
{
    const std::string assetKey = m_asset->currentText().trimmed().toStdString();
    if (assetKey.empty()) { setStatus(tr("Choose an asset."), true); return; }
    bool rateOk = false;
    const long long rate = m_rate->text().toLongLong(&rateOk);
    if (!rateOk || rate < 0) { setStatus(tr("Rate must be a non-negative integer (0 = refuse)."), true); return; }

    bool ok = false; QString err;
    std::map<std::string, int64_t> base = currentManualRates(ok, err);
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
    if (sel.isEmpty()) { setStatus(tr("Select a manually-set entry to remove."), true); return; }
    const int row = sel.first().row();
    QTableWidgetItem* assetItem = m_whitelist->item(row, 0);
    if (!assetItem || assetItem->data(Qt::UserRole).toString() != "static") {
        setStatus(tr("Only manually-set (operator) entries can be removed here."), true);
        return;
    }
    const std::string assetKey = assetItem->text().toStdString();

    bool ok = false; QString err;
    std::map<std::string, int64_t> base = currentManualRates(ok, err);
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
    // Best-effort, non-crashing. The only node-side sidecar awareness: start the price-server
    // detached so it maintains the whitelist. The GUI's working dir is not guaranteed, so we do
    // NOT resolve the script from the binary location; instead honour optional gArgs overrides and
    // give a clear hint if we cannot proceed.
    const QString hint = tr("Price server not launched: copy contrib/price-server/config.example.json "
                            "to config.json, add your API keys, then set -priceserverconfig=<path> "
                            "(or launch it manually).");

    // Full command override wins (e.g. -priceservercmd="python3 /abs/price_server.py --config /abs/config.json").
    const QString cmdArg = QString::fromStdString(gArgs.GetArg("-priceservercmd", ""));
    if (!cmdArg.isEmpty()) {
        const QStringList parts = cmdArg.split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty()) { setStatus(hint, true); return; }
        qint64 pid = 0;
        const bool launched = QProcess::startDetached(parts.first(), parts.mid(1), QString(), &pid);
        if (launched) {
            setStatus(tr("Price server launched (PID %1) — it will maintain the whitelist; Refresh to see updates.").arg(pid));
        } else {
            setStatus(hint, true);
        }
        return;
    }

    // Need python3 on PATH.
    const QString python3 = QStandardPaths::findExecutable("python3");
    if (python3.isEmpty()) { setStatus(hint, true); return; }

    // Resolve the config: explicit override, else a config.json next to the script.
    QString configArg = QString::fromStdString(gArgs.GetArg("-priceserverconfig", ""));
    const QString scriptPath = "/home/aejkohl/SequentiaByClaude/contrib/price-server/price_server.py";
    if (!QFileInfo::exists(scriptPath)) { setStatus(hint, true); return; }
    if (configArg.isEmpty()) {
        const QString defaultConfig = "/home/aejkohl/SequentiaByClaude/contrib/price-server/config.json";
        if (QFileInfo::exists(defaultConfig)) configArg = defaultConfig;
    }
    if (configArg.isEmpty() || !QFileInfo::exists(configArg)) { setStatus(hint, true); return; }

    QStringList args;
    args << scriptPath << "--config" << configArg;
    qint64 pid = 0;
    const bool launched = QProcess::startDetached(python3, args, QString(), &pid);
    if (launched) {
        setStatus(tr("Price server launched (PID %1) — it will maintain the whitelist; Refresh to see updates.").arg(pid));
    } else {
        setStatus(hint, true);
    }
}
