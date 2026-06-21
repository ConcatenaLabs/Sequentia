// Copyright (c) 2024 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/stakingpage.h>

#include <qt/clientmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <univalue.h>

#include <vector>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// Call an RPC through the node interface; on failure return NullUniValue and set err.
UniValue callRpc(ClientModel* model, const std::string& method, const UniValue& params, QString& err)
{
    err.clear();
    if (!model) { err = QObject::tr("No node connection."); return NullUniValue; }
    try {
        return model->node().executeRpc(method, params, "");
    } catch (const UniValue& e) {
        if (e.isObject() && e.exists("message")) err = QString::fromStdString(e["message"].get_str());
        else err = QString::fromStdString(e.write());
    } catch (const std::exception& e) {
        err = QString::fromUtf8(e.what());
    } catch (...) {
        err = QObject::tr("Unknown RPC error.");
    }
    return NullUniValue;
}

QString shortHex(const std::string& h)
{
    QString s = QString::fromStdString(h);
    if (s.size() > 20) return s.left(10) + QStringLiteral("…") + s.right(8);
    return s;
}
} // namespace

StakingPage::StakingPage(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent), m_platform_style(platformStyle)
{
    QVBoxLayout* root = new QVBoxLayout(this);

    QLabel* title = new QLabel(tr("Proof-of-Stake"), this);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.4);
    tf.setBold(true);
    title->setFont(tf);
    root->addWidget(title);

    m_status = new QLabel(tr("Loading staking status…"), this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    // --- network status ---
    QGroupBox* statusBox = new QGroupBox(tr("Network staking status"), this);
    QFormLayout* form = new QFormLayout(statusBox);
    m_height = new QLabel(QStringLiteral("—"), this);
    m_stakers = new QLabel(QStringLiteral("—"), this);
    m_total_weight = new QLabel(QStringLiteral("—"), this);
    m_committee = new QLabel(QStringLiteral("—"), this);
    m_slot = new QLabel(QStringLiteral("—"), this);
    form->addRow(tr("Next block height:"), m_height);
    form->addRow(tr("Registered stakers:"), m_stakers);
    form->addRow(tr("Total stake weight:"), m_total_weight);
    form->addRow(tr("Committee / quorum:"), m_committee);
    form->addRow(tr("Slot interval:"), m_slot);
    root->addWidget(statusBox);

    // --- leader schedule ---
    QGroupBox* schedBox = new QGroupBox(tr("Leader schedule (next block)"), this);
    QVBoxLayout* schedLayout = new QVBoxLayout(schedBox);
    m_table = new QTableWidget(0, 5, this);
    QStringList headers;
    headers << tr("Rank") << tr("Staker public key") << tr("Weight") << tr("Share") << tr("Slot opens");
    m_table->setHorizontalHeaderLabels(headers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    schedLayout->addWidget(m_table);
    root->addWidget(schedBox, 1);

    // --- staking output helper ---
    QGroupBox* stakeBox = new QGroupBox(tr("Create a staking output"), this);
    QVBoxLayout* stakeLayout = new QVBoxLayout(stakeBox);
    QLabel* hint = new QLabel(tr("Generate the canonical staking script for your staker public key, then send the policy asset to it to register stake. The output unlocks (unbonds) only after the delay below."), this);
    hint->setWordWrap(true);
    stakeLayout->addWidget(hint);
    QHBoxLayout* inRow = new QHBoxLayout();
    m_pubkey_in = new QLineEdit(this);
    m_pubkey_in->setPlaceholderText(tr("Staker public key (hex)"));
    m_csv_blocks = new QSpinBox(this);
    m_csv_blocks->setRange(0, 65535);
    m_csv_blocks->setValue(0);
    m_csv_blocks->setSpecialValueText(tr("chain default"));
    m_csv_blocks->setSuffix(tr(" blocks"));
    m_generate = new QPushButton(tr("Generate"), this);
    inRow->addWidget(m_pubkey_in, 1);
    inRow->addWidget(new QLabel(tr("Unbonding:"), this));
    inRow->addWidget(m_csv_blocks);
    inRow->addWidget(m_generate);
    stakeLayout->addLayout(inRow);
    m_script_out = new QPlainTextEdit(this);
    m_script_out->setReadOnly(true);
    m_script_out->setMaximumHeight(60);
    m_script_out->setPlaceholderText(tr("Staking scriptPubKey (hex) appears here"));
    stakeLayout->addWidget(m_script_out);
    m_script_info = new QLabel(this);
    m_script_info->setWordWrap(true);
    stakeLayout->addWidget(m_script_info);
    m_copy = new QPushButton(tr("Copy script"), this);
    m_copy->setEnabled(false);
    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(m_copy);
    stakeLayout->addLayout(btnRow);
    root->addWidget(stakeBox);

    connect(m_generate, &QPushButton::clicked, this, &StakingPage::generateStakeScript);
    connect(m_copy, &QPushButton::clicked, this, &StakingPage::copyScript);

    m_timer = new QTimer(this);
    m_timer->setInterval(10000);
    connect(m_timer, &QTimer::timeout, this, &StakingPage::refresh);
}

StakingPage::~StakingPage() = default;

void StakingPage::setClientModel(ClientModel* clientModel)
{
    m_client_model = clientModel;
    if (clientModel) {
        refresh();
        m_timer->start();
    } else {
        m_timer->stop();
    }
}

void StakingPage::setWalletModel(WalletModel* walletModel)
{
    m_wallet_model = walletModel;
}

void StakingPage::setStatusMessage(const QString& msg, bool error)
{
    m_status->setText(msg);
    m_status->setStyleSheet(error ? QStringLiteral("color:#c0392b;") : QStringLiteral("color:#27ae60;"));
}

void StakingPage::refresh()
{
    if (!m_client_model) return;

    QString err;
    UniValue sched = callRpc(m_client_model, "getposschedule", UniValue(UniValue::VARR), err);
    if (!err.isEmpty()) {
        setStatusMessage(tr("Proof-of-Stake unavailable: %1").arg(err), true);
        return;
    }

    QString err2;
    UniValue stakers = callRpc(m_client_model, "getstakerinfo", UniValue(UniValue::VARR), err2);
    qulonglong total = 0;
    int nstakers = 0;
    if (err2.isEmpty() && stakers.isObject()) {
        const std::vector<UniValue>& vals = stakers.getValues();
        nstakers = (int)vals.size();
        for (const UniValue& v : vals) total += (qulonglong)v.get_int64();
    }

    setStatusMessage(tr("Proof-of-Stake is active on this chain."), false);
    if (sched.exists("height")) m_height->setText(QString::number(sched["height"].get_int()));
    m_stakers->setText(QString::number(nstakers));
    m_total_weight->setText(QString::number(total));
    if (sched.exists("committee")) {
        int csize = (int)sched["committee"].size();
        int quorum = sched.exists("quorum") ? sched["quorum"].get_int() : 0;
        m_committee->setText(tr("%1 members, quorum %2").arg(csize).arg(quorum));
    } else {
        m_committee->setText(tr("single-signer (no committee)"));
    }
    if (sched.exists("slot_interval")) m_slot->setText(tr("%1 s").arg(QString::number(sched["slot_interval"].get_int64())));

    m_table->setRowCount(0);
    if (sched.exists("schedule") && sched["schedule"].isArray()) {
        const std::vector<UniValue>& rows = sched["schedule"].getValues();
        for (size_t i = 0; i < rows.size(); ++i) {
            const UniValue& e = rows[i];
            int row = (int)i;
            m_table->insertRow(row);
            int rank = e.exists("rank") ? e["rank"].get_int() : (int)i;
            qulonglong w = e.exists("weight") ? (qulonglong)e["weight"].get_int64() : 0;
            QString pk = e.exists("pubkey") ? shortHex(e["pubkey"].get_str()) : QStringLiteral("—");
            double share = total > 0 ? (100.0 * (double)w / (double)total) : 0.0;
            QString slot = QStringLiteral("—");
            if (e.exists("slot_opens")) {
                slot = QDateTime::fromSecsSinceEpoch(e["slot_opens"].get_int64()).toString("yyyy-MM-dd HH:mm:ss");
            }
            m_table->setItem(row, 0, new QTableWidgetItem(rank == 0 ? tr("%1 (leader)").arg(rank) : QString::number(rank)));
            m_table->setItem(row, 1, new QTableWidgetItem(pk));
            m_table->setItem(row, 2, new QTableWidgetItem(QString::number(w)));
            m_table->setItem(row, 3, new QTableWidgetItem(QString::asprintf("%.2f%%", share)));
            m_table->setItem(row, 4, new QTableWidgetItem(slot));
        }
    }
    m_table->resizeColumnsToContents();
}

void StakingPage::generateStakeScript()
{
    if (!m_client_model) return;
    const std::string pk = m_pubkey_in->text().trimmed().toStdString();
    if (pk.empty()) {
        m_script_info->setText(tr("Enter a staker public key (hex) first."));
        return;
    }
    UniValue params(UniValue::VARR);
    params.push_back(UniValue(pk));
    if (m_csv_blocks->value() > 0) params.push_back(UniValue(m_csv_blocks->value()));
    QString err;
    UniValue res = callRpc(m_client_model, "getstakescript", params, err);
    if (!err.isEmpty()) {
        m_script_out->clear();
        m_copy->setEnabled(false);
        m_script_info->setText(tr("Error: %1").arg(err));
        return;
    }
    if (res.exists("script")) m_script_out->setPlainText(QString::fromStdString(res["script"].get_str()));
    m_copy->setEnabled(res.exists("script"));
    QString type = res.exists("csv_type") ? QString::fromStdString(res["csv_type"].get_str()) : QStringLiteral("?");
    qlonglong lock = res.exists("lock_seconds") ? (qlonglong)res["lock_seconds"].get_int64() : 0;
    qlonglong minl = res.exists("min_unbonding_seconds") ? (qlonglong)res["min_unbonding_seconds"].get_int64() : 0;
    m_script_info->setText(tr("Unbonding lock: %1 s (%2). Chain minimum: %3 s. Send the policy asset to this script to stake.")
                               .arg(QString::number(lock), type, QString::number(minl)));
}

void StakingPage::copyScript()
{
    QApplication::clipboard()->setText(m_script_out->toPlainText());
}
