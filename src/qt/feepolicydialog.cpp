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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
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
                                "when you write it by hand here, and dynamic when a price server "
                                "updates it: connect to one below, another operator's or your own."),
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
    m_ps_url->setPlaceholderText(tr("Address of a price server's published list, e.g. http://host:8090/api/whitelist"));
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
    if (!connected) {
        QString text = tr("Not connected to a price server. The whitelist is static: it is what is written "
                          "to it, by hand here or by a price server of yours that writes to this node.");
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

void FeePolicyDialog::onConnectPriceServer()
{
    const std::string url = m_ps_url->text().trimmed().toStdString();
    if (url.empty()) { setStatus(tr("Enter the address of a price server's published list."), true); return; }
    UniValue params(UniValue::VARR);
    params.push_back(url);
    bool ok = false; QString err;
    callRpc("setfeepriceserver", params, ok, err);
    if (ok) { setStatus(tr("Connected; the whitelist is the price server's.")); refresh(); }
    else { setStatus(err, true); }
}

void FeePolicyDialog::onDisconnectPriceServer()
{
    UniValue params(UniValue::VARR);
    params.push_back(std::string());
    bool ok = false; QString err;
    callRpc("setfeepriceserver", params, ok, err);
    if (ok) { setStatus(tr("Disconnected. The list in force is kept as a static whitelist.")); refresh(); }
    else { setStatus(err, true); }
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

bool FeePolicyDialog::nodeRpcEndpoint(QJsonObject& out) const
{
    // Where THIS node listens, and how the sidecar should authenticate to it.
    // Prefer the cookie: it is what the node writes for its own tools, and it
    // keeps an rpcpassword out of a second file on disk. The sidecar re-reads
    // the cookie on every call, so a node restart rotating it is harmless.
    const uint16_t port = static_cast<uint16_t>(gArgs.GetIntArg("-rpcport", BaseParams().RPCPort()));
    out = QJsonObject();
    out["host"] = QStringLiteral("127.0.0.1");
    out["port"] = int(port);

    const std::string user = gArgs.GetArg("-rpcuser", "");
    const std::string pass = gArgs.GetArg("-rpcpassword", "");
    if (!user.empty() && !pass.empty()) {
        out["user"] = QString::fromStdString(user);
        out["password"] = QString::fromStdString(pass);
        return true;
    }
    const fs::path cookie = AbsPathForConfigVal(fs::PathFromString(gArgs.GetArg("-rpccookiefile", ".cookie")));
    out["cookie"] = QString::fromStdString(fs::PathToString(cookie));
    return true;
}

void FeePolicyDialog::writeNodeRpcInto(const QString& path) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) return;

    QJsonObject root = doc.object();
    QJsonObject rpc;
    if (!nodeRpcEndpoint(rpc)) return;
    // Keep the file's own explanation of the field; only the values are ours.
    const QJsonObject existing = root.value("node_rpc").toObject();
    if (existing.contains("_comment")) rpc["_comment"] = existing.value("_comment");
    root["node_rpc"] = rpc;

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
}

QString FeePolicyDialog::nodeRpcMismatch(const QString& path) const
{
    // An EXISTING config can point somewhere else entirely -- an older copy of
    // the example, or a node that has since moved port. The sidecar will not
    // complain in any way the operator sees: it polls, prices, pushes, and the
    // pushes are refused. Say so here, where they are about to start it.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QByteArray raw = f.readAll();
    f.close();
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) return QString();

    const QJsonObject rpc = doc.object().value("node_rpc").toObject();
    if (rpc.isEmpty()) return QString();
    const int configured = rpc.value("port").toInt();
    const uint16_t actual = static_cast<uint16_t>(gArgs.GetIntArg("-rpcport", BaseParams().RPCPort()));
    if (configured == int(actual)) return QString();
    return tr("The price server config at %1 publishes to port %2, but this node listens on %3. "
              "Nothing will reach it until that is corrected — the sidecar reports the refusals "
              "only in its own log.")
        .arg(path).arg(configured).arg(int(actual));
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

    // A Python interpreter that can actually run the sidecar (on Windows the bare
    // name "python3" is usually a Store stub that runs nothing).
    const QString python = GUIUtil::findPythonInterpreter(sdir);
    if (python.isEmpty()) {
        setStatus(tr("The price server needs Python, and no usable interpreter was found. Install Python 3 "
                     "and try again (on Windows the built-in \"python3\" shortcut only opens the Microsoft "
                     "Store and cannot run the server)."), true);
        return;
    }

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
        // The example ships a placeholder RPC endpoint, and copying it verbatim
        // was a trap: the sidecar starts, polls, prices everything correctly and
        // pushes it all at a port with no node behind it, for as long as nobody
        // reads its log. We know exactly where this node listens and how to
        // authenticate to it, so write that in rather than leave it to be
        // discovered later by its absence.
        QFile::setPermissions(cfg, QFile::ReadOwner | QFile::WriteOwner);
        writeNodeRpcInto(cfg);
    }

    const QString mismatch = nodeRpcMismatch(cfg);

    const int uiPort = 8089;
    const QStringList args{script, "--config", cfg, "--ui-port", QString::number(uiPort), "--ui-host", "127.0.0.1"};
    if (!QProcess::startDetached(python, args, sdir)) {
        setStatus(tr("Failed to start the price server using '%1'. Ensure Python is available.").arg(python), true);
        return;
    }

    // Give the sidecar a moment to bind, then open its configuration UI in the browser.
    const QString url = QString("http://127.0.0.1:%1/").arg(uiPort);
    QTimer::singleShot(1500, this, [url]{ QDesktopServices::openUrl(QUrl(url)); });
    if (!mismatch.isEmpty()) {
        setStatus(mismatch, true);
        return;
    }
    const bool connected = m_ps_state.isObject() && m_ps_state["connected"].isTrue();
    setStatus(connected
        ? tr("Your price server is starting; its configuration page will open at %1. Its first write to this "
             "node disconnects it from %2, and from then on your price server keeps the whitelist up to date.")
              .arg(url, QString::fromStdString(m_ps_state["url"].get_str()))
        : tr("Your price server is starting; its configuration page will open at %1. "
             "It keeps the whitelist up to date; Refresh to see updates.").arg(url));
}
