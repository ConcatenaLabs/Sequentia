// Copyright (c) 2024 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/feepolicydialog.h>

#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <assetsdir.h>
#include <interfaces/node.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

    layout->addWidget(new QLabel(tr("Assets this node accepts for transaction fees. Sequentia "
                                    "privileges no asset — the policy asset is just one entry. "
                                    "Rates are integer atoms of the asset equal to one reference fee atom (rfa)."),
                                 this));

    // Effective acceptance set (static ∪ dynamic).
    m_effective = new QTableWidget(0, 4, this);
    m_effective->setHorizontalHeaderLabels({tr("Asset"), tr("Rate"), tr("Origin"), tr("Source")});
    m_effective->horizontalHeader()->setStretchLastSection(true);
    m_effective->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_effective->verticalHeader()->setVisible(false);
    m_effective->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_effective->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_effective->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_effective, 1);

    // Edit the STATIC layer.
    auto* edit = new QGroupBox(tr("Static whitelist (operator-set)"), this);
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

    // Dynamic (price-server) layer, read-only.
    layout->addWidget(new QLabel(tr("Dynamic rates (published by a price server):"), this));
    m_dynamic = new QTableWidget(0, 5, this);
    m_dynamic->setHorizontalHeaderLabels({tr("Asset"), tr("Rate"), tr("Source"), tr("Age (s)"), tr("Stale")});
    m_dynamic->horizontalHeader()->setStretchLastSection(true);
    m_dynamic->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_dynamic->verticalHeader()->setVisible(false);
    m_dynamic->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_dynamic, 1);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_refresh = bb->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    layout->addWidget(bb);

    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_refresh, &QPushButton::clicked, this, &FeePolicyDialog::refresh);
    connect(m_add, &QPushButton::clicked, this, &FeePolicyDialog::onAddOrUpdate);
    connect(m_remove, &QPushButton::clicked, this, &FeePolicyDialog::onRemoveSelected);
}

void FeePolicyDialog::setModel(WalletModel* model)
{
    m_wallet_model = model;
    if (m_wallet_model) {
        m_asset->clear();
        for (const CAsset& asset : m_wallet_model->getAssetTypes()) {
            m_asset->addItem(QString::fromStdString(gAssetsDir.GetIdentifier(asset)),
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

std::map<std::string, int64_t> FeePolicyDialog::currentStaticRates(bool& ok, QString& err)
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

    // Effective acceptance set.
    UniValue policy = callRpc("getfeeacceptancepolicy", UniValue(UniValue::VARR), ok, err);
    m_effective->setRowCount(0);
    m_remove->setEnabled(false);
    if (ok) {
        const std::vector<std::string> keys = policy.getKeys();
        m_effective->setRowCount((int)keys.size());
        for (int row = 0; row < (int)keys.size(); ++row) {
            const std::string& k = keys[row];
            const UniValue& e = policy[k];
            const QString origin = e["origin"].isStr() ? QString::fromStdString(e["origin"].get_str()) : "";
            m_effective->setItem(row, 0, roItem(QString::fromStdString(k)));
            m_effective->setItem(row, 1, roItem(e["rate"].isNum() ? QString::number(e["rate"].get_int64()) : QString()));
            m_effective->setItem(row, 2, roItem(origin));
            m_effective->setItem(row, 3, roItem(e["source"].isStr() ? QString::fromStdString(e["source"].get_str()) : ""));
        }
        setStatus(tr("Loaded %1 accepted asset(s).").arg(keys.size()));
    } else {
        setStatus(err, true);
    }

    // Dynamic layer.
    UniValue dyn = callRpc("getdynamicfeerates", UniValue(UniValue::VARR), ok, err);
    m_dynamic->setRowCount(0);
    if (ok) {
        const std::vector<std::string> keys = dyn.getKeys();
        m_dynamic->setRowCount((int)keys.size());
        for (int row = 0; row < (int)keys.size(); ++row) {
            const std::string& k = keys[row];
            const UniValue& e = dyn[k];
            m_dynamic->setItem(row, 0, roItem(QString::fromStdString(k)));
            m_dynamic->setItem(row, 1, roItem(e["rate"].isNum() ? QString::number(e["rate"].get_int64()) : QString()));
            m_dynamic->setItem(row, 2, roItem(e["source"].isStr() ? QString::fromStdString(e["source"].get_str()) : ""));
            m_dynamic->setItem(row, 3, roItem(e["age"].isNum() ? QString::number(e["age"].get_int64()) : QString()));
            m_dynamic->setItem(row, 4, roItem(e["stale"].isBool() && e["stale"].get_bool() ? tr("yes") : tr("no")));
        }
    }
    // Enable Remove only when a static row is selected.
    connect(m_effective->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        const auto sel = m_effective->selectionModel()->selectedRows();
        bool isStatic = false;
        if (!sel.isEmpty()) {
            QTableWidgetItem* o = m_effective->item(sel.first().row(), 2);
            isStatic = o && o->text() == "static";
        }
        m_remove->setEnabled(isStatic);
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
    std::map<std::string, int64_t> base = currentStaticRates(ok, err);
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
    const auto sel = m_effective->selectionModel()->selectedRows();
    if (sel.isEmpty()) { setStatus(tr("Select a static entry to remove."), true); return; }
    const int row = sel.first().row();
    QTableWidgetItem* originItem = m_effective->item(row, 2);
    if (!originItem || originItem->text() != "static") {
        setStatus(tr("Only static (operator-set) entries can be removed here."), true);
        return;
    }
    const std::string assetKey = m_effective->item(row, 0)->text().toStdString();

    bool ok = false; QString err;
    std::map<std::string, int64_t> base = currentStaticRates(ok, err);
    if (!ok) { setStatus(err, true); return; }
    base.erase(assetKey);

    UniValue obj(UniValue::VOBJ);
    for (const auto& kv : base) obj.pushKV(kv.first, kv.second);
    UniValue params(UniValue::VARR);
    params.push_back(obj);
    callRpc("setfeeexchangerates", params, ok, err);
    if (ok) { setStatus(tr("Removed.")); refresh(); } else { setStatus(err, true); }
}
