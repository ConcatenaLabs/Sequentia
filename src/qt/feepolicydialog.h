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
 * Operator panel for Sequentia's open fee market: view the assets this node accepts for fee
 * payment (the effective acceptance set = static ∪ dynamic, via getfeeacceptancepolicy), see the
 * price-server-published dynamic layer (getdynamicfeerates), and edit the operator's STATIC
 * whitelist (setfeeexchangerates). No asset is privileged — the policy asset is just one entry.
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

private:
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QTableWidget* m_effective{nullptr}; // Asset | Rate | Origin | Source
    QTableWidget* m_dynamic{nullptr};   // Asset | Rate | Source | Age(s) | Stale
    QComboBox* m_asset{nullptr};         // editable: label or hex
    QLineEdit* m_rate{nullptr};
    QPushButton* m_add{nullptr};
    QPushButton* m_remove{nullptr};
    QPushButton* m_refresh{nullptr};
    QLabel* m_status{nullptr};

    UniValue callRpc(const std::string& method, const UniValue& params, bool& ok, QString& error);
    void setStatus(const QString& msg, bool error = false);
    // The current STATIC layer, recovered from getfeeacceptancepolicy (origin=="static"). Used to
    // rebuild the full map for setfeeexchangerates, which REPLACES the whole static layer.
    std::map<std::string, int64_t> currentStaticRates(bool& ok, QString& err);
};

#endif // BITCOIN_QT_FEEPOLICYDIALOG_H
