// Copyright (c) 2024 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/feepolicydialog.h>


#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <assetsdir.h>
#include <chainparamsbase.h>
#include <feeassets.h>
#include <interfaces/node.h>
#include <util/system.h>

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
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
#include <QDoubleValidator>
#include <QIntValidator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
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
#include <memory>
#include <thread>

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
    // Tall enough for the price-server section, the list and the editor at once;
    // at the old height the list was squeezed to a single row.
    resize(680, 760);
    auto* layout = new QVBoxLayout(this);

    // Wrapped, or this paragraph states its own length as the dialog's minimum
    // width and the window opens wider than the screen with no way to drag it back.
    auto* intro = new QLabel(tr("Assets this node accepts for transaction fees, and what one unit of "
                                "each is worth. A fee is only worth what the asset it is paid in is "
                                "worth, so this is what lets fees in different assets be compared. "
                                "An asset that is not listed is not accepted. The whitelist is static "
                                "when you write it by hand here, and dynamic when this node is "
                                "connected to a price server: connect to one below, another "
                                "operator's or your own."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // Which price server writes the whitelist, if any, and how to change that.
    auto* ps = new QGroupBox(tr("Price server"), this);
    auto* ps_layout = new QVBoxLayout(ps);
    m_ps_status = new QLabel(ps);
    m_ps_status->setWordWrap(true);
    m_ps_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ps_layout->addWidget(m_ps_status);
    auto* ps_row = new QHBoxLayout();
    m_ps_url = new QLineEdit(ps);
    m_ps_url->setPlaceholderText(tr("Address of a price server's published list, e.g. http://host:8089/feewhitelist"));
    m_ps_url->setToolTip(tr("A price server publishes its whitelist at an address like this one. A price "
                            "feed's address is not a price server and is refused."));
    m_ps_connect = new QPushButton(tr("Connect"), ps);
    m_ps_disconnect = new QPushButton(tr("Disconnect"), ps);
    m_ps_disconnect->setToolTip(tr("Keep the list in force as a static whitelist and stop taking updates."));
    ps_row->addWidget(m_ps_url, 1);
    ps_row->addWidget(m_ps_connect);
    ps_row->addWidget(m_ps_disconnect);
    ps_layout->addLayout(ps_row);
    layout->addWidget(ps);

    // The single fee whitelist (the effective accepted set).
    m_whitelist = new QTableWidget(0, 2, this);
    m_whitelist->setMinimumHeight(140);
    m_whitelist->setHorizontalHeaderLabels({tr("Asset"), tr("Price of 1 unit (USD)")});
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
    // A price, not the node's internal rate. The rate is atoms of the asset per
    // reference fee atom, carrying a factor for the asset's decimals; nobody
    // should have to think in that unit to say what a thing is worth.
    auto* price_validator = new QDoubleValidator(0.0, 1e12, 10, m_rate);
    price_validator->setNotation(QDoubleValidator::StandardNotation);
    m_rate->setValidator(price_validator);
    m_rate->setPlaceholderText(tr("e.g. 0.03"));
    m_rate->setToolTip(tr("What one whole unit of the asset is worth in USD. 0 refuses the asset."));
    form->addRow(tr("Price of 1 unit (USD):"), m_rate);
    auto* btns = new QHBoxLayout();
    m_add = new QPushButton(tr("Add / update"), edit);
    m_remove = new QPushButton(tr("Remove selected"), edit);
    btns->addWidget(m_add);
    btns->addWidget(m_remove);
    btns->addStretch(1);
    form->addRow(btns);
    layout->addWidget(edit);

    m_status = new QLabel(this);
    // Wrapped: an error quoting a price server's answer is long, and an
    // unwrapped label widens the whole dialog past the screen's edge.
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_refresh = bb->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    m_launch_price_server = bb->addButton(tr("Run my own price server"), QDialogButtonBox::ActionRole);
    layout->addWidget(bb);

    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_refresh, &QPushButton::clicked, this, &FeePolicyDialog::refresh);
    connect(m_add, &QPushButton::clicked, this, &FeePolicyDialog::onAddOrUpdate);
    connect(m_remove, &QPushButton::clicked, this, &FeePolicyDialog::onRemoveSelected);
    connect(m_launch_price_server, &QPushButton::clicked, this, &FeePolicyDialog::onLaunchPriceServer);
    connect(m_ps_connect, &QPushButton::clicked, this, &FeePolicyDialog::onConnectPriceServer);
    connect(m_ps_disconnect, &QPushButton::clicked, this, &FeePolicyDialog::onDisconnectPriceServer);
}

static QString timeText(int64_t t)
{
    return t > 0 ? GUIUtil::dateTimeStr(QDateTime::fromSecsSinceEpoch(t)) : QObject::tr("never");
}

void FeePolicyDialog::showPriceServerState()
{
    const UniValue& s = m_ps_state;
    if (!s.isObject()) { m_ps_status->setText(QString()); return; }
    const bool connected = s["connected"].isTrue();
    const QString network = s["network_price_server"].isStr() ? QString::fromStdString(s["network_price_server"].get_str()) : QString();
    m_ps_disconnect->setEnabled(connected);
    m_ps_disconnect->setToolTip(tr("Keep the list in force as a static whitelist and stop taking updates."));
    if (!connected) {
        QString text = tr("Not connected to a price server. The whitelist is static: it is what you write "
                          "to it by hand.");
        if (s["disconnected_because"].isStr()) {
            text += " " + tr("Disconnected because %1.").arg(QString::fromStdString(s["disconnected_because"].get_str()));
        }
        m_ps_status->setStyleSheet(QString());
        m_ps_status->setText(text);
        if (m_ps_url->text().isEmpty()) m_ps_url->setText(network);
        return;
    }
    const QString url = QString::fromStdString(s["url"].get_str());
    if (m_ps_url->text().isEmpty()) m_ps_url->setText(url);
    QString text = s["default"].isTrue()
        ? tr("Connected to the network's price server, %1. The whitelist is dynamic: that server writes it.").arg(url)
        : tr("Connected to the price server at %1. The whitelist is dynamic: that server writes it.").arg(url);
    const QString error = s["last_error"].isStr() ? QString::fromStdString(s["last_error"].get_str()) : QString();
    if (s["has_list"].isTrue()) {
        text += " " + tr("Last written %1.").arg(timeText(s["list_written"].get_int64()));
        if (!error.isEmpty()) text += " " + tr("Its last answer was refused or did not arrive (%1); the list in force is kept.").arg(error);
    } else {
        text += " " + tr("It has not sent a list yet%1, so no asset is accepted until it does.")
                          .arg(error.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(error));
    }
    const bool stale = s["has_list"].isTrue() && s["list_written"].get_int64() > 0 &&
                       QDateTime::currentSecsSinceEpoch() - s["list_written"].get_int64() > 3600;
    if (stale) text += " " + tr("The list in force is more than an hour old.");
    m_ps_status->setStyleSheet((!error.isEmpty() || stale || !s["has_list"].isTrue()) ? "color:#ffb020;" : QString());
    m_ps_status->setText(text);
}

bool FeePolicyDialog::confirmHandWrite()
{
    if (!m_ps_state.isObject() || !m_ps_state["connected"].isTrue()) return true;
    const QString url = QString::fromStdString(m_ps_state["url"].get_str());
    return QMessageBox::question(this, tr("Write the whitelist by hand?"),
               tr("This node is connected to the price server at %1, which writes its whitelist. Writing it by "
                  "hand disconnects the node from that server for good, and the whitelist becomes static. You "
                  "can connect again here at any time.\n\nWrite it by hand?").arg(url),
               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

void FeePolicyDialog::setPriceServerBusy(bool busy)
{
    m_ps_busy = busy;
    for (QPushButton* b : {m_ps_connect, m_ps_disconnect, m_add, m_remove, m_launch_price_server}) {
        if (b) b->setEnabled(!busy);
    }
    if (!busy) showPriceServerState();
}

void FeePolicyDialog::onConnectPriceServer()
{
    const std::string url = m_ps_url->text().trimmed().toStdString();
    if (url.empty()) { setStatus(tr("Enter the address of a price server's published list."), true); return; }
    if (!m_wallet_model || m_ps_busy) return;
    setPriceServerBusy(true);
    setStatus(tr("Asking the price server at %1…").arg(QString::fromStdString(url)));
    LocalPriceServer::SetPriceServerAsync(m_wallet_model->node(), this, url, [this](bool ok, const QString& err) {
        setPriceServerBusy(false);
        if (ok) { setStatus(tr("Connected; the whitelist is the price server's.")); refresh(); }
        else { setStatus(err, true); }
    });
}

void FeePolicyDialog::onDisconnectPriceServer()
{
    if (!m_wallet_model || m_ps_busy) return;
    setPriceServerBusy(true);
    LocalPriceServer::SetPriceServerAsync(m_wallet_model->node(), this, std::string(), [this](bool ok, const QString& err) {
        setPriceServerBusy(false);
        if (ok) { setStatus(tr("Disconnected. The list in force is kept as a static whitelist.")); refresh(); }
        else { setStatus(err, true); }
    });
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

int LocalPriceServer::Port()
{
    const int64_t port = gArgs.GetIntArg("-ownpriceserverport", DEFAULT_PORT);
    return port > 0 && port <= 65535 ? static_cast<int>(port) : DEFAULT_PORT;
}

QString LocalPriceServer::ListUrl()
{
    return QStringLiteral("http://127.0.0.1:%1/feewhitelist").arg(Port());
}

bool LocalPriceServer::PrepareConfig(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    f.close();
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) return false;
    QJsonObject root = doc.object();
    // A price server publishes its list and the node reads it; it writes to no
    // node and holds no node's login. A config from an older copy of the
    // wallet carries those settings, which the price server refuses: drop them.
    for (const char* key : {"node_rpc", "node_rpcs", "clear_whitelist_on_shutdown"}) root.remove(key);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

void LocalPriceServer::SetPriceServerAsync(interfaces::Node& node, QObject* context, const std::string& url,
                                           std::function<void(bool, const QString&)> done)
{
    // Off the GUI thread: connecting asks the price server first, which may
    // take as long as -feepriceservertimeout, and a frozen window is worse
    // than a moment's wait. The result comes back on the GUI thread, and only
    // while the context still exists.
    QPointer<QObject> guard(context);
    interfaces::Node* nodePtr = &node;
    std::thread([guard, nodePtr, url, done]() {
        bool ok = false;
        QString error;
        UniValue params(UniValue::VARR);
        params.push_back(url);
        try {
            nodePtr->executeRpc("setfeepriceserver", params, std::string());
            ok = true;
        } catch (const UniValue& e) {
            error = e.isObject() && e.exists("message") ? QString::fromStdString(e["message"].get_str()) : QString::fromStdString(e.write());
        } catch (const std::exception& e) {
            error = QString::fromStdString(e.what());
        } catch (...) {
            error = QObject::tr("Unknown error.");
        }
        QMetaObject::invokeMethod(qApp, [guard, done, ok, error]() {
            if (guard) done(ok, error);
        });
    }).detach();
}

void LocalPriceServer::ConnectWhenPublished(interfaces::Node& node, QObject* context, std::function<void(bool, const QString&)> done)
{
    auto* timer = new QTimer(context);
    auto attempts = std::make_shared<int>(0);
    auto inflight = std::make_shared<bool>(false);
    interfaces::Node* nodePtr = &node;
    QObject::connect(timer, &QTimer::timeout, context, [nodePtr, context, timer, attempts, inflight, done]() {
        if (*inflight) return;
        *inflight = true;
        const bool last = ++(*attempts) >= 45; // ~90 s: its first poll fetches the registry and the feed
        SetPriceServerAsync(*nodePtr, timer, ListUrl().toStdString(), [timer, inflight, last, done](bool ok, const QString& error) {
            *inflight = false;
            if (ok || last) {
                timer->stop();
                timer->deleteLater();
                done(ok, ok ? ListUrl() : error);
            }
        });
    });
    timer->start(2000);
}

void FeePolicyDialog::refresh()
{
    m_whitelist->setRowCount(0);
    m_remove->setEnabled(false);
    if (!m_wallet_model) { setStatus(tr("No wallet loaded."), true); return; }

    {
        bool ok = false; QString err;
        m_ps_state = callRpc("getfeepriceserver", UniValue(UniValue::VARR), ok, err);
        showPriceServerState();
    }

    // Shown as prices rather than as the rates the node stores. The stored rate
    // is atoms of the asset per reference fee atom, times a factor for the
    // asset's decimals -- a number that means nothing to read and cannot be
    // sanity-checked by eye, where "1 GOLD is worth 4437 USD" can.
    int accepted = 0;
    for (const FeeAssetInfo& info : m_wallet_model->node().listFeeAssetInfo()) {
        if (!info.listed) continue;
        const int row = m_whitelist->rowCount();
        m_whitelist->insertRow(row);
        // Show the asset the way the rest of the GUI does. The raw key is
        // still what the RPCs take, so it stays available on hover rather
        // than being the thing on display: unexplained, "bitcoin" reads as
        // a claim that this row is Bitcoin.
        QTableWidgetItem* name = roItem(GUIUtil::assetDisplayNameForKey(info.identifier));
        name->setToolTip(tr("Fee policy key: %1").arg(QString::fromStdString(info.identifier)));
        name->setData(Qt::UserRole, QString::fromStdString(info.identifier));
        m_whitelist->setItem(row, 0, name);
        const double price = UnitPriceFromFeeRate(info.rate, info.precision);
        auto* price_item = roItem(info.accepted ? QString::number(price, 'f', price >= 1.0 ? 2 : 8)
                                                : tr("refused"));
        // The rate is still the thing the node acts on, so keep it reachable for
        // anyone comparing this against getfeeacceptancepolicy.
        price_item->setToolTip(tr("Stored as a rate of %1").arg(info.rate));
        m_whitelist->setItem(row, 1, price_item);
        if (info.accepted) accepted++;
    }
    if (accepted == 0) {
        setStatus(m_ps_state.isObject() && m_ps_state["connected"].isTrue()
                      ? tr("No asset is accepted for fees yet, so this node cannot send until its price server sends a list.")
                      : tr("No asset is accepted for fees yet, so this node cannot send. Add one above, connect to a price server, or run your own."),
                  true);
    } else {
        setStatus(tr("%n asset(s) accepted for fees.", "", accepted));
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
    bool priceOk = false;
    const double price = m_rate->text().trimmed().toDouble(&priceOk);
    if (!priceOk || price < 0.0) { setStatus(tr("Enter what one unit is worth in USD (0 refuses the asset)."), true); return; }

    // The price is converted here with the same arithmetic the price server uses
    // (FeeRateFromUnitPrice). Doing it any other way would make a hand-set price
    // and one the price server published mean different things.
    const CAsset asset = GetAssetFromString(assetKey);
    if (asset.IsNull()) { setStatus(tr("Unknown asset: %1").arg(QString::fromStdString(assetKey)), true); return; }
    const uint8_t precision = m_wallet_model ? m_wallet_model->node().getFeeAssetInfo(asset).precision
                                             : DEFAULT_ASSET_PRECISION;
    const long long rate = (price == 0.0) ? 0 : FeeRateFromUnitPrice(price, precision);
    if (price > 0.0 && rate <= 0) { setStatus(tr("That price is too small to represent for this asset."), true); return; }
    if (!confirmHandWrite()) return;

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
    // The key the whitelist holds, not the name on display: the two differ
    // whenever the GUI shows an asset by another name, and erasing the shown
    // name would remove nothing while saying "Removed."
    const std::string assetKey = assetItem->data(Qt::UserRole).toString().toStdString();
    if (!confirmHandWrite()) return;

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
    // One launcher for the whole wallet: the main window's, which tracks the
    // price server it starts (it stops with the wallet) and connects this node
    // to it once it publishes.
    // Called by name: the main window's slot (BitcoinGUI::launchPriceServer).
    QWidget* main = parentWidget() ? parentWidget()->window() : nullptr;
    if (main && QMetaObject::invokeMethod(main, "launchPriceServer")) {
        setStatus(tr("Starting your price server; this node connects to it once it publishes its first list, "
                     "at %1.").arg(LocalPriceServer::ListUrl()));
        return;
    }
    setStatus(tr("Open Run my own price server from the main window's Settings menu."), true);
}
