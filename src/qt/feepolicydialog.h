// Copyright (c) 2024 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_FEEPOLICYDIALOG_H
#define BITCOIN_QT_FEEPOLICYDIALOG_H

#include <QDialog>

#include <univalue.h>

#include <cstdint>
#include <map>
#include <string>

class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QTableWidget;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
QT_END_NAMESPACE

/**
 * Operator panel for Sequentia's open fee market. The node accepts a set of assets for fee
 * payment at given rates — that is ONE whitelist (the "fee acceptance policy", via
 * getfeeacceptancepolicy). Entries are set manually by the operator (setfeeexchangerates) or
 * maintained by a price-server sidecar if one is running. No asset is privileged — the policy
 * asset is just one entry.
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
    bool m_remove_selection_connected{false};

    UniValue callRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    void setStatus(const QString& msg, bool error = false);
    // The FULL current acceptance policy (every accepted asset and its rate), read from
    // getfeeacceptancepolicy. setfeeexchangerates REPLACES the whole map, so callers must start
    // from this full set and then add/overwrite/erase only the edited asset to preserve the rest.
    std::map<std::string, int64_t> currentRates(bool& ok, QString& err);
};

#endif // BITCOIN_QT_FEEPOLICYDIALOG_H
