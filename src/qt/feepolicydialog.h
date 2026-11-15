// Copyright (c) 2024 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_FEEPOLICYDIALOG_H
#define BITCOIN_QT_FEEPOLICYDIALOG_H

#include <QDialog>
#include <QString>

#include <univalue.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>

class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QTableWidget;
namespace interfaces { class Node; }
class QComboBox;
class QJsonObject;
class QLineEdit;
class QLabel;
class QPushButton;
QT_END_NAMESPACE

/**
 * Operator panel for Sequentia's open fee market. The node accepts a set of assets for fee
 * payment at given rates — that is ONE whitelist (the "fee acceptance policy", via
 * getfeeacceptancepolicy). It is static when written by hand here (setfeeexchangerates), and
 * dynamic when the node is connected to a price server and reads its published list
 * (setfeepriceserver). No asset is privileged — the policy asset is just one entry.
 */
class FeePolicyDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FeePolicyDialog(const PlatformStyle* platformStyle, QWidget* parent = nullptr);
    void setModel(WalletModel* model);

private Q_SLOTS:
    void refresh();
    void onAddOrUpdate();
    void onRemoveSelected();
    void onLaunchPriceServer();
    void onConnectPriceServer();
    void onDisconnectPriceServer();

private:
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QTableWidget* m_whitelist{nullptr}; // Asset | Rate
    QComboBox* m_asset{nullptr};         // editable: label or hex
    QLineEdit* m_rate{nullptr};
    QPushButton* m_add{nullptr};
    QPushButton* m_remove{nullptr};
    QPushButton* m_refresh{nullptr};
    QPushButton* m_launch_price_server{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_ps_status{nullptr};        // which price server, when it last wrote, its last error
    QLineEdit* m_ps_url{nullptr};
    QPushButton* m_ps_connect{nullptr};
    QPushButton* m_ps_disconnect{nullptr};
    UniValue m_ps_state;                 // getfeepriceserver, as of the last refresh
    bool m_ps_busy{false};               // a setfeepriceserver call is in flight
    bool m_remove_selection_connected{false};


    UniValue callRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    void setStatus(const QString& msg, bool error = false);
    // The FULL current acceptance policy (every accepted asset and its rate), read from
    // getfeeacceptancepolicy. setfeeexchangerates REPLACES the whole map, so callers must start
    // from this full set and then add/overwrite/erase only the edited asset to preserve the rest.
    std::map<std::string, int64_t> currentRates(bool& ok, QString& err);
    //! Writing by hand disconnects the node from its price server: ask first.
    bool confirmHandWrite();
    //! The price-server section's text, from m_ps_state.
    void showPriceServerState();
    //! While a price-server call runs off the GUI thread, nothing else here writes.
    void setPriceServerBusy(bool busy);
};

/** SEQUENTIA: running your own price server, from the desktop wallet. To the
 *  node that is being connected to it, like any price server: the bundled
 *  price server publishes its list on this machine and the node connects to
 *  that address, so the price server needs no RPC login to the node. */
namespace LocalPriceServer {
//! The port the bundled price server's web UI and published list listen on,
//! on loopback, unless -ownpriceserverport says otherwise.
static constexpr int DEFAULT_PORT = 8089;
int Port();
//! The address of its published list.
QString ListUrl();
//! Make a config publish its list and write to no node: drop the node RPC
//! settings an older copy of the wallet wrote, which the price server refuses.
bool PrepareConfig(const QString& path);
//! setfeepriceserver url ("" disconnects) off the GUI thread; done(ok, error)
//! is called on the GUI thread, unless context is gone by then.
void SetPriceServerAsync(interfaces::Node& node, QObject* context, const std::string& url,
                         std::function<void(bool, const QString&)> done);
//! Connect node to it once its first list is published, retrying for up to
//! 90 s while it starts; done(ok, message) is called once.
void ConnectWhenPublished(interfaces::Node& node, QObject* context, std::function<void(bool, const QString&)> done);
} // namespace LocalPriceServer

#endif // BITCOIN_QT_FEEPOLICYDIALOG_H
